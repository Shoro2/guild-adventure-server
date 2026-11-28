-- mod-guild-adventure: world database templates (GA_world).
-- Applied by the worldserver's DB updater (module SQL, re-applied when the file changes),
-- so every statement must be idempotent. Reload at runtime with ".ga reload".
-- Design: the design log sections 2.2, 2.4 and 3.6; protocol fields: docs/protocol.md sections 7.5 and 7.6.

-- Zones a bot can be sent to. faction: A = Alliance, H = Horde, C = contested / both.
-- continent = map id (0 Eastern Kingdoms, 1 Kalimdor, 530 Outland, 571 Northrend).
-- orders = order types allowed in the zone (protocol tokens).
CREATE TABLE IF NOT EXISTS `ga_zone_template` (
    `zone_id`   INT UNSIGNED     NOT NULL COMMENT 'AreaTable.dbc zone id',
    `name`      VARCHAR(64)      NOT NULL,
    `min_level` TINYINT UNSIGNED NOT NULL,
    `max_level` TINYINT UNSIGNED NOT NULL,
    `faction`   CHAR(1)          NOT NULL,
    `continent` SMALLINT UNSIGNED NOT NULL,
    `orders`    VARCHAR(64)      NOT NULL DEFAULT 'LEVEL_ZONE,GRIND,QUEST,GATHER',
    PRIMARY KEY (`zone_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: zones for orders';

DELETE FROM `ga_zone_template`;
INSERT INTO `ga_zone_template` (`zone_id`, `name`, `min_level`, `max_level`, `faction`, `continent`) VALUES
(12,   'Elwynn Forest',          1,  10, 'A', 0),
(1,    'Dun Morogh',             1,  10, 'A', 0),
(141,  'Teldrassil',             1,  10, 'A', 1),
(3524, 'Azuremyst Isle',         1,  10, 'A', 530),
(14,   'Durotar',                1,  10, 'H', 1),
(215,  'Mulgore',                1,  10, 'H', 1),
(85,   'Tirisfal Glades',        1,  10, 'H', 0),
(3430, 'Eversong Woods',         1,  10, 'H', 530),
(40,   'Westfall',               10, 20, 'A', 0),
(38,   'Loch Modan',             10, 20, 'A', 0),
(148,  'Darkshore',              10, 20, 'A', 1),
(3525, 'Bloodmyst Isle',         10, 20, 'A', 530),
(17,   'The Barrens',            10, 25, 'H', 1),
(130,  'Silverpine Forest',      10, 20, 'H', 0),
(3433, 'Ghostlands',             10, 20, 'H', 530),
(44,   'Redridge Mountains',     15, 25, 'A', 0),
(406,  'Stonetalon Mountains',   15, 27, 'C', 1),
(331,  'Ashenvale',              18, 30, 'C', 1),
(10,   'Duskwood',               18, 30, 'A', 0),
(11,   'Wetlands',               20, 30, 'A', 0),
(267,  'Hillsbrad Foothills',    20, 30, 'C', 0),
(400,  'Thousand Needles',       25, 35, 'C', 1),
(36,   'Alterac Mountains',      30, 40, 'C', 0),
(45,   'Arathi Highlands',       30, 40, 'C', 0),
(405,  'Desolace',               30, 40, 'C', 1),
(33,   'Stranglethorn Vale',     30, 45, 'C', 0),
(15,   'Dustwallow Marsh',       35, 45, 'C', 1),
(3,    'Badlands',               35, 45, 'C', 0),
(8,    'Swamp of Sorrows',       35, 45, 'C', 0),
(357,  'Feralas',                40, 50, 'C', 1),
(440,  'Tanaris',                40, 50, 'C', 1),
(47,   'The Hinterlands',        40, 50, 'C', 0),
(51,   'Searing Gorge',          43, 50, 'C', 0),
(16,   'Azshara',                45, 55, 'C', 1),
(4,    'Blasted Lands',          45, 55, 'C', 0),
(490,  'Un''Goro Crater',        48, 55, 'C', 1),
(361,  'Felwood',                48, 55, 'C', 1),
(46,   'Burning Steppes',        50, 58, 'C', 0),
(28,   'Western Plaguelands',    51, 58, 'C', 0),
(139,  'Eastern Plaguelands',    53, 60, 'C', 0),
(618,  'Winterspring',           53, 60, 'C', 1),
(1377, 'Silithus',               55, 60, 'C', 1),
(3483, 'Hellfire Peninsula',     58, 63, 'C', 530),
(3521, 'Zangarmarsh',            60, 64, 'C', 530),
(3519, 'Terokkar Forest',        62, 65, 'C', 530),
(3518, 'Nagrand',                64, 67, 'C', 530),
(3522, 'Blade''s Edge Mountains', 65, 68, 'C', 530),
(3523, 'Netherstorm',            67, 70, 'C', 530),
(3520, 'Shadowmoon Valley',      67, 70, 'C', 530),
(3537, 'Borean Tundra',          68, 72, 'C', 571),
(495,  'Howling Fjord',          68, 72, 'C', 571),
(65,   'Dragonblight',           71, 75, 'C', 571),
(394,  'Grizzly Hills',          73, 75, 'C', 571),
(66,   'Zul''Drak',              74, 77, 'C', 571),
(3711, 'Sholazar Basin',         76, 78, 'C', 571),
(67,   'The Storm Peaks',        77, 80, 'C', 571),
(210,  'Icecrown',               77, 80, 'C', 571);

-- Progression steps. kind: slots (value = total slots after the unlock), offline (value = window
-- in minutes), feature (value = feature key, docs/protocol.md section 8.4). gold_cost 0 = owned
-- automatically when the renown level is reached. A guild is at most 40 people, its leader and 39 bots, and the paid
-- slot tiers cost twice what they did before 2026-11-27 (the owner).
CREATE TABLE IF NOT EXISTS `ga_unlock_template` (
    `id`           INT UNSIGNED     NOT NULL,
    `renown_level` TINYINT UNSIGNED NOT NULL,
    `kind`         VARCHAR(8)       NOT NULL,
    `value`        VARCHAR(32)      NOT NULL,
    `gold_cost`    BIGINT UNSIGNED  NOT NULL DEFAULT 0 COMMENT 'copper, paid from the guild bank',
    `description`  VARCHAR(128)     NOT NULL,
    PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: progression unlocks';

DELETE FROM `ga_unlock_template`;
INSERT INTO `ga_unlock_template` (`id`, `renown_level`, `kind`, `value`, `gold_cost`, `description`) VALUES
(1,  1, 'slots',   '3',           0,       '3 bot slots'),
(2,  1, 'offline', '60',          0,       'Bots keep playing 1 h after you log out'),
(3,  1, 'feature', 'orders',      0,       'Orders: Autopilot, Idle, Follow, Level in zone'),
(4,  1, 'feature', 'recruit',     0,       'Recruit bots into free slots'),
(5,  2, 'slots',   '5',           0,       '5 bot slots'),
(6,  2, 'feature', 'professions', 0,       'Profession plans'),
(7,  3, 'slots',   '8',           0,       '8 bot slots'),
(8,  3, 'offline', '120',         0,       'Offline window 2 h'),
(9,  3, 'feature', 'auto_limits', 0,       'Autopilot limits (level cap, continent, dungeons, gold)'),
(10, 4, 'slots',   '10',          1000000, '10 bot slots'),
(11, 1, 'feature', 'talents',     0,       'Talent templates'),
(12, 5, 'slots',   '15',          2000000, '15 bot slots'),
(13, 5, 'offline', '240',         0,       'Offline window 4 h'),
(14, 1, 'feature', 'gear',        0,       'Gear management'),
(15, 6, 'slots',   '20',          0,       '20 bot slots'),
(16, 6, 'feature', 'groups',      0,       'Party builder'),
(17, 7, 'slots',   '25',          5000000, '25 bot slots'),
(18, 7, 'offline', '360',         0,       'Offline window 6 h'),
(19, 7, 'feature', 'dungeons',    0,       'Dungeon orders'),
(20, 7, 'feature', 'map',         0,       'World map overlay'),
(21, 8, 'slots',   '30',          0,       '30 bot slots'),
(22, 8, 'feature', 'stats',       0,       'Bot statistics'),
(23, 9, 'slots',   '39',          10000000, '39 bot slots'),
(24, 9, 'offline', '480',         0,       'Offline window 8 h'),
(25, 9, 'feature', 'raids',       0,       'Raid orders');

-- Renown needed to advance from `level` to `level` + 1. The highest level has no row. Five times the points of before
-- 2026-11-27 (the owner: renown rose far too fast); GuildAdventure.Renown.Rate scales what a guild gains.
CREATE TABLE IF NOT EXISTS `ga_renown_level` (
    `level`  TINYINT UNSIGNED NOT NULL,
    `points` INT UNSIGNED     NOT NULL,
    PRIMARY KEY (`level`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: renown curve';

DELETE FROM `ga_renown_level`;
INSERT INTO `ga_renown_level` (`level`, `points`) VALUES
(1, 500), (2, 1250), (3, 2500), (4, 4500), (5, 7000), (6, 10000), (7, 14000), (8, 19000);

-- Renown points per event a guild bot causes (event_type = protocol event token). ACHIEVEMENT is the least a new
-- guild achievement gives: it gives its achievement points times GuildAdventure.Achievements.RenownPerPoint, but at
-- least this (Feats of Strength have no points).
CREATE TABLE IF NOT EXISTS `ga_renown_source` (
    `event_type` VARCHAR(16)  NOT NULL,
    `points`     INT UNSIGNED NOT NULL,
    PRIMARY KEY (`event_type`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: renown sources';

DELETE FROM `ga_renown_source`;
INSERT INTO `ga_renown_source` (`event_type`, `points`) VALUES
('LEVEL_UP', 10), ('QUEST_DONE', 3), ('PROF_UP', 5), ('ACHIEVEMENT', 5);

-- Ships and zeppelins guild bots may take between regions (docs/orders.md "Travel"). entry = gameobject_template
-- entry of the MO_TRANSPORT (table `transports`); the docks are the stops of its path (TransportMgr), so only the
-- faction and a short name live here. faction: A = Alliance only, H = Horde only, C = both.
CREATE TABLE IF NOT EXISTS `ga_transport_route` (
    `entry`   INT UNSIGNED NOT NULL,
    `faction` CHAR(1)      NOT NULL,
    `name`    VARCHAR(64)  NOT NULL,
    PRIMARY KEY (`entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: ships and zeppelins for travel';

DELETE FROM `ga_transport_route`;
INSERT INTO `ga_transport_route` (`entry`, `faction`, `name`) VALUES
(176310, 'A', 'The Bravery'),        -- Stormwind Harbor - Auberdine
(176244, 'A', 'The Moonspray'),      -- Rut'theran Village - Auberdine
(181646, 'A', 'Elune''s Blessing'),  -- Valaar's Berth - Auberdine
(176231, 'A', 'The Lady Mehley'),    -- Menethil Harbor - Theramore Isle
(190536, 'A', 'The Kraken'),         -- Stormwind Harbor - Valiance Keep
(181688, 'A', 'Northspear'),         -- Menethil Harbor - Valgarde
(20808,  'C', 'The Maiden''s Fancy'),-- Booty Bay - Ratchet
(177233, 'A', 'the Feathermoon Ferry'),-- the Forgotten Coast - Feathermoon Stronghold (Sardor Isle, Feralas)
(164871, 'H', 'The Thundercaller'),  -- Undercity - Orgrimmar
(175080, 'H', 'The Iron Eagle'),     -- Grom'gol - Orgrimmar
(176495, 'H', 'The Purple Princess'),-- Grom'gol - Undercity
(181689, 'H', 'Cloudkisser'),        -- Undercity - Vengeance Landing
(186238, 'H', 'The Mighty Wind');    -- Orgrimmar - Warsong Hold

-- Guild perks (PerkService, docs/guild-management.md "Guild perks"): permanent upgrades for every guild character, bought
-- with guild points (the achievement points of the guild's achievements). perk_key tells the module what the perk does
-- (PerkService.h); costs and vals hold one number per rank, comma separated (a single unlock: one cost, value 1);
-- description shows the value of a rank where it says {}. The owner tunes the costs here (the design log section 7, P1).
CREATE TABLE IF NOT EXISTS `ga_perk_template` (
    `id`          INT UNSIGNED NOT NULL,
    `perk_key`    VARCHAR(32)  NOT NULL,
    `name`        VARCHAR(64)  NOT NULL,
    `costs`       VARCHAR(128) NOT NULL COMMENT 'guild points per rank, comma separated',
    `vals`        VARCHAR(128) NOT NULL COMMENT 'value per rank, comma separated (single unlocks: 1)',
    `unit`        VARCHAR(8)   NOT NULL DEFAULT '',
    `description` VARCHAR(200) NOT NULL COMMENT '{} = the value of the rank',
    PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: guild perks';

DELETE FROM `ga_perk_template`;
INSERT INTO `ga_perk_template` (`id`, `perk_key`, `name`, `costs`, `vals`, `unit`, `description`) VALUES
(1,  'move_speed',        'Swift Feet',         '50,100,200,350,500', '5,10,15,20,25',  '%', '+{}% movement speed'),
(2,  'experience',        'Fast Learners',      '50,100,200,350,500', '10,20,30,40,50', '%', '+{}% experience'),
(3,  'gold',              'Deep Pockets',       '50,100,200,350,500', '5,10,15,20,25',  '%', '+{}% looted gold'),
(4,  'extra_material',    'Thorough Hands',     '50,100,200,350,500', '5,10,15,20,25',  '%', '{}% chance of one more crafting material'),
(5,  'kill_regen',        'Second Wind',        '50,100,200,350,500', '1,2,3,4,5',      '%', 'after a kill, {}% health and mana over 10 seconds (10 s cooldown)'),
(6,  'no_reagents',       'Reagent-Free',       '150',                '1',              '',  'class spells need no reagents'),
(7,  'instant_flight',    'Instant Flights',    '400',                '1',              '',  'flights from a flight master land at once'),
(8,  'all_flight_paths',  'Cartographers',      '250',                '1',              '',  'every flight path of your faction is known'),
(9,  'materials_storage', 'Material Haulers',   '100',                '1',              '',  'loot goes into the guild storage by itself: what the bots loot, and the crafting materials you loot yourself'),
(10, 'drop_chance',       'Lucky Finds',        '50,100,200,350,500', '5,10,15,20,25',  '%', '+{}% drop chance for items up to rare'),
(11, 'craft_speed',       'Master Crafters',    '50,100,200,350,500', '10,20,30,40,50', '%', '+{}% crafting speed'),
(12, 'gather_speed',      'Quick Hands',        '50,100,200,350,500', '10,20,30,40,50', '%', '+{}% mining, herb gathering and skinning speed'),
(13, 'water_breathing',   'Gills',              '100',                '1',              '',  'breathe under water'),
(14, 'no_fall_damage',    'Feather Light',      '100',                '1',              '',  'no fall damage'),
(15, 'shared_flight_paths', 'Shared Maps',      '150',                '1',              '',  'a flight path one character of the guild discovers, all of its side know');

-- Quests characters the AI plays do not take, because the order planner cannot finish them or has no use for them
-- (QuestSupport, docs/orders.md "Quests the bots leave alone"). A hand-checked list: add a row to keep bots off a quest,
-- delete one to let them try. Quests that complete through a script event and quests the bots learned to leave alone
-- are found by the module itself and are not listed here.
-- `setting`: empty = the row always applies; else the name of a boolean module setting without its "GuildAdventure."
-- prefix, and the row applies only while that setting is on (a setting that does not exist counts as off).
DROP TABLE IF EXISTS `ga_quest_block_template`;
CREATE TABLE `ga_quest_block_template` (
    `quest_id` INT UNSIGNED NOT NULL,
    `reason`   VARCHAR(120) NOT NULL DEFAULT '',
    `setting`  VARCHAR(60) NOT NULL DEFAULT '',
    PRIMARY KEY (`quest_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: quests AI-played characters leave alone';

INSERT INTO `ga_quest_block_template` (`quest_id`, `reason`) VALUES
(565,  'needs bought and crafted items (Bartolo''s Yeti Fur Cloak)'),
(629,  'under water (The Vile Reef)'),
(662,  'under water (Deep Sea Salvage)'),
(664,  'under water (Drowned Sorrows)'),
(734,  'needs a talk with choices (This Is Going to Be Hard)'),
(1107, 'under water (Encrusted Tail Fins)'),
(1119, 'timed delivery with a talk (Zanzil''s Mixture and a Fool''s Stout)'),
(1149, 'a leap of faith off a pillar: its trigger lies in the air (Test of Faith)'),
(3561, 'the ender is out of reach (Delivery to Archmage Xylem)'),
(4103, 'needs an item kept active (Salve via Hunting)'),
(8483, 'needs a talk with choices (The Dwarven Spy)'),
(9121, 'attunement to Naxxramas, a level-80 raid in this client (The Dread Citadel - Naxxramas)'),
(9122, 'attunement to Naxxramas, a level-80 raid in this client (The Dread Citadel - Naxxramas)'),
(9123, 'attunement to Naxxramas, a level-80 raid in this client (The Dread Citadel - Naxxramas)'),
(9378, 'attunement to Naxxramas, a level-80 raid in this client (The Dread Citadel - Naxxramas)');

-- Quest events the module cannot read from the SmartAI scripts (QuestEvents, docs/orders.md "Quest events"): quests
-- whose event runs in a C++ script, hand-checked. `kind`: 'stay' = the event runs at `npc_entry` (the quest giver) once
-- the quest is taken, the bot stays and fights; 'lead' = `npc_entry` follows the bot, which brings it to `end_entry`;
-- 'enter' = walking into area trigger `npc_entry` starts the event, the bot stays there and fights;
-- 'clear' = the creature `npc_entry` of an objective shows itself once nothing hostile is left around its spawn: the bot
-- kills what is hostile within 80 yd of the spawn, stays and fights what comes;
-- 'none' = the quest is left alone although the scripts suggest a way (its event needs more than the planner can do).
DROP TABLE IF EXISTS `ga_quest_event_template`;
CREATE TABLE `ga_quest_event_template` (
    `quest_id`  INT UNSIGNED NOT NULL,
    `kind`      VARCHAR(8) NOT NULL,
    `npc_entry` INT UNSIGNED NOT NULL DEFAULT 0,
    `end_entry` INT UNSIGNED NOT NULL DEFAULT 0,
    `comment`   VARCHAR(120) NOT NULL DEFAULT '',
    PRIMARY KEY (`quest_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: quest events of C++ scripts';

INSERT INTO `ga_quest_event_template` (`quest_id`, `kind`, `npc_entry`, `end_entry`, `comment`) VALUES
(452,   'stay', 2058, 0,    'Pyrewood Ambush: Deathstalker Faerleia, waves of Pyrewood villagers'),
(1560,  'lead', 5955, 6015, 'Tooga''s Quest: Tooga follows to Torta'),
(2845,  'lead', 7774, 7765, 'Wandering Shay: Shay follows to Rockbiter and wanders off (Shay''s Bell, quest interactions)'),
(4740,  'clear', 10323, 0,  'WANTED: Murkdeep!: no Greymist Warrior or Hunter alive near his spot, three waves, then he comes'),
(4491,  'lead', 9999, 9997, 'A Little Help From My Friends: Ringo follows to Spraggle Frock and faints (Spraggle''s Canteen)'),
(5321,  'lead', 11218, 11219, 'The Sleeper Has Awakened: Kerlonian follows to Liladris and falls asleep (Horn of Awakening)'),
(5943,  'none', 11626, 0,   'Gizelton Caravan: a caravan escort in a C++ script'),
(11198, 'none', 23905, 0,   'Take Down Tethyr!: needs the cannons of Theramore');

-- Nodes of mod-playerbots' travel node graph the road map leaves out (RoadMap, docs/orders.md "Routes"): a bot could
-- not walk to them (the ground guard cancelled the step). Written by the module; delete a row to give a node another
-- chance (".ga reload" reads the table again). Kept across restarts and re-applications of this file.
CREATE TABLE IF NOT EXISTS `ga_road_block` (
    `node_id`    INT UNSIGNED NOT NULL COMMENT 'playerbots_travelnode.id',
    `name`       VARCHAR(100) NOT NULL DEFAULT '',
    `blocked_at` INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`node_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: travel nodes bots could not walk to';

-- One-off: the first version took a node out when the walk to the first node of a route failed, which says nothing
-- about the node ("Stormwind City", "Ratchet flightMaster" went that way). Those rows go.
DELETE FROM `ga_road_block` WHERE `blocked_at` < 1794502800;

-- Class quests whose reward the module gives directly (ClassAbilities: pet taming and the first pet, demons, stances,
-- Bear Form, Redemption, totems): pointless for an AI-played character while that setting is on.
INSERT INTO `ga_quest_block_template` (`quest_id`, `reason`, `setting`)
SELECT q.`ID`, 'reward granted by ClassAbilities', 'ClassAbilities.Enable' FROM `quest_template` q WHERE q.`ID` IN (
    -- hunter: Taming the Beast (three steps per race), Training the Beast / Beast Training
    6061,6062,6063,6064,6081,6082,6083,6084,6085,6086,6087,6088,6089,6101,6102,6103,9484,9485,9486,9591,9592,9593,9673,9675,
    -- warlock: The Binding (imp, voidwalker, succubus, felhunter), The Rune of Summoning
    1471,1474,1504,1513,1689,1739,1795,9619,
    -- warrior: the Defensive Stance quests and The Affray (Berserker Stance)
    1665,1678,1683,9582,10350,1719,
    -- druid: Body and Heart (Bear Form)
    6001,6002,
    -- paladin: The Tome of Divinity, Redemption, Redeeming the Dead
    1785,1788,9600,9685,
    -- shaman: Call of Earth / Fire / Water / Air (the totems)
    63,96,100,220,1103,1516,1517,1518,1519,1520,1521,1522,1523,1524,1525,1526,1527,1528,1529,1530,1531,1532,1534,1535,1536,
    2983,2984,2985,2986,9449,9450,9451,9461,9462,9464,9465,9467,9468,9500,9501,9502,9503,9504,9508,9509,9547,9551,9552,9553,
    9554,9555,10490,10491);
