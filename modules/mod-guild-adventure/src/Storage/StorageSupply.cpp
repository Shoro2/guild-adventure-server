/*
 * StorageSupply implementation. See StorageSupply.h.
 */

#include "BotSettings.h"
#include "StorageSupply.h"
#include "Ammunition.h"
#include "Bag.h"
#include "BotDriver.h"
#include "DungeonRoles.h"
#include "DungeonSupplies.h"
#include "GearUpkeep.h"
#include "GuildAdventureSettings.h"
#include "Item.h"
#include "Log.h"
#include "LootService.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StorageService.h"
#include "StringFormat.h"
#include "Timer.h"
#include <algorithm>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace GuildAdventure;

namespace
{
    // Spell categories of food and drink items (the same playerbots' eat and drink actions look for).
    constexpr uint32 FoodSpellCategory = 11;
    constexpr uint32 DrinkSpellCategory = 59;
    // Equipment: per place on the bot at most this many pieces of the storage get playerbots' rating per look, the
    // best by the bot's own stat weights first (a cap over all places let 40 armour pieces crowd out every weapon:
    // Retadin, 2026-11-12; until 2026-11-25 the highest item levels first, so 30 green cloth helms of item level 57
    // to 62 with random suffixes, most of them no use to a caster, kept an epic Eye of Flame of item level 55 from
    // every caster of the owner's guild, one of whom wore a helm of item level 32), and at most this many are taken per
    // look (one per place on the bot).
    constexpr std::size_t MaxGearCandidatesPerPlace = 4;
    constexpr uint32 MaxGearPerStep = 3;
    // The scores are kept per bot (they change with its level and talents, and a little with what it wears): at most
    // this many kinds of the storage are scored anew per look, the likeliest first (item level, then quality: an epic
    // counts 16 item levels more, a rare 8), the rest at the next looks; all anew every ScoreKeepMs.
    constexpr std::size_t MaxGearScoresPerLook = 400;
    constexpr uint32 ScoreKeepMs = 30 * MINUTE * IN_MILLISECONDS;

    using Kind = StorageSupply::Kind;
    constexpr char const* ActivityLog = "module.ga.activity";

    // What a consumable is worth as a piece of a kind for this character (0: it is not of that kind, or above the
    // character's level). Food and drink by the level they are made for, a potion by what it restores, a bandage by
    // the First Aid it asks for.
    uint32 WorthAs(Player const* bot, ItemTemplate const* proto, Kind kind)
    {
        if (!proto || proto->Class != ITEM_CLASS_CONSUMABLE || !proto->Spells[0].SpellId || proto->RequiredLevel > bot->GetLevel() ||
            proto->Area || proto->Map)
            return 0;
        switch (kind)
        {
            case Kind::Food:
            case Kind::Drink:
                if (proto->Spells[0].SpellCategory != (kind == Kind::Food ? FoodSpellCategory : DrinkSpellCategory))
                    return 0;
                return proto->RequiredLevel * 1000 + proto->ItemLevel + 1;
            case Kind::HealingPotion:
            case Kind::ManaPotion:
            {
                if (proto->SubClass != ITEM_SUBCLASS_POTION)
                    return 0;
                SpellInfo const* info = sSpellMgr->GetSpellInfo(proto->Spells[0].SpellId);
                // (Not a potion of one place: a battleground's or a dungeon's own. And none while potions cool down:
                // the piece would only lie in the bags.)
                if (!info || info->AreaGroupId > 0 || bot->HasSpellCooldown(info->Id))
                    return 0;
                for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                    if (kind == Kind::HealingPotion ? info->Effects[i].Effect == SPELL_EFFECT_HEAL :
                        (info->Effects[i].Effect == SPELL_EFFECT_ENERGIZE && info->Effects[i].MiscValue == POWER_MANA))
                        return uint32(std::max(1, info->Effects[i].BasePoints + 1 + info->Effects[i].DieSides / 2));
                return 0;
            }
            case Kind::Bandage:
                if (proto->SubClass != ITEM_SUBCLASS_BANDAGE)
                    return 0;
                return proto->RequiredSkillRank * 1000 + proto->ItemLevel + 1;
        }
        return 0;
    }

