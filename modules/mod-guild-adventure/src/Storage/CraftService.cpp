/*
 * CraftService implementation. See CraftService.h and docs/storage.md "Crafting".
 */

#include "CraftService.h"
#include "BotDriver.h"
#include "GadvProtocol.h"
#include "GuildAdventureSettings.h"
#include "GuildBotHolder.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StorageService.h"
#include "StringFormat.h"
#include "Timer.h"
#include <algorithm>

using namespace GuildAdventure;

namespace
{
    constexpr uint32 MaxCount = 100;

    std::string ProductName(SpellInfo const* spell)
    {
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (spell->Effects[i].Effect == SPELL_EFFECT_CREATE_ITEM || spell->Effects[i].Effect == SPELL_EFFECT_CREATE_ITEM_2)
                if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(spell->Effects[i].ItemType))
                    return proto->Name1;
        return spell->SpellName[0] ? spell->SpellName[0] : "the recipe";
    }
}

CraftService& CraftService::Instance()
{
    static CraftService instance;
    return instance;
}

bool CraftService::IsCraftRecipe(SpellInfo const* spell)
{
    if (!spell || (spell->GetExplicitTargetMask() & TARGET_FLAG_ITEM_MASK))
        return false;
    return spell->HasEffect(SPELL_EFFECT_CREATE_ITEM) || spell->HasEffect(SPELL_EFFECT_CREATE_ITEM_2);
}

uint32 CraftService::CraftableCount(Player* player, SpellInfo const* spell)
{
    uint32 const owner = sStorageService.GetOwnerOf(player);
    uint32 count = MaxCount;
    for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
    {
        if (spell->Reagent[i] <= 0 || !spell->ReagentCount[i])
            continue;
        uint32 const entry = uint32(spell->Reagent[i]);
        uint32 have = player->GetItemCount(entry);
        if (owner && GetSettings().storageReagents)
            have += sStorageService.GetAmount(owner, entry, 0);
        count = std::min(count, have / spell->ReagentCount[i]);
    }
    return count;
}

bool CraftService::Start(Player* player, uint32 spellId, uint32 count, std::string& error, std::string& text)
{
    SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId);
    if (!spell)
    {
        error = "UNKNOWN_SPELL";
        text = "No such spell.";
        return false;
    }
    if (!IsCraftRecipe(spell))
    {
        error = "NOT_A_RECIPE";
        text = "Only recipes that create an item can be crafted from the storage.";
        return false;
    }
    if (!player->HasSpell(spellId))
    {
        error = "NOT_KNOWN";
        text = Acore::StringFormat("{} does not know that recipe.", player->GetName());
        return false;
    }
    if (!player->IsAlive())
    {
        error = "DEAD";
        text = Acore::StringFormat("{} is dead.", player->GetName());
        return false;
    }
    if (_jobs.count(player->GetGUID()))
    {
        error = "BUSY";
        text = Acore::StringFormat("{} is crafting already.", player->GetName());
        return false;
    }
    uint32 const craftable = CraftableCount(player, spell);
    if (!craftable)
    {
        error = "REAGENTS";
        text = "Not enough materials in the bags and the guild storage.";
        return false;
    }

    Job job;
    job.spellId = spellId;
    job.total = std::clamp<uint32>(count, 1, std::min(craftable, MaxCount));
    job.owner = sStorageService.GetOwnerOf(player);

    // A bot stands still while it crafts; the order planner leaves it alone until the queue ends (IsCrafting).
    if (sGuildBotHolder.GetPlannedPlayer(player->GetGUID()) == player)
    {
        BotDriver::ApplyProfile(player, StrategyProfile::Hold);
        player->StopMoving();
        player->GetMotionMaster()->MoveIdle();
    }

    _jobs[player->GetGUID()] = job;
    text = Acore::StringFormat("{} crafts {} x {}.", player->GetName(), job.total, ProductName(spell));
    return true;
}

void CraftService::Stop(ObjectGuid character, char const* why)
{
    auto itr = _jobs.find(character);
    if (itr == _jobs.end())
        return;
    Job const job = itr->second;
    _jobs.erase(itr);
    Finish(character, job, why ? why : "stopped");
}

