/*
 * Hooks and commands of the guild perks (PerkService, docs/guild-management.md "Guild perks").
 *
 * The value hooks (experience, gold, drops, speed, cast times, breath, falls) fire on map threads and only read the
 * perk snapshot; the hooks that act (kills, loot, logins) queue for the world update, which runs after the roster and
 * achievement scripts (registered later).
 *
 * Hooks used:
 *   PlayerScript  OnPlayerGiveXP, OnPlayerBeforeLootMoney, OnPlayerLootItem, OnPlayerCreatureKill, OnPlayerLogin;
 *                 with core patch CP-8: OnPlayerUpdateSpeed, OnPlayerSpellCastTime, OnPlayerCheckWaterBreathing,
 *                 OnPlayerFallDamage
 *   GlobalScript  OnItemRoll
 *   WorldScript   OnStartup, OnUpdate
 *
 * Commands (they join the ".ga" tree of the other command scripts):
 *   .ga perks [character]                  the guild's points and perks (GM; also console with a character)
 *   .ga perk buy <perk>                    buy the next rank for your guild (GM; perk = key or id)
 *   .ga admin perk buy <leader> <perk>     the same for a leader's guild (administrator, also console / SOAP)
 *   .ga admin perk reset <leader>          drop every rank and GM grant, every point back (administrator, also console)
 *   .ga admin perk grant <leader> <points> guild points on top of the achievements' (administrator, also console)
 *   .ga admin perk check <character>       what the perks do for that character right now (administrator, console)
 */

#include "AchievementService.h"
#include "Chat.h"
#include "CommandScript.h"
#include "DBCStores.h"
#include "GuildAdventureConfig.h"
#include "GuildAdventureMgr.h"
#include "LootMgr.h"
#include "ObjectMgr.h"
#include "PerkService.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include <mutex>
#include <vector>

using namespace Acore::ChatCommands;
using namespace GuildAdventure;

namespace
{
    enum class PerkHookType : uint8
    {
        Login,
        Kill,
        Loot
    };

    struct PerkHookEvent
    {
        PerkHookType type = PerkHookType::Login;
        ObjectGuid guid;
        uint32 value = 0; // item entry
    };

    std::mutex sPerkHookLock;
    std::vector<PerkHookEvent> sPerkHookEvents;

    void QueuePerkEvent(PerkHookType type, ObjectGuid guid, uint32 value = 0)
    {
        std::lock_guard<std::mutex> lock(sPerkHookLock);
        sPerkHookEvents.push_back({ type, guid, value });
    }

    void ProcessPerkEvents()
    {
        std::vector<PerkHookEvent> events;
        {
            std::lock_guard<std::mutex> lock(sPerkHookLock);
            events.swap(sPerkHookEvents);
        }
        for (PerkHookEvent const& event : events)
        {
            switch (event.type)
            {
                case PerkHookType::Login: sPerkService.OnCharacterInWorld(event.guid); break;
                case PerkHookType::Kill:  sPerkService.OnKill(event.guid); break;
                case PerkHookType::Loot:  sPerkService.OnLooted(event.guid, event.value); break;
            }
        }
    }

    float Factor(ObjectGuid guid, PerkKind kind)
    {
        return 1.0f + sPerkService.GetValue(guid, kind) / 100.0f;
    }

    // Spells for ".ga admin perk check": Linen Bandage (First Aid), Mining, Herb Gathering, Skinning (apprentice ranks).
    constexpr uint32 SampleCraftSpell = 3275;
    constexpr uint32 SampleGatherSpells[] = { 2575, 2366, 8613 };
}

class GuildAdventurePerkPlayerScript : public PlayerScript
{
public:
    GuildAdventurePerkPlayerScript() : PlayerScript("GuildAdventurePerkPlayerScript", {
        PLAYERHOOK_ON_GIVE_EXP,
        PLAYERHOOK_ON_BEFORE_LOOT_MONEY,
        PLAYERHOOK_ON_LOOT_ITEM,
        PLAYERHOOK_ON_CREATURE_KILL,
        PLAYERHOOK_ON_LOGIN,
#ifdef GUILDADVENTURE_CP8_PERK_HOOKS
        PLAYERHOOK_ON_UPDATE_SPEED,
        PLAYERHOOK_ON_SPELL_CAST_TIME,
        PLAYERHOOK_ON_CHECK_WATER_BREATHING,
        PLAYERHOOK_ON_FALL_DAMAGE
#endif
    }) { }

    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* /*victim*/, uint8 /*xpSource*/) override
    {
        if (player && amount)
            amount = uint32(amount * Factor(player->GetGUID(), PerkKind::Experience) + 0.5f);
    }

    void OnPlayerBeforeLootMoney(Player* player, Loot* loot) override
    {
        if (player && loot && loot->gold)
            loot->gold = uint32(loot->gold * Factor(player->GetGUID(), PerkKind::Gold) + 0.5f);
    }

    void OnPlayerLootItem(Player* player, Item* item, uint32 /*count*/, ObjectGuid /*lootguid*/) override
    {
        if (player && item && IsModuleEnabled() && sPerkService.Has(player->GetGUID(), PerkKind::ExtraMaterial))
            QueuePerkEvent(PerkHookType::Loot, player->GetGUID(), item->GetEntry());
    }

    void OnPlayerCreatureKill(Player* killer, Creature* /*killed*/) override
    {
        if (killer && IsModuleEnabled() && sPerkService.Has(killer->GetGUID(), PerkKind::KillRegen))
            QueuePerkEvent(PerkHookType::Kill, killer->GetGUID());
    }

    void OnPlayerLogin(Player* player) override
    {
        if (player && IsModuleEnabled())
            QueuePerkEvent(PerkHookType::Login, player->GetGUID());
    }

