/*
 * Readiness: see Readiness.h.
 *
 * Commands (they join the ".ga" tree of the other command scripts):
 *   .ga readiness <character>     the gear level of a guild character in the world and its readiness for every dungeon
 *                                 in its reach (GM; console and SOAP)
 */

#include "Readiness.h"
#include "DungeonRoles.h"
#include "Chat.h"
#include "CommandScript.h"
#include "DBCStores.h"
#include "Dungeons.h"
#include "GuildAdventureConfig.h"
#include "GuildAdventureMgr.h"
#include "GuildAdventureSettings.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "LFG.h"
#include "Log.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "World.h"
#include <algorithm>

using namespace Acore::ChatCommands;
using namespace GuildAdventure;

namespace
{
    // What a classic raid asks for beyond "level 60 in what the dungeons gave" (map id, gear level): the dungeon blues
    // for Molten Core, Zul'Gurub and the Ruins, Molten Core's epics for Blackwing Lair, Blackwing Lair's for the Temple.
    struct RaidGear { uint32 mapId; uint16 gearLevel; };
    constexpr RaidGear RaidGearLevels[] =
    {
        { 409, 58 },  // Molten Core
        { 309, 60 },  // Zul'Gurub
        { 509, 60 },  // Ruins of Ahn'Qiraj
        { 469, 64 },  // Blackwing Lair
        { 531, 68 }   // Temple of Ahn'Qiraj
    };

    // The dungeons inside a capital (map id, TeamId of the side that gets in).
    struct CapitalDungeon { uint32 mapId; uint8 team; };
    constexpr CapitalDungeon CapitalDungeons[] =
    {
        { 34, 0 },    // Stormwind Stockade
        { 389, 1 }    // Ragefire Chasm
    };

    // What a dungeon's target level is below the gear level it asks for.
    constexpr uint8 GearBelowTarget = 2;
    // The level part at the dungeon's lowest level.
    constexpr uint32 LevelPartAtMin = 60;

    uint8 ExpansionOfLevelCap()
    {
        uint32 const cap = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);
        return cap <= 60 ? 0 : cap <= 70 ? 1 : 2;
    }

    // LFGDungeons.dbc flags: a seasonal entry (lfg::LFG_FLAG_SEASONAL).
    constexpr uint32 LfgFlagSeasonal = 0x4;

    struct WeightedSlot { uint8 slot; uint8 weight; uint8 fromLevel; };

    // The places that count for a class at a level (Readiness.h).
    std::vector<WeightedSlot> SlotsOf(uint8 classId)
    {
        std::vector<WeightedSlot> slots =
        {
            { EQUIPMENT_SLOT_CHEST, 1, 0 }, { EQUIPMENT_SLOT_LEGS, 1, 0 }, { EQUIPMENT_SLOT_FEET, 1, 0 },
            { EQUIPMENT_SLOT_HANDS, 1, 0 }, { EQUIPMENT_SLOT_WAIST, 1, 0 }, { EQUIPMENT_SLOT_WRISTS, 1, 0 },
            { EQUIPMENT_SLOT_BACK, 1, 10 }, { EQUIPMENT_SLOT_SHOULDERS, 1, 20 }, { EQUIPMENT_SLOT_HEAD, 1, 30 }
        };
        switch (classId)
        {
            case CLASS_HUNTER:
                slots.push_back({ EQUIPMENT_SLOT_RANGED, 2, 0 });
                slots.push_back({ EQUIPMENT_SLOT_MAINHAND, 1, 0 });
                break;
            case CLASS_MAGE:
            case CLASS_PRIEST:
            case CLASS_WARLOCK:
                slots.push_back({ EQUIPMENT_SLOT_MAINHAND, 1, 0 });
                slots.push_back({ EQUIPMENT_SLOT_RANGED, 1, 10 }); // a wand
                break;
            case CLASS_DRUID:
                slots.push_back({ EQUIPMENT_SLOT_MAINHAND, 1, 0 });
                break;
            default:
                slots.push_back({ EQUIPMENT_SLOT_MAINHAND, 2, 0 });
                break;
        }
        return slots;
    }
}

Readiness& Readiness::Instance()
{
    static Readiness instance;
    return instance;
}

