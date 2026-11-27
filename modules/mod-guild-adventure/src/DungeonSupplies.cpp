/*
 * mod-guild-adventure: what a dungeon group uses out of the guild storage during a run (DungeonSupplies.h).
 */

#include "DungeonSupplies.h"
#include "BotDriver.h"
#include "DBCStores.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Spell.h"
#include "SpellAuraDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StorageService.h"
#include "BotSettings.h"
#include <algorithm>
#include <cstring>
#include <optional>
#include "StringFormat.h"
#include "Timer.h"
#include <map>

using namespace GuildAdventure;

namespace
{
    constexpr char const* ActivityLog = "module.ga.activity";
    constexpr uint32 FoodSpellCategory = 11;
    constexpr uint32 DrinkSpellCategory = 59;
    constexpr int32 LastingBuffMs = 5 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 RangedMask = (1 << ITEM_SUBCLASS_WEAPON_BOW) | (1 << ITEM_SUBCLASS_WEAPON_GUN) | (1 << ITEM_SUBCLASS_WEAPON_CROSSBOW);
    // A piece that did nothing for a bot (a stronger effect was on it already) is not tried again for this long.
    constexpr uint32 RetryMs = 10 * MINUTE * IN_MILLISECONDS;

    std::map<std::pair<ObjectGuid::LowType, uint32>, uint32> failedUntil;

    bool HasEffect(SpellInfo const* info, uint32 effect)
    {
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (info->Effects[i].Effect == effect)
                return true;
        return false;
    }

    // Whether the buff of an elixir or scroll does something for the way the bot fights.
    bool HelpsBot(Player* bot, SpellInfo const* info)
    {
        bool const melee = BotDriver::FightsInMelee(bot);
        bool const hunter = bot->getClass() == CLASS_HUNTER;
        bool const physical = melee || hunter;
        bool const usesMana = bot->getPowerType() == POWER_MANA && !hunter;
        bool const caster = usesMana && !melee;
        bool any = false;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (info->Effects[i].Effect != SPELL_EFFECT_APPLY_AURA)
                continue;
            any = true;
            int32 const misc = info->Effects[i].MiscValue;
            switch (info->Effects[i].ApplyAuraName)
            {
                case SPELL_AURA_MOD_STAT:
                    if (misc == STAT_STRENGTH ? melee : misc == STAT_AGILITY ? physical : misc == STAT_INTELLECT ? usesMana :
                        misc == STAT_SPIRIT ? caster : true)
                        return true;
                    break;
                case SPELL_AURA_MOD_ATTACK_POWER:
                    if (melee)
                        return true;
                    break;
                case SPELL_AURA_MOD_RANGED_ATTACK_POWER:
                    if (hunter)
                        return true;
                    break;
                case SPELL_AURA_MOD_DAMAGE_DONE:
                    if ((misc & SPELL_SCHOOL_MASK_NORMAL) ? physical : caster)
                        return true;
                    break;
                case SPELL_AURA_MOD_HEALING_DONE:
                case SPELL_AURA_MOD_POWER_REGEN:
                case SPELL_AURA_MOD_SPELL_CRIT_CHANCE:
                    if (usesMana)
                        return true;
                    break;
                default:
                    // Armour, resistances, health, regeneration, what else there is: for everybody.
                    return true;
            }
        }
        return !any;
    }

    // Whether a battle or guardian elixir is on the bot already (an elixir of the same kind would replace it).
    bool HasElixirOfGroup(Player* bot, SpellGroup group)
    {
        for (auto const& [spellId, application] : bot->GetAppliedAuras())
            if (sSpellMgr->IsSpellMemberOfSpellGroup(spellId, group))
                return true;
        return false;
    }

    // What a stone or oil does to a weapon: more weapon damage (stones, for those who strike) or something for spells
    // (oils, for casters). 0 unknown.
    uint32 CoatKind(SpellInfo const* info)
    {
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (info->Effects[i].Effect != SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY)
                continue;
            SpellItemEnchantmentEntry const* enchant = sSpellItemEnchantmentStore.LookupEntry(uint32(info->Effects[i].MiscValue));
            if (!enchant)
                return 0;
            for (uint8 slot = 0; slot < MAX_SPELL_ITEM_ENCHANTMENT_EFFECTS; ++slot)
            {
                if (enchant->type[slot] == ITEM_ENCHANTMENT_TYPE_DAMAGE)
                    return ITEM_ENCHANTMENT_TYPE_DAMAGE;
                if (enchant->type[slot] == ITEM_ENCHANTMENT_TYPE_COMBAT_SPELL)
                    return ITEM_ENCHANTMENT_TYPE_COMBAT_SPELL;
            }
            return ITEM_ENCHANTMENT_TYPE_STAT;
        }
        return 0;
    }

    // Casts the use spell of a supply for the bot at once (the piece itself is taken afterwards, when it worked).
    void CastOn(Player* bot, SpellInfo const* info, Item* target)
    {
        Spell* spell = new Spell(bot, info, TRIGGERED_FULL_MASK);
        SpellCastTargets targets;
        if (target)
            targets.SetItemTarget(target);
        else
            targets.SetUnitTarget(bot);
        spell->prepare(&targets);
    }
}

