-- mod-guild-adventure: hunters that got their good pet (GA_characters). Applied by the worldserver's DB updater; idempotent.
-- A hunter gets a good pet once (ga_class_ability.pet_kind 1 or 2, docs/orders.md "Class abilities"): the row keeps the
-- module from replacing a pet the guild leader chose afterwards.
CREATE TABLE IF NOT EXISTS `ga_bot_pet_upgrade` (
    `bot_guid`    INT UNSIGNED NOT NULL,
    `creature_id` INT UNSIGNED NOT NULL COMMENT 'creature_template.entry the pet was made from',
    `given_at`    INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`bot_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: the good pet a hunter got';
