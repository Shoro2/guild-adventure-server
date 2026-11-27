# Sets up a throwaway load-test instance from the integration server, without a build:
# its binaries and PDBs are copied, its four databases cloned, its configs copied and
# re-pointed, and test guilds with leaders on throwaway accounts are created by SQL.
# Nothing of the integration server is changed (mysqldump reads it in one transaction).
#
#   $env:GA_INSTANCE = 'perf'                       # registered in tools\ga-env.ps1 ($GaInstances)
#   .\tools\loadtest\setup-instance.ps1 -Guilds 40 -MapThreads 4
#   .\tools\start-server.ps1 -WorldOnly             # no authserver: nobody logs in with a client
#   .\tools\loadtest\load.ps1 recruit 1 40          # see load.ps1
#
# Tear down with teardown-instance.ps1 (server stopped, folder and databases deleted).
#
# The test guilds: owner accounts 1001.., leader characters 5001.. (copies of the probe
# leader, never logged in), WoW guilds 101.., ga_guild rows with 40 slots and an offline
# window that starts now (the module caps it at 8 hours), so their bots log in with
# `load.ps1 login`, play on without an owner and come back by themselves after a restart.

param(
    [int] $Guilds = 40,
    [int] $MapThreads = 4,
    [switch] $SkipRuntime,
    [switch] $ConfigsOnly,
    [switch] $SkipClone
)

. "$PSScriptRoot\..\ga-env.ps1"
. "$PSScriptRoot\loadtest-common.ps1"
if (-not $GA.Instance) { throw 'Set GA_INSTANCE to a development instance registered in tools\ga-env.ps1' }

$src = Join-Path $GA.Work 'server'
$dst = $GA.Server
$inst = $GA.Instance
$mysql = $GA.MySqlExe
$dump = Join-Path $GA.MySqlRoot 'bin\mysqldump.exe'
$cnf = $GA.MySqlCnf

