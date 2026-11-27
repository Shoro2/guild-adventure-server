-- mod-guild-adventure: tiered guild achievements (GA_characters). Applied by the worldserver's DB updater; idempotent.
-- MilestoneService, docs/guild-management.md "Tiered guild achievements"; the tiers are ga_milestone_template (GA_world).

-- The tiers a guild reached: once per guild and tier, with the guild points and renown it gave.
CREATE TABLE IF NOT EXISTS `ga_guild_milestone` (
    `owner_account` INT UNSIGNED NOT NULL,
    `milestone_id`  INT UNSIGNED NOT NULL COMMENT 'ga_milestone_template.id',
    `earned_by`     INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'character guid of the member that reached it (0 = the guild as a whole)',
    `earned_at`     INT UNSIGNED NOT NULL,
    `points`        INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'guild points it gave (perks)',
    `renown`        INT          NOT NULL DEFAULT 0 COMMENT 'renown it gave',
    PRIMARY KEY (`owner_account`, `milestone_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: tiered guild achievements a guild reached';

-- What the guild counted for its series: kills per creature type, quests, dungeon and raid runs (sums), and the
-- highest level per class and race and skill per profession any member reached (maxima). Storage items and gold are
-- read live and not kept here.
CREATE TABLE IF NOT EXISTS `ga_guild_counter` (
    `owner_account` INT UNSIGNED    NOT NULL,
    `counter_key`   VARCHAR(32)     NOT NULL COMMENT 'kind or kind_subject, e.g. kills_7, quests, level_class_1, skill_164',
    `value`         BIGINT UNSIGNED NOT NULL DEFAULT 0,
    `updated_at`    INT UNSIGNED    NOT NULL DEFAULT 0,
    PRIMARY KEY (`owner_account`, `counter_key`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: counters of the tiered guild achievements';
