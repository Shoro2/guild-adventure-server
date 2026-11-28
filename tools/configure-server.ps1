# Writes the runtime configs in _work\server\configs from the installed *.conf.dist
# templates: Guild Adventure ports, GA_* databases, data/log paths, playerbots with
# random bots off (decision D6), and the module config.
#
#   .\tools\configure-server.ps1          # create missing configs only
#   .\tools\configure-server.ps1 -Force   # regenerate all configs from the templates
#
# Credentials come from _work\mysql-client.cnf; the generated *.conf files are
# git-ignored and stay in _work.

param(
    [switch] $Force
)

. "$PSScriptRoot\ga-env.ps1"

function Set-ConfValue
{
    param([string] $Text, [string] $Key, [string] $Value)
    $pattern = '(?m)^(\s*)' + [regex]::Escape($Key) + '\s*=.*$'
    if ($Text -notmatch $pattern) { throw "Config key not found in template: $Key" }
    return [regex]::Replace($Text, $pattern, { param($m) "$($m.Groups[1].Value)$Key = $Value" })
}

function Write-Conf
{
    # $Extra: keys the template does not have (e.g. an own log appender); appended at the end.
    param([string] $Dist, [string] $Target, [hashtable] $Values, [System.Collections.Specialized.OrderedDictionary] $Extra)
    if (-not (Test-Path $Dist)) { throw "Template missing: $Dist (run tools\build-server.ps1 first)" }
    if ((Test-Path $Target) -and -not $Force) { Write-Host "keep:  $Target"; return }
    $text = [IO.File]::ReadAllText($Dist)
    foreach ($k in $Values.Keys) { $text = Set-ConfValue $text $k $Values[$k] }
    if ($Extra)
    {
        $text = $text.TrimEnd() + "`r`n`r`n# Added by tools\configure-server.ps1`r`n"
        foreach ($k in $Extra.Keys) { $text += "$k = $($Extra[$k])`r`n" }
    }
    [IO.File]::WriteAllText($Target, $text)
    Write-Host "wrote: $Target"
}

function Quote([string] $s) { return '"' + $s + '"' }

$cred = Get-GaMySqlCredential
function DbInfo([string] $db) { return Quote("$($cred.host);$($cred.port);$($cred.user);$($cred.password);$db") }

$fwd = { param($p) $p -replace '\\', '/' }
$logs = Join-Path $GA.Server 'logs'
New-Item -ItemType Directory -Force $logs | Out-Null
$modConfigs = Join-Path $GA.Configs 'modules'

# authserver (DB updates are owned by the worldserver, which also updates GA_auth;
# a second updater would race it on the first start)
Write-Conf (Join-Path $GA.Configs 'authserver.conf.dist') (Join-Path $GA.Configs 'authserver.conf') @{
    'RealmServerPort'   = "$($GA.AuthPort)"
    'LoginDatabaseInfo' = DbInfo $GA.Databases.Auth
    'LogsDir'           = Quote (& $fwd $logs)
    'SourceDirectory'   = Quote (& $fwd $GA.Core)
    'MySQLExecutable'   = Quote (& $fwd $GA.MySqlExe)
    'Updates.EnableDatabases' = '0'
}

