/*
 * QuestSupply: quest items from the guild (part of OrderService; docs/orders.md "Quest items from the guild",
 * The design log §7 "Classic content at 60: plan", package K2).
 *
 * A quest in a bot's log that asks for an item the bot does not have is first a matter for the guild:
 *   1. the guild storage holds it: the bot takes it out where it stands (the storage is reached from anywhere, as for
 *      The owner);
 *   2. another guild character the AI plays carries it and needs it for no quest of its own: it goes through the
 *      storage to the bot;
 *   3. somebody of the guild in the world knows a recipe that makes it and has the materials (bags and storage, core
 *      patch CP-7): that character crafts it (CraftService; it stands still for the casts), and step 2 brings it over
 *      on a later look.
 * Only what the storage takes can travel this way (no soulbound quest items). Buying from a vendor is the planner's
 * vendor task and is paid from the guild's budget (GuildBudget); the auction house is not built.
 *
 * World thread only.
 */

#include "OrderService.h"
#include "BotSettings.h"
#include "BotDriver.h"
#include "GuildAdventureMgr.h"
#include "GuildAdventureSettings.h"
#include "Item.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Professions.h"
#include "QuestDef.h"
#include "QuestSupport.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Storage/CraftService.h"
#include "Storage/StorageService.h"
#include "StringFormat.h"
#include "Timer.h"
#include <unordered_map>

using namespace GuildAdventure;

namespace
{
    constexpr char const* ActivityLog = "module.ga.activity";
    constexpr uint32 SupplyCheckMs = 30 * IN_MILLISECONDS;
    // A quest item crafted for a bot is not crafted for it again for this long, and a crafter that crafted for another
    // bot's quest is left to its own work for a while. (Quest 6026 "That's Asking A Lot" asks for Thorium Bars, a
    // Golden Rod and Unstable Triggers: from 2026-11-16 on the guild's smiths and engineers were set to craft them every
    // 30 s for each of a dozen bots that had the quest, 9000 times, mostly at no forge or anvil, so the cast failed and
    // the next look started it again; each start ended the crafter's own task, "ends turn-in quest 8285 after 1 s:
    // crafting from the guild storage", and what was made went into the guild storage, where crafting for skill used
    // it up before the bot that needed it looked again.)
    constexpr uint32 QuestCraftAgainMs = 30 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 CrafterRestMs = 5 * MINUTE * IN_MILLISECONDS;
    std::unordered_map<uint64, uint32> questCraftAt;        // (needer << 32) | item -> getMSTime() of its last craft
    std::unordered_map<ObjectGuid, uint32> crafterRestUntil; // crafter -> getMSTime(): no craft for another bot before

    // The recipe can be cast where the crafter stands: its anvil, forge or fire in reach, its tools in the bags.
    bool CanCraftHere(Player* crafter, SpellInfo const* info)
    {
        if (info->RequiresSpellFocus && !Professions::FocusNear(crafter, info->RequiresSpellFocus))
            return false;
        for (uint8 i = 0; i < 2; ++i) // (the two totem fields of a spell)
        {
            if (info->Totem[i] && !crafter->HasItemCount(info->Totem[i]))
                return false;
            if (info->TotemCategory[i] && !crafter->HasItemTotemCategory(info->TotemCategory[i]))
                return false;
        }
        return true;
    }

    // How many of an item the character's open quests still ask for beyond what it carries.
    uint32 MissingForQuests(Player* player, uint32 itemId, uint32* questOut = nullptr)
    {
        uint32 wanted = 0;
        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const questId = player->GetQuestSlotQuestId(slot);
            Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
            if (!quest || player->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
                continue;
            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                if (quest->RequiredItemId[i] == itemId)
                {
                    wanted += quest->RequiredItemCount[i];
                    if (questOut)
                        *questOut = questId;
                }
        }
        uint32 const have = player->GetItemCount(itemId);
        return wanted > have ? wanted - have : 0;
    }

    // What a character carries of an item beyond what its own open quests ask for.
    uint32 SpareOf(Player* player, uint32 itemId)
    {
        uint32 wanted = 0;
        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const questId = player->GetQuestSlotQuestId(slot);
            Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
            if (!quest)
                continue;
            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                if (quest->RequiredItemId[i] == itemId)
                    wanted += quest->RequiredItemCount[i];
        }
        uint32 const have = player->GetItemCount(itemId);
        return have > wanted ? have - wanted : 0;
    }
}

