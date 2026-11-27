-- mod-guild-adventure: bosses of a dungeon that an event brings (docs/orders.md "Dungeon runs", "Bosses that an event
-- brings"). A dungeon run stops at the NPC that starts the event: the leader of the group picks its gossip option,
-- the group stays with the NPC (and walks with it when it walks off) until the boss comes, and kills it.
-- before_boss: the stop comes before the boss with this creature entry in the dungeon's order (0 = after the last).
-- x, y, z: the place of an NPC or a boss that has no spawn because the instance brings it (Molten Core: Majordomo
-- Executus comes when the seven rune bosses are dead, and waits in the lair of Ragnaros after his defeat). A row with
-- npc_entry 0, no objects and a place is a boss the group waits for at that place (the raid runs, docs/orders.md "Raid runs").
-- gossip_steps: how many times the leader picks gossip_option (an option that opens the next menu before the one that
-- starts the event).

DROP TABLE IF EXISTS `ga_dungeon_event_template`;
CREATE TABLE `ga_dungeon_event_template` (
  `dungeon_id` INT UNSIGNED NOT NULL COMMENT 'LFGDungeons.dbc (Readiness.h)',
  `before_boss` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'creature entry of the boss this stop comes before; 0 = at the end',
  `npc_entry` INT UNSIGNED NOT NULL COMMENT 'the NPC that starts the event',
  `gossip_option` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'the option of its gossip menu that starts it',
  `boss_entry` INT UNSIGNED NOT NULL COMMENT 'the boss that comes',
  `follow` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '1: the NPC walks off and the group goes with it',
  `timeout_s` INT UNSIGNED NOT NULL DEFAULT 600 COMMENT 'the group gives the event this long',
  `go_entries` VARCHAR(64) NOT NULL DEFAULT '' COMMENT 'npc_entry 0: game objects to use, in this order, before the (spawned) boss can be fought',
  `area_trigger` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'npc_entry 0: an area trigger whose script starts the boss''s event (the leader walks into it)',
  `x` FLOAT NOT NULL DEFAULT 0 COMMENT 'the place of an NPC or boss that has no spawn (the instance brings it); 0 = its spawn',
  `y` FLOAT NOT NULL DEFAULT 0,
  `z` FLOAT NOT NULL DEFAULT 0,
  `gossip_steps` TINYINT UNSIGNED NOT NULL DEFAULT 1 COMMENT 'how many times gossip_option is picked, one menu after the other',
  `comment` VARCHAR(255) NOT NULL DEFAULT '',
  PRIMARY KEY (`dungeon_id`, `boss_entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: dungeon bosses that an event brings';

INSERT INTO `ga_dungeon_event_template` (`dungeon_id`, `before_boss`, `npc_entry`, `gossip_option`, `boss_entry`, `follow`, `timeout_s`, `go_entries`, `area_trigger`, `comment`) VALUES
(22, 0, 0, 0, 2748, 0, 0, '130511,133234', 0, 'Uldaman: the Altar of the Keepers opens the door to Archaedas, the Altar of Archaedas wakes him'),
(44, 0, 0, 0, 9816, 0, 0, '175706', 0, 'Upper Blackrock Spire: the Blackrock Altar wakes Pyroguard Emberseer (its spell sends event 4884), and the Blackhand Incarcerators around him fight first'),
(44, 0, 0, 0, 10429, 0, 900, '', 2026, 'Upper Blackrock Spire: area trigger 2026 (at_blackrock_stadium) starts the waves of the stadium; Warchief Rend Blackhand comes down on Gyth at the end'),
(36, 0, 0, 0, 11496, 0, 0, '177259,177257,177258,179504,179505', 0, 'Dire Maul, West: the five crystal generators shut down by themselves when no Arcane Aberration or Mana Remnant lives within 70 yd of them (their SmartAI conditions), and Immol''thar''s force field drops with the fifth: the group clears them first'),
(1, 0, 3678, 0, 3654, 1, 1500, '', 0, 'Wailing Caverns: the Disciple of Naralex walks to Naralex and wakes him, Mutanus the Devourer comes at the end'),
(14, 7079, 7998, 0, 7361, 1, 600, '', 0, 'Gnomeregan: Blastmaster Emi Shortfuse walks from tunnel to tunnel and blasts them shut, Grubbis comes at the end'),
(20, 7357, 0, 0, 7355, 0, 600, '148917', 0, 'Razorfen Downs: the gong brings two waves of spiders and then Tuten''kash (its SmartAI: a group per use, the gong unusable until it is dead); before Mordresh Fire Eye'),
(28, 0, 0, 0, 8580, 0, 0, '148830,148831,148832,148833,148834,148835', 0, 'Sunken Temple: the six Atal''ai statues around the pit, in the order the Altar of Hakkar shows (instance data 10 counts them), bring Atal''alarion into the phase of the players'),
(50, 0, 0, 0, 14020, 0, 0, '179148', 0, 'Blackwing Lair: Chromaggus is immune to everything behind his door until the lever beside it is used (go_chromaggus_lever: he walks out and attacks its user)');

-- The raids (docs/orders.md "Raid runs").
INSERT INTO `ga_dungeon_event_template` (`dungeon_id`, `before_boss`, `npc_entry`, `gossip_option`, `boss_entry`, `follow`, `timeout_s`, `go_entries`, `area_trigger`, `x`, `y`, `z`, `gossip_steps`, `comment`) VALUES
(48, 11502, 0, 0, 12018, 0, 180, '', 0, 759.542, -1173.43, -118.974, 1, 'Molten Core: Majordomo Executus is summoned at his place when the seven rune bosses are dead (instance_molten_core SummonMajordomoExecutus); his Flamewaker Healers and Elites die, he submits'),
(48, 0, 12018, 0, 11502, 0, 420, '', 0, 848.933, -812.875, -229.601, 3, 'Molten Core: Majordomo Executus waits in the lair after his defeat; three menus (4093, 4109, 4108) and he summons Ragnaros, who emerges after the intro'),
(42, 14834, 0, 0, 14515, 0, 600, '180526', 0, 0, 0, 0, 1, 'Zul''Gurub: the Gong of Bethekk summons High Priestess Arlokk (go_gong_of_bethekk); before Hakkar, whose Power stack she holds while she lives'),
(50, 13020, 13020, 0, 13020, 0, 300, '', 0, 0, 0, 0, 2, 'Blackwing Lair: Vaelastrasz the Corrupt is friendly until his two menus (21333 and 21334, option 0 each) are picked; after his speech he turns hostile at 30 % health and gives the raid Essence of the Red'),
(50, 0, 10162, 0, 11583, 0, 900, '', 0, -7502, -1256.5, 476.8, 3, 'Blackwing Lair: Lord Victor Nefarius starts his event after three menus (21330, 21331, 21332, option 0 each); 42 drakonids come from the two spawners, then Nefarian flies in on his path and lands here'),
(161, 15727, 0, 0, 15517, 0, 600, '', 0, -9173, 2107, -65, 1, 'Ahn''Qiraj Temple: Ouro comes out of his mound when a player is within 40 yd of it in its sight (npc_ouro_spawner); he is no boss of the dungeon finder''s list, so he stands before C''Thun here');

-- Raid mechanics the core leaves out: the poison cloud of a Son of Hakkar (Zul'Gurub) gives Poisonous Blood through
-- the module's spell script (OrderDungeons.cpp, spell_ga_poisonous_blood).
DELETE FROM `spell_script_names` WHERE `spell_id` = 24320 AND `ScriptName` = 'spell_ga_poisonous_blood';
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES (24320, 'spell_ga_poisonous_blood');
