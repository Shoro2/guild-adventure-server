# Clones (or updates) the Guild Adventure core and mod-playerbots into _work and links
# our module into the core's modules folder.
#
#   .\tools\setup-server.ps1            # clone/fetch, check out the pinned commits
#   .\tools\setup-server.ps1 -Latest    # move playerbots to the tip of its branch and
#                                       # write the new commit into tools\pins.json
#
# Core:       origin   = Shoro2/azerothcore-ga (private, branch ga/main, our core patches)
#             upstream = mod-playerbots/azerothcore-wotlk (branch Playerbot), push disabled
# Playerbots: modules\mod-playerbots = mod-playerbots/mod-playerbots, pinned commit, plus our patches
#             (patches\mod-playerbots\*.patch, applied by apply-playerbots-patches.ps1)
# Extra:      modules\<name> for every entry of pins.json "modules" (third-party AzerothCore modules,
#             e.g. mod-ah-bot-plus), pinned commit; -Latest does not move them
# Module:     modules\mod-guild-adventure = junction to the repo's module folder
#
# Updating the core itself (merging upstream Playerbot into ga/main) is a manual,
# reviewed step - see docs\server-setup.md "Updating upstream".
#
# Development instance (GA_INSTANCE set, tools\ga-env.ps1): _work\azerothcore-<name> becomes a
# detached git worktree of the integration core checkout at origin/ga/main (or -CoreRef),
# with a junction to the integration checkout's mod-playerbots (shared, never changed from
# here) and a junction to the module of the repository this script runs from.
#
#   $env:GA_INSTANCE = 'm4'; .\tools\setup-server.ps1

param(
    [switch] $Latest,
    # Development instance only: the core commit to check out (default origin/ga/main).
    [string] $CoreRef
)

. "$PSScriptRoot\ga-env.ps1"

function Invoke-Git
{
    param([string] $Repo, [Parameter(ValueFromRemainingArguments)] [string[]] $GitArgs)
    & git -C $Repo @GitArgs
    if ($LASTEXITCODE -ne 0) { throw "git $($GitArgs -join ' ') failed in $Repo" }
}

New-Item -ItemType Directory -Force $GA.Work | Out-Null
$pins = $GA.Pins

function Set-GaJunction([string] $Link, [string] $Target)
{
    if (Test-Path $Link)
    {
        $item = Get-Item $Link -Force
        if ($item.LinkType -ne 'Junction') { throw "$Link exists and is not a junction" }
        if ($item.Target -and (($item.Target | Select-Object -First 1) -replace '^\\\?\?\\', '') -ne $Target)
        {
            [IO.Directory]::Delete($Link)   # removes the junction only, never its target
            New-Item -ItemType Junction -Path $Link -Target $Target | Out-Null
        }
    }
    else
    {
        New-Item -ItemType Junction -Path $Link -Target $Target | Out-Null
    }
    Write-Host "junction:   $Link -> $Target"
}

if ($GA.Instance)
{
    # --- development instance: worktree of the integration core + junctions -------------
    $mainCore = Join-Path $GA.Work 'azerothcore-wotlk'
    if (-not (Test-Path (Join-Path $mainCore '.git'))) { throw "Integration core $mainCore missing - run this script without GA_INSTANCE first" }
    Invoke-Git $mainCore fetch origin $pins.core.branch
    $ref = if ($CoreRef) { $CoreRef } else { "origin/$($pins.core.branch)" }
    if (-not (Test-Path (Join-Path $GA.Core '.git')))
    {
        Invoke-Git $mainCore worktree add --detach $GA.Core $ref
    }
    elseif ($CoreRef)
    {
        Invoke-Git $GA.Core checkout --detach $CoreRef
    }
    Write-Host "core:       $((& git -C $GA.Core rev-parse HEAD).Trim()) (worktree of $mainCore)"
    Set-GaJunction $GA.Playerbots (Join-Path $mainCore 'modules\mod-playerbots')
    foreach ($extra in @($pins.modules))
    {
        if (-not $extra) { continue }
        $shared = Join-Path $mainCore "modules\$($extra.name)"
        if (Test-Path (Join-Path $shared '.git')) { Set-GaJunction (Join-Path $GA.Core "modules\$($extra.name)") $shared }
    }
    Set-GaJunction $GA.ModuleLink $GA.ModuleSrc
    return
}

