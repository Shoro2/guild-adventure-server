-- mod-guild-adventure: the dungeons a guild has finished (docs/orders.md "The dungeons in their order").
-- A run that brings the end boss down counts; the first one opens the next dungeon of the order for the guild.

CREATE TABLE IF NOT EXISTS `ga_guild_dungeon` (
  `owner_account` INT UNSIGNED NOT NULL,
  `dungeon_id` INT UNSIGNED NOT NULL COMMENT 'Readiness.h: the id of LFGDungeons.dbc',
  `runs_done` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'runs with the end boss down',
  `first_at` INT UNSIGNED NOT NULL DEFAULT 0,
  `last_at` INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`owner_account`, `dungeon_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: dungeons a guild has finished';
