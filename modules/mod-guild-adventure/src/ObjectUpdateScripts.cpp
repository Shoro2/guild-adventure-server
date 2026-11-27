/*
 * Object updates for characters the AI plays on a client session (watch mode).
 *
 * Map::SendObjectUpdates asks every AllMapScript whether a player gets the SMSG_UPDATE_OBJECT value blocks collected
 * for it (health, max health, flags, a creature's death, ...) and sends none when one script says no. mod-playerbots
 * says no for every player that carries a PlayerbotAI and is not one of its own self bots (PlayerbotsAllMapScript in
 * src/Script/Playerbots.cpp): a bot has no client to show them. Watch mode (GuildBotHolder::StartWatch) puts a
 * PlayerbotAI without a master on a character that stays on its owner's client session, so that rule cut the watching
 * client off from every field change of its character and of everything around it. Seen by the owner (2026-11-12): the
 * health bar stayed full while mana moved (Unit::SetPower also sends SMSG_POWER_UPDATE, a packet of its own), creatures
 * kept the health they had when they came into view and never died on the screen.
 *
 * A second script cannot overrule a "no" (CALL_ENABLED_BOOLEAN_HOOKS stops at the first one), so at startup this file
 * takes playerbots' script out of the hook's list and stands in for it: a player on a session with a client (not
 * headless) gets its updates, every other player gets playerbots' own answer. Bots on their headless home sessions
 * keep playerbots' saving, and playerbots' self bots are on client sessions anyway. No playerbots or core change: the
 * hook list is a public static of the core's ScriptRegistry, changed once on the world thread before the maps update.
 * If playerbots' script is not found (renamed upstream), nothing is replaced and the module logs an error.
 *
 * The same stand-in for a second hook, PlayerScript::OnPlayerAfterUpdate: playerbots updates the AI of every player
 * that has one there. A placeholder AI (GuildBotHolder::StartPlaceholder: the owner plays a character by hand inside
 * a dungeon group of bots) must exist and never run: playerbots makes a party member without an AI the master of
 * every bot in that party (PlayerbotAI::UpdateAIGroupMaster / FindNewMaster), which would take the group from its
 * tank. With the AI object there the owner's character counts as a bot for that rule; this file keeps its update
 * away, so nothing of the AI ever acts (GuildAdventure::IdleAI).
 */

#include "GuildBotHolder.h"
#include "Log.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldSession.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <unordered_set>

namespace
{
    // The name PlayerbotsAllMapScript registers itself with (mod-playerbots src/Script/Playerbots.cpp).
    char const* const PlayerbotsMapScriptName = "PlayerbotsAllMapScript";
    // The same for its player script, whose OnPlayerAfterUpdate updates every AI.
    char const* const PlayerbotsPlayerScriptName = "PlayerbotsPlayerScript";

    // Characters whose playerbots AI is a placeholder that never runs. Written on the world thread, read by the map
    // threads (Player::Update) and the world thread; the count is the fast way out for the usual case of none.
    std::atomic<uint32> idleCount{0};
    std::atomic<bool> idleAvailable{false};
    std::shared_mutex idleMutex;
    std::unordered_set<ObjectGuid> idleSet;
}

namespace GuildAdventure::IdleAI
{
    bool Available() { return idleAvailable.load(std::memory_order_relaxed); }

    void Set(ObjectGuid guid, bool idle)
    {
        std::unique_lock lock(idleMutex);
        if (idle)
            idleSet.insert(guid);
        else
            idleSet.erase(guid);
        idleCount.store(uint32(idleSet.size()), std::memory_order_relaxed);
    }

    bool IsSet(ObjectGuid guid)
    {
        if (!idleCount.load(std::memory_order_relaxed))
            return false;
        std::shared_lock lock(idleMutex);
        return idleSet.find(guid) != idleSet.end();
    }
}

// Stands in for playerbots' OnPlayerAfterUpdate (see the file comment): every player gets playerbots' own update,
// except a character whose AI is a placeholder.
class GuildAdventureAfterUpdateScript : public PlayerScript
{
public:
    GuildAdventureAfterUpdateScript() : PlayerScript("GuildAdventureAfterUpdateScript", {
        PLAYERHOOK_ON_AFTER_UPDATE
    }) { }

