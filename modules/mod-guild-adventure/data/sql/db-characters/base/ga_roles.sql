-- mod-guild-adventure: dungeon roles of the guild bots and their dungeon sets (GA_characters).
-- Applied by the worldserver's DB updater; every statement is idempotent. Design: docs/dungeon-roles.md.

-- The dungeon role the guild leader gave a bot. No row: none (the bot goes into a dungeon as it levels).
CREATE TABLE IF NOT EXISTS `ga_bot_role` (
    `bot_guid`    INT UNSIGNED     NOT NULL,
    `role`        TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 none, 1 tank, 2 healer',
    `template_id` INT UNSIGNED     NOT NULL DEFAULT 0 COMMENT '0 = the default talent template of the class for the role, else ga_spec_template.id (world) or ga_guild_spec_template.id',
    `worn`        TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '1 = the dungeon set is on and the second talent spec is active (a dungeon run)',
    `set_at`      INT UNSIGNED     NOT NULL DEFAULT 0,
    PRIMARY KEY (`bot_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: dungeon role per bot';

-- The role store of a bot: equipment outside its bags. One row per piece, with everything item_instance holds about
-- it, so the piece comes back as it went in (random property, enchantments, durability, soulbound).
--   kind 1: a piece of the dungeon set (collected while levelling; in the store while the bot wears its levelling set)
--   kind 2: a piece of the levelling set, put aside while the bot wears its dungeon set (`slot` = where it was worn)
CREATE TABLE IF NOT EXISTS `ga_bot_role_gear` (
    `id`              INT UNSIGNED      NOT NULL,
    `bot_guid`        INT UNSIGNED      NOT NULL,
    `kind`            TINYINT UNSIGNED  NOT NULL DEFAULT 1,
    `slot`            TINYINT UNSIGNED  NOT NULL DEFAULT 255 COMMENT 'equipment slot it was worn in, 255 = none',
    `item_entry`      INT UNSIGNED      NOT NULL,
    `random_property` INT               NOT NULL DEFAULT 0,
    `enchantments`    TEXT              NOT NULL COMMENT 'id duration charges per enchantment slot, as item_instance.enchantments',
    `charges`         VARCHAR(64)       NOT NULL DEFAULT '' COMMENT 'as item_instance.charges',
    `durability`      SMALLINT UNSIGNED NOT NULL DEFAULT 0,
    `flags`           INT UNSIGNED      NOT NULL DEFAULT 0 COMMENT 'ITEM_FIELD_FLAGS (soulbound)',
    `creator`         INT UNSIGNED      NOT NULL DEFAULT 0,
    `stored_at`       INT UNSIGNED      NOT NULL DEFAULT 0,
    PRIMARY KEY (`id`),
    KEY `idx_bot` (`bot_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: role store (dungeon set and put-aside levelling pieces) per bot';