# worldserver
Write-Conf (Join-Path $GA.Configs 'worldserver.conf.dist') (Join-Path $GA.Configs 'worldserver.conf') @{
    'WorldServerPort'       = "$($GA.WorldPort)"
    'LoginDatabaseInfo'     = DbInfo $GA.Databases.Auth
    'WorldDatabaseInfo'     = DbInfo $GA.Databases.World
    'CharacterDatabaseInfo' = DbInfo $GA.Databases.Characters
    'DataDir'               = Quote (& $fwd $GA.Data)
    'LogsDir'               = Quote (& $fwd $logs)
    'SourceDirectory'       = Quote (& $fwd $GA.Core)
    'MySQLExecutable'       = Quote (& $fwd $GA.MySqlExe)
    'Updates.EnableDatabases' = '7'
    'Updates.AutoSetup'     = '1'
    # SOAP on localhost only, for scripted GM commands (tools\ga-command.ps1).
    'SOAP.Enabled'          = '1'
    'SOAP.IP'               = Quote '127.0.0.1'
    'SOAP.Port'             = "$($GA.SoapPort)"
    # the owner's game settings (set by hand on 2026-11-12, kept here so -Force does not undo them):
    # level cap 60, double XP, drops and money, half aggro range, no resurrection sickness below
    # level 81, no intro cinematics.
    'MaxPlayerLevel'           = '60'
    'Rate.XP.Kill'             = '2'
    'Rate.XP.Quest'            = '2'
    'Rate.XP.Quest.DF'         = '2'
    'Rate.XP.Explore'          = '2'
    'Rate.XP.Pet'              = '2'
    'Rate.Drop.Item.Uncommon'  = '2'
    'Rate.Drop.Item.Rare'      = '2'
    'Rate.Drop.Item.Epic'      = '2'
    'Rate.Drop.Item.Legendary' = '2'
    'Rate.Drop.Item.Artifact'  = '2'
    'Rate.Drop.Item.Referenced' = '2'
    'Rate.Drop.Money'          = '2'
    'Rate.Creature.Aggro'      = '0.5'
    'Death.SicknessLevel'      = '81'
    'SkipCinematics'           = '1'
    # Also set by the owner by hand later on 2026-11-12: every flight path known, flights without travel time.
    'AllFlightPaths'           = '1'
    'InstantFlightPaths'       = '1'
    # Freeze detector (2026-11-12): a world update stuck for 10 minutes crashes the worldserver with a crash dump in
    # Crashes\ instead of hanging silently (the playerbots chat reply loop froze the world for 20+ minutes).
    'MaxCoreStuckTime'         = '600'
    # Map threads (2026-11-21, the stress test's result): one map thread saturates at about 480 bots, four keep the
    # world update smooth at 800.
    'MapUpdate.Threads'        = '4'
    # The world loop's shortest tick (2026-11-21, docs/performance.md PF-4, decision PD3): with 1 ms a lightly loaded
    # server spins through hundreds of ticks a second (an 800-bot server burnt 1.8 cores on 100 ticks a second). With
    # 10 ms a loaded server is not affected and an idle one stops spinning.
    'MinWorldUpdateTime'       = '10'
    # Instances (2026-11-24, the test-guild thread's finding): a trash creature in Shadowfang Keep that could not path
    # to its target held a bot group in a fight for 14 minutes. With 1 a non-boss creature in a dungeon or raid that
    # cannot reach its target teleports to it (retail-like) instead of evading or regenerating in place.
    'Creature.Instance.TeleportToUnreachableTarget' = '1'
    # AFK logout (2026-11-28): players watching their bots (watch mode, a dungeon run) for longer than the idle time
    # were logged out. 2 = no AFK logout in any zone.
    'PreventAFKLogout'         = '2'
    # Logs (2026-11-12): a restart no longer wipes what happened before it. Server.log and Errors.log are
    # appended to, each line with its time; Playerbots.log (thousands of AI lines a minute) starts anew with
    # every start, timestamped too. A log that passes its size cap (bytes, last field: 50 MB, 10 MB, 100 MB,
    # 20 MB) is renamed to <file>.<time> and started anew; tools\start-server.ps1 keeps the newest three of
    # those per log and deletes older ones.
    'Appender.Server'          = '2,5,1,Server.log,a,52428800'
    'Appender.Errors'          = '2,2,1,Errors.log,a,10485760'
    'Appender.Playerbots'      = '2,5,1,Playerbots.log,w,104857600'
} ([ordered]@{
    # The guild bots' activity log (docs/orders.md "Activity log"): task starts and ends, deaths, a status line per bot
    # every five minutes, in its own file only (not Server.log, not the console).
    'Appender.GuildAdventure'   = '2,5,1,GuildAdventure.log,a,20971520'
    'Logger.module.ga.activity' = '4,GuildAdventure'
})