    // The best piece of a kind the bot carries and can use (nullptr: none).
    Item* BestInBags(Player* bot, Kind kind, uint32& worth)
    {
        Item* best = nullptr;
        worth = 0;
        auto consider = [&](Item* item)
        {
            if (!item)
                return;
            uint32 const value = WorthAs(bot, item->GetTemplate(), kind);
            if (value > worth && bot->CanUseItem(item) == EQUIP_ERR_OK)
            {
                best = item;
                worth = value;
            }
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            consider(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = bot->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    consider(bag->GetItemByPos(uint8(slot)));
        return best;
    }

    // The best kind of such a piece in the storage that the bot can use (nullptr: none).
    StorageItem const* BestInStorage(Player const* bot, std::vector<StorageItem> const& stock, Kind kind, uint32& worth)
    {
        StorageItem const* best = nullptr;
        worth = 0;
        for (StorageItem const& item : stock)
        {
            if (item.itemClass != ITEM_CLASS_CONSUMABLE || !item.amount || item.randomProperty)
                continue;
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item.entry);
            uint32 const value = WorthAs(bot, proto, kind);
            if (value > worth && bot->CanUseItem(proto) == EQUIP_ERR_OK)
            {
                best = &item;
                worth = value;
            }
        }
        return best;
    }

    // Whether the bot's settings let it take that kind out of the storage.
    bool MayTake(Settings const& settings, Kind kind)
    {
        if (!settings.storageEnable)
            return false;
        switch (kind)
        {
            case Kind::Food:
            case Kind::Drink: return settings.storageSupplyProvisions;
            case Kind::HealingPotion:
            case Kind::ManaPotion: return settings.storageSupplyPotions;
            case Kind::Bandage: return settings.storageSupplyBandages;
        }
        return false;
    }

    // Has(): its answers, kept for a few seconds (the planner asks several times per step, and the answer is a walk
    // through the whole storage). Any thread.
    struct HasAnswer
    {
        uint32 at = 0;
        bool has = false;
    };
    constexpr uint32 HasKeepMs = 10 * IN_MILLISECONDS;
    std::mutex hasLock;
    std::unordered_map<uint64, HasAnswer> hasAnswers;

    // The stat a character fights with: what a scroll should raise for it.
    uint32 MainStat(Player* bot)
    {
        bool const melee = BotDriver::FightsInMelee(bot);
        switch (bot->getClass())
        {
            case CLASS_WARRIOR:
            case CLASS_DEATH_KNIGHT: return STAT_STRENGTH;
            case CLASS_PALADIN: return melee ? STAT_STRENGTH : STAT_INTELLECT;
            case CLASS_ROGUE:
            case CLASS_HUNTER: return STAT_AGILITY;
            case CLASS_SHAMAN:
            case CLASS_DRUID: return melee ? STAT_AGILITY : STAT_INTELLECT;
            default: return STAT_INTELLECT;
        }
    }

    // A buff of at least that much on the stat is on the bot already (a buff a scroll does not add to: Arcane
    // Intellect and a Scroll of Intellect, a totem's Strength of Earth and a Scroll of Agility). Not what the
    // character has of itself for good (a passive aura of its own).
    bool HasStatBuff(Player const* bot, uint32 stat, int32 amount)
    {
        for (AuraEffect const* effect : bot->GetAuraEffectsByType(SPELL_AURA_MOD_STAT))
            if (effect->GetMiscValue() == int32(stat) && effect->GetAmount() >= amount &&
                !(effect->GetBase()->IsPassive() && effect->GetCasterGUID() == bot->GetGUID()))
                return true;
        return false;
    }

    // Something on the bot does not stack with the buff of a piece (the core's spell groups: a scroll of agility and
    // an elixir of agility, a totem's strength, a better scroll): the piece would do nothing, or it would take the
    // other effect off and the next look would put that one back (in the first live round 13 scrolls "did nothing"
    // and went back, and scrolls and elixirs of one stat took turns on a bot).
    bool StacksWith(SpellInfo const* a, SpellInfo const* b)
    {
        return a->Id == b->Id || sSpellMgr->CheckSpellGroupStackRules(a, b) == SPELL_GROUP_STACK_RULE_DEFAULT;
    }

    bool ConflictsWithAuras(Player const* bot, SpellInfo const* info)
    {
        for (auto const& [spellId, application] : bot->GetAppliedAuras())
            if (SpellInfo const* existing = application->GetBase()->GetSpellInfo(); existing && !StacksWith(info, existing))
                return true;
        return false;
    }

    // A scroll per bot at most once per the scroll's own duration: a death takes the buff off, and the next look
    // would use the next scroll (getMSTime() until which the bot uses none).
    std::unordered_map<ObjectGuid, uint32> scrollUntil;

    // A class whose own buff takes the scroll's place leaves the scroll to the others: its next cast would replace it
    // (a shaman used a Scroll of Agility, its Strength of Earth Totem took the scroll's effect off in the next fight,
    // and a minute later the next scroll "did nothing for it"). Strength of Earth Totem and Horn of Winter for
    // strength and agility, Arcane Intellect for intellect.
    bool OwnBuffReplaces(Player const* bot, uint32 stat)
    {
        constexpr uint32 StrengthOfEarthTotem = 8075;
        constexpr uint32 HornOfWinter = 57330;
        constexpr uint32 ArcaneIntellect = 1459;
        auto knows = [bot](uint32 first)
        {
            for (uint32 spell = first; spell; spell = sSpellMgr->GetNextSpellInChain(spell))
                if (bot->HasSpell(spell))
                    return true;
            return false;
        };
        if (stat == STAT_INTELLECT)
            return bot->getClass() == CLASS_MAGE && knows(ArcaneIntellect);
        return (bot->getClass() == CLASS_SHAMAN && knows(StrengthOfEarthTotem)) || (bot->getClass() == CLASS_DEATH_KNIGHT && knows(HornOfWinter));
    }

    // Whether the storage holds ammunition of that kind a character could shoot at its level (for a ranged weapon it
    // does not hold yet).
    bool AmmoInStock(Player const* bot, std::vector<StorageItem> const& stock, uint32 kind)
    {
        for (StorageItem const& item : stock)
        {
            if (item.itemClass != ITEM_CLASS_PROJECTILE || item.subClass != kind || !item.amount)
                continue;
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item.entry);
            if (proto && bot->CanUseItem(proto) == EQUIP_ERR_OK)
                return true;
        }
        return false;
    }