void OrderService::SupplyQuestItems(AdventureGuild& guild, GuildBot& bot, Player* player, BotRuntime& rt)
{
    uint32 const now = getMSTime();
    if (!sBotSettings.IsOn(player, Setting::QuestsItemsFromGuild) || now < rt.nextSupplyCheck || !player->IsAlive() || player->IsInCombat())
        return;
    // Not on an order that works on no quest (the profession mode, a farm order, a dungeon run): what it would take
    // waits in its bags for nothing, and its crafting uses up the bags' reagents first. A death knight in the profession
    // mode took Thorium Bars for "That's Asking A Lot" three times in 80 s, made Thorium Widgets out of them and smelted
    // one more bar for the quest, round 184.
    if (bot.order.type == OrderType::Craft || bot.order.type == OrderType::Farm || bot.order.type == OrderType::Dungeon)
        return;
    rt.nextSupplyCheck = now + SupplyCheckMs;
    uint32 const owner = sStorageService.GetOwnerOf(player);
    if (!owner)
        return;

    // The other characters of the guild the AI plays, in the world.
    std::vector<Player*> others;
    auto consider = [&](ObjectGuid guid)
    {
        Player* other = guid != player->GetGUID() ? ObjectAccessor::FindPlayer(guid) : nullptr;
        if (other && other->IsInWorld() && other->IsAlive() && BotDriver::GetAI(other))
            others.push_back(other);
    };
    for (auto const& [guid, member] : guild.bots)
        consider(guid);
    consider(guild.leader);

    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = player->GetQuestSlotQuestId(slot);
        Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
        // (Not for a quest the planner leaves alone: the guild's Arcane Crystals went to the Naxxramas attunement of a
        // level-60 hunter, round 179.)
        if (!quest || player->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE || sQuestSupport.IsLeftAlone(player, questId))
            continue;
        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        {
            uint32 const itemId = quest->RequiredItemId[i];
            ItemTemplate const* proto = itemId ? sObjectMgr->GetItemTemplate(itemId) : nullptr;
            if (!proto || StorageService::RefuseReason(proto))
                continue;
            uint32 missing = MissingForQuests(player, itemId);
            if (!missing)
                continue;

            // (2) Somebody carries it and does not need it: into the storage first.
            // Only what it can spare, and no more than is missing: the first build put the whole stack into the
            // storage ("Wardps puts 9 x Goretusk Liver into the guild storage: Dotlock needs it", and 19 s later
            // Wardps took 2 of them back for its own quest).
            for (Player* other : others)
            {
                uint32 const stored = sStorageService.GetAmount(owner, itemId);
                if (stored >= missing)
                    break;
                uint32 const spare = SpareOf(other, itemId);
                if (!spare)
                    continue;
                // Not what the other one uses itself (its drink, its food, its ammunition).
                Item* stack = other->GetItemByEntry(itemId);
                if (!stack || BotDriver::WantsItem(other, stack) || StorageService::RefuseReason(stack))
                    continue;
                uint32 const give = std::min(spare, missing - stored);
                if (!sStorageService.Add(owner, proto, stack->GetItemRandomPropertyId(), give))
                    continue;
                other->DestroyItemCount(itemId, give, true);
                LOG_INFO(ActivityLog, "activity: {} puts {} x {} into the guild storage: {} needs it for quest {} \"{}\"",
                    other->GetName(), give, proto->Name1, bot.name, questId, quest->GetTitle());
            }

            // (1) Out of the storage.
            if (uint32 const stored = sStorageService.GetAmount(owner, itemId))
            {
                std::string error;
                if (uint32 const got = sStorageService.Withdraw(player, itemId, 0, std::min(stored, missing), error))
                {
                    LOG_INFO(ActivityLog, "activity: {} took {} x {} from the guild storage for quest {} \"{}\"", bot.name, got,
                        proto->Name1, questId, quest->GetTitle());
                    missing -= std::min(missing, got);
                }
            }
            if (!missing)
                continue;

            // (3) Made by the guild: the bot itself, else another character, whoever knows a recipe and has the
            // materials. One job at a time per crafter.
            std::vector<uint32> const& recipes = sProfessions.RecipesFor(itemId);
            if (recipes.empty())
                continue;
            uint64 const craftKey = (uint64(player->GetGUID().GetCounter()) << 32) | itemId;
            if (auto last = questCraftAt.find(craftKey); last != questCraftAt.end() && getMSTimeDiff(last->second, now) < QuestCraftAgainMs)
                continue;
            std::vector<Player*> crafters{ player };
            crafters.insert(crafters.end(), others.begin(), others.end());
            bool started = false;
            for (Player* crafter : crafters)
            {
                if (started || sCraftService.IsCrafting(crafter->GetGUID()) || crafter->IsInCombat())
                    continue;
                if (crafter != player)
                    if (auto rest = crafterRestUntil.find(crafter->GetGUID()); rest != crafterRestUntil.end() && now < rest->second)
                        continue;
                for (uint32 spellId : recipes)
                {
                    SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
                    if (!info || !crafter->HasSpell(spellId) || !CanCraftHere(crafter, info))
                        continue;
                    uint32 const can = CraftService::CraftableCount(crafter, info);
                    if (!can)
                        continue;
                    std::string error, text;
                    if (sCraftService.Start(crafter, spellId, std::min(can, missing), error, text))
                    {
                        LOG_INFO(ActivityLog, "activity: {} crafts for {}'s quest {} \"{}\": {}", crafter->GetName(), bot.name, questId,
                            quest->GetTitle(), text);
                        BotDriver::Rest(crafter);
                        questCraftAt[craftKey] = now;
                        if (crafter != player)
                            crafterRestUntil[crafter->GetGUID()] = now + CrafterRestMs;
                        started = true;
                        break;
                    }
                }
            }
        }
    }
}