# mod-playerbots: guild/alt bots only, no random bots (D6)
Write-Conf (Join-Path $modConfigs 'playerbots.conf.dist') (Join-Path $modConfigs 'playerbots.conf') @{
    'PlayerbotsDatabaseInfo'             = DbInfo $GA.Databases.Playerbots
    'Playerbots.Updates.EnableDatabases' = '1'
    'Playerbots.Enabled'                 = '1'
    'Playerbots.RandomBotAutologin'      = '0'
    'Playerbots.MinRandomBots'           = '0'
    'Playerbots.MaxRandomBots'           = '0'
    'Playerbots.AddClassAccountPoolSize' = '0'
    'Playerbots.MaxAddedBots'            = '40'
    'Playerbots.AllowGuildBots'          = '1'
    'Playerbots.AllowAccountBots'        = '1'
    # Altbots stay in the group when their master leaves (needed by the take-control
    # swap, where the master character changes hands). BotAutologin stays 0 and
    # SelfBotLevel at its default 1 (GM only); the swap work relies on both.
    'Playerbots.KeepAltsInGroup'         = '1'
    'Playerbots.BotAutologin'            = '0'
    'Playerbots.SelfBotLevel'            = '1'
    # M4 orders (docs/orders.md "Playerbots settings"): the module plans what guild bots do and
    # uses NewRpg only to carry it out, so NewRpg never picks a status by itself (an idle NewRpg
    # rests until the module gives it the next one).
    'Playerbots.RpgStatusProbWeight.WanderRandom' = '0'
    'Playerbots.RpgStatusProbWeight.WanderNpc'    = '0'
    'Playerbots.RpgStatusProbWeight.GoGrind'      = '0'
    'Playerbots.RpgStatusProbWeight.GoCamp'       = '0'
    'Playerbots.RpgStatusProbWeight.DoQuest'      = '0'
    'Playerbots.RpgStatusProbWeight.TravelFlight' = '0'
    'Playerbots.RpgStatusProbWeight.Rest'         = '0'
    'Playerbots.RpgStatusProbWeight.OutdoorPvp'   = '0'
    'Playerbots.RpgStatusProbWeight.DoGather'     = '0'
    # Guild bots progress legitimately (the design log section 1): no free food and water, no known
    # flight paths, no raid cheats; no summon to the master when a bot joins its party.
    'Playerbots.BotCheats'                        = '""'
    'Playerbots.SummonWhenGroup'                  = '0'
    # The module spends the guild bots' talent points along their talent templates (docs/specs.md); with 1
    # mod-playerbots would spend them first with its premade specs.
    'Playerbots.AutoPickTalentsForPlayerControlledBots' = '0'
    # No chat replies (2026-11-12): with it on, a guild bot answers a guild line that names it every time and any
    # other with a 1 in 5 chance, without a pause between its answers, and the answers name the bot they answer. A few
    # guild bots kept each other talking until the map threads did nothing else (two world stalls of 104 s and 387 s
    # on the integration server). Commands to bots are a separate path and keep working; the bots' broadcasts about
    # loot, quests and level-ups stay (EnableBroadcasts), the module shows one of them per 30-60 s
    # (GuildAdventure.BotGuildChat.*).
    'Playerbots.RandomBotTalk'                    = '0'
}

# mod-guild-adventure: template defaults (orders through the guild from afar are gone from the module since 64f3dc7)
Write-Conf (Join-Path $modConfigs 'mod_guild_adventure.conf.dist') (Join-Path $modConfigs 'mod_guild_adventure.conf') @{}