    // Ammunition out of the storage, for a hunter (the owner, 2026-11-13: [quote left out]): the best kind for the ranged weapon it holds that is no worse than what it
    // carries, up to its stock, when it carries less than half of it. The ammo slot follows. Other classes with a
    // bow buy their one stack.
    std::string TakeAmmo(Player* bot)
    {
        std::string taken;
        uint32 amount = 0;
        if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(Ammo::BestInStorage(bot, &amount)))
        {
            uint32 const have = Ammo::Count(bot);
            uint32 const wanted = Ammo::Stock(bot, proto);
            std::string error;
            if (have < wanted / 2)
                if (uint32 const count = sStorageService.Withdraw(bot, proto->ItemId, 0, std::min<uint32>(wanted - have, amount), error))
                {
                    taken = Acore::StringFormat("{} {}", count, proto->Name1);
                    Ammo::NoteTaken(bot, proto->ItemId, count); // it goes back there if the weapon changes
                }
        }
        // The ammo slot: the best it carries for the weapon in its hands (it kept arrows in the slot for a gun).
        Ammo::UseBest(bot);
        return taken;
    }

    // What a bot knows about the storage's equipment: the storage it last rated (kinds and items), at which level, and
    // the kinds it took and could not put on. A first failure is tried again at the next look (playerbots refuses to
    // change a weapon right after a fight or while a cast runs: a hunter put back a better gun in the second its fight
    // ended, 2026-11-12); the second leaves the piece alone for a few minutes, the third until the next level (a second
    // weapon without dual wield never fits).
    struct GearFailure
    {
        uint8 count = 0;
        uint32 since = 0;   // getMSTime() of the last failure; after the first one the piece waits GearRetryMs
        uint8 level = 0;    // the bot's level at the last failure
    };
    struct GearMemory
    {
        uint64 seen = 0;
        uint8 level = 0;
        std::unordered_map<uint32, GearFailure> failed;
        // playerbots' score of each kind of the storage for this bot (entry << 32 | random property), with the talent
        // tree and the time they were made for.
        std::unordered_map<uint64, float> scores;
        uint8 scoreTab = 255;
        uint32 scoredAt = 0;
    };
    constexpr uint32 GearRetryMs = 10 * MINUTE * IN_MILLISECONDS;

    // After one failure the piece waits GearRetryMs, after the second until the bot is GearRetryLevels levels on
    // (it was: twice in a row, a third time ten minutes later, and all of that again at every level: one hunter took
    // a Forester's Axe out and put it back 14 times, and a never-worn bound blade went out and in at every look).
    constexpr uint8 GearRetryLevels = 3;
    bool IsDeclined(GearMemory const& memory, uint32 entry, uint32 now)
    {
        auto itr = memory.failed.find(entry);
        if (itr == memory.failed.end() || !itr->second.count)
            return false;
        return itr->second.count >= 2 || getMSTimeDiff(itr->second.since, now) < GearRetryMs;
    }
    std::unordered_map<ObjectGuid, GearMemory> gearMemory;

    // The first piece of a kind in the bags that is not one of `except`.
    Item* FindInBags(Player* player, uint32 entry, std::set<ObjectGuid> const* except = nullptr)
    {
        auto fits = [&](Item* item) { return item && item->GetEntry() == entry && (!except || !except->count(item->GetGUID())); };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot); fits(item))
                return item;
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = player->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    if (Item* item = bag->GetItemByPos(uint8(slot)); fits(item))
                        return item;
        return nullptr;
    }

    // Puts this very piece on. mod-playerbots' equipping goes by the item's entry and takes the first copy of that kind
    // in the bags: with an older copy there (a worn one, soulbound, which nothing sells: a unique-equipped ring, a
    // piece that came out of the guild storage) it looked at that one, found it no better than what the bot wears,
    // and the upgrade stayed in the bags (round 234: Retadin, Hunterbm, Lilwar and Roguecombat took an Aquamarine
    // Signet out of the storage beside an older one in their bags and put it back; the owner: [quote left out]). A ring or trinket goes to the bare place, else
    // in place of the worn one that scores less (as mod-playerbots chooses). Only for a piece IsUpgrade calls one.
    bool PutOnThisOne(Player* bot, Item* item)
    {
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        if (!proto || item->IsEquipped() || !bot->IsInventoryPos(item->GetPos()))
            return false;
        uint8 slot = bot->FindEquipSlot(proto, NULL_SLOT, true);
        if (proto->InventoryType == INVTYPE_FINGER || proto->InventoryType == INVTYPE_TRINKET)
        {
            uint8 const first = proto->InventoryType == INVTYPE_FINGER ? EQUIPMENT_SLOT_FINGER1 : EQUIPMENT_SLOT_TRINKET1;
            uint8 const second = first + 1;
            Item const* a = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, first);
            Item const* b = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, second);
            if (!a)
                slot = first;
            else if (!b)
                slot = second;
            else
                slot = BotDriver::ItemScore(bot, a->GetEntry(), a->GetItemRandomPropertyId()) <=
                    BotDriver::ItemScore(bot, b->GetEntry(), b->GetItemRandomPropertyId()) ? first : second;
        }
        else if (proto->InventoryType == INVTYPE_WEAPON && bot->CanDualWield())
        {
            // A one-hand weapon of a bot that holds two goes in place of the one that scores less.
            Item const* main = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
            Item const* off = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
            if (main && off && off->GetTemplate()->Class == ITEM_CLASS_WEAPON &&
                main->GetTemplate()->InventoryType != INVTYPE_2HWEAPON &&
                BotDriver::ItemScore(bot, off->GetEntry(), off->GetItemRandomPropertyId()) <
                    BotDriver::ItemScore(bot, main->GetEntry(), main->GetItemRandomPropertyId()))
                slot = EQUIPMENT_SLOT_OFFHAND;
        }
        if (slot >= EQUIPMENT_SLOT_END)
            return false;
        // Not in place of what scores as much or more there: IsUpgrade compares a piece with the bot's weakest place
        // of its kind, and the place it goes to here may hold a copy of it or a better piece. Mojowind (50, shaman)
        // put one of its five Franciscas (main hand only) in place of the Francisca it wore every minute, 573 times in
        // a day, and in place of an Axe of Rin'ji (53) it had just taken out of the guild storage; Zulkash 144 times
        // (round 257).
        if (Item const* worn = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            if ((worn->GetEntry() == item->GetEntry() && worn->GetItemRandomPropertyId() == item->GetItemRandomPropertyId()) ||
                BotDriver::ItemScore(bot, worn->GetEntry(), worn->GetItemRandomPropertyId()) >=
                    BotDriver::ItemScore(bot, item->GetEntry(), item->GetItemRandomPropertyId()))
                return false;
        uint16 dest = 0;
        if (bot->CanEquipItem(slot, dest, item, true) != EQUIP_ERR_OK)
            return false;
        bot->SwapItem(item->GetPos(), dest);
        return bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot) == item;
    }

    // An upgrade in its bags beside another copy of its kind is put on by the module (PutOnThisOne): loot that
    // mod-playerbots' equipping never reaches for the older copy before it. One piece per kind and look.
    std::string WearBagDuplicates(Player* bot)
    {
        std::unordered_map<uint32, std::vector<Item*>> byEntry;
        auto add = [&](Item* item)
        {
            ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
            if (proto && (proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR) && Gear::SlotGroup(proto))
                byEntry[proto->ItemId].push_back(item);
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            add(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = bot->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    add(bag->GetItemByPos(uint8(slot)));
        std::string on;
        for (auto const& [entry, items] : byEntry)
        {
            if (items.size() < 2)
                continue;
            Item* best = nullptr;
            float bestScore = 0.0f;
            for (Item* item : items)
                if (BotDriver::IsUpgrade(bot, entry, item->GetItemRandomPropertyId()))
                    if (float const score = BotDriver::ItemScore(bot, entry, item->GetItemRandomPropertyId()); !best || score > bestScore)
                    {
                        best = item;
                        bestScore = score;
                    }
            if (best && PutOnThisOne(bot, best))
                on += (on.empty() ? "" : ", ") + best->GetTemplate()->Name1;
        }
        return on;
    }

    std::string TakeGear(Player* bot, uint32 owner, std::vector<StorageItem> const& stock)
    {
        // Armour cannot be changed in a fight, a weapon not while a cast runs (an auto shot too): the next look.
        if (bot->IsInCombat() || bot->IsNonMeleeSpellCast(false) || bot->HasUnitState(UNIT_STATE_CASTING))
            return "";
        if (std::string const wore = WearBagDuplicates(bot); !wore.empty())
            LOG_INFO(ActivityLog, "activity: {} puts on what it carried beside an older one of its kind: {}", bot->GetName(), wore);

        GearMemory& memory = gearMemory[bot->GetGUID()];
        if (memory.level != bot->GetLevel())
        {
            memory.level = bot->GetLevel();
            // A piece it could not put on twice is tried again GearRetryLevels levels later (a skill it has by then).
            std::erase_if(memory.failed, [&](auto const& entry)
            {
                return entry.second.count < 2 || bot->GetLevel() >= entry.second.level + GearRetryLevels;
            });
            memory.seen = 0;
            memory.scores.clear();
        }
        uint32 const now = std::max<uint32>(getMSTime(), 1);
        if (uint8 const tab = BotDriver::SpecTab(bot);
            tab != memory.scoreTab || !memory.scoredAt || getMSTimeDiff(memory.scoredAt, now) >= ScoreKeepMs)
        {
            memory.scores.clear();
            memory.scoreTab = tab;
            memory.scoredAt = now;
            memory.seen = 0;
        }
        std::pair<uint32, uint64> const totals = sStorageService.GetTotals(owner);
        uint64 const fingerprint = ((uint64(totals.first) << 40) ^ totals.second) | 1;
        if (memory.seen == fingerprint)
            return "";
        memory.seen = fingerprint;

        struct Candidate
        {
            uint32 itemLevel;
            uint32 group;
            StorageItem const* item;
            ItemTemplate const* proto;
            float score = 0.0f;
        };
        std::vector<Candidate> candidates;
        for (StorageItem const& item : stock)
        {
            if ((item.itemClass != ITEM_CLASS_WEAPON && item.itemClass != ITEM_CLASS_ARMOR) || !item.amount ||
                IsDeclined(memory, item.entry, getMSTime()))
                continue;
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item.entry);
            if (!proto || proto->RequiredLevel > bot->GetLevel() || bot->CanUseItem(proto) != EQUIP_ERR_OK)
                continue;
            // A hunter's bow or gun is no use without what it shoots: only when the bot carries that ammunition or
            // the storage holds some (a level-2 hunter with arrows was handed a Pellet Rifle and could not shoot for
            // half an hour).
            // (What it carries for another kind of weapon is sold when it changes: a weapon of the kind it has
            // ammunition for, or one the storage has ammunition for.)
            if (bot->getClass() == CLASS_HUNTER)
                if (uint32 const kind = Ammo::KindOf(proto); kind && (kind != Ammo::KindOf(bot) || !Ammo::Count(bot)) &&
                    !AmmoInStock(bot, stock, kind))
                    continue;
            // No shield or off-hand item for a bot that fights with a weapon in each hand: mod-playerbots' upgrade
            // check rates it above the off-hand weapon, its equipping puts it on, and its next comparison takes the
            // weapon back, which leaves the piece in the bags bound to it. A shaman that dual-wields had a Heroic
            // Guard of the guild storage so, and it could not go back (round 178).
            if (proto->InventoryType == INVTYPE_SHIELD || proto->InventoryType == INVTYPE_HOLDABLE)
                if (Item const* off = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
                    off && off->GetTemplate()->Class == ITEM_CLASS_WEAPON)
                    continue;
            // Pieces that compete for the same place on the bot (0: nothing a bot wears for its fights).
            if (uint32 const group = Gear::SlotGroup(proto))
                candidates.push_back({ proto->ItemLevel, group, &item, proto });
        }

        // playerbots' score for this bot of every candidate (kept from earlier looks; the likeliest first when there
        // are more new kinds than one look scores). A candidate not scored yet waits for the next look.
        auto scoreKey = [](StorageItem const* item) { return (uint64(item->entry) << 32) | uint32(item->randomProperty); };
        auto likeliness = [](ItemTemplate const* proto)
        {
            return proto->ItemLevel + (proto->Quality >= ITEM_QUALITY_EPIC ? 16u : proto->Quality == ITEM_QUALITY_RARE ? 8u : 0u);
        };
        {
            std::vector<Candidate*> unscored;
            for (Candidate& candidate : candidates)
                if (auto it = memory.scores.find(scoreKey(candidate.item)); it != memory.scores.end())
                    candidate.score = it->second;
                else
                    unscored.push_back(&candidate);
            if (!unscored.empty())
            {
                std::sort(unscored.begin(), unscored.end(), [&](Candidate const* a, Candidate const* b)
                {
                    return likeliness(a->proto) > likeliness(b->proto);
                });
                if (unscored.size() > MaxGearScoresPerLook)
                {
                    unscored.resize(MaxGearScoresPerLook);
                    memory.seen = 0;
                }
                std::vector<std::pair<uint32, int32>> kinds;
                kinds.reserve(unscored.size());
                for (Candidate const* candidate : unscored)
                    kinds.push_back({ candidate->item->entry, candidate->item->randomProperty });
                std::vector<float> scores;
                BotDriver::ItemScores(bot, kinds, scores);
                for (std::size_t i = 0; i < unscored.size(); ++i)
                {
                    unscored[i]->score = scores[i];
                    memory.scores[scoreKey(unscored[i]->item)] = scores[i];
                }
            }
            std::erase_if(candidates, [&](Candidate const& candidate) { return !memory.scores.count(scoreKey(candidate.item)); });
        }
        std::sort(candidates.begin(), candidates.end(), [](Candidate const& a, Candidate const& b)
        {
            if (a.score != b.score)
                return a.score > b.score;
            return a.itemLevel > b.itemLevel || (a.itemLevel == b.itemLevel && a.item->entry < b.item->entry);
        });
        {
            std::unordered_map<uint32, std::size_t> perPlace;
            std::vector<Candidate> kept;
            for (Candidate const& candidate : candidates)
                if (++perPlace[candidate.group] <= MaxGearCandidatesPerPlace)
                    kept.push_back(candidate);
            candidates.swap(kept);
        }

        // The best upgrade per place (playerbots' score for this bot's class and spec); weapons first.
        std::map<uint32, std::pair<float, Candidate const*>, std::greater<uint32>> best;
        for (Candidate const& candidate : candidates)
        {
            if (!BotDriver::IsUpgrade(bot, candidate.item->entry, candidate.item->randomProperty))
                continue;
            float const score = candidate.score;
            auto [it, isNew] = best.try_emplace(candidate.group, score, &candidate);
            if (!isNew && score > it->second.first)
                it->second = { score, &candidate };
        }

        // (Each piece by its own instance: the bags may hold another of that kind already.)
        std::vector<std::pair<Candidate const*, ObjectGuid>> withdrawn;
        for (auto const& [group, pick] : best)
        {
            if (withdrawn.size() >= MaxGearPerStep)
                break;
            std::set<ObjectGuid> before;
            for (Item* item = FindInBags(bot, pick.second->item->entry, nullptr); item; item = FindInBags(bot, pick.second->item->entry, &before))
                before.insert(item->GetGUID());
            std::string error;
            if (sStorageService.Withdraw(bot, pick.second->item->entry, pick.second->item->randomProperty, 1, error))
            {
                Item* const fresh = FindInBags(bot, pick.second->item->entry, &before);
                withdrawn.push_back({ pick.second, fresh ? fresh->GetGUID() : ObjectGuid::Empty });
            }
        }
        if (withdrawn.empty())
            return "";

        BotDriver::EquipUpgrades(bot);
        // A piece mod-playerbots did not reach for an older copy of its kind in the bags: this one on (PutOnThisOne).
        for (auto const& [candidate, guid] : withdrawn)
            if (Item* fresh = guid.IsEmpty() ? nullptr : bot->GetItemByGuid(guid); fresh && !fresh->IsEquipped())
            {
                std::set<ObjectGuid> const self{ guid };
                if (FindInBags(bot, candidate->item->entry, &self))
                    PutOnThisOne(bot, fresh);
            }
        // Jewellery for a bare place is put on by the module (mod-playerbots does not: BotDriver::FillsEmptyPlace).
        Gear::FillEmptySlots(bot);
        // The ammunition for a hunter's new ranged weapon comes with it, once it holds the weapon (a weapon it could
        // not put on goes back). What it carries for the weapon it held before goes first: back into the storage
        // where it came from there or was made, else sold (Ammo::Trim).
        if (std::string const sold = Ammo::Trim(bot); !sold.empty())
            LOG_INFO("module.ga.activity", "activity: {} sold ammunition at vendor price: {}", bot->GetName(), sold);
        std::string const ammoTaken = TakeAmmo(bot);

        // What it could not put on goes back at once and is left alone until the next level.
        std::string taken;
        std::string returned;
        std::string kept;
        for (auto const& [candidate, guid] : withdrawn)
        {
            Item* left = guid.IsEmpty() ? nullptr : bot->GetItemByGuid(guid);
            if (left && (left->IsEquipped() || !bot->IsInventoryPos(left->GetPos())))
                left = nullptr;
            if (!left)
            {
                taken += (taken.empty() ? "" : ", ") + candidate->proto->Name1;
                continue;
            }
            GearFailure& failure = memory.failed[candidate->item->entry];
            ++failure.count;
            failure.since = getMSTime();
            failure.level = bot->GetLevel();
            // (Bound by being put on for a moment and taken off again in the same step: it goes back all the same.)
            std::string error;
            if (sStorageService.DepositItem(bot, left, error, false, true))
                returned += (returned.empty() ? "" : ", ") + candidate->proto->Name1;
            else
                kept += (kept.empty() ? "" : ", ") + Acore::StringFormat("{} ({})", candidate->proto->Name1, error);
        }
        // The pieces it wore before go through the guild's loot filter like loot (into the storage, as a rule).
        if (!taken.empty())
            sLootService.SweepBags(bot);
        // The storage changed by this: look again next time (the next place, a second weapon, or nothing).
        memory.seen = 0;

        if (!ammoTaken.empty())
            taken += (taken.empty() ? "" : ", ") + ammoTaken;
        if (!returned.empty())
            taken += (taken.empty() ? "" : "; ") + ("could not wear and put back: " + returned);
        if (!kept.empty())
            taken += (taken.empty() ? "" : "; ") + ("could not wear and could not put back, it stays in its bags: " + kept);
        return taken;
    }
}