#ifdef GUILDADVENTURE_CP8_PERK_HOOKS
    void OnPlayerUpdateSpeed(Player* player, UnitMoveType moveType, float& rate) override
    {
        switch (moveType)
        {
            case MOVE_RUN:
            case MOVE_RUN_BACK:
            case MOVE_SWIM:
            case MOVE_SWIM_BACK:
            case MOVE_FLIGHT:
            case MOVE_FLIGHT_BACK:
                rate *= Factor(player->GetGUID(), PerkKind::MoveSpeed);
                break;
            default:
                break;
        }
    }

    void OnPlayerSpellCastTime(Player* player, SpellInfo const* spellInfo, int32& castTime) override
    {
        if (castTime <= 0)
            return;
        PerkKind kind = PerkKind::Count;
        if (PerkService::IsCraftSpell(spellInfo))
            kind = PerkKind::CraftSpeed;
        else if (PerkService::IsGatherSpell(spellInfo))
            kind = PerkKind::GatherSpeed;
        else
            return;
        float const pct = sPerkService.GetValue(player->GetGUID(), kind);
        if (pct > 0.0f)
            castTime = int32(castTime * 100.0f / (100.0f + pct));
    }

    void OnPlayerCheckWaterBreathing(Player* player, bool& breathes) override
    {
        if (sPerkService.Has(player->GetGUID(), PerkKind::WaterBreathing))
            breathes = true;
    }

    void OnPlayerFallDamage(Player* player, uint32& damage) override
    {
        if (damage && sPerkService.Has(player->GetGUID(), PerkKind::NoFallDamage))
            damage = 0;
    }
#endif
};

class GuildAdventurePerkGlobalScript : public GlobalScript
{
public:
    GuildAdventurePerkGlobalScript() : GlobalScript("GuildAdventurePerkGlobalScript", {
        GLOBALHOOK_ON_ITEM_ROLL
    }) { }

    bool OnItemRoll(Player const* player, LootStoreItem const* item, float& chance, Loot& /*loot*/, LootStore const& /*store*/) override
    {
        // Items up to rare (not references, not quest drops, not what drops anyway).
        if (!player || !item || item->reference || item->needs_quest || chance <= 0.0f || chance >= 100.0f)
            return true;
        float const pct = sPerkService.GetValue(player->GetGUID(), PerkKind::DropChance);
        if (pct <= 0.0f)
            return true;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item->itemid);
        if (proto && proto->Quality <= ITEM_QUALITY_RARE)
            chance = std::min(100.0f, chance * (1.0f + pct / 100.0f));
        return true;
    }
};

class GuildAdventurePerkWorldScript : public WorldScript
{
public:
    GuildAdventurePerkWorldScript() : WorldScript("GuildAdventurePerkWorldScript", {
        WORLDHOOK_ON_STARTUP,
        WORLDHOOK_ON_UPDATE
    }) { }

    void OnStartup() override
    {
        // After the roster and achievement scripts (registered first), which load the guilds and their achievements.
        sPerkService.LoadTemplates();
        sPerkService.Load();
        _started = true;
    }

    void OnUpdate(uint32 diff) override
    {
        if (!_started || !IsModuleEnabled())
            return;
        ProcessPerkEvents();
        sPerkService.Update(diff);
    }

private:
    bool _started = false;
};

