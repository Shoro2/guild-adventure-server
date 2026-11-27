# Records a CPU sampling profile of the instance's worldserver with xperf (Windows Performance
# Toolkit, ETW) and turns it into a text dump that analyze_profile.py reads. Needs an elevated
# shell. The kernel logger samples the whole machine (1 kHz, a few percent overhead) while a
# window records, so keep windows short.
#
#   $env:GA_INSTANCE = 'perf'
#   .\tools\loadtest\profile.ps1 -Label t4_0800 -Seconds 60
#   .\tools\loadtest\profile.ps1 -Label t4_0800_waits -Seconds 20 -Waits   # also stacks of every context switch
#   python tools\loadtest\analyze_profile.py <server>\loadtest\profiles\t4_0800.dump.csv --pid <pid> ...
#
# Output in <server>\loadtest\profiles: <label>.etl (the trace, opens in WPA), <label>.dump.csv
# (xperf's dumper output with function names from worldserver.pdb), <label>.info.txt (.server info
# before and after, pid, settings). System DLLs stay unresolved: no symbol server is used.

param(
    [Parameter(Mandatory)] [string] $Label,
    [int] $Seconds = 60,
    [switch] $Waits,
    [switch] $KeepEtl
)

. "$PSScriptRoot\..\ga-env.ps1"
if (-not $GA.Instance) { throw 'Set GA_INSTANCE to the load test instance' }

$xperf = @('C:\Program Files (x86)\Windows Kits\10\Windows Performance Toolkit\xperf.exe',
           'C:\Program Files\Windows Kits\10\Windows Performance Toolkit\xperf.exe') | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $xperf) { throw 'xperf.exe not found (Windows Performance Toolkit, part of the Windows SDK / ADK)' }

$p = Get-Process worldserver -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$($GA.Server)\*" }
if (-not $p) { throw 'the instance''s worldserver is not running' }

$out = Join-Path $GA.Server 'loadtest\profiles'
New-Item -ItemType Directory -Force $out | Out-Null
$kernel = Join-Path $out "$Label.kernel.etl"
$etl = Join-Path $out "$Label.etl"
$dump = Join-Path $out "$Label.dump.csv"
$info = Join-Path $out "$Label.info.txt"

$load = Join-Path $PSScriptRoot 'load.ps1'
"label $Label, pid $($p.Id), $Seconds s, waits $([bool]$Waits), started $(Get-Date -Format s)" | Set-Content $info
(& $load cmd '.server info' 6>&1 | Out-String) | Add-Content $info

$flags = 'PROC_THREAD+LOADER+PROFILE+CSWITCH'
$stack = 'Profile'
if ($Waits) { $flags += '+DISPATCHER'; $stack = 'Profile+CSwitch+ReadyThread' }
$cpu0 = $p.TotalProcessorTime.TotalSeconds; $t0 = Get-Date
& $xperf -on $flags -stackwalk $stack -BufferSize 1024 -MinBuffers 256 -MaxBuffers 2048 -f $kernel | Out-Null
if ($LASTEXITCODE -ne 0) { throw "xperf -on failed ($LASTEXITCODE); another kernel trace may be running (xperf -stop)" }
Start-Sleep -Seconds $Seconds
& $xperf -d $etl 2>&1 | Out-Null
$p.Refresh()
$cores = ($p.TotalProcessorTime.TotalSeconds - $cpu0) / ((Get-Date) - $t0).TotalSeconds
"process CPU while recording: {0:N2} cores" -f $cores | Add-Content $info
(& $load cmd '.server info' 6>&1 | Out-String) | Add-Content $info
Remove-Item $kernel -ErrorAction SilentlyContinue

$env:_NT_SYMBOL_PATH = $GA.Server
$env:_NT_SYMCACHE_PATH = Join-Path $out 'symcache'
$sw = [Diagnostics.Stopwatch]::StartNew()
cmd /c "`"$xperf`" -i `"$etl`" -o `"$dump`" -symbols -a dumper 2> `"$out\$Label.dumper.err`""
Write-Host ("{0}: {1:N0} MB trace, {2:N0} MB dump in {3:N0} s, worldserver pid {4}, {5:N2} cores" -f $Label,
    ((Get-Item $etl).Length / 1MB), ((Get-Item $dump).Length / 1MB), $sw.Elapsed.TotalSeconds, $p.Id, $cores)
if (-not $KeepEtl) { Remove-Item $etl -ErrorAction SilentlyContinue }
