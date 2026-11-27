# Runs the offline addon tests (addon/tests/run_tests.lua) with a Lua 5.1/5.2 interpreter.
# Usage: .\addon\tests\run.ps1 [-Lua <path to lua.exe>] [-FrameXml <folder>]
param(
    [string]$Lua = "lua",
    [string]$FrameXml = ""
)

$ErrorActionPreference = "Stop"
$testsDir = $PSScriptRoot

if ($FrameXml -ne "") {
    $env:GA_FRAMEXML = $FrameXml
}

& $Lua (Join-Path $testsDir "run_tests.lua") $testsDir
exit $LASTEXITCODE
