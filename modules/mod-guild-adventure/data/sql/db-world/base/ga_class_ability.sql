-- mod-guild-adventure: what a class only gets from its class quests (GA_world).
-- Applied by the worldserver's DB updater (module SQL, re-applied when the file changes), so every statement must
-- be idempotent. Reload at runtime with ".ga reload". Design: docs/orders.md "Class abilities", the design log section 7 (D24).
--
-- A guild bot gets every row of its class (and race, 0 = every race) when it reaches min_level: the spell, the item,
-- or, for a hunter, a first pet made from the creature (the row of its own race wins over the row for every race).
-- In this world database the class trainers teach none of these; they are the rewards of the class quests
-- ("Taming the Beast", "The Binding", "Call of Earth", "Body and Heart", the warrior's stance quests ...).
-- pet_kind (hunter rows with a creature): 0 = the first pet (given to a hunter without one), 1 = the good pet at
-- min_level (replaces the first one, once; not for a beast master, who waits for 2), 2 = the exotic pet of a hunter
-- with the Beast Mastery talent (the owner, 2026-11-12: a standard pet at 10, a good one at 60, or for beast mastery once it
-- can tame exotic beasts).
-- The table holds template data only, so it is dropped and built again whenever this file changes.
DROP TABLE IF EXISTS `ga_class_ability`;
CREATE TABLE `ga_class_ability` (
    `id`          INT UNSIGNED     NOT NULL,
    `class`       TINYINT UNSIGNED NOT NULL COMMENT 'ChrClasses.dbc id',
    `race`        TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'ChrRaces.dbc id, 0 = every race',
    `min_level`   TINYINT UNSIGNED NOT NULL,
    `spell_id`    INT UNSIGNED     NOT NULL DEFAULT 0,
    `item_id`     INT UNSIGNED     NOT NULL DEFAULT 0,
    `creature_id` INT UNSIGNED     NOT NULL DEFAULT 0 COMMENT 'hunter: the beast a pet is made from',
    `pet_kind`    TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 first pet, 1 good pet, 2 exotic pet (Beast Mastery talent)',
    `comment`     VARCHAR(96)      NOT NULL DEFAULT '',
    PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: abilities from class quests';

DELETE FROM `ga_class_ability`;
INSERT INTO `ga_class_ability` (`id`, `class`, `race`, `min_level`, `spell_id`, `item_id`, `creature_id`, `comment`) VALUES
-- Warrior
(100, 1, 0, 10,    71, 0, 0, 'Defensive Stance'),
(101, 1, 0, 10,   355, 0, 0, 'Taunt'),
(102, 1, 0, 10,  7386, 0, 0, 'Sunder Armor'),
(103, 1, 0, 30,  2458, 0, 0, 'Berserker Stance'),
-- Paladin
(200, 2, 0, 12,  7328, 0, 0, 'Redemption'),
-- Hunter: pet handling at 10, and a first pet (the owner, 2026-11-12: a standard pet at level 10)
(300, 3, 0, 10,  1515, 0, 0, 'Tame Beast'),
(301, 3, 0, 10,   883, 0, 0, 'Call Pet'),
(302, 3, 0, 10,  2641, 0, 0, 'Dismiss Pet'),
(303, 3, 0, 10,  6991, 0, 0, 'Feed Pet'),
(304, 3, 0, 10,   982, 0, 0, 'Revive Pet'),
(310, 3, 0, 10, 0, 0,  1126, 'first pet, any race: Large Crag Boar'),
(311, 3, 2, 10, 0, 0,  3099, 'first pet, orc: Dire Mottled Boar'),
(312, 3, 3, 10, 0, 0,  1196, 'first pet, dwarf: Ice Claw Bear'),
(313, 3, 4, 10, 0, 0,  2043, 'first pet, night elf: Nightsaber Stalker'),
(314, 3, 6, 10, 0, 0,  2959, 'first pet, tauren: Prairie Stalker'),
(315, 3, 8, 10, 0, 0,  3126, 'first pet, troll: Armored Scorpid'),
(316, 3, 10, 10, 0, 0, 15652, 'first pet, blood elf: Elder Springpaw'),
(317, 3, 11, 10, 0, 0, 17203, 'first pet, draenei: Nightstalker'),
(320, 3, 0, 60, 0, 0,  9696, 'good pet at 60: Bloodaxe Worg (a wolf)'),
(321, 3, 0, 60, 0, 0,  6498, 'exotic pet of a beast master: Devilsaur'),
-- (pet_kind of the two rows above is set below the insert)
-- Shaman: the totem of each element (every totem spell needs it) and the first totem spell of its quest
(700, 7, 0,  4,  8071, 0, 0, 'Stoneskin Totem (Call of Earth)'),
(701, 7, 0,  4,     0, 5175, 0, 'Earth Totem'),
(702, 7, 0, 10,  3599, 0, 0, 'Searing Totem (Call of Fire)'),
(703, 7, 0, 10,     0, 5176, 0, 'Fire Totem'),
(704, 7, 0, 20,  5394, 0, 0, 'Healing Stream Totem (Call of Water)'),
(705, 7, 0, 20,     0, 5177, 0, 'Water Totem'),
(706, 7, 0, 30,     0, 5178, 0, 'Air Totem (Call of Air)'),
-- Warlock
(900, 9, 0, 10,   697, 0, 0, 'Summon Voidwalker'),
(901, 9, 0, 20,   712, 0, 0, 'Summon Succubus'),
(902, 9, 0, 30,   691, 0, 0, 'Summon Felhunter'),
(903, 9, 0, 50,  1122, 0, 0, 'Inferno'),
(904, 9, 0, 60, 18540, 0, 0, 'Ritual of Doom'),
-- Druid
(1100, 11, 0, 10, 5487, 0, 0, 'Bear Form'),
(1101, 11, 0, 10, 6795, 0, 0, 'Growl'),
(1102, 11, 0, 10, 6807, 0, 0, 'Maul'),
(1103, 11, 0, 14, 8946, 0, 0, 'Cure Poison');

UPDATE `ga_class_ability` SET `pet_kind` = 1 WHERE `id` = 320;
UPDATE `ga_class_ability` SET `pet_kind` = 2 WHERE `id` = 321;
