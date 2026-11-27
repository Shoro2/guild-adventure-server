# CMake configure + MSVC build of the Guild Adventure core into _work\build and
# install to _work\server. Modules (mod-playerbots, mod-guild-adventure) are built
# statically into the worldserver.
#
#   .\tools\build-server.ps1                 # configure (if needed) + build + install
#   .\tools\build-server.ps1 -Reconfigure    # force a fresh CMake configure
#   .\tools\build-server.ps1 -SkipInstall    # build only
#   .\tools\build-server.ps1 -Reconfigure -Tools maps-only   # also the data extractors (maps, vmaps, mmaps)
#   .\tools\build-server.ps1 -SkipBuild      # install only, from the build tree as it is (no lock: nothing compiles)
#
# For the integration server: build with -SkipInstall while it runs (any wait for another build's lock happens then),
# stop it, install with -SkipBuild, start it. That keeps it down only for the copy, not for a foreign build.
#
# Requires: Visual Studio 2022 (C++ workload), CMake, Boost >= 1.78, OpenSSL 3, MySQL
# Server 8.4 (headers + libmysql). Paths come from tools\ga-env.ps1.
#
# One build at a time on this PC (the owner, 2026-11-13: several parallel builds filled the 61 GB of RAM). A build takes
# the lock file _work\build.lock (owner, process, start, build tree) and keeps it fresh once a minute; another build
# waits while the lock is younger than 40 minutes and takes it over only when it went stale. The integration server's
# build (no GA_INSTANCE) has priority: while it waits it leaves _work\build.lock.priority, and instance builds wait for
# that too. Instance builds that wait take their turn first come, first served: each one keeps a file in
# _work\build.queue fresh while it waits and takes the lock only when no fresh file older than its own is left.
# At most 12 compiler processes per build by default (1 project at a time, /MP12; the owner, 2026-11-14: "ja 12
# Compiler-Prozesse", was 4).

param(
    [switch] $Reconfigure,
    # AzerothCore's TOOLS_BUILD: none (default), maps-only (the extractors the server data is made with), all.
    # A change needs -Reconfigure.
    [ValidateSet('none', 'maps-only', 'all')] [string] $Tools = 'none',
    [switch] $SkipInstall,
    # Install what the build tree holds, without building and without the build lock.
    [switch] $SkipBuild,
    # MSBuild project-level parallelism (concurrent projects).
    [int] $Jobs = 1,
    # cl.exe processes per project. The core CMake passes a bare /MP (= all 32 cores);
    # with PCH that runs out of commit memory (C1076/C3859), so cap it via _CL_.
    [int] $CompilerProcesses = 12,
    # Who holds the lock (shown to whoever waits); defaults to GA_BUILD_OWNER, else the instance name or "server".
    [string] $Owner = '',
    # Give up after waiting this long for another build.
    [int] $LockWaitMinutes = 240
)

. "$PSScriptRoot\ga-env.ps1"

$lockFile = Join-Path $GA.Work 'build.lock'
$priorityFile = Join-Path $GA.Work 'build.lock.priority'
$queueDir = Join-Path $GA.Work 'build.queue'
$script:queueFile = $null
$staleMinutes = 40
if (-not $Owner) { $Owner = if ($env:GA_BUILD_OWNER) { $env:GA_BUILD_OWNER } elseif ($GA.Instance) { $GA.Instance } else { 'server' } }
$isPriority = -not $GA.Instance

function Get-FreshAge([string] $path)
{
    if (-not (Test-Path $path)) { return $null }
    return ((Get-Date) - (Get-Item $path).LastWriteTime).TotalMinutes
}

