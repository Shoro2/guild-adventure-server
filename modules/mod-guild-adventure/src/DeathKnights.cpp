/*
 * DeathKnights implementation. See DeathKnights.h.
 */

#include "DeathKnights.h"
#include "BotDriver.h"
#include "BotSettings.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "DungeonRoles.h"
#include "Item.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include <algorithm>
#include <vector>

using namespace GuildAdventure;

namespace
{
    // The starting campaign of Acherus in its order (as mod-skip-dk-starting-area lists it, with "The Scarlet
    // Apocalypse" and "The Lich King's Command", which reward nothing, so that the whole chain counts as done). One quest
    // of the race and one of the side follow below.
    constexpr uint32 StarterQuests[] = {
        12593, 12619, 12842, 12848, 12636, 12641, 12657, 12678, 12679, 12680, 12687, 12698, 12701, 12706, 12716, 12719,
        12720, 12722, 12724, 12725, 12727, 12733, 12751, 12754, 12755, 12756, 12757, 12778, 12779, 12800, 12801, 13165,
        13166
    };
    constexpr uint32 QuestLightOfDawn = 12801;   // the campaign's end: it teaches Death Gate
    constexpr uint32 QuestWhereKingsWalk = 13188; // the Alliance's hand-in at Stormwind
    constexpr uint32 QuestWarchiefsBlessing = 13189;

    constexpr uint32 SpellDeathGate = 50977;
    constexpr uint32 SpellRuneforging = 53428;
    constexpr uint32 ItemHearthstone = 6948;

    // "A Special Surprise": the quest of the race.
    uint32 RaceQuest(uint8 race)
    {
        switch (race)
        {
            case RACE_TAUREN:       return 12739;
            case RACE_HUMAN:        return 12742;
            case RACE_NIGHTELF:     return 12743;
            case RACE_DWARF:        return 12744;
            case RACE_GNOME:        return 12745;
            case RACE_DRAENEI:      return 12746;
            case RACE_BLOODELF:     return 12747;
            case RACE_ORC:          return 12748;
            case RACE_TROLL:        return 12749;
            case RACE_UNDEAD_PLAYER: return 12750;
            default:                return 0;
        }
    }

    // Where a death knight that skips Acherus begins: mod-skip-dk-starting-area's places (a spot in Stormwind's trade
    // district and the inn there; Orgrimmar's valley of strength and its inn).
    struct Capital
    {
        uint32 map;
        float x, y, z, o;
        uint32 zone;
        float bindX, bindY, bindZ;
    };
    constexpr Capital Stormwind = { 0, -8833.37f, 628.62f, 94.00f, 1.06f, 1519, -8866.55f, 671.39f, 97.90f };
    constexpr Capital Orgrimmar = { 1, 1569.59f, -4397.63f, 7.70f, 0.54f, 1637, 1637.62f, -4440.22f, 15.78f };

