# Removes a throwaway load-test instance made by setup-instance.ps1: its worldserver is asked to
# shut down (and killed if it does not), its runtime folder and its four databases are deleted.
# The integration server, its databases and the shared Data folder are never touched.
#
#   $env:GA_INSTANCE = 'perf'
#   .\tools\loadtest\teardown-instance.ps1            # keeps <server>\loadtest (samples, reports)
#   .\tools\loadtest\teardown-instance.ps1 -All       # deletes the whole runtime folder
#
# Afterwards take the instance's line out of $GaInstances in tools\ga-env.ps1.

param([switch] $All)

. "$PSScriptRoot\..\ga-env.ps1"
if (-not $GA.Instance) { throw 'Set GA_INSTANCE to the load test instance (never run this for the integration server)' }
$dst = $GA.Server
if ($dst -notmatch '\\server-[a-z0-9]+$') { throw "refusing to delete '$dst'" }

$p = Get-Process worldserver -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$dst\*" }
if ($p)
{
    try { & (Join-Path $PSScriptRoot 'load.ps1') cmd '.server shutdown 1' | Out-Null } catch { }
    if (-not $p.WaitForExit(60000)) { Stop-Process -Id $p.Id -Force }
    Write-Host 'worldserver stopped'
}

foreach ($d in 'auth', 'characters', 'playerbots', 'world')
{
    $db = "GA_$($GA.Instance)_$d"
    & $GA.MySqlExe "--defaults-extra-file=$($GA.MySqlCnf)" -e "DROP DATABASE IF EXISTS ``$db``;"
    Write-Host "dropped $db"
}

if (Test-Path $dst)
{
    # Never delete through a junction (the design log section 7): the runtime copy has none, so any is a reason to stop.
    $links = Get-ChildItem -LiteralPath $dst -Recurse -Force -Attributes ReparsePoint -ErrorAction SilentlyContinue
    if ($links) { throw "junctions inside ${dst}: $($links.FullName -join ', ') - remove them with 'cmd /c rmdir' first" }
    if ($All)
    {
        Remove-Item -LiteralPath $dst -Recurse -Force
        Write-Host "deleted $dst"
    }
    else
    {
        Get-ChildItem -LiteralPath $dst -Force | Where-Object { $_.Name -ne 'loadtest' } | Remove-Item -Recurse -Force
        Write-Host "deleted $dst except its loadtest folder"
    }
}
