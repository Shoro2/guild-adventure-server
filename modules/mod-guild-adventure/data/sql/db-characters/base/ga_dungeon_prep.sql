-- mod-guild-adventure: what the guild leader set in the run window's preparation step for a dungeon group
-- (docs/orders.md "The dungeon run window", stage C). One row per group that has such choices; it goes with the
-- group's row in ga_dungeon_group.

CREATE TABLE IF NOT EXISTS `ga_dungeon_group_options` (
  `leader_guid` INT UNSIGNED NOT NULL COMMENT 'the bot that leads the group (ga_dungeon_group)',
  `items` TEXT NOT NULL COMMENT 'item entries the group may use out of the guild storage, comma separated',
  `fight` VARCHAR(64) NOT NULL DEFAULT '' COMMENT 'fight rules of the run: tank lead s, emergency health, careful pulls, pack, focus fleeing; empty = the server''s',
  PRIMARY KEY (`leader_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: the preparation step''s choices of a dungeon group';
