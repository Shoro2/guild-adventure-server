-- mod-guild-adventure: talent templates of the guilds and the template each bot plays by (GA_characters).
-- Applied by the worldserver's DB updater; every statement is idempotent. Design: docs/specs.md.

-- The template a guild bot plays by. No row: the default of its class (and, the first time the module sees the bot,
-- talents that do not fit that default are reset once).
CREATE TABLE IF NOT EXISTS `ga_bot_spec` (
    `bot_guid`      INT UNSIGNED     NOT NULL,
    `template_id`   INT UNSIGNED     NOT NULL DEFAULT 0 COMMENT '0 = default of the class, 1 = manual, else ga_spec_template.id (world) or ga_guild_spec_template.id',
    `reset_pending` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '1 = assigned while the bot was not in the world: reset and learn at its next login',
    `set_at`        INT UNSIGNED     NOT NULL DEFAULT 0,
    PRIMARY KEY (`bot_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: talent template per bot';

-- Templates a guild leader saved from the talents of a character he played. ids from 100000 (below: built-in).
CREATE TABLE IF NOT EXISTS `ga_guild_spec_template` (
    `id`            INT UNSIGNED     NOT NULL,
    `owner_account` INT UNSIGNED     NOT NULL,
    `class`         TINYINT UNSIGNED NOT NULL,
    `name`          VARCHAR(48)      NOT NULL,
    `purpose`       TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 leveling, 1 dungeon and raid, 2 raid, 3 PvP',
    `role`          TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `tab`           TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `points`        TEXT             NOT NULL COMMENT 'tab.row.column per talent point, in spending order',
    `created_at`    INT UNSIGNED     NOT NULL DEFAULT 0,
    PRIMARY KEY (`id`),
    KEY `idx_owner` (`owner_account`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: talent templates of a guild';