if (Get-Process worldserver -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$dst\*" })
{
    throw "The worldserver of instance '$inst' is running - stop it first"
}

# 1. Runtime: binaries, PDBs, DLLs, configs (not Data, logs, crash dumps). With -ConfigsOnly only the configs: the
#    instance has binaries of its own (built from a branch with build-server.ps1), measured against the live configs.
if ($ConfigsOnly)
{
    robocopy (Join-Path $src 'configs') (Join-Path $dst 'configs') /E /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
    New-Item -ItemType Directory -Force (Join-Path $dst 'logs') | Out-Null
    Write-Host "configs copied to $dst (binaries kept)"
}
elseif (-not $SkipRuntime)
{
    robocopy $src $dst /E /XD Data logs Crashes /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
    New-Item -ItemType Directory -Force (Join-Path $dst 'logs') | Out-Null
    Write-Host "runtime copied to $dst"
}

# 2. Databases: GA_<x> -> GA_<inst>_<x>. cmd does the pipe (PowerShell would re-encode the dump).
if (-not $SkipClone)
{
    foreach ($d in 'auth', 'characters', 'playerbots', 'world')
    {
        $from = "GA_$d"; $to = "GA_${inst}_$d"
        Write-Host "$(Get-Date -Format HH:mm:ss) $from -> $to"
        & $mysql "--defaults-extra-file=$cnf" -e "DROP DATABASE IF EXISTS ``$to``; CREATE DATABASE ``$to`` DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;"
        if ($LASTEXITCODE -ne 0) { throw "could not create $to" }
        cmd /c "`"$dump`" --defaults-extra-file=`"$cnf`" --single-transaction --quick --routines --no-tablespaces --set-gtid-purged=OFF $from | `"$mysql`" --defaults-extra-file=`"$cnf`" $to"
        if ($LASTEXITCODE -ne 0) { throw "clone of $from failed" }
    }
}

# 3. Configs: the integration server's own (the owner's live values), re-pointed.
function Edit-Conf([string] $file, [scriptblock] $edit)
{
    $text = [IO.File]::ReadAllText($file)
    $text = & $edit $text
    [IO.File]::WriteAllText($file, $text)
}
function Set-Key([string] $text, [string] $key, [string] $value)
{
    $pattern = '(?m)^(\s*)' + [regex]::Escape($key) + '\s*=.*$'
    if ($text -notmatch $pattern) { return $text.TrimEnd() + "`r`n$key = $value`r`n" }
    return [regex]::Replace($text, $pattern, { param($m) "$($m.Groups[1].Value)$key = $value" })
}
$logs = (Join-Path $dst 'logs') -replace '\\', '/'
$dbRe = ';GA_(auth|world|characters|playerbots)"'
Edit-Conf (Join-Path $GA.Configs 'worldserver.conf') {
    param($t)
    $t = [regex]::Replace($t, $dbRe, ";GA_${inst}_`$1`"")
    $t = Set-Key $t 'WorldServerPort' "$($GA.WorldPort)"
    $t = Set-Key $t 'SOAP.Port' "$($GA.SoapPort)"
    $t = Set-Key $t 'LogsDir' "`"$logs`""
    $t = Set-Key $t 'Updates.EnableDatabases' '0'
    $t = Set-Key $t 'MapUpdate.Threads' "$MapThreads"
    return $t
}
# The authserver is not needed for the bots, but a copied config must not point at the integration server's port,
# database and logs when somebody starts it (for a ga-probe check against the instance).
Edit-Conf (Join-Path $GA.Configs 'authserver.conf') {
    param($t)
    $t = [regex]::Replace($t, $dbRe, ";GA_${inst}_`$1`"")
    $t = Set-Key $t 'RealmServerPort' "$($GA.AuthPort)"
    $t = Set-Key $t 'LogsDir' "`"$logs`""
    return $t
}
Edit-Conf (Join-Path $GA.Configs 'modules\playerbots.conf') {
    param($t)
    $t = [regex]::Replace($t, $dbRe, ";GA_${inst}_`$1`"")
    $t = Set-Key $t 'Playerbots.Updates.EnableDatabases' '0'
    return $t
}
Write-Host "configs re-pointed (ports $($GA.WorldPort)/$($GA.SoapPort), databases GA_${inst}_*, $MapThreads map threads)"

# 4. The clone's own guilds stay offline; the test guilds are created.
$chars = "GA_${inst}_characters"; $auth = "GA_${inst}_auth"
$sql = New-Object Text.StringBuilder
[void]$sql.AppendLine("UPDATE $chars.ga_guild SET owner_logout_at = 1;")
[void]$sql.AppendLine("UPDATE $chars.characters SET online = 0;")
[void]$sql.AppendLine("UPDATE $auth.account SET online = 0;")
[void]$sql.AppendLine("UPDATE $auth.realmlist SET flag = 0, name = 'Guild Adventure ($inst)', port = $($GA.WorldPort) WHERE id = 1;")
# re-runnable: what an earlier run of this step made goes first
[void]$sql.AppendLine("DELETE FROM $auth.account WHERE id BETWEEN 1001 AND $(1000 + $Guilds);")
[void]$sql.AppendLine("DELETE FROM $chars.characters WHERE guid BETWEEN 5001 AND $(5000 + $Guilds);")
[void]$sql.AppendLine("DELETE FROM $chars.guild WHERE guildid BETWEEN 101 AND $(100 + $Guilds);")
[void]$sql.AppendLine("DELETE FROM $chars.guild_rank WHERE guildid BETWEEN 101 AND $(100 + $Guilds);")
[void]$sql.AppendLine("DELETE FROM $chars.guild_member WHERE guildid BETWEEN 101 AND $(100 + $Guilds);")
[void]$sql.AppendLine("DELETE FROM $chars.ga_guild WHERE owner_account BETWEEN 1001 AND $(1000 + $Guilds);")
foreach ($i in 1..$Guilds)
{
    $acc = 1000 + $i; $cg = 5000 + $i; $gid = 100 + $i
    $name = Get-LoadLeaderName $i
    $team = if ($i % 2 -eq 1) { 0 } else { 1 }      # odd: Alliance, even: Horde
    $race = if ($i % 2 -eq 1) { 1 } else { 2 }
    [void]$sql.AppendLine(@"
INSERT INTO $auth.account (id, username, salt, verifier, email, reg_mail, joindate, expansion)
  SELECT $acc, 'LOADOWN$('{0:D3}' -f $i)', salt, verifier, '', '', NOW(), 2 FROM $auth.account WHERE id = 3;
DROP TEMPORARY TABLE IF EXISTS t_char;
CREATE TEMPORARY TABLE t_char AS SELECT * FROM $chars.characters WHERE guid = 3;
UPDATE t_char SET guid = $cg, account = $acc, name = '$name', race = $race, online = 0;
INSERT INTO $chars.characters SELECT * FROM t_char;
INSERT INTO $chars.guild (guildid, name, leaderguid, EmblemStyle, EmblemColor, BorderStyle, BorderColor, BackgroundColor, info, motd, createdate, BankMoney)
  VALUES ($gid, 'Load$i Adventurers', $cg, 0, 0, 0, 0, 0, '', '', UNIX_TIMESTAMP(), 0);
INSERT INTO $chars.guild_rank (guildid, rid, rname, rights, BankMoneyPerDay)
  SELECT $gid, rid, rname, rights, BankMoneyPerDay FROM $chars.guild_rank WHERE guildid = 2;
INSERT INTO $chars.guild_member (guildid, guid, ``rank``, pnote, offnote) VALUES ($gid, $cg, 0, '', '');
INSERT INTO $chars.ga_guild (owner_account, guild_id, leader_guid, team, unlocked_slots, renown_total, renown_level, offline_window_min, owner_logout_at, created_at)
  VALUES ($acc, $gid, $cg, $team, 40, 0, 1, 600, UNIX_TIMESTAMP(), UNIX_TIMESTAMP());
"@)
}
$file = Join-Path $dst 'loadtest-guilds.sql'
[IO.File]::WriteAllText($file, $sql.ToString())
cmd /c "`"$mysql`" --defaults-extra-file=`"$cnf`" $chars < `"$file`""
if ($LASTEXITCODE -ne 0) { throw 'creating the test guilds failed' }
Write-Host "$Guilds test guilds created (owner accounts 1001.., leaders $(Get-LoadLeaderName 1) ..)"
