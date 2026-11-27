-- mod-guild-adventure: quest NPCs the characters of a guild could not get to (GA_characters).
-- Applied by the worldserver's DB updater; every statement is idempotent. Design: docs/orders.md "Quest NPCs out of
-- reach", src/OrderService.cpp (AddNpcFailure, OrderService::LoadGuildDeaths).

-- One row per NPC (by its spawn) and guild: how often guild characters gave a pickup or a hand-in there up for the
-- way within a day, and until when the whole guild leaves it alone. Rows older than a day are deleted at startup.
CREATE TABLE IF NOT EXISTS `ga_guild_npc_block` (
    `owner_account` INT UNSIGNED NOT NULL,
    `spawn_id`      INT UNSIGNED NOT NULL,
    `is_gameobject` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `fails`         TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `last_at`       INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'unix time of the last failure',
    `blocked_until` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'unix time, 0: not blocked',
    PRIMARY KEY (`owner_account`, `spawn_id`, `is_gameobject`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: quest NPCs out of reach for a guild';