bool StorageSupply::Conjures(Player const* bot, Kind kind)
{
    constexpr uint32 ConjureFood = 587;
    constexpr uint32 ConjureWater = 5504;
    if (!bot || bot->getClass() != CLASS_MAGE || (kind != Kind::Food && kind != Kind::Drink))
        return false;
    for (uint32 spell = kind == Kind::Drink ? ConjureWater : ConjureFood; spell; spell = sSpellMgr->GetNextSpellInChain(spell))
        if (bot->HasSpell(spell))
            return true;
    return false;
}

bool StorageSupply::Has(Player* bot, Kind kind)
{
    if (!bot || Conjures(bot, kind))
        return false;
    uint64 const key = (uint64(bot->GetGUID().GetCounter()) << 8) | uint64(kind);
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    {
        std::lock_guard<std::mutex> lock(hasLock);
        auto itr = hasAnswers.find(key);
        if (itr != hasAnswers.end() && getMSTimeDiff(itr->second.at, now) < HasKeepMs)
            return itr->second.has;
    }
    bool has = false;
    if (MayTake(sBotSettings.For(bot), kind))
        if (uint32 const owner = sStorageService.GetOwnerOf(bot))
        {
            uint32 worth = 0;
            has = BestInStorage(bot, sStorageService.List(owner), kind, worth) != nullptr;
        }
    std::lock_guard<std::mutex> lock(hasLock);
    hasAnswers[key] = { now, has };
    return has;
}

