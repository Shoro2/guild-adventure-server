/*
 * Readiness: how ready a guild character is for a dungeon or raid (docs/orders.md "Readiness", package K5; the entry
 * condition of the dungeon orders of package K6).
 *
 * One number per character and dungeon, 0 to 100:
 *
 *   - the level part: 0 below the dungeon's lowest level, 60 at it, 100 from its target level on;
 *   - the gear part: the character's gear level over the gear level the dungeon asks for, at most 100;
 *   - readiness = level part x gear part.
 *
 * The gear level of a character is the average item level of what its fights depend on: the armour on its body
 * (chest, legs, feet, hands, waist, wrists; the back from level 10, the shoulders from 20, the head from 30, as such
 * pieces exist) and its weapons (the main hand, twice for those who strike with it; the bow or gun of a hunter twice;
 * the wand of a caster). A bare place counts as 0. Jewellery and trinkets are left out: few exist before 60.
 *
 * The dungeons are the client's own list (LFGDungeons.dbc: lowest, highest and target level), those of the content the
 * server's level cap allows. A dungeon asks for a gear level of its target level minus 2 (a character that levelled
 * in what quests gave it wears about its level); the classic raids ask for more (the table in Readiness.cpp).
 *
 * World thread only.
 */

#ifndef MOD_GUILD_ADVENTURE_READINESS_H
#define MOD_GUILD_ADVENTURE_READINESS_H

#include "Define.h"
#include <functional>
#include <string>
#include <vector>

class Player;

namespace GuildAdventure
{
    struct DungeonInfo
    {
        uint32 id = 0;            // LFGDungeons.dbc (a map can have several: the wings of the Scarlet Monastery)
        uint32 mapId = 0;
        std::string name;
        uint8 minLevel = 0;
        uint8 maxLevel = 0;
        uint8 targetLevel = 0;
        uint8 size = 5;           // players
        bool raid = false;
        uint16 gearLevel = 0;     // the gear level it asks for
        uint8 team = 2;           // 0 Alliance, 1 Horde: it lies in that side's capital; 2: for both
    };

    struct ReadinessVerdict
    {
        uint8 percent = 0;
        uint8 levelPart = 0;
        uint8 gearPart = 0;
        uint16 gearLevel = 0;     // the character's
        std::string why;          // what holds it back ("" when nothing does)
    };
}

class Readiness
{
public:
    static Readiness& Instance();

    // Reads the dungeon list (after the DBC stores are loaded).
    void Load();

    [[nodiscard]] std::vector<GuildAdventure::DungeonInfo> const& GetDungeons() const { return _dungeons; }
    [[nodiscard]] GuildAdventure::DungeonInfo const* Find(uint32 id) const;
    // The dungeons a character of that level looks at: those it can enter soon (two levels below their lowest level)
    // and has not left behind (five levels above their highest); raids from their lowest level on. Not the dungeon in
    // the other side's capital.
    [[nodiscard]] std::vector<GuildAdventure::DungeonInfo const*> InReach(uint8 level, uint8 team) const;

    [[nodiscard]] static uint16 GearLevel(Player const* player);
    [[nodiscard]] static GuildAdventure::ReadinessVerdict Judge(Player const* player, GuildAdventure::DungeonInfo const& dungeon);
    // The same for a set of equipment the character does not wear right now (its dungeon set, DungeonRoles.h):
    // `itemLevelAt` gives the item level of what that set has in an equipment slot (0: nothing).
    [[nodiscard]] static uint16 GearLevelWith(Player const* player, std::function<uint32(uint8 slot)> const& itemLevelAt);
    // The same without a character in the world (its class and level; the roster's last known gear level).
    [[nodiscard]] static uint16 GearLevelOf(uint8 classId, uint8 level, std::function<uint32(uint8 slot)> const& itemLevelAt);
    [[nodiscard]] static GuildAdventure::ReadinessVerdict JudgeWith(Player const* player, GuildAdventure::DungeonInfo const& dungeon,
        uint16 gearLevel);
    // "id:percent,id:percent" for the dungeons in reach (the BOT record's rdy).
    [[nodiscard]] std::string Summary(Player const* player) const;
    // For ".ga readiness": the gear level and a line per dungeon in reach.
    [[nodiscard]] std::vector<std::string> Describe(Player const* player) const;

private:
    Readiness() = default;

    std::vector<GuildAdventure::DungeonInfo> _dungeons;
};

#define sReadiness Readiness::Instance()

#endif
