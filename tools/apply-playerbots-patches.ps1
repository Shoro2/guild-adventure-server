# Applies this repository's patches for mod-playerbots (patches\mod-playerbots\*.patch, in name order) to a checkout of
# the pinned commit (tools\pins.json). Idempotent: a patch that is applied already (its reverse applies) is skipped.
# setup-server.ps1 calls it after it checked out the pinned commit; run it by hand on an existing checkout:
#
#   .\tools\apply-playerbots-patches.ps1                         # the checkout of the current instance ($GA.Playerbots)
#   .\tools\apply-playerbots-patches.ps1 -Checkout <folder>      # any checkout
#   .\tools\apply-playerbots-patches.ps1 -Check                  # only say which are applied
#
# The patches are listed in docs\core-patches.md ("mod-playerbots"). A rebuild of the worldserver follows a change.

param(
    [string] $Checkout,
    [switch] $Check
)

. "$PSScriptRoot\ga-env.ps1"

if (-not $Checkout) { $Checkout = $GA.Playerbots }
if (-not (Test-Path (Join-Path $Checkout 'src'))) { throw "No mod-playerbots checkout at $Checkout" }
$patchDir = Join-Path $GA.Root 'patches\mod-playerbots'
$patches = @(Get-ChildItem $patchDir -Filter *.patch -ErrorAction SilentlyContinue | Sort-Object Name)
if (-not $patches.Count) { Write-Host "no patches in $patchDir"; return }

function Test-GitApply([string] $patch, [switch] $Reverse)
{
    $gitArgs = @('-C', $Checkout, 'apply', '--check')
    if ($Reverse) { $gitArgs += '--reverse' }
    $gitArgs += $patch
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & git @gitArgs 2>&1 | Out-Null } finally { $ErrorActionPreference = $old }
    return $LASTEXITCODE -eq 0
}

foreach ($patch in $patches)
{
    if (Test-GitApply $patch.FullName -Reverse)
    {
        Write-Host "applied:    $($patch.Name)"
        continue
    }
    if ($Check)
    {
        Write-Host "missing:    $($patch.Name)"
        continue
    }
    if (-not (Test-GitApply $patch.FullName)) { throw "$($patch.Name) does not apply to $Checkout (pinned commit $($GA.Pins.playerbots.commit)?)" }
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & git -C $Checkout apply $patch.FullName 2>&1 | Out-Null } finally { $ErrorActionPreference = $old }
    if ($LASTEXITCODE -ne 0) { throw "git apply $($patch.Name) failed" }
    Write-Host "applied now: $($patch.Name)"
}