    std::string SpellNameOf(uint32 spellId)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        return info && info->SpellName[0] ? std::string(info->SpellName[0]) : Acore::StringFormat("spell {}", spellId);
    }

    std::string ItemNameOf(uint32 entry)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        return proto ? proto->Name1 : Acore::StringFormat("item {}", entry);
    }

    std::string Join(std::vector<std::string> const& parts)
    {
        std::string text;
        for (std::string const& part : parts)
            text += (text.empty() ? "" : ", ") + part;
        return text;
    }

    // What a quest's reward spell teaches: its "learn spell" effects (and those of a spell it triggers), as
    // Player::RewardQuest's cast of the spell would. Other effects of the spell (auras, scripts) are not wanted here.
    void LearnFrom(Player* player, uint32 spellId, std::vector<std::string>& learned, uint8 depth = 0)
    {
        SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
        if (!info || depth > 2)
            return;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            SpellEffectInfo const& effect = info->Effects[i];
            if (effect.Effect == SPELL_EFFECT_LEARN_SPELL && effect.TriggerSpell)
            {
                if (!player->HasSpell(effect.TriggerSpell))
                {
                    player->learnSpell(effect.TriggerSpell);
                    learned.push_back(SpellNameOf(effect.TriggerSpell));
                }
            }
            else if (effect.Effect == SPELL_EFFECT_TRIGGER_SPELL && effect.TriggerSpell)
                LearnFrom(player, effect.TriggerSpell, learned, depth + 1);
        }
    }

    bool IsStartingGear(Player* player, uint32 entry)
    {
        if (CharStartOutfitEntry const* outfit = GetCharStartOutfitEntry(player->getRace(), player->getClass(), player->getGender()))
            for (int32 j = 0; j < MAX_OUTFIT_ITEMS; ++j)
                if (outfit->ItemId[j] > 0 && uint32(outfit->ItemId[j]) == entry)
                    return true;
        return false;
    }

    // The places on the body a piece can go to: both rings, both trinkets, else the one the core finds.
    std::vector<uint8> PlacesFor(Player* player, ItemTemplate const* proto)
    {
        switch (proto->InventoryType)
        {
            case INVTYPE_FINGER:  return { EQUIPMENT_SLOT_FINGER1, EQUIPMENT_SLOT_FINGER2 };
            case INVTYPE_TRINKET: return { EQUIPMENT_SLOT_TRINKET1, EQUIPMENT_SLOT_TRINKET2 };
            default:
            {
                uint8 const slot = player->FindEquipSlot(proto, NULL_SLOT, true);
                if (slot == NULL_SLOT)
                    return {};
                return { slot };
            }
        }
    }

    float ScoreOf(Player* player, Item* item)
    {
        return item ? BotDriver::ItemScore(player, item->GetEntry(), item->GetItemRandomPropertyId()) : 0.0f;
    }

    // Puts a new piece on at that place when it is better than what is worn there: the worn piece goes into the bags,
    // or is destroyed when it is a piece of the starting gear (soulbound, worth copper). Returns false when it is not
    // put on (no better, cannot be worn, no room for the old piece).
    bool PutOn(Player* player, uint32 entry, std::string& replaced)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        if (!proto || player->CanUseItem(proto) != EQUIP_ERR_OK)
            return false;

        float const score = BotDriver::ItemScore(player, entry, 0);
        uint8 slot = NULL_SLOT;
        Item* worn = nullptr;
        for (uint8 place : PlacesFor(player, proto))
        {
            Item* there = player->GetItemByPos(INVENTORY_SLOT_BAG_0, place);
            if (!there)
            {
                slot = place;
                worn = nullptr;
                break;
            }
            if (slot == NULL_SLOT || ScoreOf(player, there) < ScoreOf(player, worn))
            {
                slot = place;
                worn = there;
            }
        }
        if (slot == NULL_SLOT || (worn && score <= ScoreOf(player, worn)))
            return false;

        uint16 dest = 0;
        if (player->CanEquipNewItem(slot, dest, entry, true) != EQUIP_ERR_OK)
            return false;

        if (worn)
        {
            replaced = worn->GetTemplate()->Name1;
            if (IsStartingGear(player, worn->GetEntry()))
                player->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
            else
            {
                ItemPosCountVec bagDest;
                if (player->CanStoreItem(NULL_BAG, NULL_SLOT, bagDest, worn, false) != EQUIP_ERR_OK)
                    return false;
                player->RemoveItem(INVENTORY_SLOT_BAG_0, slot, true);
                player->StoreItem(bagDest, worn, true);
            }
        }

        if (!player->EquipNewItem(dest, entry, true))
            return false;
        if (proto->InventoryType == INVTYPE_2HWEAPON)
            player->AutoUnequipOffhandIfNeed();
        return true;
    }

    // The campaign's item rewards: the fixed ones and, of a quest with a choice, the one the bot rates best.
    std::vector<uint32> RewardItems(Player* player, std::vector<Quest const*> const& quests)
    {
        std::vector<uint32> items;
        for (Quest const* quest : quests)
        {
            for (uint8 i = 0; i < QUEST_REWARDS_COUNT; ++i)
                if (quest->RewardItemId[i])
                    for (uint32 n = 0; n < std::max<uint32>(1, quest->RewardItemIdCount[i]); ++n)
                        items.push_back(quest->RewardItemId[i]);

            uint32 best = 0;
            float bestScore = -1.0f;
            for (uint8 i = 0; i < QUEST_REWARD_CHOICES_COUNT; ++i)
            {
                uint32 const entry = quest->RewardChoiceItemId[i];
                ItemTemplate const* proto = entry ? sObjectMgr->GetItemTemplate(entry) : nullptr;
                if (!proto || player->CanUseItem(proto) != EQUIP_ERR_OK)
                    continue;
                float const score = BotDriver::ItemScore(player, entry, 0);
                if (score > bestScore)
                {
                    best = entry;
                    bestScore = score;
                }
            }
            if (best)
                items.push_back(best);
        }

        // The best pieces first, so that a ring or a trinket meets the weaker worn one.
        std::stable_sort(items.begin(), items.end(), [](uint32 a, uint32 b)
        {
            ItemTemplate const* pa = sObjectMgr->GetItemTemplate(a);
            ItemTemplate const* pb = sObjectMgr->GetItemTemplate(b);
            return (pa ? pa->ItemLevel : 0) > (pb ? pb->ItemLevel : 0);
        });
        return items;
    }

    // Runes, the best first: tank runes, then the others. A rune a weapon cannot take (two-hand only, one-hand only)
    // is passed over by the spell's own item requirements.
    struct RuneSpell
    {
        uint32 spell;
        bool tank;
    };
    constexpr RuneSpell RuneOrder[] = {
        { 62158, true },  // Rune of the Stoneskin Gargoyle
        { 70164, true },  // Rune of the Nerubian Carapace
        { 53323, true },  // Rune of Swordshattering
        { 54446, true },  // Rune of Swordbreaking
        { 53344, false }, // Rune of the Fallen Crusader
        { 53343, false }, // Rune of Razorice
        { 53341, false }, // Rune of Cinderglacier
        { 53331, false }, // Rune of Lichbane
    };

    uint32 EnchantOf(SpellInfo const* info)
    {
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (info->Effects[i].Effect == SPELL_EFFECT_ENCHANT_ITEM && info->Effects[i].MiscValue > 0)
                return uint32(info->Effects[i].MiscValue);
        return 0;
    }
}

