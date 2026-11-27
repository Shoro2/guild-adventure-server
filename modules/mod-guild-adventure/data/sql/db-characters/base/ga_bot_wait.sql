-- mod-guild-adventure: what a guild character waits with, kept across a restart (GA_characters).
-- Applied by the worldserver's DB updater; every statement is idempotent. Design: docs/orders.md "Live round 42"
-- (round 82), src/OrderService.cpp (OrderService::LoadBotWaits, SaveBotWaits, RestoreBotWaits).

-- One row per wait: the hour a character stays on its side of the sea after it went ashore (kind 1; id = the quest
-- of the errand the crossing was for, whose second leg may follow, else 0), a zone that waits because the character
-- found no way into it or kept dying there (kind 2, id = the zone), a hand-in outside its zone it gave up (kind 3,
-- id = the quest). Rows whose time is over are deleted at startup.
CREATE TABLE IF NOT EXISTS `ga_bot_wait` (
    `bot_guid`   INT UNSIGNED NOT NULL,
    `kind`       TINYINT UNSIGNED NOT NULL,
    `id`         INT UNSIGNED NOT NULL DEFAULT 0,
    `until_time` INT UNSIGNED NOT NULL COMMENT 'unix time the wait ends',
    PRIMARY KEY (`bot_guid`, `kind`, `id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: waits of a character that survive a restart';
