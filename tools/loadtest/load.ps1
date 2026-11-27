# Load test driver for a throwaway instance made by setup-instance.ps1 (GA_INSTANCE set).
#
#   load.ps1 recruit <from> <to>        40 recruits in each of the test guilds from..to
#   load.ps1 boost   <from> <to>        every recruit of those guilds to a random level 10-60
#   load.ps1 login   <from> <to>        log in the bots of those guilds (after a restart they come back by themselves)
#   load.ps1 logout  <from> <to>
#   load.ps1 sample  <label> <seconds>  .server info + the process's CPU (total and per thread) every 15 s
#                                       into <server>\loadtest\samples.tsv
#   load.ps1 cmd "<command>"            one GM command over SOAP
#   load.ps1 where                      bots in the world per map and zone (from the characters database, after .saveall)
#
# Recruits are created offline: `login` brings a guild's bots into the world (their guild's
# offline window runs), at level 1 in their start zones; wait a minute, then `boost` puts them to
# their levels (it needs them in the world) and the autopilot spreads them over the zones. The
# first quarter of an hour after a boost is travel-heavy (every bot looks for its zone at once):
# restart the instance and give it five minutes before sampling a steady state.

param([string] $Step, [string] $A, [string] $B)

. "$PSScriptRoot\..\ga-env.ps1"
. "$PSScriptRoot\loadtest-common.ps1"
if (-not $GA.Instance) { throw 'Set GA_INSTANCE to the load test instance' }

$out = Join-Path $GA.Server 'loadtest'
New-Item -ItemType Directory -Force $out | Out-Null
$cred = Get-Content (Join-Path $GA.Work 'probe-accounts.local.txt') | Where-Object { $_ -match '^GAPROBE1\t' } | Select-Object -First 1
$parts = $cred -split "`t"
$auth = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("$($parts[0]):$($parts[1])"))

function Soap([string] $command)
{
    $escaped = [Security.SecurityElement]::Escape($command)
    $body = "<?xml version=`"1.0`" encoding=`"utf-8`"?><SOAP-ENV:Envelope xmlns:SOAP-ENV=`"http://schemas.xmlsoap.org/soap/envelope/`" xmlns:ns1=`"urn:AC`"><SOAP-ENV:Body><ns1:executeCommand><command>$escaped</command></ns1:executeCommand></SOAP-ENV:Body></SOAP-ENV:Envelope>"
    try
    {
        $resp = Invoke-WebRequest -Uri "http://127.0.0.1:$($GA.SoapPort)/" -Method Post -Body $body -UseBasicParsing -TimeoutSec 60 `
            -ContentType 'text/xml; charset=utf-8' -Headers @{ Authorization = "Basic $auth" }
        $text = $resp.Content
    }
    catch
    {
        $r = $_.Exception.Response
        if (-not $r) { return "ERROR: $($_.Exception.Message)" }
        $text = (New-Object IO.StreamReader($r.GetResponseStream())).ReadToEnd()
        if (-not $text) { return "ERROR: HTTP $([int]$r.StatusCode)" }
    }
    $xml = [xml]$text
    $res = $xml.SelectSingleNode('//result'); $f = $xml.SelectSingleNode('//faultstring')
    if ($f) { return "FAULT: $($f.InnerText)" }
    if ($res) { return $res.InnerText }
    return ''
}

function Title([string] $s) { return $s.Substring(0, 1).ToUpperInvariant() + $s.Substring(1).ToLowerInvariant() }

# race, class per recruit (10 combinations per side, four of each per guild)
$alliance = @(@(1,1), @(1,2), @(1,8), @(1,5), @(3,3), @(4,11), @(7,9), @(1,4), @(11,7), @(3,1))
$horde    = @(@(2,1), @(2,3), @(5,8), @(5,5), @(6,11), @(8,4), @(2,9), @(6,7), @(10,2), @(8,3))

