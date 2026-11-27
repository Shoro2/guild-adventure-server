-- Quest events whose creatures wait to be attacked (the owner, 2026-11-25: [quote left out]).
--
-- This server halves the creatures' aggro radius (Rate.Creature.Aggro 0.5). The creatures below are summoned by a quest
-- event, or made hostile by its script, and then wait until a player comes into that radius: the guild leader stood
-- beside Ogron for minutes in "Questioning Reethe" while Lieutenant Caldwell's men stood beside him. Here each of them
-- attacks the nearest player within 40 yd (60 for the Kolkar waves) the moment it is summoned, or, for Ogron's ambush,
-- the moment Paval Reethe's script makes it hostile. The rows use ids 50 and up, which the world database does not use
-- for these creatures; the file can be applied again (DELETE first).
--
-- SmartAI: event 54 just summoned, event 38 data set, action 49 attack start, action 45 set data, target 21 closest
-- player (param1 max yards), target 9 creatures in range (entry, min, max).

-- "Questioning Reethe" (1273): Paval Reethe's list 498000 sets faction 16 on Private Hallan (5045), the Theramore
-- Skirmishers (5044) and Lieutenant Caldwell (5046); then it gives them data 2 2, and they attack.
DELETE FROM `smart_scripts` WHERE `source_type` = 9 AND `entryorguid` = 498000 AND `id` BETWEEN 50 AND 52;
INSERT INTO `smart_scripts` (`entryorguid`, `source_type`, `id`, `link`, `event_type`, `event_phase_mask`, `event_chance`,
    `event_flags`, `event_param1`, `event_param2`, `event_param3`, `event_param4`, `event_param5`, `event_param6`,
    `action_type`, `action_param1`, `action_param2`, `action_param3`, `action_param4`, `action_param5`, `action_param6`,
    `target_type`, `target_param1`, `target_param2`, `target_param3`, `target_param4`, `target_x`, `target_y`, `target_z`,
    `target_o`, `comment`) VALUES
(498000, 9, 50, 0, 0, 0, 100, 0, 1000, 1000, 0, 0, 0, 0, 45, 2, 2, 0, 0, 0, 0, 9, 5045, 0, 50, 0, 0, 0, 0, 0,
    'Paval Reethe - On Script - Set Data 2 2 (Private Hallan) (GuildAdventure: they attack)'),
(498000, 9, 51, 0, 0, 0, 100, 0, 0, 0, 0, 0, 0, 0, 45, 2, 2, 0, 0, 0, 0, 9, 5044, 0, 50, 0, 0, 0, 0, 0,
    'Paval Reethe - On Script - Set Data 2 2 (Theramore Skirmisher) (GuildAdventure: they attack)'),
(498000, 9, 52, 0, 0, 0, 100, 0, 0, 0, 0, 0, 0, 0, 45, 2, 2, 0, 0, 0, 0, 9, 5046, 0, 50, 0, 0, 0, 0, 0,
    'Paval Reethe - On Script - Set Data 2 2 (Lieutenant Caldwell) (GuildAdventure: they attack)');

DELETE FROM `smart_scripts` WHERE `source_type` = 0 AND `entryorguid` IN (5044, 5045, 5046) AND `id` = 50;
INSERT INTO `smart_scripts` (`entryorguid`, `source_type`, `id`, `link`, `event_type`, `event_phase_mask`, `event_chance`,
    `event_flags`, `event_param1`, `event_param2`, `event_param3`, `event_param4`, `event_param5`, `event_param6`,
    `action_type`, `action_param1`, `action_param2`, `action_param3`, `action_param4`, `action_param5`, `action_param6`,
    `target_type`, `target_param1`, `target_param2`, `target_param3`, `target_param4`, `target_x`, `target_y`, `target_z`,
    `target_o`, `comment`) VALUES
(5044, 0, 50, 0, 38, 0, 100, 0, 2, 2, 0, 0, 0, 0, 49, 0, 0, 0, 0, 0, 0, 21, 40, 0, 0, 0, 0, 0, 0, 0,
    'Theramore Skirmisher - On Data Set 2 2 - Attack Closest Player (GuildAdventure)'),
