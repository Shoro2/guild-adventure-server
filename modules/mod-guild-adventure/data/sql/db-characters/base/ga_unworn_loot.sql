-- Never-worn bind-on-pickup loot (the design log section 7 DL1, docs/storage.md "Soulbound loot"): a bind-on-pickup weapon or
-- armour piece (green or better) a guild character looted or took out of the guild storage and has not put on since.
-- Only such a soulbound piece may go into the guild storage. Written by StorageService::MarkUnworn / ForgetUnworn;
-- rows of items that no longer exist are deleted at startup.
CREATE TABLE IF NOT EXISTS `ga_unworn_loot` (
    `item_guid` INT UNSIGNED NOT NULL COMMENT 'item_instance.guid',
    `owner_account` INT UNSIGNED NOT NULL COMMENT 'the adventure guild it was marked for',
    `character_guid` INT UNSIGNED NOT NULL COMMENT 'the character that looted or took it',
    `item_entry` INT UNSIGNED NOT NULL,
    `marked_at` INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`item_guid`),
    KEY `idx_owner` (`owner_account`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-guild-adventure: never-worn bind-on-pickup loot';
