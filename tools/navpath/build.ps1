# Builds tools\navpath\navpath.cpp (walks on the server's navmesh offline) against the core checkout's Detour.
# Needs Visual Studio 2022 with the C++ workload; no server build.
#
#   .\tools\navpath\build.ps1                       # -> _work\tools-bin\navpath.exe
#   .\tools\navpath\build.ps1 -Core E:\path\to\azerothcore-wotlk

param(
    [string] $Core
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $Core) { $Core = Join-Path $root '_work\azerothcore-wotlk' }
$detour = Join-Path $Core 'deps\recastnavigation\Detour'
if (-not (Test-Path (Join-Path $detour 'Include\DetourNavMesh.h'))) { throw "Detour not found under $Core" }

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio with the C++ tools not found' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

$out = Join-Path $root '_work\tools-bin'
New-Item -ItemType Directory -Force $out | Out-Null
$obj = Join-Path $out 'navpath-obj'
New-Item -ItemType Directory -Force $obj | Out-Null
$exe = Join-Path $out 'navpath.exe'

$sources = @((Join-Path $PSScriptRoot 'navpath.cpp')) + (Get-ChildItem (Join-Path $detour 'Source') -Filter *.cpp | ForEach-Object { $_.FullName })
$quoted = $sources | ForEach-Object { "`"$_`"" }
$cmd = "`"$vcvars`" >nul && cl /nologo /std:c++20 /EHsc /O2 /W3 /I`"$detour\Include`" /Fo`"$obj\\`" /Fe`"$exe`" $($quoted -join ' ')"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "compile failed ($LASTEXITCODE)" }
Write-Host "built $exe"