switch ($Step)
{
    'recruit'
    {
        foreach ($i in [int]$A..[int]$B)
        {
            $combos = if ($i % 2 -eq 1) { $alliance } else { $horde }
            foreach ($k in 0..39)
            {
                $c = $combos[$k % $combos.Count]
                $res = Soap ".ga admin recruit $(Title (Get-LoadLeaderName $i)) $($c[0]) $($c[1]) $($k % 2) $(Title (Get-LoadBotName $i $k))"
                if ($res -notmatch '^Recruiting') { Write-Host "$i/$k $res" }
                Start-Sleep -Milliseconds 40
            }
            Write-Host "$(Get-Date -Format HH:mm:ss) guild $i recruited"
        }
    }
    'boost'
    {
        $rng = New-Object Random (4242 + [int]$A)
        foreach ($i in [int]$A..[int]$B)
        {
            foreach ($k in 0..39)
            {
                $lvl = $rng.Next(10, 61)
                $res = Soap ".ga admin boost $(Title (Get-LoadBotName $i $k)) $lvl"
                if ($res -match 'ERROR|FAULT|not ') { Write-Host "$i/$k $res" }
            }
            Write-Host "$(Get-Date -Format HH:mm:ss) guild $i boosted"
        }
    }
    'login'  { foreach ($i in [int]$A..[int]$B) { Write-Host (Soap ".ga admin login $(Title (Get-LoadLeaderName $i))") } }
    'logout' { foreach ($i in [int]$A..[int]$B) { Write-Host (Soap ".ga admin logout $(Title (Get-LoadLeaderName $i))") } }
    'cmd'    { Write-Host (Soap $A) }
    'where'
    {
        $db = $GA.Databases.Characters
        Invoke-GaMySql -Sql "SELECT map, COUNT(*) FROM characters WHERE online = 1 GROUP BY map ORDER BY 2 DESC" -Database $db
        Invoke-GaMySql -Sql "SELECT map, zone, COUNT(*) FROM characters WHERE online = 1 GROUP BY map, zone ORDER BY 3 DESC LIMIT 60" -Database $db
    }
    'sample'
    {
        $label = $A; $seconds = [int]$B
        $file = Join-Path $out 'samples.tsv'
        if (-not (Test-Path $file)) { "time`tlabel`tchars`tmean`tmedian`tp95`tp99`tmax`tcpu_cores`tws_mb`tmysql_cpu`ttop_threads" | Set-Content $file }
        $p = Get-Process worldserver | Where-Object { $_.Path -like "$($GA.Server)\*" }
        if (-not $p) { throw 'the instance''s worldserver is not running' }
        $end = (Get-Date).AddSeconds($seconds)
        $lastCpu = $p.TotalProcessorTime.TotalSeconds; $lastT = Get-Date
        $lastM = (Get-Process mysqld | ForEach-Object { $_.TotalProcessorTime.TotalSeconds } | Measure-Object -Sum).Sum
        $lastThreads = @{}; foreach ($t in $p.Threads) { $lastThreads[$t.Id] = $t.TotalProcessorTime.TotalSeconds }
        while ((Get-Date) -lt $end)
        {
            Start-Sleep -Seconds 15
            $p.Refresh(); $mNow = (Get-Process mysqld | ForEach-Object { $_.TotalProcessorTime.TotalSeconds } | Measure-Object -Sum).Sum
            $now = Get-Date; $dt = ($now - $lastT).TotalSeconds
            $cpu = ($p.TotalProcessorTime.TotalSeconds - $lastCpu) / $dt
            $mc = ($mNow - $lastM) / $dt
            $th = foreach ($t in $p.Threads) { $prev = $lastThreads[$t.Id]; if ($null -eq $prev) { $prev = 0 }
                [pscustomobject]@{ Id = $t.Id; C = ($t.TotalProcessorTime.TotalSeconds - $prev) / $dt } }
            $top = ($th | Sort-Object C -Descending | Select-Object -First 8 | ForEach-Object { "{0}:{1:N2}" -f $_.Id, $_.C }) -join ' '
            $lastThreads = @{}; foreach ($t in $p.Threads) { $lastThreads[$t.Id] = $t.TotalProcessorTime.TotalSeconds }
            $lastCpu = $p.TotalProcessorTime.TotalSeconds; $lastM = $mNow; $lastT = $now
            $info = Soap '.server info'
            $chars = if ($info -match 'Characters in world: (\d+)') { $Matches[1] } else { '?' }
            $mean = if ($info -match '[Mm]ean[^\d]*(\d+)') { $Matches[1] } else { '?' }
            $med = if ($info -match '[Mm]edian[^\d]*(\d+)') { $Matches[1] } else { '?' }
            $pc = if ($info -match '\(95, 99, max\)[^\d]*(\d+)ms, (\d+)ms, (\d+)ms') { @($Matches[1], $Matches[2], $Matches[3]) } else { @('?','?','?') }
            $line = "{0}`t{1}`t{2}`t{3}`t{4}`t{5}`t{6}`t{7}`t{8:N2}`t{9}`t{10:N2}`t{11}" -f $now.ToString('HH:mm:ss'), $label, $chars, $mean, $med, $pc[0], $pc[1], $pc[2], $cpu, [int]($p.WorkingSet64 / 1MB), $mc, $top
            Add-Content $file $line
            Write-Host $line
        }
    }
    default { throw "unknown step '$Step' (recruit, boost, login, logout, sample, cmd, where)" }
}