bool DeathKnights::PlaceRecruit(Player* player, uint32 ownerAccount, CharacterDatabaseTransaction trans)
{
    if (!player || player->getClass() != CLASS_DEATH_KNIGHT || !sBotSettings.GetForGuild(ownerAccount, Setting::DeathKnightsSkipStart))
        return false;

    Capital const& capital = player->GetTeamId(true) == TEAM_HORDE ? Orgrimmar : Stormwind;
    Player::SavePositionInDB(WorldLocation(capital.map, capital.x, capital.y, capital.z, capital.o), uint16(capital.zone),
        player->GetGUID(), trans);

    // Without a row Player::_LoadHomeBind binds the hearthstone at the race and class's start: Acherus.
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_PLAYER_HOMEBIND);
    stmt->SetData(0, player->GetGUID().GetRawValue());
    stmt->SetData(1, uint16(capital.map));
    stmt->SetData(2, uint16(capital.zone));
    stmt->SetData(3, capital.bindX);
    stmt->SetData(4, capital.bindY);
    stmt->SetData(5, capital.bindZ);
    trans->Append(stmt);
    return true;
}

bool DeathKnights::IsStartPending(Player* player)
{
    return player && player->getClass() == CLASS_DEATH_KNIGHT && player->GetMapId() != MAP_EBON_HOLD &&
        !player->GetQuestRewardStatus(QuestLightOfDawn);
}

