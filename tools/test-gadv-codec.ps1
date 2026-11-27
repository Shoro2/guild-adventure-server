# Builds and runs the offline GADV codec test (modules\mod-guild-adventure\tests\gadv_codec_test.cpp) against
# the shared protocol vectors. Needs Visual Studio 2022 with the C++ workload; no server build required.
#
#   .\tools\test-gadv-codec.ps1
#   .\tools\test-gadv-codec.ps1 -Vectors path\to\protocol-vectors.txt

param(
    [string] $Vectors
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Vectors) { $Vectors = Join-Path $root 'docs\protocol-vectors.txt' }
if (-not (Test-Path $Vectors)) { throw "Vectors not found: $Vectors" }

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio with the C++ tools not found' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

$module = Join-Path $root 'modules\mod-guild-adventure'
$out = Join-Path $root '_work\test-bin'
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out 'gadv_codec_test.exe'

$sources = @(
    (Join-Path $module 'tests\gadv_codec_test.cpp'),
    (Join-Path $module 'src\Protocol\GadvCodec.cpp')
) | ForEach-Object { "`"$_`"" }

$cmd = "`"$vcvars`" >nul && cl /nologo /std:c++20 /EHsc /W4 /WX /O2 /Fo`"$out\\`" /Fe`"$exe`" $($sources -join ' ')"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "compile failed ($LASTEXITCODE)" }

& $exe $Vectors
exit $LASTEXITCODE