# mod-ah-bot-plus (pins.json "modules"; docs\server-setup.md "Auction house bot"): a buyer only (the owner,
# 2026-11-12: the bot buys what players and the guild list, it lists nothing itself). Its character
# (account GAAHBOT1, never enters the world) is looked up by name; without it the bot stays off.
$ahDist = Join-Path $modConfigs 'mod_ahbot.conf.dist'
if (Test-Path $ahDist)
{
    $ahGuid = Invoke-GaMySql "SELECT guid FROM characters WHERE name = '$($GA.AhBotCharacter)';" $GA.Databases.Characters
    $ahGuid = "$ahGuid".Trim()
    if (-not $ahGuid) { Write-Warning "AH bot character $($GA.AhBotCharacter) not found - the auction house bot stays off (docs\server-setup.md)" }
    Write-Conf $ahDist (Join-Path $modConfigs 'mod_ahbot.conf') @{
        'AuctionHouseBot.GUIDs'                          = $(if ($ahGuid) { $ahGuid } else { '0' })
        'AuctionHouseBot.EnableSeller'                   = 'false'
        'AuctionHouseBot.Buyer.Enabled'                  = $(if ($ahGuid) { 'true' } else { 'false' })
        # Every 2-10 minutes it prices 1-3 player auctions per auction house and buys or bids on the
        # ones at or below its own price for the item (AcceptablePriceModifier 1 = fair); vendor goods
        # above the vendor price and auctions other players bid on are left alone.
        'AuctionHouseBot.MinutesBetweenBuyCycle'         = '2:10'
        'AuctionHouseBot.Buyer.BuyCandidatesPerBuyCycle' = '1:3'
        'AuctionHouseBot.Buyer.AcceptablePriceModifier'  = '1'
        'AuctionHouseBot.Buyer.PreventOverpayingForVendorItems' = 'true'
        'AuctionHouseBot.Buyer.BidAgainstPlayers'        = 'false'
    }
}

# mod-autolearn-skills (pins.json "modules"; the owner, 2026-11-12): weapon skills, riding and mounts come by
# themselves on a level-up, so the bots need no weapon master or riding trainer (class spells: the guild
# module, GuildAdventure.AutoLearnClassSpells). Every character, the leader too; characters that exist get
# it at their next level-up. Apprentice riding and a race mount at 20, journeyman riding and an epic race
# mount at 40; no flying (MaxPlayerLevel 60).
$alDist = Join-Path $modConfigs 'mod_autolearn_skills.conf.dist'
if (Test-Path $alDist)
{
    Write-Conf $alDist (Join-Path $modConfigs 'mod_autolearn_skills.conf') @{
        'AutoLearnSkills.Enable'                      = '1'
        'AutoLearnSkills.AutoLearnWeaponSkills'       = '1'
        'AutoLearnSkills.MaxWeaponSkills'             = '1'
        'AutoLearnSkills.AutoLearnRiding'             = '1'
        'AutoLearnSkills.Riding.Apprentice'           = '1'
        'AutoLearnSkills.Riding.GrantApprenticeMount' = '1'
        'AutoLearnSkills.Riding.Journeyman'           = '1'
        'AutoLearnSkills.Riding.GrantJourneymanMount' = '1'
        'AutoLearnSkills.Riding.Expert'               = '0'
        'AutoLearnSkills.Riding.GrantExpertMount'     = '0'
        'AutoLearnSkills.Riding.Artisan'              = '0'
        'AutoLearnSkills.Riding.GrantArtisanMount'    = '0'
        'AutoLearnSkills.Riding.ColdWeather'          = '0'
    }
}

# Realm entry (exists once the worldserver has populated GA_auth on its first start).
$hasRealm = Invoke-GaMySql "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = '$($GA.Databases.Auth)' AND table_name = 'realmlist';"
if ([int]$hasRealm -gt 0)
{
    Invoke-GaMySql "UPDATE realmlist SET name = '$($GA.RealmName)', address = '127.0.0.1', localAddress = '127.0.0.1', port = $($GA.WorldPort) WHERE id = 1;" $GA.Databases.Auth | Out-Null
    Write-Host "realmlist: id 1 -> '$($GA.RealmName)' 127.0.0.1:$($GA.WorldPort)"
}
else
{
    Write-Host 'realmlist: GA_auth not populated yet - run this script again after the first worldserver start'
}
