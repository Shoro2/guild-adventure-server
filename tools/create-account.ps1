# Creates a game account in GA_auth directly (no worldserver console needed), using
# the same SRP6 registration data as AzerothCore's AccountMgr::CreateAccount
# (src/common/Cryptography/Authentication/SRP6.cpp):
#   salt     = 32 random bytes
#   verifier = g ^ SHA1(salt || SHA1(UPPER(user) ":" UPPER(pass))) mod N   (little-endian)
#
#   .\tools\create-account.ps1 -Username CHRIS -GmLevel 3          # prompts for the password
#   .\tools\create-account.ps1 -Username GATEST1 -GmLevel 3 -Generate
#       # random password, written to _work\test-accounts.local.txt (git-ignored)
#
# Run it while the worldserver is stopped or idle; the account is usable immediately.

param(
    [Parameter(Mandatory)] [ValidatePattern('^[A-Za-z0-9]{3,16}$')] [string] $Username,
    [ValidateRange(0, 3)] [int] $GmLevel = 0,
    [switch] $Generate
)

. "$PSScriptRoot\ga-env.ps1"

Add-Type -AssemblyName System.Numerics

$user = $Username.ToUpperInvariant()
if ($Generate)
{
    $chars = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'
    $bytes = New-Object byte[] 12
    [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
    $plain = -join ($bytes | ForEach-Object { $chars[$_ % $chars.Length] })
}
else
{
    $secure = Read-Host "Password for $user" -AsSecureString
    $plain = [Runtime.InteropServices.Marshal]::PtrToStringAuto([Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure))
}
if ($plain.Length -lt 1 -or $plain.Length -gt 16) { throw 'Password must be 1-16 characters (client limit)' }
$pass = $plain.ToUpperInvariant()

$exists = Invoke-GaMySql "SELECT COUNT(*) FROM account WHERE username = '$user';" $GA.Databases.Auth
if ([int]$exists -gt 0) { throw "Account $user already exists" }

$sha1 = [Security.Cryptography.SHA1]::Create()
$salt = New-Object byte[] 32
[Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($salt)
$h1 = $sha1.ComputeHash([Text.Encoding]::UTF8.GetBytes("${user}:${pass}"))
$h2 = $sha1.ComputeHash([byte[]]($salt + $h1))

function ConvertTo-UBigInt([byte[]] $le) { return [Numerics.BigInteger]::new([byte[]]($le + [byte]0)) }
$nHex = '894B645E89E1535BBDAD5B8B290650530801B18EBFBF5E8FAB3C82872A3E9BB7'
$nBe = for ($i = 0; $i -lt $nHex.Length; $i += 2) { [Convert]::ToByte($nHex.Substring($i, 2), 16) }
[array]::Reverse($nBe)
$N = ConvertTo-UBigInt $nBe
$v = [Numerics.BigInteger]::ModPow([Numerics.BigInteger]7, (ConvertTo-UBigInt $h2), $N)

$vBytes = New-Object byte[] 32
$raw = $v.ToByteArray()                      # little-endian, may carry a trailing sign byte
[array]::Copy($raw, $vBytes, [Math]::Min(32, $raw.Length))

function ConvertTo-Hex([byte[]] $b) { return ($b | ForEach-Object { $_.ToString('X2') }) -join '' }

Invoke-GaMySql ("INSERT INTO account (username, salt, verifier, expansion, reg_mail, email, joindate) " +
    "VALUES ('$user', 0x$(ConvertTo-Hex $salt), 0x$(ConvertTo-Hex $vBytes), 2, '', '', NOW());") $GA.Databases.Auth | Out-Null
Invoke-GaMySql ("INSERT INTO realmcharacters (realmid, acctid, numchars) SELECT realmlist.id, account.id, 0 " +
    "FROM realmlist, account LEFT JOIN realmcharacters ON acctid = account.id WHERE acctid IS NULL;") $GA.Databases.Auth | Out-Null
$id = Invoke-GaMySql "SELECT id FROM account WHERE username = '$user';" $GA.Databases.Auth
if ($GmLevel -gt 0)
{
    Invoke-GaMySql "INSERT INTO account_access (id, gmlevel, RealmID) VALUES ($id, $GmLevel, -1);" $GA.Databases.Auth | Out-Null
}
Write-Host "account created: $user (id $id, gmlevel $GmLevel)"

if ($Generate)
{
    $file = Join-Path $GA.Work 'test-accounts.local.txt'
    Add-Content -Path $file -Value "$user`t$plain`tgm=$GmLevel`t$(Get-Date -Format s)" -Encoding ascii
    Write-Host "password stored in $file"
}