class GuildAdventurePerkCommandScript : public CommandScript
{
public:
    GuildAdventurePerkCommandScript() : CommandScript("GuildAdventurePerkCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable perkTable =
        {
            { "buy", HandlePerkBuy, SEC_GAMEMASTER, Console::No }
        };
        static ChatCommandTable adminPerkTable =
        {
            { "buy",   HandleAdminPerkBuy,   SEC_ADMINISTRATOR, Console::Yes },
            { "reset", HandleAdminPerkReset, SEC_ADMINISTRATOR, Console::Yes },
            { "grant", HandleAdminPerkGrant, SEC_ADMINISTRATOR, Console::Yes },
            { "check", HandleAdminPerkCheck, SEC_ADMINISTRATOR, Console::Yes }
        };
        static ChatCommandTable adminTable =
        {
            { "perk", adminPerkTable }
        };
        static ChatCommandTable gaTable =
        {
            { "perks", HandlePerks, SEC_GAMEMASTER, Console::Yes },
            { "perk",  perkTable },
            { "admin", adminTable }
        };
        static ChatCommandTable commandTable =
        {
            { "ga", gaTable }
        };
        return commandTable;
    }

    static AdventureGuild* GuildOf(ChatHandler* handler, Optional<PlayerIdentifier> const& target)
    {
        if (!IsModuleEnabled())
        {
            handler->SendErrorMessage("Guild Adventure is disabled (GuildAdventure.Enable = 0).", false);
            return nullptr;
        }
        AdventureGuild* guild = nullptr;
        if (target)
            guild = sAchievementService.FindGuildOf(target->GetGUID());
        else if (WorldSession* session = handler->GetSession())
            guild = sGuildAdventureMgr.FindGuild(session->GetAccountId());
        if (!guild)
            handler->SendErrorMessage(target ? Acore::StringFormat("{} is neither the leader nor a bot of an adventure guild.",
                target->GetName()) : std::string("No adventure guild (name a guild character from the console)."), false);
        return guild;
    }

    static PerkTemplate const* FindPerk(ChatHandler* handler, std::string const& name)
    {
        PerkTemplate const* perk = sPerkService.FindTemplate(name);
        if (!perk)
            if (Optional<uint32> id = Acore::StringTo<uint32>(name))
                perk = sPerkService.FindTemplate(*id);
        if (!perk)
        {
            std::string keys;
            for (PerkTemplate const& entry : sPerkService.GetTemplates())
                keys += (keys.empty() ? "" : ", ") + entry.key;
            handler->SendErrorMessage(Acore::StringFormat("Unknown perk '{}'. Perks: {}.", name, keys), false);
        }
        return perk;
    }

    static bool HandlePerks(ChatHandler* handler, Optional<PlayerIdentifier> target)
    {
        AdventureGuild* guild = GuildOf(handler, target);
        if (!guild)
            return false;
        uint32 const owner = guild->ownerAccount;
        handler->PSendSysMessage("Guild perks of account {}: {} guild points ({} earned from achievements, {} granted, {} spent).", owner,
            sPerkService.GetAvailable(*guild), sPerkService.GetEarned(*guild), sPerkService.GetGranted(owner), sPerkService.GetSpent(owner));
        for (PerkTemplate const& perk : sPerkService.GetTemplates())
        {
            uint8 const rank = sPerkService.GetRank(owner, perk.id);
            std::string next = rank < perk.MaxRank() ?
                Acore::StringFormat("next {} points: {}", perk.costs[rank], perk.Describe(rank + 1)) : std::string("highest rank");
            if (!PerkService::IsAvailable(perk.kind))
                next = "needs core patch CP-8";
            handler->PSendSysMessage("  {} {} ({}): rank {}/{}{}; {}", perk.id, perk.name, perk.key, uint32(rank), uint32(perk.MaxRank()),
                rank ? Acore::StringFormat(", {}", perk.Describe(rank)) : std::string(), next);
        }
        return true;
    }

    static bool Buy(ChatHandler* handler, AdventureGuild& guild, std::string const& name)
    {
        PerkTemplate const* perk = FindPerk(handler, name);
        if (!perk)
            return false;
        std::string code;
        std::string text;
        if (!sPerkService.Buy(guild, perk->id, code, text))
        {
            handler->SendErrorMessage(Acore::StringFormat("{} ({})", text, code), false);
            return false;
        }
        uint8 const rank = sPerkService.GetRank(guild.ownerAccount, perk->id);
        handler->PSendSysMessage("Bought {} rank {}: {}. {} guild points left.", perk->name, uint32(rank), perk->Describe(rank),
            sPerkService.GetAvailable(guild));
        return true;
    }

    static bool HandlePerkBuy(ChatHandler* handler, std::string name)
    {
        AdventureGuild* guild = GuildOf(handler, std::nullopt);
        return guild && Buy(handler, *guild, name);
    }

    static bool HandleAdminPerkBuy(ChatHandler* handler, PlayerIdentifier leader, std::string name)
    {
        AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfLeader(leader.GetGUID());
        if (!guild)
        {
            handler->SendErrorMessage(Acore::StringFormat("{} leads no adventure guild.", leader.GetName()), false);
            return false;
        }
        return Buy(handler, *guild, name);
    }

