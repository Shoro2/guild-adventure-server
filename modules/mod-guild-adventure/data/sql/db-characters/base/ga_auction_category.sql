-- Auction rules for whole storage categories (docs/storage.md section 8, 2026-11-13): every kind of the category without
-- a rule of its own is auctioned above keep_amount, at the default price. Written by AuctionService::SetCategoryRule.
CREATE TABLE IF NOT EXISTS `ga_auction_category_rule` (
    `owner_account` INT UNSIGNED NOT NULL,
    `category` VARCHAR(16) NOT NULL COMMENT 'StorageCategory: cloth, leather, metal, herb, meat, elemental, enchanting, gems, trade, recipes, consumables, weapons, armor, bags, other',
    `keep_amount` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'per kind of item',
    `stack_size` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 = up to 20',
    `enabled` TINYINT UNSIGNED NOT NULL DEFAULT 1,
    PRIMARY KEY (`owner_account`, `category`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-guild-adventure: auction rules per storage category';