std::string DeathKnights::FinishStart(Player* player, bool withRewards)
{
    if (!player)
        return "";

    std::vector<uint32> ids(std::begin(StarterQuests), std::end(StarterQuests));
    if (uint32 const raceQuest = RaceQuest(player->getRace()))
        ids.push_back(raceQuest);
    ids.push_back(player->GetTeamId(true) == TEAM_HORDE ? QuestWarchiefsBlessing : QuestWhereKingsWalk);

    // The quests count as rewarded: no experience (the recruit stays at the core's death knight level), no gold (nothing
    // for the guild tax either), no quest scripts; their reputation and the spells their rewards teach are given.
    std::vector<Quest const*> quests;
    std::vector<std::string> spells;
    for (uint32 id : ids)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(id);
        if (!quest || player->GetQuestRewardStatus(id) || player->GetQuestStatus(id) != QUEST_STATUS_NONE)
            continue;
        player->SetRewardedQuest(id);
        player->RewardReputation(quest);
        LearnFrom(player, quest->GetRewSpellCast() > 0 ? uint32(quest->GetRewSpellCast()) : quest->GetRewSpell(), spells);
        quests.push_back(quest);
    }
    // What leaves Acherus and enchants its weapons, whatever the quest data teaches.
    for (uint32 spell : { SpellDeathGate, SpellRuneforging })
        if (!player->HasSpell(spell))
        {
            player->learnSpell(spell);
            spells.push_back(SpellNameOf(spell));
        }

    std::vector<std::string> worn;
    std::vector<std::string> bagged;
    std::vector<std::pair<uint32, uint32>> mail;
    auto keep = [&](uint32 entry)
    {
        ItemPosCountVec dest;
        if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, entry, 1) == EQUIP_ERR_OK && player->StoreNewItem(dest, entry, true))
            bagged.push_back(ItemNameOf(entry));
        else
            mail.emplace_back(entry, 1);
    };
    if (withRewards)
        for (uint32 entry : RewardItems(player, quests))
        {
            std::string replaced;
            if (PutOn(player, entry, replaced))
                worn.push_back(replaced.empty() ? ItemNameOf(entry) : Acore::StringFormat("{} (for {})", ItemNameOf(entry), replaced));
            else
                keep(entry);
        }
    if (!player->HasItemCount(ItemHearthstone, 1, true))
        keep(ItemHearthstone);
    if (!mail.empty())
        player->SendItemRetrievalMail(mail);

    // Outside Acherus the core counts the talent points of its level (Player::CalculateTalentsPoints).
    player->InitTalentForLevel();
    std::string const runes = Runeforge(player);
    player->SaveToDB(false, false);

    std::string text = Acore::StringFormat("(level {}) skipped the death knight starting area in {}: {} quests of the campaign "
        "count as done (no experience, no gold)", uint32(player->GetLevel()), player->GetTeamId(true) == TEAM_HORDE ?
        "Orgrimmar" : "Stormwind", quests.size());
    if (!spells.empty())
        text += "; learned " + Join(spells);
    if (!worn.empty())
        text += "; put on " + Join(worn);
    if (!bagged.empty())
        text += "; into its bags " + Join(bagged);
    if (!mail.empty())
        text += Acore::StringFormat("; {} item{} by mail (no room in its bags)", mail.size(), mail.size() == 1 ? "" : "s");
    if (!withRewards)
        text += "; no item rewards (DeathKnights.SkipStart: starting gear only)";
    if (!runes.empty())
        text += "; " + runes;
    text += Acore::StringFormat("; {} talent points to spend", player->GetFreeTalentPoints());
    return text;
}

std::string DeathKnights::Runeforge(Player* player)
{
    if (!player || player->getClass() != CLASS_DEATH_KNIGHT || !sBotSettings.IsOn(player, Setting::ClassAbilitiesRuneforging))
        return "";

    bool const tank = sRoleService.GetRole(player->GetGUID()) == DungeonRole::Tank && sRoleService.IsDungeonSetOn(player->GetGUID());
    std::vector<std::string> done;
    for (uint8 slot : { uint8(EQUIPMENT_SLOT_MAINHAND), uint8(EQUIPMENT_SLOT_OFFHAND) })
    {
        Item* weapon = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!weapon || weapon->GetTemplate()->Class != ITEM_CLASS_WEAPON || weapon->GetEnchantmentId(PERM_ENCHANTMENT_SLOT))
            continue;

        for (bool tankRunes : { true, false })
        {
            if (tankRunes && !tank)
                continue;
            uint32 enchant = 0;
            uint32 runeSpell = 0;
            for (RuneSpell const& rune : RuneOrder)
            {
                if (rune.tank != tankRunes || !player->HasSpell(rune.spell))
                    continue;
                SpellInfo const* info = sSpellMgr->GetSpellInfo(rune.spell);
                if (!info || !weapon->IsFitToSpellRequirements(info) || !sSpellItemEnchantmentStore.LookupEntry(EnchantOf(info)))
                    continue;
                enchant = EnchantOf(info);
                runeSpell = rune.spell;
                break;
            }
            if (!enchant)
                continue;

            // As Spell::EffectEnchantItemPerm does at a runeforge.
            player->ApplyEnchantment(weapon, PERM_ENCHANTMENT_SLOT, false);
            weapon->SetEnchantment(PERM_ENCHANTMENT_SLOT, enchant, 0, 0, player->GetGUID());
            player->ApplyEnchantment(weapon, PERM_ENCHANTMENT_SLOT, true);
            done.push_back(Acore::StringFormat("{} on its {}", SpellNameOf(runeSpell), weapon->GetTemplate()->Name1));
            break;
        }
    }
    return done.empty() ? "" : "runeforged " + Join(done);
}
