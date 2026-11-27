-- mod-guild-adventure: the bosses and the entrance of a dungeon on a map with several dungeons, where the rule of
-- Dungeons::Load is wrong (a wing's bosses are those that lie nearest to its entrance; docs/orders.md "Wings").
-- A dungeon with rows here has exactly the listed bosses, and the entrance of the listed area trigger (0 = by the
-- rule). A dungeon that is no wing can be listed too, to leave a boss out (Uldaman without Ironaya). A wing whose end
-- boss has no spawn (Stratholme's Main Gate: Balnazzar) names its entrance trigger; every listed boss counts for it.
-- Checked against the dungeon finder (instance_encounters.lastEncounterDungeon) and the wings' maps on 2026-11-14.

DROP TABLE IF EXISTS `ga_dungeon_wing_template`;
CREATE TABLE `ga_dungeon_wing_template` (
  `dungeon_id` INT UNSIGNED NOT NULL COMMENT 'LFGDungeons.dbc (Readiness.h)',
  `boss_entry` INT UNSIGNED NOT NULL COMMENT 'a boss of this wing (creature entry)',
  `entrance_trigger` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'areatrigger_teleport.ID of its entrance; 0 = by the rule',
  `before_boss` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'a boss without an encounter comes before this boss; 0 = before the last',
  `comment` VARCHAR(255) NOT NULL DEFAULT '',
  PRIMARY KEY (`dungeon_id`, `boss_entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: bosses and entrance of a dungeon wing, by hand';

INSERT INTO `ga_dungeon_wing_template` (`dungeon_id`, `boss_entry`, `entrance_trigger`, `comment`) VALUES
(26, 13282, 3133, 'Maraudon, orange: Noxxion'),
(26, 12258, 3133, 'Maraudon, orange: Razorlash'),
(272, 12236, 3134, 'Maraudon, purple: Lord Vyletongue (the dungeon finder names no other boss of this wing)'),
(273, 12225, 3134, 'Maraudon, Pristine Waters (through the purple wing): Celebras the Cursed'),
(273, 12203, 3134, 'Maraudon, Pristine Waters: Landslide'),
(273, 13601, 3134, 'Maraudon, inner part: Tinkerer Gizlock'),
(273, 13596, 3134, 'Maraudon, inner part: Rotgrip'),
(273, 12201, 3134, 'Maraudon, inner part: Princess Theradras'),
(22, 6910, 0, 'Uldaman (no wing; a list without Ironaya, who stands behind a sealed door): Revelosh'),
(22, 6906, 0, 'Uldaman: Baelog (the Alliance leaves him alone)'),
(22, 7023, 0, 'Uldaman: Obsidian Sentinel'),
(22, 7206, 0, 'Uldaman: Ancient Stone Keeper'),
(22, 7291, 0, 'Uldaman: Galgann Firehammer'),
(22, 4854, 0, 'Uldaman: Grimlok'),
(22, 2748, 0, 'Uldaman: Archaedas'),
(30, 9025, 0, 'Blackrock Depths, prison (the dungeon finder ends it with Gerstahn): Lord Roccor'),
(30, 9319, 0, 'Blackrock Depths, prison: Houndmaster Grebmar'),
(30, 9018, 0, 'Blackrock Depths, prison: High Interrogator Gerstahn'),
(276, 9024, 0, 'Blackrock Depths, upper city (without the Seven and the bar, which are events): Pyromancer Loregrain'),
(276, 9017, 0, 'Blackrock Depths, upper city: Lord Incendius'),
(276, 9041, 0, 'Blackrock Depths, upper city: Warder Stilgiss'),
(276, 9056, 0, 'Blackrock Depths, upper city: Fineous Darkvire'),
(276, 9016, 0, 'Blackrock Depths, upper city: Bael''Gar'),
(276, 9033, 0, 'Blackrock Depths, upper city: General Angerforge'),
(276, 8983, 0, 'Blackrock Depths, upper city: Golem Lord Argelmach'),
(276, 9156, 0, 'Blackrock Depths, upper city: Ambassador Flamelash'),
(276, 9938, 0, 'Blackrock Depths, upper city: Magmus'),
(276, 9019, 0, 'Blackrock Depths, upper city: Emperor Dagran Thaurissan'),
(32, 9196, 0, 'Lower Blackrock Spire (the map holds the upper spire too): Highlord Omokk'),
(32, 9236, 0, 'Lower Blackrock Spire: Shadow Hunter Vosh''gajin'),
(32, 9237, 0, 'Lower Blackrock Spire: War Master Voone'),
(32, 10596, 0, 'Lower Blackrock Spire: Mother Smolderweb'),
(32, 9736, 0, 'Lower Blackrock Spire: Quartermaster Zigris'),
(32, 10220, 0, 'Lower Blackrock Spire: Halycon'),
(32, 9568, 0, 'Lower Blackrock Spire: Overlord Wyrmthalak'),
(44, 9816, 0, 'Upper Blackrock Spire: Pyroguard Emberseer'),
(44, 10429, 0, 'Upper Blackrock Spire: Warchief Rend Blackhand'),
(44, 10430, 0, 'Upper Blackrock Spire: The Beast'),
(44, 10363, 0, 'Upper Blackrock Spire: General Drakkisath'),
(28, 8580, 0, 'Sunken Temple (no wing; without Dreamscythe and Weaver, which fly rounds no way on foot reaches): Atal''alarion'),
(28, 5710, 0, 'Sunken Temple: Jammal''an the Prophet'),
(28, 5719, 0, 'Sunken Temple: Morphaz'),
(28, 5722, 0, 'Sunken Temple: Hazzas'),
(28, 5709, 0, 'Sunken Temple: Shade of Eranikus'),
(164, 4542, 0, 'Scarlet Monastery, Cathedral (Mograine has no encounter of his own): High Inquisitor Fairbanks'),
(164, 3976, 0, 'Scarlet Monastery, Cathedral: Scarlet Commander Mograine (first; Whitemane comes out when he falls)'),
(164, 3977, 0, 'Scarlet Monastery, Cathedral: High Inquisitor Whitemane'),
(34, 11490, 0, 'Dire Maul, East (Pusillin is a chase, no boss of the list): Zevrim Thornhoof'),
(34, 13280, 0, 'Dire Maul, East: Hydrospawn'),
(34, 14327, 0, 'Dire Maul, East: Lethtendris'),
(34, 11492, 0, 'Dire Maul, East: Alzzin the Wildshaper'),
(36, 11488, 0, 'Dire Maul, West: Illyanna Ravenoak'),
(36, 11487, 0, 'Dire Maul, West: Magister Kalendris'),
(36, 11489, 0, 'Dire Maul, West: Tendris Warpwood'),
(36, 11486, 0, 'Dire Maul, West: Prince Tortheldrin'),
(40, 10516, 2216, 'Stratholme, Main Gate (the living side; Balnazzar has no spawn: Grand Crusader Dathrohan turns into him): The Unforgiven'),
(40, 10558, 2216, 'Stratholme, Main Gate: Hearthsinger Forresten'),
(40, 10808, 2216, 'Stratholme, Main Gate: Timmy the Cruel'),
(40, 10997, 2216, 'Stratholme, Main Gate: Cannon Master Willey'),
(40, 11032, 2216, 'Stratholme, Main Gate: Malor the Zealous'),
(40, 10811, 2216, 'Stratholme, Main Gate: Archivist Galford'),
(40, 10812, 2216, 'Stratholme, Main Gate: Grand Crusader Dathrohan (Balnazzar)'),
(274, 10435, 0, 'Stratholme, Service Entrance (the undead side; Ramstein comes from the slaughterhouse event): Magistrate Barthilas'),
(274, 10436, 0, 'Stratholme, Service Entrance: Baroness Anastari'),
(274, 10437, 0, 'Stratholme, Service Entrance: Nerub''enkan'),
(274, 10438, 0, 'Stratholme, Service Entrance: Maleki the Pallid'),
(274, 10440, 0, 'Stratholme, Service Entrance: Baron Rivendare');

-- Bosses without an encounter of their own that come before a named boss (not before the last one).
INSERT INTO `ga_dungeon_wing_template` (`dungeon_id`, `boss_entry`, `entrance_trigger`, `before_boss`, `comment`) VALUES
(36, 11496, 0, 11486, 'Dire Maul, West: Immol''thar, behind the force field of the five pylons (ga_dungeon_event_template); Prince Tortheldrin turns hostile when he is dead'),
(28, 5712, 0, 5710, 'Sunken Temple: Zolo, a defender of the balcony (Jammal''an''s force field drops when all six are dead)'),
(28, 5713, 0, 5710, 'Sunken Temple: Gasher, a defender of the balcony'),
(28, 5714, 0, 5710, 'Sunken Temple: Loro, a defender of the balcony'),
(28, 5715, 0, 5710, 'Sunken Temple: Hukku, a defender of the balcony'),
(28, 5716, 0, 5710, 'Sunken Temple: Zul''Lor, a defender of the balcony'),
(28, 5717, 0, 5710, 'Sunken Temple: Mijan, a defender of the balcony');

-- Zul'Gurub: its bosses as they are, and the Vilebranch Speaker before Bloodlord Mandokir. The speaker's death starts
-- Mandokir (npc_vilebranch_speaker: boss state SPECIAL; he rides down 6 s later and puts the whole instance in his
-- fight), so the run gathers and gets ready before it, as before a boss: the owner's raid of 20 killed the speaker as a
-- creature on its way, scattered and low on mana, and Mandokir wiped it, 2026-11-26.
INSERT INTO `ga_dungeon_wing_template` (`dungeon_id`, `boss_entry`, `entrance_trigger`, `before_boss`, `comment`) VALUES
(42, 14507, 0, 0, 'Zul''Gurub: High Priest Venoxis'),
(42, 14517, 0, 0, 'Zul''Gurub: High Priestess Jeklik'),
(42, 14510, 0, 0, 'Zul''Gurub: High Priestess Mar''li'),
(42, 11382, 0, 0, 'Zul''Gurub: Bloodlord Mandokir'),
(42, 14509, 0, 0, 'Zul''Gurub: High Priest Thekal'),
(42, 11380, 0, 0, 'Zul''Gurub: Jin''do the Hexxer'),
(42, 14515, 0, 0, 'Zul''Gurub: High Priestess Arlokk (her gong: ga_dungeon_event_template)'),
(42, 14834, 0, 0, 'Zul''Gurub: Hakkar'),
(42, 11391, 0, 11382, 'Zul''Gurub: the Vilebranch Speaker, whose death starts Bloodlord Mandokir');