# --- core -------------------------------------------------------------------------
if ($pins.core.patches)
{
    # Public layout: the upstream Playerbot branch at its pinned commit plus our core patches as patch files
    # (patches\azerothcore, docs\core-patches.md). The private workbench pins a fork instead and has no "patches" key.
    if (-not (Test-Path (Join-Path $GA.Core '.git')))
    {
        & git clone --branch $pins.core.branch $pins.core.repo $GA.Core
        if ($LASTEXITCODE -ne 0) { throw 'core clone failed' }
    }
    Invoke-Git $GA.Core fetch origin $pins.core.branch
    # The patches are committed on a local branch "guild-adventure"; a stamp of the pinned commit and the patch files'
    # hashes says whether that branch is current. A change (a pull with a new pin or patch) builds the branch anew;
    # edits of tracked core files on it are discarded then. Untracked folders (the modules) are never touched.
    $patchFiles = @($pins.core.patches | ForEach-Object { Join-Path (Split-Path $PSScriptRoot -Parent) $_ })
    $want = (@($pins.core.commit) + @($patchFiles | ForEach-Object { (Get-FileHash $_ -Algorithm SHA256).Hash })) -join ' '
    $stampFile = Join-Path $GA.Core '.git\guild-adventure-patches'
    $have = if (Test-Path $stampFile) { (Get-Content $stampFile -Raw).Trim() } else { '' }
    if ($have -ne $want)
    {
        Invoke-Git $GA.Core checkout -f -B guild-adventure $pins.core.commit
        foreach ($file in $patchFiles)
        {
            Invoke-Git $GA.Core apply --index --whitespace=nowarn $file
            Write-Host "core patch: $file"
        }
        Invoke-Git $GA.Core -c user.name=GuildAdventure -c user.email=setup@localhost commit -q -m 'Guild Adventure core patches'
        Set-Content $stampFile $want -Encoding ascii
    }
    Write-Host "core:       $($pins.core.commit) + $(@($pins.core.patches).Count) patch file(s) (branch guild-adventure)"
}
elseif (-not (Test-Path (Join-Path $GA.Core '.git')))
{
    & git clone --branch $pins.core.branch $pins.core.repo $GA.Core
    if ($LASTEXITCODE -ne 0) { throw 'core clone failed' }
}
if (-not $pins.core.patches) {
$remotes = & git -C $GA.Core remote
if ($remotes -notcontains 'upstream')
{
    Invoke-Git $GA.Core remote add upstream $pins.core.upstreamRepo
    Invoke-Git $GA.Core remote set-url --push upstream DISABLED
}
Invoke-Git $GA.Core fetch origin $pins.core.branch
$coreHead = (& git -C $GA.Core rev-parse HEAD).Trim()
Write-Host "core:       $coreHead ($(& git -C $GA.Core rev-parse --abbrev-ref HEAD))"
}

# --- mod-playerbots -----------------------------------------------------------------
if (-not (Test-Path (Join-Path $GA.Playerbots '.git')))
{
    & git clone --branch $pins.playerbots.branch $pins.playerbots.repo $GA.Playerbots
    if ($LASTEXITCODE -ne 0) { throw 'mod-playerbots clone failed' }
}
Invoke-Git $GA.Playerbots fetch origin $pins.playerbots.branch
if ($Latest)
{
    Invoke-Git $GA.Playerbots checkout -B $pins.playerbots.branch "origin/$($pins.playerbots.branch)"
    $pins.playerbots.commit = (& git -C $GA.Playerbots rev-parse HEAD).Trim()
    $pins | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $PSScriptRoot 'pins.json') -Encoding ascii
    Write-Host 'pins.json updated - record the new commit in docs\server-setup.md'
}
else
{
    Invoke-Git $GA.Playerbots checkout -B $pins.playerbots.branch $pins.playerbots.commit
}
Write-Host "playerbots: $((& git -C $GA.Playerbots rev-parse HEAD).Trim())"
# Our patches of mod-playerbots (patches\mod-playerbots, docs\core-patches.md "mod-playerbots").
& (Join-Path $PSScriptRoot 'apply-playerbots-patches.ps1') -Checkout $GA.Playerbots

# --- extra AzerothCore modules (pins.json "modules") --------------------------------
foreach ($extra in @($pins.modules))
{
    if (-not $extra) { continue }
    $dir = Join-Path $GA.Core "modules\$($extra.name)"
    if (-not (Test-Path (Join-Path $dir '.git')))
    {
        & git clone --branch $extra.branch $extra.repo $dir
        if ($LASTEXITCODE -ne 0) { throw "$($extra.name) clone failed" }
    }
    Invoke-Git $dir fetch origin $extra.branch
    Invoke-Git $dir checkout -B $extra.branch $extra.commit
    Write-Host ("{0,-11} {1}" -f "$($extra.name):", (& git -C $dir rev-parse HEAD).Trim())
}

# --- mod-guild-adventure junction ---------------------------------------------------
if (Test-Path $GA.ModuleLink)
{
    $item = Get-Item $GA.ModuleLink -Force
    if ($item.LinkType -ne 'Junction') { throw "$($GA.ModuleLink) exists and is not a junction" }
}
else
{
    New-Item -ItemType Junction -Path $GA.ModuleLink -Target $GA.ModuleSrc | Out-Null
}
Write-Host "module:     $($GA.ModuleLink) -> $($GA.ModuleSrc)"
