# Runs a GM console command on the local Guild Adventure worldserver through SOAP
# (127.0.0.1:7880, enabled by tools\configure-server.ps1).
#
#   .\tools\ga-command.ps1 "server info"
#   .\tools\ga-command.ps1 "server shutdown 5"          # graceful stop
#   .\tools\ga-command.ps1 "account create NAME PASS"
#
# Credentials: a GM level 3 account. Defaults to the first gm=3 entry in
# _work\test-accounts.local.txt (written by tools\create-account.ps1 -Generate);
# override with -Username / -Password.

param(
    [Parameter(Mandatory, Position = 0)] [string] $Command,
    [string] $Username,
    [string] $Password
)

. "$PSScriptRoot\ga-env.ps1"

if (-not $Username)
{
    $file = Join-Path $GA.Work 'test-accounts.local.txt'
    if (-not (Test-Path $file)) { throw "No credentials: pass -Username/-Password or create $file with tools\create-account.ps1 -Generate -GmLevel 3" }
    $entry = Get-Content $file | Where-Object { $_ -match "`tgm=3`t" } | Select-Object -First 1
    if (-not $entry) { throw "No gm=3 account in $file" }
    $parts = $entry -split "`t"
    $Username = $parts[0]; $Password = $parts[1]
}

$escaped = [Security.SecurityElement]::Escape($Command)
$body = @"
<?xml version="1.0" encoding="utf-8"?>
<SOAP-ENV:Envelope xmlns:SOAP-ENV="http://schemas.xmlsoap.org/soap/envelope/" xmlns:ns1="urn:AC">
  <SOAP-ENV:Body><ns1:executeCommand><command>$escaped</command></ns1:executeCommand></SOAP-ENV:Body>
</SOAP-ENV:Envelope>
"@
$auth = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("${Username}:${Password}"))
try
{
    $resp = Invoke-WebRequest -Uri "http://127.0.0.1:$($GA.SoapPort)/" -Method Post -Body $body -UseBasicParsing `
        -ContentType 'text/xml; charset=utf-8' -Headers @{ Authorization = "Basic $auth" }
    $xml = [xml]$resp.Content
}
catch
{
    $r = $_.Exception.Response
    if (-not $r) { throw }
    $text = (New-Object IO.StreamReader($r.GetResponseStream())).ReadToEnd()
    if ([int]$r.StatusCode -eq 401) { Write-Error "SOAP: 401 unauthorized (wrong credentials or account below gm level 3)"; exit 1 }
    if (-not $text) { Write-Error "SOAP: HTTP $([int]$r.StatusCode) - command failed or unknown (the worldserver console shows details)"; exit 1 }
    $xml = [xml]$text
}
$result = $xml.SelectSingleNode('//result')
$fault  = $xml.SelectSingleNode('//faultstring')
if ($fault) { Write-Error "SOAP fault: $($fault.InnerText)"; exit 1 }
if (-not $result) { Write-Error 'SOAP: response has no result element'; exit 1 }
$result.InnerText