void Readiness::Load()
{
    _dungeons.clear();
    uint8 const expansion = ExpansionOfLevelCap();
    bool const lock = GetSettings().zonesWithinLevelCap;
    for (LFGDungeonEntry const* entry : sLFGDungeonStore)
    {
        if (!entry || (entry->TypeID != lfg::LFG_TYPE_DUNGEON && entry->TypeID != lfg::LFG_TYPE_RAID) || entry->Difficulty != 0)
            continue;
        if (lock && entry->ExpansionLevel > expansion)
            continue;
        // A holiday boss (the Headless Horseman in the Scarlet Monastery, the Crown Chemical Co. in Shadowfang Keep,
        // Coren Direbrew in the Blackrock Depths) is an entry of the dungeon finder on another dungeon's map, no
        // dungeon to run: it would also make that map look like one that holds several dungeons (Dungeons.cpp).
        if (entry->Flags & LfgFlagSeasonal)
            continue;
        MapEntry const* map = sMapStore.LookupEntry(entry->MapID);
        if (!map || !map->IsDungeon() || !entry->Name[0] || !entry->Name[0][0])
            continue;

        DungeonInfo dungeon;
        dungeon.id = entry->ID;
        dungeon.mapId = entry->MapID;
        dungeon.name = entry->Name[0];
        dungeon.minLevel = uint8(std::min<uint32>(entry->MinLevel, 255));
        dungeon.maxLevel = uint8(std::min<uint32>(std::max(entry->MaxLevel, entry->MinLevel), 255));
        uint32 const target = entry->TargetLevel ? entry->TargetLevel : (entry->MinLevel + entry->MaxLevel) / 2;
        dungeon.targetLevel = uint8(std::clamp<uint32>(target, dungeon.minLevel, dungeon.maxLevel));
        dungeon.raid = map->IsRaid();
        MapDifficulty const* difficulty = GetMapDifficultyData(entry->MapID, Difficulty(0));
        uint32 const players = difficulty && difficulty->maxPlayers ? difficulty->maxPlayers : map->maxPlayers;
        dungeon.size = uint8(players ? std::min<uint32>(players, 40) : (dungeon.raid ? 40 : 5));
        dungeon.gearLevel = uint16(std::max<int32>(int32(dungeon.targetLevel) - GearBelowTarget, 5));
        for (RaidGear const& raid : RaidGearLevels)
            if (raid.mapId == dungeon.mapId)
                dungeon.gearLevel = raid.gearLevel;
        for (CapitalDungeon const& capital : CapitalDungeons)
            if (capital.mapId == dungeon.mapId)
                dungeon.team = capital.team;
        _dungeons.push_back(std::move(dungeon));
    }
    std::sort(_dungeons.begin(), _dungeons.end(), [](DungeonInfo const& a, DungeonInfo const& b)
    {
        if (a.raid != b.raid)
            return !a.raid;
        if (a.minLevel != b.minLevel)
            return a.minLevel < b.minLevel;
        return a.id < b.id;
    });
    uint32 const raids = uint32(std::count_if(_dungeons.begin(), _dungeons.end(), [](DungeonInfo const& d) { return d.raid; }));
    LOG_INFO("module", "mod-guild-adventure: readiness: {} dungeons and {} raids within the content of level cap {}",
        uint32(_dungeons.size()) - raids, raids, sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL));
}

DungeonInfo const* Readiness::Find(uint32 id) const
{
    for (DungeonInfo const& dungeon : _dungeons)
        if (dungeon.id == id)
            return &dungeon;
    return nullptr;
}

std::vector<DungeonInfo const*> Readiness::InReach(uint8 level, uint8 team) const
{
    std::vector<DungeonInfo const*> list;
    for (DungeonInfo const& dungeon : _dungeons)
    {
        if (dungeon.team < 2 && dungeon.team != team)
            continue;
        if (dungeon.raid ? level < dungeon.minLevel : uint32(level) + 2 < dungeon.minLevel)
            continue;
        if (!dungeon.raid && level > uint32(dungeon.maxLevel) + 5)
            continue;
        list.push_back(&dungeon);
    }
    return list;
}

uint16 Readiness::GearLevel(Player const* player)
{
    return GearLevelWith(player, [player](uint8 slot)
    {
        Item const* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        return item ? item->GetTemplate()->ItemLevel : 0u;
    });
}

uint16 Readiness::GearLevelWith(Player const* player, std::function<uint32(uint8 slot)> const& itemLevelAt)
{
    return GearLevelOf(player->getClass(), player->GetLevel(), itemLevelAt);
}

uint16 Readiness::GearLevelOf(uint8 classId, uint8 level, std::function<uint32(uint8 slot)> const& itemLevelAt)
{
    uint32 sum = 0, weights = 0;
    for (WeightedSlot const& place : SlotsOf(classId))
    {
        if (level < place.fromLevel)
            continue;
        weights += place.weight;
        sum += itemLevelAt(place.slot) * place.weight;
    }
    return weights ? uint16((sum + weights / 2) / weights) : 0;
}

ReadinessVerdict Readiness::Judge(Player const* player, DungeonInfo const& dungeon)
{
    return JudgeWith(player, dungeon, GearLevel(player));
}

