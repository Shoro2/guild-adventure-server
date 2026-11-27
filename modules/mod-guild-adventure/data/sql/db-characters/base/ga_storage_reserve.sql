-- Reserves of the guild storage (docs/storage.md "Reserves", 2026-11-13): the bots leave `reserve` of a kind in the
-- storage; every automatic take keeps it, the owner's own Take, Sell and Disenchant do not. Written by
-- StorageService::SetReserve.
CREATE TABLE IF NOT EXISTS `ga_storage_reserve` (
    `owner_account` INT UNSIGNED NOT NULL,
    `item_entry` INT UNSIGNED NOT NULL,
    `random_property` INT NOT NULL DEFAULT 0,
    `reserve` INT UNSIGNED NOT NULL DEFAULT 0,
    `updated_at` INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`owner_account`, `item_entry`, `random_property`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-guild-adventure: reserves of the guild storage';