function Enter-BuildLock
{
    $deadline = (Get-Date).AddMinutes($LockWaitMinutes)
    $lastNote = [datetime]::MinValue
    while ($true)
    {
        $age = Get-FreshAge $lockFile
        $priorityAge = Get-FreshAge $priorityFile
        $blockedByPriority = (-not $isPriority) -and ($null -ne $priorityAge) -and ($priorityAge -lt $staleMinutes)
        # First come, first served among instance builds: a queue file per waiter, kept fresh while it waits.
        $blockedByQueue = $false
        $ahead = @()
        if (-not $isPriority)
        {
            if (-not $script:queueFile)
            {
                New-Item -ItemType Directory -Force $queueDir | Out-Null
                $script:queueFile = Join-Path $queueDir ("{0}-{1}.wait" -f (Get-Date).ToString('yyyyMMddHHmmss'), $PID)
                Set-Content -Path $script:queueFile -Value "owner=$Owner pid=$PID build=$($GA.Build)" -Encoding ascii
            }
            else { (Get-Item $script:queueFile).LastWriteTime = Get-Date }
            $own = Split-Path $script:queueFile -Leaf
            Get-ChildItem $queueDir -Filter '*.wait' -ErrorAction SilentlyContinue |
                Where-Object { ((Get-Date) - $_.LastWriteTime).TotalMinutes -ge 10 } | Remove-Item -Force -ErrorAction SilentlyContinue
            $ahead = @(Get-ChildItem $queueDir -Filter '*.wait' -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -lt $own -and ((Get-Date) - $_.LastWriteTime).TotalMinutes -lt 2 })
            $blockedByQueue = $ahead.Count -gt 0
        }
        if ($null -ne $age -and $age -ge $staleMinutes)
        {
            Write-Host "build lock: taking over a stale lock ($([int]$age) min without a sign of life): $(Get-Content $lockFile -Raw)"
            Remove-Item $lockFile -Force -ErrorAction SilentlyContinue
            $age = $null
        }
        if ($null -eq $age -and -not $blockedByPriority -and -not $blockedByQueue)
        {
            try
            {
                $stream = [IO.File]::Open($lockFile, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
                $text = "owner=$Owner pid=$PID start=$((Get-Date).ToString('yyyy-MM-dd HH:mm:ss')) build=$($GA.Build)"
                $bytes = [Text.Encoding]::UTF8.GetBytes($text)
                $stream.Write($bytes, 0, $bytes.Length)
                $stream.Close()
                if ($isPriority) { Remove-Item $priorityFile -Force -ErrorAction SilentlyContinue }
                Remove-QueueFile
                Write-Host "build lock: taken ($text)"
                return
            }
            catch [IO.IOException] { }   # someone else was quicker
        }
        if ($isPriority) { Set-Content -Path $priorityFile -Value "owner=$Owner pid=$PID waiting since $((Get-Date).ToString('HH:mm:ss'))" -Encoding ascii }
        if ((Get-Date) -gt $deadline) { Remove-QueueFile; throw "build lock: waited $LockWaitMinutes min for $lockFile - giving up" }
        if (((Get-Date) - $lastNote).TotalMinutes -ge 5)
        {
            $holder = if (Test-Path $lockFile) { Get-Content $lockFile -Raw }
                elseif ($blockedByPriority) { "a priority build (" + (Get-Content $priorityFile -Raw -ErrorAction SilentlyContinue) + ")" }
                else { "$($ahead.Count) build(s) queued before it (" + (($ahead | ForEach-Object { (Get-Content $_.FullName -Raw -ErrorAction SilentlyContinue).Trim() }) -join '; ') + ")" }
            Write-Host "build lock: waiting for $holder"
            $lastNote = Get-Date
        }
        Start-Sleep -Seconds 30
    }
}

function Remove-QueueFile
{
    if ($script:queueFile) { Remove-Item $script:queueFile -Force -ErrorAction SilentlyContinue; $script:queueFile = $null }
}

function Exit-BuildLock
{
    Remove-QueueFile
    if (Test-Path $lockFile)
    {
        $content = Get-Content $lockFile -Raw -ErrorAction SilentlyContinue
        if ($content -match "pid=$PID\b") { Remove-Item $lockFile -Force -ErrorAction SilentlyContinue }
    }
}

