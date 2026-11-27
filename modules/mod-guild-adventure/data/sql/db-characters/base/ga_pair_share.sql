-- mod-guild-adventure: what two foes at once cost a guild character, kept across restarts (GA_characters).
-- Applied by the worldserver's DB updater; every statement is idempotent. Design: docs/orders.md "Round 252" (two foes
-- at once by class), src/RiskModel.h (Risk::PairShare), src/FightEscape.h (Escape::OthersShareOf).

-- One row per character that has fought exactly two ordinary foes at once: what the second foe cost it on top of two
-- single fights, as a share of the cheaper foe (a running average). The estimate of every guild character takes the mean
-- of its class from the characters with three such fights or more (0.5 while none has: two foes are 2.5 single fights).
CREATE TABLE IF NOT EXISTS `ga_bot_pair_share` (
    `bot_guid`   INT UNSIGNED      NOT NULL,
    `share`      FLOAT             NOT NULL DEFAULT 0.5 COMMENT 'the second foe as a share of the cheaper one, on top of two single fights',
    `fights`     SMALLINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'fights against two foes it has learned from',
    `updated_at` INT UNSIGNED      NOT NULL DEFAULT 0 COMMENT 'unix time',
    PRIMARY KEY (`bot_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: what two foes at once cost a guild character';