bool DungeonSupplies::Helps(Player* bot, SpellInfo const* spell)
{
    return bot && spell && HelpsBot(bot, spell);
}

SupplyKind DungeonSupplies::Classify(ItemTemplate const* proto, SpellInfo const** spell)
{
    if (!proto)
        return SupplyKind::None;
    for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
    {
        if (!proto->Spells[i].SpellId || proto->Spells[i].SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(proto->Spells[i].SpellId);
        if (!info)
            continue;
        SupplyKind kind = SupplyKind::None;
        // (Not a lure: a temporary enchantment of a fishing pole.)
        if (HasEffect(info, SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY))
            kind = info->EquippedItemClass == ITEM_CLASS_WEAPON &&
                uint32(info->EquippedItemSubClassMask) == (1u << ITEM_SUBCLASS_WEAPON_FISHING_POLE) ? SupplyKind::None : SupplyKind::WeaponCoat;
        else if (HasEffect(info, SPELL_EFFECT_ENCHANT_ITEM) && info->EquippedItemClass == ITEM_CLASS_WEAPON &&
            (uint32(info->EquippedItemSubClassMask) & RangedMask) && !(uint32(info->EquippedItemSubClassMask) & ~RangedMask))
            kind = SupplyKind::Scope;
        else if (proto->Class == ITEM_CLASS_CONSUMABLE && HasEffect(info, SPELL_EFFECT_APPLY_AURA) && info->GetDuration() >= LastingBuffMs &&
            proto->Spells[i].SpellCategory != FoodSpellCategory && proto->Spells[i].SpellCategory != DrinkSpellCategory)
        {
            if (proto->SubClass == ITEM_SUBCLASS_ELIXIR || proto->SubClass == ITEM_SUBCLASS_FLASK)
                kind = SupplyKind::Elixir;
            else if (proto->SubClass == ITEM_SUBCLASS_SCROLL)
                kind = SupplyKind::Scroll;
        }
        if (kind == SupplyKind::None)
            continue;
        if (spell)
            *spell = info;
        return kind;
    }
    return SupplyKind::None;
}

std::vector<SupplyItem> DungeonSupplies::List(uint32 owner)
{
    std::vector<SupplyItem> result;
    for (StorageItem const& item : sStorageService.List(owner))
    {
        if (!item.amount || item.randomProperty)
            continue;
        SupplyKind const kind = Classify(sObjectMgr->GetItemTemplate(item.entry));
        if (kind == SupplyKind::None)
            continue;
        SupplyItem supply;
        supply.entry = item.entry;
        supply.kind = kind;
        supply.amount = item.amount;
        result.push_back(supply);
    }
    return result;
}

namespace
{
    // For a dungeon run: what fits the character best of what is allowed and in the storage (the owner, 2026-11-14:
    // [quote left out]). Elixirs and scrolls are rated like gear: a tank or healer by the stat weights
    // of its role (BotDriver::RoleScore), a damage dealer by mod-playerbots' weights of its talents (ItemScore); what
    // rates nothing for it is left out. One battle and one guardian elixir (a flask is both), one scroll. Stones,
    // oils, poisons and scopes follow, the strongest first (Use fits them to the weapons).
    // Whether two buffs can be on a character together (the core's spell groups: a scroll of agility and an elixir of
    // agility cannot, nor a scroll of spirit and a priest's Divine Spirit).
    bool StacksWith(SpellInfo const* a, SpellInfo const* b)
    {
        return a->Id == b->Id || sSpellMgr->CheckSpellGroupStackRules(a, b) == SPELL_GROUP_STACK_RULE_DEFAULT;
    }

    // Something on the bot does not stack with the buff: the piece would do nothing, or take the other effect off.
    bool ConflictsWithAuras(Player const* bot, SpellInfo const* info)
    {
        for (auto const& [spellId, application] : bot->GetAppliedAuras())
        {
            SpellInfo const* existing = application->GetBase()->GetSpellInfo();
            if (!existing || existing->Id == info->Id)
                continue;
            // Another rank of the same buff (the spell groups say nothing about two ranks of one chain): a weaker
            // scroll would take the stronger one's place (a tank with Scroll of Stamina III on it from the run before
            // used a Scroll of Stamina II at the start of the next), a stronger one may replace a weaker.
            if (existing->IsDifferentRankOf(info))
            {
                if (!info->IsHighRankOf(existing))
                    return true;
                continue;
            }
            if (!StacksWith(info, existing))
                return true;
        }
        return false;
    }

    std::vector<uint32> ChooseFor(Player* bot, int8 role, std::vector<uint32> const& allowed, uint32 owner)
    {
        struct Rated
        {
            uint32 entry = 0;
            SupplyKind kind = SupplyKind::None;
            SpellInfo const* spell = nullptr;
            float score = 0.0f;
            bool battle = false;
            bool guardian = false;
        };
        std::vector<Rated> rated;
        for (uint32 entry : allowed)
        {
            if (!sStorageService.GetAmount(owner, entry, 0))
                continue;
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
            SpellInfo const* info = nullptr;
            Rated candidate;
            candidate.entry = entry;
            candidate.kind = DungeonSupplies::Classify(proto, &info);
            if (candidate.kind == SupplyKind::None || !info || bot->CanUseItem(proto) != EQUIP_ERR_OK)
                continue;
            if (candidate.kind == SupplyKind::Elixir || candidate.kind == SupplyKind::Scroll)
            {
                // (Not a piece whose effect is on the bot, and none that something on it would keep from working: a
                // scroll of spirit under Divine Spirit "did nothing" and went back while the next best scroll stayed
                // unused.)
                if (bot->HasAura(info->Id) || !HelpsBot(bot, info) || ConflictsWithAuras(bot, info))
                    continue;
                candidate.spell = info;
                candidate.score = BotDriver::BuffScore(bot, role, entry, info);
                if (candidate.score <= 0.0f)
                    continue;
                candidate.battle = sSpellMgr->IsSpellMemberOfSpellGroup(info->Id, SPELL_GROUP_ELIXIR_BATTLE);
                candidate.guardian = sSpellMgr->IsSpellMemberOfSpellGroup(info->Id, SPELL_GROUP_ELIXIR_GUARDIAN);
            }
            else
                candidate.score = float(proto->ItemLevel);
            rated.push_back(candidate);
        }
        std::stable_sort(rated.begin(), rated.end(), [](Rated const& a, Rated const& b) { return a.score > b.score; });

        std::vector<uint32> chosen;
        bool battle = HasElixirOfGroup(bot, SPELL_GROUP_ELIXIR_BATTLE);
        bool guardian = HasElixirOfGroup(bot, SPELL_GROUP_ELIXIR_GUARDIAN);
        bool other = false, scroll = false;
        std::vector<SpellInfo const*> buffs;
        for (Rated const& candidate : rated)
        {
            // (What is chosen already comes first: the elixir of agility, and then no scroll of agility on top.)
            if (candidate.spell && std::any_of(buffs.begin(), buffs.end(),
                    [&](SpellInfo const* earlier) { return !StacksWith(candidate.spell, earlier); }))
                continue;
            if (candidate.kind == SupplyKind::Elixir)
            {
                if ((candidate.battle && battle) || (candidate.guardian && guardian) || (!candidate.battle && !candidate.guardian && other))
                    continue;
                battle = battle || candidate.battle;
                guardian = guardian || candidate.guardian;
                other = other || (!candidate.battle && !candidate.guardian);
            }
            else if (candidate.kind == SupplyKind::Scroll)
            {
                if (scroll)
                    continue;
                scroll = true;
            }
            if (candidate.spell)
                buffs.push_back(candidate.spell);
            chosen.push_back(candidate.entry);
        }
        return chosen;
    }
}

std::string DungeonSupplies::Use(Player* bot, std::vector<uint32> const& allowed, char const* purpose, int8 role)
{
    if (!bot || allowed.empty() || !bot->IsAlive() || bot->IsInCombat() || bot->IsMounted() || bot->IsBeingTeleported())
        return "";
    uint32 const owner = sStorageService.GetOwnerOf(bot);
    if (!owner)
        return "";
    // The guild storage's reserves (docs/storage.md "Reserves") are what the owner keeps for such uses: what he allowed
    // for a dungeon run may take into them (bot setting Storage.ReserveForDungeons), what a bot uses while it levels not.
    std::optional<StorageService::OwnerOrder> intoReserve;
    if (purpose && std::strcmp(purpose, "for its dungeon run") == 0 && sBotSettings.GetForGuild(owner, Setting::StorageReserveForDungeons))
        intoReserve.emplace();
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    std::string used;
    std::vector<uint32> const pool = role >= 0 ? ChooseFor(bot, role, allowed, owner) : allowed;
    for (uint32 entry : pool)
    {
        if (!sStorageService.GetAmount(owner, entry, 0))
            continue;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        SpellInfo const* info = nullptr;
        SupplyKind const kind = Classify(proto, &info);
        if (kind == SupplyKind::None || !info || bot->CanUseItem(proto) != EQUIP_ERR_OK)
            continue;
        auto const key = std::make_pair(bot->GetGUID().GetCounter(), entry);
        if (auto failed = failedUntil.find(key); failed != failedUntil.end() && now < failed->second)
            continue;

        // Does the bot lack what it gives, and would it help it?
        Item* weapon = nullptr;
        EnchantmentSlot slot = TEMP_ENCHANTMENT_SLOT;
        switch (kind)
        {
            case SupplyKind::Elixir:
            case SupplyKind::Scroll:
            {
                if (bot->HasAura(info->Id) || !HelpsBot(bot, info))
                    continue;
                bool const battle = sSpellMgr->IsSpellMemberOfSpellGroup(info->Id, SPELL_GROUP_ELIXIR_BATTLE);
                bool const guardian = sSpellMgr->IsSpellMemberOfSpellGroup(info->Id, SPELL_GROUP_ELIXIR_GUARDIAN);
                if ((battle && HasElixirOfGroup(bot, SPELL_GROUP_ELIXIR_BATTLE)) || (guardian && HasElixirOfGroup(bot, SPELL_GROUP_ELIXIR_GUARDIAN)))
                    continue;
                break;
            }
            case SupplyKind::WeaponCoat:
            {
                uint32 const coat = CoatKind(info);
                bool const melee = BotDriver::FightsInMelee(bot);
                // Stones for those who strike, poisons for rogues, oils for casters.
                if (coat == ITEM_ENCHANTMENT_TYPE_DAMAGE ? !melee : coat == ITEM_ENCHANTMENT_TYPE_COMBAT_SPELL ?
                    bot->getClass() != CLASS_ROGUE : (melee || bot->getPowerType() != POWER_MANA || bot->getClass() == CLASS_HUNTER))
                    continue;
                for (uint8 hand : { uint8(EQUIPMENT_SLOT_MAINHAND), uint8(EQUIPMENT_SLOT_OFFHAND) })
                {
                    Item* candidate = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, hand);
                    if (!weapon && candidate && candidate->GetTemplate()->Class == ITEM_CLASS_WEAPON &&
                        !candidate->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT) && candidate->IsFitToSpellRequirements(info))
                        weapon = candidate;
                }
                if (!weapon)
                    continue;
                break;
            }
            case SupplyKind::Scope:
            {
                // (For hunters: a scope on the bow a warrior carries for pulling would be wasted.)
                Item* ranged = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
                if (bot->getClass() != CLASS_HUNTER || !ranged || ranged->GetEnchantmentId(PERM_ENCHANTMENT_SLOT) ||
                    !ranged->IsFitToSpellRequirements(info))
                    continue;
                weapon = ranged;
                slot = PERM_ENCHANTMENT_SLOT;
                break;
            }
            default:
                continue;
        }

        // One piece out of the storage, used at once; a piece that did nothing goes back.
        std::string error;
        if (!sStorageService.Withdraw(bot, entry, 0, 1, error))
            continue;
        CastOn(bot, info, weapon);
        bool const worked = weapon ? weapon->GetEnchantmentId(slot) != 0 : bot->HasAura(info->Id);
        Item* piece = bot->GetItemByEntry(entry);
        if (worked)
        {
            bot->DestroyItemCount(entry, 1, true);
            uint32 const left = sStorageService.GetAmount(owner, entry, 0);
            LOG_INFO(ActivityLog, "activity: {} used 1 x {} out of the guild storage {}{} ({} left there)", bot->GetName(),
                proto->Name1, purpose, weapon ? Acore::StringFormat(" (on its {})", weapon->GetTemplate()->Name1) : std::string(), left);
            used += (used.empty() ? "" : ", ") + std::string(proto->Name1);
        }
        else
        {
            failedUntil[key] = now + RetryMs;
            std::string ignored;
            bool const back = piece && sStorageService.DepositItem(bot, piece, ignored);
            LOG_INFO(ActivityLog, "activity: {} took 1 x {} out of the guild storage {}, it did nothing for it "
                "(a stronger effect is on it): {}", bot->GetName(), proto->Name1, purpose, back ? "put back" : "it keeps it in its bags");
        }
    }
    return used;
}
