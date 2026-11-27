-- mod-guild-adventure: the guild's budget for vendor gear and quest items (GA_characters).
-- Applied by the worldserver's DB updater; every statement is idempotent. Design: docs/orders.md "Gear and the
-- guild's budget", src/GuildBudget.h.

-- What each guild character has spent of its allowance so far (GuildAdventure.Budget.BotAllowance).
CREATE TABLE IF NOT EXISTS `ga_bot_budget` (
    `bot_guid`   INT UNSIGNED NOT NULL,
    `spent`      INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'copper',
    `updated_at` INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`bot_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: budget spent per guild character';
