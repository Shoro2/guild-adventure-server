# Starts the Guild Adventure authserver (port 3726) and worldserver (port 8087).
# Each runs in its own console window: the worldserver reads its console from stdin
# and halts when stdin is closed, so it must not be started as a redirected
# background process.
#
#   .\tools\start-server.ps1             # MySQL84 check + authserver + worldserver
#   .\tools\start-server.ps1 -AuthOnly
#   .\tools\start-server.ps1 -WorldOnly
#
# Readiness: the worldserver logs "WORLD: World Initialized In ..." to
# _work\server\logs\Server.log once it accepts logins. Server.log and Errors.log are
# appended to across restarts (configure-server.ps1), so look for the newest such line;
# lines of a run with these settings start with the time ("2026-11-12 06:33:16 WORLD: ...").
#
# Log housekeeping: a log that passed its size cap was renamed to <file>.<time> by the
# worldserver; before a start this script keeps the newest three of those per log and
# deletes the older ones (only in the server's own logs folder).

param(
    [switch] $AuthOnly,
    [switch] $WorldOnly
)

. "$PSScriptRoot\ga-env.ps1"

$svc = Get-Service $GA.MySqlSvc
if ($svc.Status -ne 'Running')
{
    Write-Host "Starting MySQL service $($GA.MySqlSvc) ..."
    try { Start-Service $GA.MySqlSvc -ErrorAction Stop }
    catch { throw "Could not start $($GA.MySqlSvc) (needs an elevated shell): $($_.Exception.Message)" }
}

function Remove-OldLogBackups
{
    $logs = Join-Path $GA.Server 'logs'
    if (-not (Test-Path $logs)) { return }
    foreach ($name in 'Server.log', 'Errors.log', 'Playerbots.log', 'GuildAdventure.log', 'Auth.log')
    {
        Get-ChildItem -LiteralPath $logs -File -Filter "$name.*" -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -ne $name } | Sort-Object LastWriteTime -Descending | Select-Object -Skip 3 |
            ForEach-Object { Remove-Item -LiteralPath $_.FullName -ErrorAction SilentlyContinue }
    }
}

function Start-GaProcess([string] $exe, [int] $port)
{
    $path = Join-Path $GA.Server $exe
    if (-not (Test-Path $path)) { throw "$path not found - run tools\build-server.ps1 first" }
    $running = Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($exe)) -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -eq $path }
    if ($running) { Write-Host "$exe already running (pid $($running.Id))"; return }
    $busy = Get-NetTCPConnection -State Listen -LocalPort $port -ErrorAction SilentlyContinue
    if ($busy) { throw "Port $port is already in use by pid $($busy[0].OwningProcess) - refusing to start $exe" }
    $p = Start-Process -FilePath $path -WorkingDirectory $GA.Server -PassThru
    Write-Host "$exe started (pid $($p.Id), port $port)"
    return $p
}

# The authserver marks every realm offline when it starts and quits when its realm list is empty, and a worldserver
# that is still initialising has its realm at REALM_FLAG_VERSION_MISMATCH: both at once give flag 3, which the
# authserver's realm query excludes ("No valid realms specified", 2026-11-12 12:56). So the worldserver starts only
# once the authserver listens (its realm list is loaded by then).
function Wait-AuthReady($process)
{
    $deadline = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $deadline)
    {
        if ($process.HasExited) { throw "authserver.exe quit at its start (exit code $($process.ExitCode)) - see logs\Auth.log" }
        if (Get-NetTCPConnection -State Listen -LocalPort $GA.AuthPort -ErrorAction SilentlyContinue) { return }
        Start-Sleep -Milliseconds 500
    }
    Write-Warning "authserver.exe does not listen on port $($GA.AuthPort) after 30 s; starting the worldserver anyway"
}

Remove-OldLogBackups
if (-not $WorldOnly)
{
    $auth = Start-GaProcess 'authserver.exe' $GA.AuthPort
    if ($auth -and -not $AuthOnly) { Wait-AuthReady $auth }
}
if (-not $AuthOnly)  { Start-GaProcess 'worldserver.exe' $GA.WorldPort | Out-Null }