(5045, 0, 50, 0, 38, 0, 100, 0, 2, 2, 0, 0, 0, 0, 49, 0, 0, 0, 0, 0, 0, 21, 40, 0, 0, 0, 0, 0, 0, 0,
    'Private Hallan - On Data Set 2 2 - Attack Closest Player (GuildAdventure)'),
(5046, 0, 50, 0, 38, 0, 100, 0, 2, 2, 0, 0, 0, 0, 49, 0, 0, 0, 0, 0, 0, 21, 40, 0, 0, 0, 0, 0, 0, 0,
    'Lieutenant Caldwell - On Data Set 2 2 - Attack Closest Player (GuildAdventure)');

-- Summoned hostile and left standing or wandering: "The Exorcism", "Mana Surges", "Cleansing of the Orb of Orahil"
-- (1955, 1957, 4961: Tabetha's Mana Surges, 6550), "Get Me Out of Here!" (6132: Maraudine Wranglers, 4655, summoned
-- only there by event 54; their 15 world spawns have no such event), "Counterattack!" (4021: Kolkar Invaders 9524,
-- Kolkar Stormseers 9523, Warlord Krom'zar 9456), "This Is Going to Be Hard" (778: the Fam'retor Guardian 2919, which
-- has no AI and no world spawn).
DELETE FROM `smart_scripts` WHERE `source_type` = 0 AND `entryorguid` IN (6550, 4655, 9524, 9523, 9456, 2919) AND `id` = 50;
INSERT INTO `smart_scripts` (`entryorguid`, `source_type`, `id`, `link`, `event_type`, `event_phase_mask`, `event_chance`,
    `event_flags`, `event_param1`, `event_param2`, `event_param3`, `event_param4`, `event_param5`, `event_param6`,
    `action_type`, `action_param1`, `action_param2`, `action_param3`, `action_param4`, `action_param5`, `action_param6`,
    `target_type`, `target_param1`, `target_param2`, `target_param3`, `target_param4`, `target_x`, `target_y`, `target_z`,
    `target_o`, `comment`) VALUES
(6550, 0, 50, 0, 54, 0, 100, 0, 0, 0, 0, 0, 0, 0, 49, 0, 0, 0, 0, 0, 0, 21, 40, 0, 0, 0, 0, 0, 0, 0,
    'Mana Surge - On Just Summoned - Attack Closest Player (GuildAdventure)'),
(4655, 0, 50, 0, 54, 0, 100, 0, 0, 0, 0, 0, 0, 0, 49, 0, 0, 0, 0, 0, 0, 21, 40, 0, 0, 0, 0, 0, 0, 0,
    'Maraudine Wrangler - On Just Summoned - Attack Closest Player (GuildAdventure)'),
(9524, 0, 50, 0, 54, 0, 100, 0, 0, 0, 0, 0, 0, 0, 49, 0, 0, 0, 0, 0, 0, 21, 60, 0, 0, 0, 0, 0, 0, 0,
    'Kolkar Invader - On Just Summoned - Attack Closest Player (GuildAdventure)'),
(9523, 0, 50, 0, 54, 0, 100, 0, 0, 0, 0, 0, 0, 0, 49, 0, 0, 0, 0, 0, 0, 21, 60, 0, 0, 0, 0, 0, 0, 0,
    'Kolkar Stormseer - On Just Summoned - Attack Closest Player (GuildAdventure)'),
(9456, 0, 50, 0, 54, 0, 100, 0, 0, 0, 0, 0, 0, 0, 49, 0, 0, 0, 0, 0, 0, 21, 60, 0, 0, 0, 0, 0, 0, 0,
    'Warlord Krom''zar - On Just Summoned - Attack Closest Player (GuildAdventure)'),
(2919, 0, 50, 0, 54, 0, 100, 0, 0, 0, 0, 0, 0, 0, 49, 0, 0, 0, 0, 0, 0, 21, 40, 0, 0, 0, 0, 0, 0, 0,
    'Fam''retor Guardian - On Just Summoned - Attack Closest Player (GuildAdventure)');
UPDATE `creature_template` SET `AIName` = 'SmartAI' WHERE `entry` = 2919 AND `AIName` = '';
