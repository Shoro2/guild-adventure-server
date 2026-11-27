# Creates the Guild Adventure databases in the shared MySQL84 instance.
# Tables are created and updated by the worldserver/authserver themselves on first
# start (Updates.AutoSetup / Updates.EnableDatabases), so this script only creates the
# empty schemas.
#
#   .\tools\setup-db.ps1
#
# Needs _work\mysql-client.cnf (see docs\server-setup.md). Only touches GA_* schemas.
# Note: the instance runs with lower_case_table_names = 1, so MySQL stores the names
# in lower case (ga_auth, ...); AzerothCore connects case-insensitively.

. "$PSScriptRoot\ga-env.ps1"

$svc = Get-Service $GA.MySqlSvc
if ($svc.Status -ne 'Running') { throw "MySQL service $($GA.MySqlSvc) is not running - start it first (tools\start-server.ps1 does)" }

foreach ($db in $GA.Databases.Values)
{
    if ($db -notmatch '^GA_([a-z0-9]+_)?[a-z]+$') { throw "Refusing to touch non-GA database '$db'" }
    Invoke-GaMySql "CREATE DATABASE IF NOT EXISTS ``$db`` DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;" | Out-Null
    Write-Host "database ready: $db"
}

Invoke-GaMySql "SHOW DATABASES LIKE 'ga\_%';"
