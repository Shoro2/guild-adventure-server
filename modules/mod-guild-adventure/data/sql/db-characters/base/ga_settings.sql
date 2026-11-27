-- mod-guild-adventure: behaviour settings the guild leader changed (GA_characters).
-- Applied by the worldserver's DB updater; every statement is idempotent. Design: docs/bot-settings.md.
-- A setting without a row keeps the conf default (GuildAdventure.<key> in mod_guild_adventure.conf).

-- A guild's own values: they hold for every character of the guild that has no value of its own.
CREATE TABLE IF NOT EXISTS `ga_guild_setting` (
    `owner_account` INT UNSIGNED NOT NULL,
    `setting_key`   VARCHAR(64)  NOT NULL COMMENT 'the conf key without GuildAdventure., e.g. Rest.BelowHealth',
    `value`         INT          NOT NULL COMMENT 'bool 0/1, enum index, copper, percent, decimal x scale',
    `updated_at`    INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`owner_account`, `setting_key`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: behaviour settings of a guild';

-- One guild character's own values (the leader or a roster bot), for settings that allow it.
CREATE TABLE IF NOT EXISTS `ga_character_setting` (
    `character_guid` INT UNSIGNED NOT NULL,
    `owner_account`  INT UNSIGNED NOT NULL,
    `setting_key`    VARCHAR(64)  NOT NULL,
    `value`          INT          NOT NULL,
    `updated_at`     INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`character_guid`, `setting_key`),
    KEY `idx_owner` (`owner_account`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: behaviour settings of one guild character';