ReadinessVerdict Readiness::JudgeWith(Player const* player, DungeonInfo const& dungeon, uint16 gearLevel)
{
    ReadinessVerdict verdict;
    verdict.gearLevel = gearLevel;
    uint8 const level = player->GetLevel();
    if (level < dungeon.minLevel)
    {
        verdict.why = Acore::StringFormat("below level {}", uint32(dungeon.minLevel));
        return verdict;
    }
    uint32 levelPart = 100;
    if (level < dungeon.targetLevel && dungeon.targetLevel > dungeon.minLevel)
        levelPart = LevelPartAtMin + (100 - LevelPartAtMin) * uint32(level - dungeon.minLevel) / uint32(dungeon.targetLevel - dungeon.minLevel);
    uint32 const gearPart = dungeon.gearLevel ? std::min<uint32>(100, uint32(verdict.gearLevel) * 100 / dungeon.gearLevel) : 100;
    verdict.levelPart = uint8(levelPart);
    verdict.gearPart = uint8(gearPart);
    verdict.percent = uint8(levelPart * gearPart / 100);
    if (levelPart < 100)
        verdict.why = Acore::StringFormat("level {} of {}", uint32(level), uint32(dungeon.targetLevel));
    if (gearPart < 100)
        verdict.why += Acore::StringFormat("{}gear level {} of {}", verdict.why.empty() ? "" : ", ", uint32(verdict.gearLevel),
            uint32(dungeon.gearLevel));
    return verdict;
}

std::string Readiness::Summary(Player const* player) const
{
    std::string text;
    for (DungeonInfo const* dungeon : InReach(player->GetLevel(), uint8(player->GetTeamId())))
    {
        if (!text.empty())
            text += ',';
        text += Acore::StringFormat("{}:{}", dungeon->id, uint32(Judge(player, *dungeon).percent));
    }
    return text;
}

std::vector<std::string> Readiness::Describe(Player const* player) const
{
    std::vector<std::string> lines;
    lines.push_back(Acore::StringFormat("{} (level {}): gear level {} (the average item level of its armour and weapons).",
        player->GetName(), uint32(player->GetLevel()), uint32(GearLevel(player))));
    std::vector<DungeonInfo const*> const reach = InReach(player->GetLevel(), uint8(player->GetTeamId()));
    if (reach.empty())
        lines.push_back("  No dungeon in its reach yet.");
    for (DungeonInfo const* dungeon : reach)
    {
        ReadinessVerdict const verdict = Judge(player, *dungeon);
        lines.push_back(Acore::StringFormat("  {} {} (id {}, level {}-{}, {} players, asks for gear level {}): {}%{}",
            dungeon->raid ? "raid" : "dungeon", dungeon->name, dungeon->id, uint32(dungeon->minLevel), uint32(dungeon->maxLevel),
            uint32(dungeon->size), uint32(dungeon->gearLevel), uint32(verdict.percent),
            verdict.why.empty() ? "" : " (" + verdict.why + ")"));
    }
    return lines;
}

namespace
{
    class GuildAdventureReadinessWorldScript : public WorldScript
    {
    public:
        GuildAdventureReadinessWorldScript() : WorldScript("GuildAdventureReadinessWorldScript", {
            WORLDHOOK_ON_STARTUP
        }) { }

        void OnStartup() override
        {
            if (IsModuleEnabled())
            {
                sReadiness.Load();
                sDungeons.Load();
            }
        }
    };

    class GuildAdventureReadinessCommandScript : public CommandScript
    {
    public:
        GuildAdventureReadinessCommandScript() : CommandScript("GuildAdventureReadinessCommandScript") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable gaTable =
            {
                { "readiness", HandleReadiness, SEC_GAMEMASTER, Console::Yes }
            };
            static ChatCommandTable commandTable =
            {
                { "ga", gaTable }
            };
            return commandTable;
        }

        static bool HandleReadiness(ChatHandler* handler, PlayerIdentifier target)
        {
            if (!IsModuleEnabled())
            {
                handler->SendErrorMessage("Guild Adventure is disabled (GuildAdventure.Enable = 0).", false);
                return false;
            }
            Player* player = target.GetConnectedPlayer();
            if (!player || !player->IsInWorld())
            {
                handler->SendErrorMessage(Acore::StringFormat("{} is not in the world (its gear is read from the character itself).",
                    target.GetName()), false);
                return false;
            }
            AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfMember(player->GetGUID());
            if (!guild)
            {
                handler->SendErrorMessage(Acore::StringFormat("{} is neither the leader nor a bot of an adventure guild.",
                    target.GetName()), false);
                return false;
            }
            for (std::string const& line : sReadiness.Describe(player))
                handler->SendSysMessage(line);
            // A tank or healer by its dungeon role goes into a dungeon in its dungeon set: how ready that is.
            if (GuildBot const* bot = guild->FindBot(player->GetGUID()); bot && sRoleService.GetRole(bot->guid) != DungeonRole::None)
                for (std::string const& line : sRoleService.Describe(*guild, *bot, player))
                    handler->SendSysMessage(line);
            return true;
        }
    };
}

void AddGuildAdventureReadinessScripts()
{
    new GuildAdventureReadinessWorldScript();
    new GuildAdventureReadinessCommandScript();
}
