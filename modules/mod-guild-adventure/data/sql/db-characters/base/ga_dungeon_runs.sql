-- mod-guild-adventure: how many runs a dungeon order still has (GA_characters).
-- Applied by the worldserver's DB updater; idempotent. Design: docs/orders.md "Dungeon runs", "Repeated runs".

-- A dungeon order can ask for several runs of the dungeon. No row: one run.
CREATE TABLE IF NOT EXISTS `ga_bot_dungeon_runs` (
    `bot_guid`  INT UNSIGNED     NOT NULL COMMENT 'the bot that has the order (the leader of the group)',
    `runs_left` TINYINT UNSIGNED NOT NULL DEFAULT 1 COMMENT 'runs still to do, the one that is going included',
    `runs_done` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `set_at`    INT UNSIGNED     NOT NULL DEFAULT 0,
    PRIMARY KEY (`bot_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: runs left of a dungeon order';
