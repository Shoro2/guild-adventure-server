-- mod-guild-adventure: craft orders of a guild ("make <amount> x <item>", CraftOrders.h, docs/orders.md "Craft orders").
CREATE TABLE IF NOT EXISTS `ga_craft_order` (
  `id` INT UNSIGNED NOT NULL,
  `owner_account` INT UNSIGNED NOT NULL,
  `item_entry` INT UNSIGNED NOT NULL,
  `amount` INT UNSIGNED NOT NULL DEFAULT 1,
  `made` INT UNSIGNED NOT NULL DEFAULT 0,
  `created_at` INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`),
  KEY `idx_owner` (`owner_account`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