bool StorageSupply::HasDrink(Player* bot)
{
    return Has(bot, Kind::Drink);
}

Item* StorageSupply::TakeOne(Player* bot, Kind kind, char const* purpose)
{
    if (!bot)
        return nullptr;
    uint32 carriedWorth = 0;
    Item* const carried = BestInBags(bot, kind, carriedWorth);
    // (A mage takes no water or food it can conjure: one took 37 of the 40 bottles a priest of its guild was waiting for.)
    if (Conjures(bot, kind) || !MayTake(sBotSettings.For(bot), kind))
        return carried;
    uint32 const owner = sStorageService.GetOwnerOf(bot);
    if (!owner)
        return carried;
    std::vector<StorageItem> const stock = sStorageService.List(owner);
    uint32 storedWorth = 0;
    StorageItem const* const best = BestInStorage(bot, stock, kind, storedWorth);
    // Food and drink: what it carries is used up first, whatever its kind (the stacks bots bought at vendors before
    // they took single pieces). A potion or a bandage: the better of the two, its own when they are equal.
    // (In a dungeon or raid the whole group waits for each rest: there a kind 20 levels better out of the storage comes
    // first. Raid healers of level 60 drank level-1 water and sat at 0 to 11 % mana after three minutes of rest,
    // Molten Core, 2026-11-21.)
    bool const groupWaits = bot->FindMap() && bot->GetMap()->IsDungeon() && storedWorth >= carriedWorth + 20 * 1000;
    if (!best || storedWorth <= carriedWorth || (carried && (kind == Kind::Food || kind == Kind::Drink) && !groupWaits))
        return carried;
    std::string error;
    if (!sStorageService.Withdraw(bot, best->entry, 0, 1, error))
        return carried;
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(best->entry);
    LOG_INFO(ActivityLog, "activity: {} took 1 x {} out of the guild storage {} ({} left there)", bot->GetName(),
        proto ? proto->Name1 : "?", purpose ? purpose : "to use it", sStorageService.GetAmount(owner, best->entry, 0));
    // The piece itself (it may have gone onto a stack the bot carried).
    Item* const taken = bot->GetItemByEntry(best->entry);
    return taken ? taken : carried;
}

