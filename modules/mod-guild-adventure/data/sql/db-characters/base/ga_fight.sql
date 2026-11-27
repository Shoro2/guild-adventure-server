-- mod-guild-adventure: fight profiles of the guilds (GA_characters).
-- Applied by the worldserver's DB updater; every statement is idempotent. Design: docs/fight-profiles.md section 4.4.

-- A fight profile of a guild: a class, the talent tree it is made for, two rule lists and its options.
CREATE TABLE IF NOT EXISTS `ga_fight_profile` (
    `id`            INT UNSIGNED     NOT NULL,
    `owner_account` INT UNSIGNED     NOT NULL,
    `class`         TINYINT UNSIGNED NOT NULL,
    `tab`           TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'the talent tree (0-2) it is made for',
    `name`          VARCHAR(48)      NOT NULL,
    `note`          VARCHAR(200)     NOT NULL DEFAULT '',
    `template_key`  VARCHAR(128)     NOT NULL DEFAULT '' COMMENT 'the mod-playerbots strategies its template came from',
    `revision`      INT UNSIGNED     NOT NULL DEFAULT 1,
    `created_at`    INT UNSIGNED     NOT NULL DEFAULT 0,
    `updated_at`    INT UNSIGNED     NOT NULL DEFAULT 0,
    PRIMARY KEY (`id`),
    KEY `idx_owner` (`owner_account`, `class`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: fight profiles of a guild';

-- The rules of a profile, list by list in their order.
CREATE TABLE IF NOT EXISTS `ga_fight_profile_rule` (
    `profile_id`    INT UNSIGNED      NOT NULL,
    `list`          TINYINT UNSIGNED  NOT NULL COMMENT '0 the fight, 1 between fights (buffs)',
    `position`      SMALLINT UNSIGNED NOT NULL,
    `enabled`       TINYINT UNSIGNED  NOT NULL DEFAULT 1,
    `kind`          TINYINT UNSIGNED  NOT NULL COMMENT '0 row of mod-playerbots, 1 its action under conditions, 2 spell, 3 item, 4 trinket slot',
    `trigger_name`  VARCHAR(64)       NOT NULL DEFAULT '' COMMENT 'a trigger of mod-playerbots (kinds 0 and 1)',
    `action_name`   VARCHAR(64)       NOT NULL DEFAULT '' COMMENT 'an action of mod-playerbots (kinds 0 and 1)',
    `ref`           INT UNSIGNED      NOT NULL DEFAULT 0 COMMENT 'spell id (first rank), item entry or trinket slot 1 / 2',
    `target`        TINYINT UNSIGNED  NOT NULL DEFAULT 0 COMMENT '0 current target, 1 self, 2 pet, 3 lowest member, 4 tank, 5 an enemy that meets the conditions',
    `flags`         TINYINT UNSIGNED  NOT NULL DEFAULT 0 COMMENT '1 once per target, 2 not twice in a row',
    `min_interval`  SMALLINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'seconds',
    `relevance`     FLOAT             NOT NULL DEFAULT 0,
    `node`          SMALLINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'rows of one trigger node of the template share it',
    `source`        VARCHAR(32)       NOT NULL DEFAULT '' COMMENT 'the strategy a row of the template came from',
    `conditions`    VARCHAR(255)      NOT NULL DEFAULT '' COMMENT '[!]key[:param][op value] joined by ;',
    PRIMARY KEY (`profile_id`, `list`, `position`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: rules of a fight profile';

-- Named values of a profile that conditions can refer to; a bot may have a value of its own (ga_bot_fight_option).
CREATE TABLE IF NOT EXISTS `ga_fight_profile_option` (
    `profile_id`    INT UNSIGNED     NOT NULL,
    `option_key`    VARCHAR(32)      NOT NULL,
    `label`         VARCHAR(64)      NOT NULL DEFAULT '',
    `kind`          TINYINT UNSIGNED NOT NULL DEFAULT 1 COMMENT '0 switch, 1 number, 2 percent',
    `value`         FLOAT            NOT NULL DEFAULT 0,
    `min_value`     FLOAT            NOT NULL DEFAULT 0,
    `max_value`     FLOAT            NOT NULL DEFAULT 100,
    PRIMARY KEY (`profile_id`, `option_key`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: options of a fight profile';

-- The profile of a bot per slot. No row: the default of mod-playerbots.
CREATE TABLE IF NOT EXISTS `ga_bot_fight_profile` (
    `bot_guid`      INT UNSIGNED     NOT NULL,
    `slot`          TINYINT UNSIGNED NOT NULL COMMENT '0 solo, 1 group (on a dungeon or raid map, or a member of a dungeon run group)',
    `profile_id`    INT UNSIGNED     NOT NULL,
    `set_at`        INT UNSIGNED     NOT NULL DEFAULT 0,
    PRIMARY KEY (`bot_guid`, `slot`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: fight profile per bot and slot';

-- A bot's own value of an option of a profile. No row: the profile's value.
CREATE TABLE IF NOT EXISTS `ga_bot_fight_option` (
    `bot_guid`      INT UNSIGNED     NOT NULL,
    `profile_id`    INT UNSIGNED     NOT NULL,
    `option_key`    VARCHAR(32)      NOT NULL,
    `value`         FLOAT            NOT NULL,
    PRIMARY KEY (`bot_guid`, `profile_id`, `option_key`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: a bot''s own values of fight profile options';
