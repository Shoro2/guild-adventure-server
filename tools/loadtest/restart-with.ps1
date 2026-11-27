# Restarts the load test instance with changed config values, waits until its bots are back,
# lets it settle and samples it: one step of an A/B comparison.
#
#   $env:GA_INSTANCE = 'perf'
#   .\tools\loadtest\restart-with.ps1 -Label vis60 -World @{ 'Visibility.Distance.Continents' = '60' } -Bots 1600
#   .\tools\loadtest\restart-with.ps1 -Label react200 -Playerbots @{ 'Playerbots.ReactDelay' = '200' } -Bots 1600 -Profile
#
# -World, -Playerbots, -Module: key = value pairs for worldserver.conf, modules\playerbots.conf and
# modules\mod_guild_adventure.conf (a missing key is appended). The values stay until the next call
# changes them again, so give the baseline value to go back.

param(
    [Parameter(Mandatory)] [string] $Label,
    [hashtable] $World = @{},
    [hashtable] $Playerbots = @{},
    [hashtable] $Module = @{},
    [int] $Bots = 1600,
    [int] $SettleSeconds = 300,
    [int] $SampleSeconds = 120,
    [switch] $Profile
)

. "$PSScriptRoot\..\ga-env.ps1"
if (-not $GA.Instance) { throw 'Set GA_INSTANCE to the load test instance' }
$load = Join-Path $PSScriptRoot 'load.ps1'

function Set-Keys([string] $file, [hashtable] $values)
{
    if (-not $values.Count) { return }
    $text = [IO.File]::ReadAllText($file)
    foreach ($key in $values.Keys)
    {
        $pattern = '(?m)^(\s*)' + [regex]::Escape($key) + '\s*=.*$'
        $line = "$key = $($values[$key])"
        if ($text -match $pattern) { $text = [regex]::Replace($text, $pattern, { param($m) $m.Groups[1].Value + $line }) }
        else { $text = $text.TrimEnd() + "`r`n$line`r`n" }
        Write-Host "$(Split-Path -Leaf $file): $line"
    }
    [IO.File]::WriteAllText($file, $text)
}

$p = Get-Process worldserver -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$($GA.Server)\*" }
if ($p)
{
    & $load cmd '.saveall' | Out-Null
    & $load cmd '.server shutdown 3' | Out-Null
    # (1600 bots log out with a save each: the characters database queue takes minutes to drain.)
    if (-not $p.WaitForExit(600000)) { throw 'the worldserver did not stop within 10 minutes' }
    Write-Host "$(Get-Date -Format HH:mm:ss) stopped"
}
Set-Keys (Join-Path $GA.Configs 'worldserver.conf') $World
Set-Keys (Join-Path $GA.Configs 'modules\playerbots.conf') $Playerbots
Set-Keys (Join-Path $GA.Configs 'modules\mod_guild_adventure.conf') $Module

& (Join-Path $PSScriptRoot '..\start-server.ps1') -WorldOnly
$deadline = (Get-Date).AddMinutes(15)
$chars = 0
while ((Get-Date) -lt $deadline)
{
    Start-Sleep -Seconds 10
    $info = & $load cmd '.server info' 6>&1 | Out-String
    if ($info -match 'Characters in world: (\d+)') { $chars = [int]$Matches[1] }
    if ($chars -ge $Bots) { break }
}
Write-Host "$(Get-Date -Format HH:mm:ss) $chars characters in the world; settling for $SettleSeconds s"
if ($chars -lt $Bots) { throw "only $chars of $Bots bots came back" }
Start-Sleep -Seconds $SettleSeconds
& $load sample $Label $SampleSeconds
if ($Profile)
{
    & (Join-Path $PSScriptRoot 'profile.ps1') -Label $Label -Seconds 60
    & (Join-Path $PSScriptRoot 'profile.ps1') -Label "${Label}_w" -Seconds 30 -Waits
}
Write-Host "$(Get-Date -Format HH:mm:ss) $Label done"