std::string StorageSupply::UseBuffs(Player* bot)
{
    if (!bot || !bot->IsAlive() || bot->IsInCombat() || bot->IsMounted() || !bot->FindMap() || bot->GetMap()->IsDungeon())
        return "";
    Settings const settings = sBotSettings.For(bot);
    if (!settings.storageEnable || (!settings.storageSupplyScrolls && !settings.storageSupplyElixirs))
        return "";
    uint32 const owner = sStorageService.GetOwnerOf(bot);
    if (!owner)
        return "";

    // The best of each: the scroll of the bot's main stat, a battle elixir, a guardian elixir, an elixir of neither
    // kind (the old ones). One piece each, and only what does something for the way it fights.
    struct Pick
    {
        uint32 entry = 0;
        uint32 worth = 0;
        SpellInfo const* spell = nullptr;
    };
    Pick scroll, battle, guardian, other;
    uint32 const stat = MainStat(bot);
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    auto const lastScroll = scrollUntil.find(bot->GetGUID());
    bool const scrolls = settings.storageSupplyScrolls && !OwnBuffReplaces(bot, stat) &&
        (lastScroll == scrollUntil.end() || getMSTimeDiff(lastScroll->second, now) < uint32(DAY * IN_MILLISECONDS));
    for (SupplyItem const& item : DungeonSupplies::List(owner))
    {
        if (item.kind != SupplyKind::Scroll && item.kind != SupplyKind::Elixir)
            continue;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item.entry);
        SpellInfo const* info = nullptr;
        if (!proto || DungeonSupplies::Classify(proto, &info) != item.kind || !info || proto->RequiredLevel > bot->GetLevel() ||
            bot->CanUseItem(proto) != EQUIP_ERR_OK || bot->HasAura(info->Id) || ConflictsWithAuras(bot, info))
            continue;
        uint32 const worth = proto->RequiredLevel * 1000 + proto->ItemLevel + 1;
        Pick* pick = nullptr;
        if (item.kind == SupplyKind::Scroll)
        {
            if (!scrolls)
                continue;
            int32 amount = 0;
            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                if (info->Effects[i].Effect == SPELL_EFFECT_APPLY_AURA && info->Effects[i].ApplyAuraName == SPELL_AURA_MOD_STAT &&
                    info->Effects[i].MiscValue == int32(stat))
                    amount = std::max(amount, info->Effects[i].BasePoints + 1);
            if (amount <= 0 || HasStatBuff(bot, stat, amount))
                continue;
            pick = &scroll;
        }
        else
        {
            if (!settings.storageSupplyElixirs || !DungeonSupplies::Helps(bot, info))
                continue;
            pick = sSpellMgr->IsSpellMemberOfSpellGroup(info->Id, SPELL_GROUP_ELIXIR_BATTLE) ? &battle :
                sSpellMgr->IsSpellMemberOfSpellGroup(info->Id, SPELL_GROUP_ELIXIR_GUARDIAN) ? &guardian : &other;
        }
        if (worth > pick->worth)
            *pick = { item.entry, worth, info };
    }
    // The scroll first (the owner named it); an elixir only when it stacks with what was chosen before it.
    std::vector<uint32> allowed;
    std::vector<SpellInfo const*> chosen;
    for (Pick const* pick : { &scroll, &battle, &guardian, &other })
    {
        if (!pick->entry)
            continue;
        bool stacks = true;
        for (SpellInfo const* earlier : chosen)
            stacks = stacks && StacksWith(pick->spell, earlier);
        if (!stacks)
            continue;
        allowed.push_back(pick->entry);
        chosen.push_back(pick->spell);
    }
    if (allowed.empty())
        return "";
    std::string const used = DungeonSupplies::Use(bot, allowed, "while it levels");
    if (scroll.spell && bot->HasAura(scroll.spell->Id))
        scrollUntil[bot->GetGUID()] = now + uint32(std::max<int32>(scroll.spell->GetDuration(), 5 * MINUTE * IN_MILLISECONDS));
    return used;
}

