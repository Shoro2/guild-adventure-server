-- mod-guild-adventure: dungeon groups of the addon's run window (docs/orders.md "The dungeon run window").
-- The guild leader picks the bots of a run; for the run they are the squad of one of them. These rows say what they
-- were before (the leader's order, everybody's squad), so that it comes back when the order is over, also after a
-- restart of the server.

CREATE TABLE IF NOT EXISTS `ga_dungeon_group` (
  `leader_guid` INT UNSIGNED NOT NULL COMMENT 'the bot that leads the group (its order is DUNGEON)',
  `owner_account` INT UNSIGNED NOT NULL,
  `dungeon_id` INT UNSIGNED NOT NULL COMMENT 'Readiness.h: the id of LFGDungeons.dbc',
  `prev_order` VARCHAR(16) NOT NULL DEFAULT 'AUTO' COMMENT 'the order of the leader before the run',
  `prev_zone` INT UNSIGNED NOT NULL DEFAULT 0,
  `started_at` INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`leader_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: dungeon groups of the run window';

CREATE TABLE IF NOT EXISTS `ga_dungeon_group_member` (
  `leader_guid` INT UNSIGNED NOT NULL,
  `member_guid` INT UNSIGNED NOT NULL COMMENT 'a bot of the group, or a follower of one that stays outside',
  `prev_leader_guid` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'the squad leader it followed before (0 = none)',
  `in_group` TINYINT UNSIGNED NOT NULL DEFAULT 1,
  PRIMARY KEY (`leader_guid`, `member_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: what the bots of a dungeon group were before';
