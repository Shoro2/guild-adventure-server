-- mod-guild-adventure: where and on which quest the characters of a guild died (GA_characters).
-- Applied by the worldserver's DB updater; every statement is idempotent. Design: docs/orders.md "The guild's deaths",
-- src/OrderService.cpp (AddGuildDeath, OrderService::LoadGuildDeaths).

-- One row per death of an AI-played guild character; rows older than six hours are deleted at startup. The order
-- planner reads them for its zone pick (a zone that keeps killing the guild's characters of a level) and for the
-- quests it leaves to higher levels.
CREATE TABLE IF NOT EXISTS `ga_guild_death` (
    `id`            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `owner_account` INT UNSIGNED NOT NULL,
    `zone_id`       INT UNSIGNED NOT NULL DEFAULT 0,
    `quest_id`      INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'the quest it was working on, 0: none',
    `level`         TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'the level of the character that died',
    `died_at`       INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'unix time',
    PRIMARY KEY (`id`),
    KEY `idx_owner_time` (`owner_account`, `died_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: deaths of guild characters by zone and quest';
