-- mod-guild-adventure: characters database tables (GA_characters).
-- Applied by the worldserver's DB updater (module SQL, re-applied when the file changes),
-- so every statement must be idempotent. Design: the design log section 3.6.

-- One adventure guild per owner account (decision D1: designed per owner, single owner first).
CREATE TABLE IF NOT EXISTS `ga_guild` (
    `owner_account`      INT UNSIGNED      NOT NULL COMMENT 'auth.account.id of the player who owns the guild',
    `guild_id`           INT UNSIGNED      NOT NULL DEFAULT 0 COMMENT 'guild.guildid of the WoW guild, 0 until it exists',
    `leader_guid`        INT UNSIGNED      NOT NULL COMMENT 'characters.guid of the guild leader (the owner''s own character)',
    `team`               TINYINT UNSIGNED  NOT NULL COMMENT 'TeamId of the leader: 0 Alliance, 1 Horde (decision D9)',
    `unlocked_slots`     TINYINT UNSIGNED  NOT NULL DEFAULT 3,
    `renown_total`       INT UNSIGNED      NOT NULL DEFAULT 0 COMMENT 'renown ever earned',
    `renown_level`       TINYINT UNSIGNED  NOT NULL DEFAULT 1,
    `offline_window_min` SMALLINT UNSIGNED NOT NULL DEFAULT 60 COMMENT 'minutes the bots keep playing after the owner logged out',
    `owner_logout_at`    INT UNSIGNED      NOT NULL DEFAULT 0 COMMENT 'unix time the owner went offline, 0 while online',
    `created_at`         INT UNSIGNED      NOT NULL,
    PRIMARY KEY (`owner_account`),
    KEY `idx_guild` (`guild_id`),
    KEY `idx_leader` (`leader_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: adventure guilds';

-- Unlocks an owner has (ga_unlock_template rows that are owned: free ones on reaching the level, bought ones).
CREATE TABLE IF NOT EXISTS `ga_guild_unlock` (
    `owner_account` INT UNSIGNED NOT NULL,
    `unlock_id`     INT UNSIGNED NOT NULL COMMENT 'ga_unlock_template.id (world database)',
    `acquired_at`   INT UNSIGNED NOT NULL,
    PRIMARY KEY (`owner_account`, `unlock_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: owned unlocks';

-- Achievements are guild-wide (AchievementService): the first guild character (leader or bot) that earns one earns it
-- for the guild, once; the renown it gave is kept here. Every member also gets it in its own achievement list
-- (character_achievement) unless GuildAdventure.Achievements.Share = 0.
CREATE TABLE IF NOT EXISTS `ga_guild_achievement` (
    `owner_account`  INT UNSIGNED NOT NULL,
    `achievement_id` INT UNSIGNED NOT NULL COMMENT 'Achievement.dbc id',
    `earned_by`      INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'character guid of the member that earned it first',
    `earned_at`      INT UNSIGNED NOT NULL,
    `renown`         INT          NOT NULL DEFAULT 0 COMMENT 'renown it gave the guild',
    PRIMARY KEY (`owner_account`, `achievement_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: guild-wide achievements';

-- Module-owned accounts that hold the bot characters (decision D11): a pool per owner,
-- at most GuildAdventure.BotAccount.MaxCharacters bots per account. Players never log into them.
CREATE TABLE IF NOT EXISTS `ga_bot_account` (
    `account_id`    INT UNSIGNED     NOT NULL COMMENT 'auth.account.id',
    `owner_account` INT UNSIGNED     NOT NULL,
    `pool_index`    TINYINT UNSIGNED NOT NULL COMMENT '1-based position in the owner''s pool',
    `created_at`    INT UNSIGNED     NOT NULL,
    PRIMARY KEY (`account_id`),
    UNIQUE KEY `uk_owner_index` (`owner_account`, `pool_index`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: bot account pool';

-- One row per guild bot.
CREATE TABLE IF NOT EXISTS `ga_bot` (
    `bot_guid`      INT UNSIGNED     NOT NULL COMMENT 'characters.guid',
    `owner_account` INT UNSIGNED     NOT NULL,
    `bot_account`   INT UNSIGNED     NOT NULL COMMENT 'ga_bot_account.account_id the character lives on',
    `slot`          TINYINT UNSIGNED NOT NULL COMMENT 'roster slot 1..40',
    `state`         TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 active, 1 benched',
    `auto_advance`  TINYINT UNSIGNED NOT NULL DEFAULT 1 COMMENT 'move on to the next zone when out-levelled',
    `recruited_at`  INT UNSIGNED     NOT NULL,
    `kills`         INT UNSIGNED     NOT NULL DEFAULT 0 COMMENT 'lifetime counters since recruitment',
    `deaths`        INT UNSIGNED     NOT NULL DEFAULT 0,
    `quests`        INT UNSIGNED     NOT NULL DEFAULT 0,
    PRIMARY KEY (`bot_guid`),
    UNIQUE KEY `uk_owner_slot` (`owner_account`, `slot`),
    KEY `idx_bot_account` (`bot_account`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: guild bots';

-- The bot's order (queue_pos 0 = current; later positions are the order queue, the design log section 2.4).
CREATE TABLE IF NOT EXISTS `ga_bot_order` (
    `bot_guid`    INT UNSIGNED      NOT NULL,
    `queue_pos`   TINYINT UNSIGNED  NOT NULL DEFAULT 0,
    `order_type`  VARCHAR(16)       NOT NULL COMMENT 'protocol token: AUTO, IDLE, FOLLOW, LEVEL_ZONE, ...',
    `zone_id`     INT UNSIGNED      NOT NULL DEFAULT 0 COMMENT 'LEVEL_ZONE / GRIND / QUEST / GATHER',
    `level_cap`   TINYINT UNSIGNED  NOT NULL DEFAULT 0 COMMENT 'AUTO limit, 0 = none',
    `continent`   SMALLINT          NOT NULL DEFAULT -1 COMMENT 'AUTO limit, map id of the continent, -1 = anywhere',
    `no_dungeons` TINYINT UNSIGNED  NOT NULL DEFAULT 0 COMMENT 'AUTO limit',
    `gold_cap`    BIGINT UNSIGNED   NOT NULL DEFAULT 0 COMMENT 'AUTO limit, copper per trip, 0 = no limit',
    `set_at`      INT UNSIGNED      NOT NULL,
    PRIMARY KEY (`bot_guid`, `queue_pos`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: bot orders';

-- Planned professions: plan_slot 1/2 = primary professions, 0 = secondary.
CREATE TABLE IF NOT EXISTS `ga_bot_profession_plan` (
    `bot_guid`     INT UNSIGNED      NOT NULL,
    `skill_id`     SMALLINT UNSIGNED NOT NULL COMMENT 'SkillLine.dbc id',
    `plan_slot`    TINYINT UNSIGNED  NOT NULL COMMENT '1, 2 = primary; 0 = secondary',
    `target_skill` SMALLINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'target rank for primaries, 0 for secondaries',
    PRIMARY KEY (`bot_guid`, `skill_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: profession plans';

-- Event feed (protocol EVENT) and source of the "while you were away" summary.
CREATE TABLE IF NOT EXISTS `ga_event_log` (
    `id`            INT UNSIGNED     NOT NULL COMMENT 'increasing, assigned by the module',
    `owner_account` INT UNSIGNED     NOT NULL,
    `bot_guid`      INT UNSIGNED     NOT NULL DEFAULT 0 COMMENT '0 = guild event',
    `event_type`    VARCHAR(16)      NOT NULL COMMENT 'protocol token: LEVEL_UP, DEATH, QUEST_DONE, ...',
    `message`       VARCHAR(255)     NOT NULL DEFAULT '',
    `level`         TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `zone_id`       INT UNSIGNED     NOT NULL DEFAULT 0,
    `points`        INT              NOT NULL DEFAULT 0 COMMENT 'renown points of this event',
    `created_at`    INT UNSIGNED     NOT NULL,
    PRIMARY KEY (`id`),
    KEY `idx_owner_id` (`owner_account`, `id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: event log';

-- Quests the order planner failed on (docs/orders.md "Failure rules", the wowbot335 WAQ rule A21): the first
-- automatic failure abandons the quest and allows one retry, the second blocks it for this bot for good.
CREATE TABLE IF NOT EXISTS `ga_bot_quest_block` (
    `bot_guid`   INT UNSIGNED     NOT NULL,
    `quest_id`   INT UNSIGNED     NOT NULL,
    `failures`   TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `blocked`    TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '1 = never picked again',
    `reason`     VARCHAR(32)      NOT NULL DEFAULT '' COMMENT 'last failure: no_progress, cannot_turn_in, stuck, no_spot, ...',
    `updated_at` INT UNSIGNED     NOT NULL,
    PRIMARY KEY (`bot_guid`, `quest_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: quests the planner failed on';

-- The guild storage (StorageService, docs/storage.md): one unlimited store per adventure guild that every guild
-- character (leader and bots) uses. Looted items go there through the loot filter, crafting takes its reagents from
-- it (core patch CP-7). A row is one kind of item: its entry and its random property (the suffix of "of the Bear"
-- gear; 0 for everything else). Items with their own state (soulbound, conjured, timed, charges) never go in.
CREATE TABLE IF NOT EXISTS `ga_guild_storage` (
    `owner_account`   INT UNSIGNED     NOT NULL,
    `item_entry`      INT UNSIGNED     NOT NULL,
    `random_property` INT              NOT NULL DEFAULT 0 COMMENT 'ItemRandomPropertyId: >0 property, <0 suffix, 0 none',
    `amount`          INT UNSIGNED     NOT NULL,
    `item_class`      TINYINT UNSIGNED NOT NULL COMMENT 'item_template.class, for the categories of the addon',
    `item_subclass`   TINYINT UNSIGNED NOT NULL,
    `updated_at`      INT UNSIGNED     NOT NULL DEFAULT 0,
    PRIMARY KEY (`owner_account`, `item_entry`, `random_property`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: guild storage';

-- Loot filter rules per guild (LootService, docs/storage.md "Loot filter"; the model of mod-loot-filter without its
-- Paragon part): one action plus up to eight conditions in up to three groups, checked top to bottom.
CREATE TABLE IF NOT EXISTS `ga_loot_rule` (
    `rule_id`       INT UNSIGNED     NOT NULL COMMENT 'assigned by the module',
    `owner_account` INT UNSIGNED     NOT NULL,
    `position`      TINYINT UNSIGNED NOT NULL COMMENT '1-based evaluation order',
    `action`        TINYINT UNSIGNED NOT NULL COMMENT '0 keep, 1 sell, 2 disenchant, 3 delete, 4 to the storage',
    `enabled`       TINYINT UNSIGNED NOT NULL DEFAULT 1,
    `logic`         TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'bit g = group g matches on any condition, 16 = any group',
    PRIMARY KEY (`rule_id`),
    KEY `idx_owner_position` (`owner_account`, `position`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: loot filter rules';

CREATE TABLE IF NOT EXISTS `ga_loot_condition` (
    `rule_id` INT UNSIGNED     NOT NULL,
    `slot`    TINYINT UNSIGNED NOT NULL COMMENT '0-7',
    `grp`     TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'condition group 0-2',
    `type`    TINYINT UNSIGNED NOT NULL COMMENT '0 quality, 1 item level, 2 sell price, 3 item type, 6 item, 7 name',
    `op`      TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 is, 1 at least, 2 at most',
    `value`   INT UNSIGNED     NOT NULL DEFAULT 0,
    `value2`  INT UNSIGNED     NOT NULL DEFAULT 0 COMMENT 'item type: subclass, 255 = any',
    `text`    VARCHAR(40)      NOT NULL DEFAULT '',
    PRIMARY KEY (`rule_id`, `slot`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: loot filter conditions';

-- Loot settings and totals per guild. default_ai / default_player: what happens to loot no rule decides, for a
-- character the AI plays (a bot, the leader while the owner plays a bot, a watched character) and for the character
-- the owner plays himself (action numbers as in ga_loot_rule).
CREATE TABLE IF NOT EXISTS `ga_loot_settings` (
    `owner_account`      INT UNSIGNED     NOT NULL,
    `filter_enabled`     TINYINT UNSIGNED NOT NULL DEFAULT 1,
    `default_ai`         TINYINT UNSIGNED NOT NULL DEFAULT 4 COMMENT 'to the storage',
    `default_player`     TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'keep',
    `auto_loot`          TINYINT UNSIGNED NOT NULL DEFAULT 1 COMMENT 'guild characters loot, skin and gather within 10 yards',
    `total_sold`         BIGINT UNSIGNED  NOT NULL DEFAULT 0 COMMENT 'copper',
    `total_disenchanted` INT UNSIGNED     NOT NULL DEFAULT 0,
    `total_deleted`      INT UNSIGNED     NOT NULL DEFAULT 0,
    `total_stored`       INT UNSIGNED     NOT NULL DEFAULT 0,
    PRIMARY KEY (`owner_account`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: loot settings and totals';

-- Auction house surplus of the guild storage (AuctionService, docs/storage.md "Auction house"): per guild and item kind,
-- keep this many in the storage and auction the rest in stacks, posted by the guild leader.
CREATE TABLE IF NOT EXISTS `ga_auction_rule` (
    `owner_account`   INT UNSIGNED     NOT NULL,
    `item_entry`      INT UNSIGNED     NOT NULL,
    `random_property` INT              NOT NULL DEFAULT 0,
    `keep_amount`     INT UNSIGNED     NOT NULL DEFAULT 0 COMMENT 'stays in the storage',
    `stack_size`      INT UNSIGNED     NOT NULL DEFAULT 1 COMMENT 'items per auction',
    `price`           INT UNSIGNED     NOT NULL DEFAULT 0 COMMENT 'copper per item, 0 = the default price',
    `enabled`         TINYINT UNSIGNED NOT NULL DEFAULT 1,
    PRIMARY KEY (`owner_account`, `item_entry`, `random_property`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: auction rules of the guild storage';

-- The auctions AuctionService posted (the leader's own auctions are never touched): an unsold one comes back into the
-- guild storage when it expires.
CREATE TABLE IF NOT EXISTS `ga_auction_listing` (
    `auction_id`      INT UNSIGNED NOT NULL COMMENT 'auctionhouse.id',
    `owner_account`   INT UNSIGNED NOT NULL,
    `item_entry`      INT UNSIGNED NOT NULL,
    `random_property` INT          NOT NULL DEFAULT 0,
    `item_count`      INT UNSIGNED NOT NULL,
    `created_at`      INT UNSIGNED NOT NULL,
    PRIMARY KEY (`auction_id`),
    KEY `idx_owner` (`owner_account`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: auctions of the guild storage surplus';

-- Guild perks the guild bought (PerkService): the rank of each and the guild points it cost; the points left are the
-- achievement points of the guild's achievements minus the sum of `spent`.
CREATE TABLE IF NOT EXISTS `ga_guild_perk` (
    `owner_account` INT UNSIGNED     NOT NULL,
    `perk_id`       INT UNSIGNED     NOT NULL COMMENT 'ga_perk_template.id (world database)',
    `perk_rank`     TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `spent`         INT UNSIGNED     NOT NULL DEFAULT 0 COMMENT 'guild points paid for all its ranks',
    `updated_at`    INT UNSIGNED     NOT NULL,
    PRIMARY KEY (`owner_account`, `perk_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: guild perks bought';
