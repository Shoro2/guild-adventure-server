# Names of the load test's guild leaders and bots (dot-sourced by the loadtest scripts).
# The core refuses names with three equal letters in a row and names with digits, so the
# names are built from a fixed prefix, two consonants for the guild and a vowel and a
# consonant for the bot.

$LoadConsonants = 'bcdfghjklmnpqrstvwxz'
$LoadVowels = 'aeiouy'

function Get-LoadGuildCode([int] $i)
{
    $n = $i - 1
    return [string]$LoadConsonants[[int][math]::Floor($n / 20) % 20] + [string]$LoadConsonants[$n % 20]
}

function Get-LoadLeaderName([int] $i) { return 'Leado' + (Get-LoadGuildCode $i) + 'a' }

function Get-LoadBotName([int] $i, [int] $k)
{
    return 'Lo' + (Get-LoadGuildCode $i) + [string]$LoadVowels[[int][math]::Floor($k / 20)] + [string]$LoadConsonants[$k % 20]
}
