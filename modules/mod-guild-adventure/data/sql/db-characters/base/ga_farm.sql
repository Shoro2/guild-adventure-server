-- mod-guild-adventure: farm orders (GA_characters). Applied by the worldserver's DB updater; idempotent.
-- Design: docs/orders.md "Farm orders" (Farming.h, OrderFarm.cpp).

-- The target stock of a bot's farm order: the order ends when the guild storage holds this many of its item.
-- The order itself is the bot's row in ga_bot_order (order_type FARM, zone_id = the item); a bot whose farm order
-- has no target stock has no row here.
CREATE TABLE IF NOT EXISTS `ga_bot_farm` (
    `bot_guid`     INT UNSIGNED NOT NULL,
    `target_stock` INT UNSIGNED NOT NULL DEFAULT 0,
    `set_at`       INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`bot_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: target stock of farm orders';