    // Map threads. Until TakeOver ran, playerbots' script is still in the list and updates for itself.
    void OnPlayerAfterUpdate(Player* player, uint32 diff) override
    {
        if (!_playerbots || GuildAdventure::IdleAI::IsSet(player->GetGUID()))
            return;
        _playerbots->OnPlayerAfterUpdate(player, diff);
    }

    // World thread, once at startup. True when playerbots' script was replaced.
    bool TakeOver()
    {
        if (_playerbots)
            return true;

        auto& hooks = ScriptRegistry<PlayerScript>::EnabledHooks;
        if (hooks.size() <= PLAYERHOOK_ON_AFTER_UPDATE)
            return false;

        auto& scripts = hooks[PLAYERHOOK_ON_AFTER_UPDATE];
        auto itr = std::find_if(scripts.begin(), scripts.end(), [](PlayerScript const* script)
        {
            return script && script->GetName() == PlayerbotsPlayerScriptName;
        });
        if (itr == scripts.end())
            return false;

        _playerbots = *itr;
        scripts.erase(itr);
        idleAvailable.store(true, std::memory_order_relaxed);
        return true;
    }

private:
    PlayerScript* _playerbots = nullptr;
};

class GuildAdventureObjectUpdateScript : public AllMapScript
{
public:
    GuildAdventureObjectUpdateScript() : AllMapScript("GuildAdventureObjectUpdateScript", {
        ALLMAPHOOK_CAN_SEND_OBJECT_UPDATES_TO_PLAYER
    }) { }

    // Map threads. Until TakeOverPlayerbotsFilter ran, playerbots' script is still in the list and decides for itself.
    bool CanSendObjectUpdatesToPlayer(Map* map, Player* player) override
    {
        WorldSession* session = player->GetSession();
        if (session && !session->IsHeadless())
            return true;

        return !_playerbots || _playerbots->CanSendObjectUpdatesToPlayer(map, player);
    }

    // World thread, once at startup (before the first map update). True when playerbots' script was replaced.
    bool TakeOverPlayerbotsFilter()
    {
        if (_playerbots)
            return true;

        auto& hooks = ScriptRegistry<AllMapScript>::EnabledHooks;
        if (hooks.size() <= ALLMAPHOOK_CAN_SEND_OBJECT_UPDATES_TO_PLAYER)
            return false;

        auto& scripts = hooks[ALLMAPHOOK_CAN_SEND_OBJECT_UPDATES_TO_PLAYER];
        auto itr = std::find_if(scripts.begin(), scripts.end(), [](AllMapScript const* script)
        {
            return script && script->GetName() == PlayerbotsMapScriptName;
        });
        if (itr == scripts.end())
            return false;

        _playerbots = *itr;
        scripts.erase(itr);
        return true;
    }

private:
    AllMapScript* _playerbots = nullptr;
};

class GuildAdventureObjectUpdateWorldScript : public WorldScript
{
public:
    GuildAdventureObjectUpdateWorldScript(GuildAdventureObjectUpdateScript* filter, GuildAdventureAfterUpdateScript* afterUpdate)
        : WorldScript("GuildAdventureObjectUpdateWorldScript", { WORLDHOOK_ON_STARTUP }), _filter(filter), _afterUpdate(afterUpdate) { }

    void OnStartup() override
    {
        if (_filter->TakeOverPlayerbotsFilter())
            LOG_INFO("module", "mod-guild-adventure: object updates reach every character on a client session, also one "
                "the AI plays (watch mode); bots keep playerbots' filter");
        else
            LOG_ERROR("module", "mod-guild-adventure: playerbots' object update filter ({}) not found; a watched "
                "character's client gets no health or death updates", PlayerbotsMapScriptName);

        if (_afterUpdate->TakeOver())
            LOG_INFO("module", "mod-guild-adventure: playerbots' AI update runs through the module (a character its owner "
                "plays by hand inside a dungeon group keeps a placeholder AI that never runs)");
        else
            LOG_ERROR("module", "mod-guild-adventure: playerbots' player script ({}) not found; a character its owner plays "
                "by hand cannot be in a dungeon group of bots", PlayerbotsPlayerScriptName);
    }

private:
    GuildAdventureObjectUpdateScript* _filter;
    GuildAdventureAfterUpdateScript* _afterUpdate;
};

void AddGuildAdventureObjectUpdateScripts()
{
    auto* filter = new GuildAdventureObjectUpdateScript();
    auto* afterUpdate = new GuildAdventureAfterUpdateScript();
    new GuildAdventureObjectUpdateWorldScript(filter, afterUpdate);
}
