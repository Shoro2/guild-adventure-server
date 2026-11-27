# Shared paths and settings for the Guild Adventure workbench scripts.
# Dot-source it: . "$PSScriptRoot\ga-env.ps1"
#
# Everything lives under the repository root (the design log sections 4 and 5). The only external
# dependency locations are the build toolchain (Boost, OpenSSL, MySQL Server 8.4) and
# the shared MySQL84 service.
#
# Server instances. Without GA_INSTANCE the scripts work on the integration server
# (_work\server, ports 3726/8087/7880, GA_* databases), which the integration server owns.
# A development instance (GA_INSTANCE=<name>, see $GaInstances below) has its own core
# checkout, build tree, runtime, ports and databases, so a thread can build and restart
# it without touching the integration server:
#
#   $env:GA_INSTANCE = 'm4'; .\tools\build-server.ps1
#
# Instance <name> uses _work\azerothcore-<name> (a worktree of the core repo with junctions
# to mod-playerbots and to the module of the repository the script runs from),
# _work\build-<name>, _work\server-<name> and the databases GA_<name>_auth/_world/
# _characters/_playerbots. The client data (_work\server\Data) is shared read-only.
#
# Scripts may run from a worktree under _work (e.g. _work\wt-m4\tools): _work is then the
# nearest parent folder of that name. GA_WORK overrides the detection.
#
# Guard: from a worktree or clone under _work the scripts refuse to run without GA_INSTANCE,
# unless that checkout is the one the integration server is built from (the module junction
# of _work\azerothcore-wotlk points into it). A forgotten GA_INSTANCE would otherwise build,
# configure or command the integration server from a development checkout. To work on the
# integration server from another checkout on purpose (e.g. to re-point its junction with
# setup-server.ps1), set GA_INSTANCE=integration.

$ErrorActionPreference = 'Stop'

# Development instances: name -> ports. Never reuse a port of another server on this box
# (3724/8085 are left free; the server here uses 3726/8087/7880).
# Add a line while a development instance exists and delete it with the instance (the design log section 7: an
# instance is stopped and deleted once its tests are done; m4, m5, watch, ach and storage went on 2026-11-12).
# Example: name = @{ AuthPort = 3727; WorldPort = 8088; SoapPort = 7881; Purpose = '...' }
$GaInstances = @{
}

function Find-GaWork([string] $root)
{
    if ($env:GA_WORK) { return $env:GA_WORK }
    $dir = Get-Item $root
    while ($dir)
    {
        if ($dir.Name -ieq '_work') { return $dir.FullName }
        $dir = $dir.Parent
    }
    return Join-Path $root '_work'
}

$GA = [ordered]@{}
$GA.Root        = Split-Path -Parent $PSScriptRoot
$GA.Work        = Find-GaWork $GA.Root
$GA.Instance    = if ($env:GA_INSTANCE) { $env:GA_INSTANCE.ToLowerInvariant() } else { '' }
$GaExplicitIntegration = $GA.Instance -eq 'integration'
if ($GaExplicitIntegration) { $GA.Instance = '' }
if ($GA.Instance -and -not $GaInstances.ContainsKey($GA.Instance))
{
    throw "Unknown GA_INSTANCE '$($GA.Instance)' - known: $($GaInstances.Keys -join ', '), integration (tools\ga-env.ps1)"
}

