-- mod-guild-adventure: squads of guild bots (GA_characters). Applied by the worldserver's DB updater; idempotent.
-- Design: docs/orders.md "Squads" (Squads.h).

-- A guild bot that follows another guild bot of its guild, its squad leader. The leader has no row of its own.
CREATE TABLE IF NOT EXISTS `ga_bot_squad` (
    `bot_guid`    INT UNSIGNED     NOT NULL,
    `leader_guid` INT UNSIGNED     NOT NULL,
    `set_at`      INT UNSIGNED     NOT NULL DEFAULT 0,
    `automatic`   TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '1 = formed by GuildAdventure.Squads.Auto, given up by itself when the bots part',
    PRIMARY KEY (`bot_guid`),
    KEY `idx_leader` (`leader_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: squad members and their leader';
