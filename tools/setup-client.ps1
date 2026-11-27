# Copies the clean WoW 3.3.5a client into _work\client, points it at the local
# Guild Adventure authserver and junctions the GuildAdventure addon into it.
#
# The source folder is never modified (note the real "Wacraft" typo in its path).
#
#   .\tools\setup-client.ps1            # copy (if missing) + realmlist + addon junction
#   .\tools\setup-client.ps1 -Force     # re-run robocopy even if the client exists

param(
    [string] $Source = '<your 3.3.5a client>',
    [switch] $Force
)

. "$PSScriptRoot\ga-env.ps1"

if (-not (Test-Path (Join-Path $GA.Client 'Wow.exe')) -or $Force)
{
    if (-not (Test-Path (Join-Path $Source 'Wow.exe'))) { throw "Clean client not found: $Source" }
    Write-Host "Copying client: $Source -> $($GA.Client)"
    robocopy $Source $GA.Client /E /MT:16 /R:1 /W:1 /NFL /NDL /NP | Out-Host
    # robocopy: 0-7 = success (1 = files copied), >= 8 = failure
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed with exit code $LASTEXITCODE" }
}

# Realmlist: every locale folder under Data\ that carries a realmlist.wtf.
$realmlist = "set realmlist 127.0.0.1:$($GA.AuthPort)"
$files = Get-ChildItem (Join-Path $GA.Client 'Data') -Recurse -Filter realmlist.wtf
if (-not $files) { throw 'No realmlist.wtf found under the client Data folder' }
foreach ($f in $files)
{
    Set-Content -Path $f.FullName -Value $realmlist -Encoding ascii
    Write-Host "realmlist: $($f.FullName) -> $realmlist"
}

# Addon junction (the addon source stays in the repo).
if (Test-Path $GA.AddonSrc)
{
    if (Test-Path $GA.AddonLink)
    {
        $item = Get-Item $GA.AddonLink -Force
        if ($item.LinkType -ne 'Junction') { throw "$($GA.AddonLink) exists and is not a junction - remove it manually" }
    }
    else
    {
        New-Item -ItemType Junction -Path $GA.AddonLink -Target $GA.AddonSrc | Out-Null
    }
    Write-Host "addon: $($GA.AddonLink) -> $($GA.AddonSrc)"
}
else
{
    Write-Warning "Addon source not found ($($GA.AddonSrc)); junction skipped."
}