bool CraftService::IsCrafting(ObjectGuid character) const
{
    return _jobs.count(character) != 0;
}

std::string CraftService::Describe(ObjectGuid character) const
{
    auto itr = _jobs.find(character);
    if (itr == _jobs.end())
        return {};
    SpellInfo const* spell = sSpellMgr->GetSpellInfo(itr->second.spellId);
    return Acore::StringFormat("Crafting {}: {} of {}", spell ? ProductName(spell) : "a recipe", itr->second.done,
        itr->second.total);
}

void CraftService::OnSpellCast(Player* player, uint32 spellId)
{
    std::lock_guard<std::mutex> lock(_castLock);
    _casts.push_back({ player->GetGUID(), spellId });
}

void CraftService::Finish(ObjectGuid character, Job const& job, std::string const& why)
{
    SpellInfo const* spell = sSpellMgr->GetSpellInfo(job.spellId);
    std::string name;
    if (Player* player = ObjectAccessor::FindPlayer(character))
        name = player->GetName();
    std::string const message = Acore::StringFormat("{} crafted {} of {} x {}{}.", name.empty() ? "A guild character" : name,
        job.done, job.total, spell ? ProductName(spell) : "a recipe", why.empty() ? std::string() : " (" + why + ")");
    if (job.owner)
        sGadvProtocol.PushNotice(job.owner, message, job.done < job.total);
    LOG_INFO("module", "mod-guild-adventure: {}", message);
}

void CraftService::Update(uint32 /*diff*/)
{
    std::vector<std::pair<ObjectGuid, uint32>> casts;
    {
        std::lock_guard<std::mutex> lock(_castLock);
        casts.swap(_casts);
    }
    for (auto const& [guid, spellId] : casts)
    {
        auto itr = _jobs.find(guid);
        if (itr != _jobs.end() && itr->second.casting && itr->second.spellId == spellId)
        {
            ++itr->second.done;
            itr->second.casting = false;
        }
    }

    for (auto itr = _jobs.begin(); itr != _jobs.end();)
    {
        ObjectGuid const guid = itr->first;
        Job& job = itr->second;
        Player* player = ObjectAccessor::FindPlayer(guid);
        std::string why;
        bool finished = false;

        if (!player || !player->IsInWorld() || player->IsDuringRemoveFromWorld())
        {
            why = "left the world";
            finished = true;
        }
        else if (player->IsNonMeleeSpellCast(false))
        {
            ++itr;
            continue; // our cast (or another one) is running
        }
        else if (job.casting)
        {
            why = "interrupted";      // the cast ended without being cast: moved, attacked, cancelled
            finished = true;
        }
        else if (job.done >= job.total)
            finished = true;
        else if (!player->IsAlive())
        {
            why = "died";
            finished = true;
        }
        else
        {
            // A bot rides between its tasks (riding from level 20): crafting is refused on a mount (NOT_MOUNTED) and
            // while it still moves (MOVING): it gets off and stops first, for up to a few seconds.
            if (player->IsMounted())
            {
                player->Dismount();
                player->RemoveAurasByType(SPELL_AURA_MOUNTED);
            }
            if (player->isMoving() && (!job.stillSince || getMSTimeDiff(job.stillSince, getMSTime()) < 5 * IN_MILLISECONDS))
            {
                if (!job.stillSince)
                    job.stillSince = getMSTime();
                player->StopMoving();
                player->GetMotionMaster()->MoveIdle();
                ++itr;
                continue;
            }
            SpellCastResult const result = player->CastSpell(player, job.spellId, false);
            if (result == SPELL_CAST_OK)
                job.casting = true;
            else
            {
                why = result == SPELL_FAILED_REAGENTS ? "out of materials" : Acore::StringFormat("cast failed ({})", uint32(result));
                finished = true;
            }
        }

        if (finished)
        {
            Job const done = job;
            itr = _jobs.erase(itr);
            Finish(guid, done, why);
        }
        else
            ++itr;
    }
}
