-- mod-guild-adventure: where guild characters died, kept across restarts (GA_characters).
-- Applied by the worldserver's DB updater; every statement is idempotent. Design: docs/orders.md "Risk",
-- src/RiskModel.h.

-- One row per place (80 yards) a character died at. From the second death there within six hours the place is set
-- aside for the character, longer each time, until it is above the foes (level) or the time (set_aside_until) is over.
CREATE TABLE IF NOT EXISTS `ga_bot_death_place` (
    `bot_guid`        INT UNSIGNED      NOT NULL,
    `slot`            SMALLINT UNSIGNED NOT NULL COMMENT 'the character''s nth place',
    `map`             SMALLINT UNSIGNED NOT NULL,
    `x`               FLOAT             NOT NULL,
    `y`               FLOAT             NOT NULL,
    `level`           TINYINT UNSIGNED  NOT NULL DEFAULT 0 COMMENT 'set aside while the character is at most this level',
    `deaths`          SMALLINT UNSIGNED NOT NULL DEFAULT 1,
    `last_death`      INT UNSIGNED      NOT NULL DEFAULT 0 COMMENT 'unix time',
    `set_aside_until` INT UNSIGNED      NOT NULL DEFAULT 0 COMMENT 'unix time, 0 = not set aside',
    PRIMARY KEY (`bot_guid`, `slot`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: places where a guild character died';

-- What a character has learned from its own fights (FightEscape): what a fight against one foe of its level costs it,
-- how fast such a foe takes its health, and what its runs really cost against the estimate. Without the row a
-- character starts from the default of its level after every restart and is over-careful for its first fights.
CREATE TABLE IF NOT EXISTS `ga_bot_fight_stats` (
    `bot_guid`    INT UNSIGNED NOT NULL,
    `fight_cost`  FLOAT        NOT NULL DEFAULT 0.4 COMMENT 'share of its full strength (health plus half the mana)',
    `health_rate` FLOAT        NOT NULL DEFAULT 0.02 COMMENT 'share of its health per second',
    `run_factor`  FLOAT        NOT NULL DEFAULT 1,
    `fights`      INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'fights against one foe it has learned from',
    `updated_at`  INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'unix time',
    PRIMARY KEY (`bot_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: what a guild character learned from its fights';

-- The same per kind of creature: what a fight against one creature of that kind and the character's own level costs
-- it. A kind whose fights cost a character far more than its average (a caster it cannot kite, a creature that hits
-- hard) is judged by its own number, in the check before a pull and in the verdict about a spot.
CREATE TABLE IF NOT EXISTS `ga_bot_foe_cost` (
    `bot_guid`   INT UNSIGNED NOT NULL,
    `entry`      INT UNSIGNED NOT NULL COMMENT 'creature_template.entry',
    `cost`       FLOAT        NOT NULL DEFAULT 0 COMMENT 'share of its full strength, against one of its own level',
    `fights`     INT UNSIGNED NOT NULL DEFAULT 0,
    `updated_at` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'unix time',
    PRIMARY KEY (`bot_guid`, `entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: what a kind of creature costs a guild character';