# True when the integration core's module junction points into this repository (or nothing is
# set up yet): this checkout is the integration server's own.
function Test-GaIntegrationCheckout([string] $work, [string] $root)
{
    $link = Join-Path $work 'azerothcore-wotlk\modules\mod-guild-adventure'
    if (-not (Test-Path $link)) { return $true }
    $target = (@((Get-Item $link -Force).Target) | Select-Object -First 1) -replace '^\\\?\?\\', ''
    if (-not $target) { return $false }
    $own = Join-Path $root 'modules\mod-guild-adventure'
    return [IO.Path]::GetFullPath($target).TrimEnd('\') -ieq [IO.Path]::GetFullPath($own).TrimEnd('\')
}

if (-not $GA.Instance -and -not $GaExplicitIntegration -and
    $GA.Root.StartsWith($GA.Work.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) -and
    -not (Test-GaIntegrationCheckout $GA.Work $GA.Root))
{
    throw ("GA_INSTANCE is not set and this checkout ($($GA.Root)) is not the one the integration server is built from. " +
        "Set `$env:GA_INSTANCE to a development instance ($($GaInstances.Keys -join ', ')), or to 'integration' to work on " +
        "the integration server on purpose (tools\ga-env.ps1).")
}
$suffix         = if ($GA.Instance) { "-$($GA.Instance)" } else { '' }
$GA.Core        = Join-Path $GA.Work $(if ($GA.Instance) { "azerothcore$suffix" } else { 'azerothcore-wotlk' })
$GA.Playerbots  = Join-Path $GA.Core 'modules\mod-playerbots'
$GA.ModuleLink  = Join-Path $GA.Core 'modules\mod-guild-adventure'
$GA.ModuleSrc   = Join-Path $GA.Root 'modules\mod-guild-adventure'
$GA.Build       = Join-Path $GA.Work "build$suffix"
$GA.Server      = Join-Path $GA.Work "server$suffix"
$GA.Configs     = Join-Path $GA.Server 'configs'
$GA.Data        = Join-Path $GA.Work 'server\Data'   # shared by every instance, read only
$GA.Client      = Join-Path $GA.Work 'client'
$GA.AddonSrc    = Join-Path $GA.Root 'addon\GuildAdventure'
$GA.AddonLink   = Join-Path $GA.Client 'Interface\AddOns\GuildAdventure'

# Local MySQL client option file with the DB credentials ([client] host/port/user/password).
# Lives in the git-ignored _work folder; never commit credentials.
$GA.MySqlCnf    = Join-Path $GA.Work 'mysql-client.cnf'

# Pinned versions (single source: tools\pins.json, documented in docs\server-setup.md).
$GA.Pins        = Get-Content (Join-Path $PSScriptRoot 'pins.json') -Raw | ConvertFrom-Json

# Toolchain (build PC defaults; override with environment variables if they move).
$GA.BoostRoot   = if ($env:GA_BOOST_ROOT)   { $env:GA_BOOST_ROOT }   else { 'C:\local\boost_1_81_0' }
$GA.OpenSslRoot = if ($env:GA_OPENSSL_ROOT) { $env:GA_OPENSSL_ROOT } else { 'C:\OpenSSL-Win64' }
$GA.MySqlRoot   = if ($env:GA_MYSQL_ROOT)   { $env:GA_MYSQL_ROOT }   else { 'C:\Program Files\MySQL\MySQL Server 8.4' }
$GA.MySqlExe    = Join-Path $GA.MySqlRoot 'bin\mysql.exe'
$GA.MySqlSvc    = 'MySQL84'
$GA.BuildConfig = 'RelWithDebInfo'

# Network / database layout. Never reuse the ports of the other servers on this box
# (the defaults 3724/8085 are left free).
$GA.AuthPort    = 3726
$GA.WorldPort   = 8087
$GA.SoapPort    = 7880     # localhost only
$GA.RealmName   = 'Guild Adventure'
# Auction house bot (mod-ah-bot-plus): its account and character; the character never enters the world
# (docs\server-setup.md "Auction house bot").
$GA.AhBotAccount   = 'GAAHBOT1'
$GA.AhBotCharacter = 'Tradewind'
$dbPrefix       = 'GA_'
if ($GA.Instance)
{
    $inst = $GaInstances[$GA.Instance]
    $GA.AuthPort  = $inst.AuthPort
    $GA.WorldPort = $inst.WorldPort
    $GA.SoapPort  = $inst.SoapPort
    $GA.RealmName = "Guild Adventure ($($GA.Instance))"
    $dbPrefix     = "GA_$($GA.Instance)_"
}
$GA.Databases   = [ordered]@{
    Auth       = "${dbPrefix}auth"
    World      = "${dbPrefix}world"
    Characters = "${dbPrefix}characters"
    Playerbots = "${dbPrefix}playerbots"
}

function Invoke-GaMySql
{
    param(
        [Parameter(Mandatory)] [string] $Sql,
        [string] $Database
    )
    if (-not (Test-Path $GA.MySqlCnf)) { throw "Missing $($GA.MySqlCnf) - see docs\server-setup.md" }
    $mysqlArgs = @("--defaults-extra-file=$($GA.MySqlCnf)", '--batch', '--skip-column-names')
    if ($Database) { $mysqlArgs += $Database }
    $mysqlArgs += @('-e', $Sql)
    $out = & $GA.MySqlExe @mysqlArgs
    if ($LASTEXITCODE -ne 0) { throw "mysql failed ($LASTEXITCODE): $Sql" }
    return $out
}

function Get-GaMySqlCredential
{
    if (-not (Test-Path $GA.MySqlCnf)) { throw "Missing $($GA.MySqlCnf) - see docs\server-setup.md" }
    $cred = @{ host = '127.0.0.1'; port = '3306'; user = ''; password = '' }
    foreach ($line in Get-Content $GA.MySqlCnf)
    {
        if ($line -match '^\s*(host|port|user|password)\s*=\s*(.*?)\s*$') { $cred[$Matches[1]] = $Matches[2] }
    }
    return $cred
}