    static bool HandleAdminPerkReset(ChatHandler* handler, PlayerIdentifier leader)
    {
        AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfLeader(leader.GetGUID());
        if (!guild)
        {
            handler->SendErrorMessage(Acore::StringFormat("{} leads no adventure guild.", leader.GetName()), false);
            return false;
        }
        sPerkService.Reset(*guild);
        handler->PSendSysMessage("The perks of {}'s guild were reset: {} guild points.", leader.GetName(), sPerkService.GetAvailable(*guild));
        return true;
    }

    static bool HandleAdminPerkGrant(ChatHandler* handler, PlayerIdentifier leader, uint32 points)
    {
        AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfLeader(leader.GetGUID());
        if (!guild)
        {
            handler->SendErrorMessage(Acore::StringFormat("{} leads no adventure guild.", leader.GetName()), false);
            return false;
        }
        sPerkService.Grant(*guild, points);
        handler->PSendSysMessage("{}'s guild got {} guild points: {} now.", leader.GetName(), points, sPerkService.GetAvailable(*guild));
        return true;
    }

    static bool HandleAdminPerkCheck(ChatHandler* handler, PlayerIdentifier target)
    {
        Player* player = target.GetConnectedPlayer();
        if (!player || !player->IsInWorld())
        {
            handler->SendErrorMessage(Acore::StringFormat("{} is not in the world.", target.GetName()), false);
            return false;
        }
        if (!sAchievementService.FindGuildOf(player->GetGUID()))
        {
            handler->SendErrorMessage(Acore::StringFormat("{} is neither the leader nor a bot of an adventure guild.", target.GetName()), false);
            return false;
        }

        PerkEffects const effects = sPerkService.GetEffects(player->GetGUID());
        std::string owned;
        for (std::size_t i = 0; i < PerkKindCount; ++i)
            if (effects.value[i] > 0.0f)
                owned += Acore::StringFormat("{}{}={}", owned.empty() ? "" : ", ", ToKey(PerkKind(i)), effects.value[i]);
        handler->PSendSysMessage("{}: perks {}.", player->GetName(), owned.empty() ? "none" : owned);
        handler->PSendSysMessage("{}: run speed rate {:.3f}, swim {:.3f}.", player->GetName(), player->GetSpeedRate(MOVE_RUN),
            player->GetSpeedRate(MOVE_SWIM));

        if (SpellInfo const* spell = sSpellMgr->GetSpellInfo(SampleCraftSpell))
            handler->PSendSysMessage("{}: cast time {} {} ms (base {} ms).", player->GetName(), spell->SpellName[LOCALE_enUS],
                spell->CalcCastTime(player), spell->CastTimeEntry ? spell->CastTimeEntry->CastTime : 0);
        for (uint32 id : SampleGatherSpells)
            if (SpellInfo const* spell = sSpellMgr->GetSpellInfo(id))
                handler->PSendSysMessage("{}: cast time {} {} ms (base {} ms).", player->GetName(), spell->SpellName[LOCALE_enUS],
                    spell->CalcCastTime(player), spell->CastTimeEntry ? spell->CastTimeEntry->CastTime : 0);

        uint32 known = 0;
        uint32 usable = 0;
        for (uint32 node = 1; node < sTaxiNodesStore.GetNumRows(); ++node)
            if (sTaxiNodesStore.LookupEntry(node) && sObjectMgr->GetTaxiMountDisplayId(node, player->GetTeamId()))
            {
                ++usable;
                if (player->m_taxi.IsTaximaskNodeKnown(node))
                    ++known;
            }
        handler->PSendSysMessage("{}: knows {} of {} flight paths of its faction; no-reagent mask {:08X}.", player->GetName(), known,
            usable, player->GetUInt32Value(PLAYER_NO_REAGENT_COST_1));

        uint32 xp = 1000;
        sScriptMgr->OnPlayerGiveXP(player, xp, nullptr, 0);
        Loot loot;
        loot.gold = 1000;
        sScriptMgr->OnPlayerBeforeLootMoney(player, &loot);
        bool breathes = false;
        uint32 fall = 1000;
#ifdef GUILDADVENTURE_CP8_PERK_HOOKS
        sScriptMgr->OnPlayerCheckWaterBreathing(player, breathes);
        sScriptMgr->OnPlayerFallDamage(player, fall);
#endif
        handler->PSendSysMessage("{}: 1000 xp -> {}, 1000 copper -> {}, breathes under water {}, a fall of 1000 damage -> {}.",
            player->GetName(), xp, loot.gold, breathes ? "yes" : "no", fall);
        return true;
    }
};

void AddGuildAdventurePerkScripts()
{
    new GuildAdventurePerkPlayerScript();
    new GuildAdventurePerkGlobalScript();
    new GuildAdventurePerkWorldScript();
    new GuildAdventurePerkCommandScript();
}
