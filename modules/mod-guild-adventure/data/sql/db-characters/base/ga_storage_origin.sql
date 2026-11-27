-- Weapons and armour a guild character took out of the guild storage (the design log section 7 WG1, docs/storage.md "Gear it
-- has worn"): such a piece is never disenchanted or sold by the worn-gear rule, whatever it became since (worn and
-- soulbound included). Written by StorageService::MarkFromStorage when the storage hands a piece out; rows of items that
-- no longer exist are deleted at startup.
CREATE TABLE IF NOT EXISTS `ga_storage_origin` (
    `item_guid` INT UNSIGNED NOT NULL COMMENT 'item_instance.guid',
    `owner_account` INT UNSIGNED NOT NULL COMMENT 'the adventure guild whose storage it came from',
    `character_guid` INT UNSIGNED NOT NULL COMMENT 'the character that took it',
    `item_entry` INT UNSIGNED NOT NULL,
    `taken_at` INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`item_guid`),
    KEY `idx_owner` (`owner_account`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-guild-adventure: gear that came out of the guild storage';
