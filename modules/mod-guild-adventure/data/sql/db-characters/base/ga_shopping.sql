-- The guild storage's shopping list (docs/storage.md "Shopping list", 2026-11-14): a minimum stock per item vendors
-- sell; a bot at a vendor it visits anyway buys what is missing, paid from the guild bank. Written by ShoppingList.
CREATE TABLE IF NOT EXISTS `ga_shopping_list` (
    `owner_account` INT UNSIGNED NOT NULL,
    `item_entry` INT UNSIGNED NOT NULL,
    `min_stock` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'the guild storage is filled up to this many',
    `enabled` TINYINT UNSIGNED NOT NULL DEFAULT 1,
    `updated_at` INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`owner_account`, `item_entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-guild-adventure: the guild storage shopping list';

-- What a guild spent on its shopping list today (Shopping.MaxPerDay), kept across restarts.
CREATE TABLE IF NOT EXISTS `ga_shopping_spent` (
    `owner_account` INT UNSIGNED NOT NULL,
    `day` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'days since 1970 (server time, UTC)',
    `spent` BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'copper',
    PRIMARY KEY (`owner_account`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-guild-adventure: copper spent on the shopping list today';