if ($SkipBuild -and $SkipInstall) { throw "-SkipBuild and -SkipInstall together do nothing" }
if (-not $SkipBuild) { Enter-BuildLock }
# Keep the lock fresh while the build runs (a full build with 4 compilers takes longer than the 40 minutes).
$heartbeat = if ($SkipBuild) { $null } else { Start-Job -ArgumentList $lockFile, $PID -ScriptBlock {
    param($file, $owner)
    while ($true)
    {
        if ((Test-Path $file) -and ((Get-Content $file -Raw) -match "pid=$owner\b")) { (Get-Item $file).LastWriteTime = Get-Date }
        Start-Sleep -Seconds 60
    }
} }
try
{

if (-not (Test-Path (Join-Path $GA.Core 'CMakeLists.txt'))) { throw "Core not found at $($GA.Core) - run tools\setup-server.ps1 first" }
foreach ($p in @($GA.BoostRoot, $GA.OpenSslRoot, $GA.MySqlRoot)) { if (-not (Test-Path $p)) { throw "Toolchain path missing: $p" } }

# deps/boost/CMakeLists.txt reads the Boost_ROOT environment variable.
$env:Boost_ROOT = $GA.BoostRoot

$cache = Join-Path $GA.Build 'CMakeCache.txt'
if ($SkipBuild -and -not (Test-Path $cache)) { throw "-SkipBuild: no build tree at $($GA.Build)" }
if (-not $SkipBuild -and ($Reconfigure -or -not (Test-Path $cache)))
{
    New-Item -ItemType Directory -Force $GA.Build | Out-Null
    $cmakeArgs = @(
        '-S', $GA.Core,
        '-B', $GA.Build,
        '-G', 'Visual Studio 17 2022', '-A', 'x64',
        "-DCMAKE_INSTALL_PREFIX=$($GA.Server)",
        '-DSCRIPTS=static',
        '-DMODULES=static',
        "-DTOOLS_BUILD=$Tools",
        '-DAPPS_BUILD=all',
        '-DBUILD_TESTING=OFF',
        "-DBOOST_ROOT=$($GA.BoostRoot)",
        "-DOPENSSL_ROOT_DIR=$($GA.OpenSslRoot)",
        "-DMYSQL_INCLUDE_DIR=$(Join-Path $GA.MySqlRoot 'include')",
        "-DMYSQL_LIBRARY=$(Join-Path $GA.MySqlRoot 'lib\libmysql.lib')",
        "-DMYSQL_EXECUTABLE=$($GA.MySqlExe)"
    )
    Write-Host "cmake $($cmakeArgs -join ' ')"
    & cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }
}

# _CL_ options are appended after the command line, so this /MPn overrides the bare /MP.
$env:_CL_ = "/MP$CompilerProcesses"

if (-not $SkipBuild)
{
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & cmake --build $GA.Build --config $GA.BuildConfig --parallel $Jobs -- /nologo /verbosity:minimal
    if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }
    Write-Host ("Build finished in {0:N1} min" -f $sw.Elapsed.TotalMinutes)
}

if (-not $SkipInstall)
{
    & cmake --install $GA.Build --config $GA.BuildConfig
    if ($LASTEXITCODE -ne 0) { throw "Install failed ($LASTEXITCODE)" }

    # The install step copies only the executables and config templates. Add the PDBs
    # (crash dumps) and the runtime DLLs the executables load.
    $bin = Join-Path $GA.Build "bin\$($GA.BuildConfig)"
    $runtime = @(
        (Join-Path $bin 'authserver.pdb'),
        (Join-Path $bin 'worldserver.pdb'),
        (Join-Path $GA.MySqlRoot 'lib\libmysql.dll'),
        (Join-Path $GA.OpenSslRoot 'bin\libcrypto-3-x64.dll'),
        (Join-Path $GA.OpenSslRoot 'bin\libssl-3-x64.dll'),
        (Join-Path $GA.OpenSslRoot 'bin\legacy.dll')
    )
    foreach ($file in $runtime)
    {
        if (-not (Test-Path $file)) { throw "Runtime file missing: $file" }
        Copy-Item $file $GA.Server -Force
    }
    Write-Host "Installed to $($GA.Server)"
}
}
finally
{
    if ($heartbeat)
    {
        Stop-Job $heartbeat -ErrorAction SilentlyContinue
        Remove-Job $heartbeat -Force -ErrorAction SilentlyContinue
    }
    if (-not $SkipBuild) { Exit-BuildLock }
}