std::string StorageSupply::Step(Player* bot)
{
    Settings const settings = sBotSettings.For(bot);
    if (!bot || !settings.storageEnable || (!settings.storageSupplyProvisions && !settings.storageSupplyGear))
        return "";
    if (!bot->IsAlive())
        return "";
    uint32 const owner = sStorageService.GetOwnerOf(bot);
    if (!owner)
        return "";

    std::vector<StorageItem> const stock = sStorageService.List(owner);
    if (stock.empty())
        return "";

    std::string given;
    auto add = [&given](std::string const& what)
    {
        if (!what.empty())
            given += given.empty() ? what : ", " + what;
    };
    // (Food, drink, potions and bandages are not stocked here any more: a bot takes one piece at the moment it uses
    // it, TakeOne.)
    if (settings.storageSupplyGear)
    {
        add(TakeGear(bot, owner, stock));
        // A tank or healer by its dungeon role: pieces for its dungeon set that nobody would wear for levelling
        // (DungeonRoles.h). The storage as it is after TakeGear.
        if (std::string const forRole = sRoleService.TakeFromStorage(bot, owner, sStorageService.List(owner)); !forRole.empty())
            add("for its dungeon set (into its role store): " + forRole);
    }
    // A hunter's arrows or bullets, when the storage has some (Ammunition.h): after the gear, for the ranged weapon
    // it ends up with (a hunter took 1603 arrows for its bow and then a gun out of the storage in one look).
    if (settings.storageSupplyProvisions && !bot->IsInCombat())
        add(TakeAmmo(bot));
    return given;
}
