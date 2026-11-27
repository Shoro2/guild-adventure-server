/*
 * Fishing (package K3, docs/orders.md "Fishing"): where a bot can fish and what it needs for it. The task itself is
 * OrderService::StepFishing (Fishing.cpp).
 */

#ifndef MOD_GUILD_ADVENTURE_FISHING_H
#define MOD_GUILD_ADVENTURE_FISHING_H

#include "Define.h"
#include "WorldIndex.h"
#include <optional>
#include <string>

class Item;
class Player;
class Quest;

namespace GuildAdventure
{
    struct BotRuntime;

    namespace Fishing
    {
        // The fishing spell the character knows (its highest rank), 0 for none.
        uint32 SpellOf(Player const* player);
        // The fishing pole the character has (in its hand or in its bags), nullptr for none.
        Item* FindPole(Player const* player);
        // Whether the character can fish: it knows the spell (a pole is handed out when a fishing trip begins).
        bool CanFish(Player const* player);
        // Whether a place at the water is one to stand at for minutes: at most one hostile creature of any level
        // spawns within 45 yd of it (a murloc coast is none).
        bool IsQuietWater(Player const* player, SpawnPoint const& place);
        // The chance in percent that a bite is a fish and not junk where the character stands or at a place (the core's
        // rule: the square of skill over the zone's skill + 95).
        uint32 CatchChance(Player const* player, uint32 zoneId, uint32 areaId);
        // Whether a quest asks for an item that is only to be had by fishing.
        bool NeedsFishing(Quest const* quest);

        // A place at the water where `item` is fished (fishing_loot_template: by zone or area): where the character
        // stands when it is in such a zone, else the nearest school of fish there, else the zone's hub. The exact
        // spot on the shore is found when the bot gets there (FindShore).
        std::optional<SpawnPoint> PlaceFor(Player* player, BotRuntime const& rt, uint32 item);
        // The nearest water the character's skill is enough for (a school of fish marks water): for fishing for skill.
        std::optional<SpawnPoint> PlaceForSkill(Player* player, BotRuntime const& rt);

        // A spot to stand on within `reach` of a place: dry ground the path finder reaches, with water 11 to 19 yd
        // ahead in the direction `orientation`.
        // `fromPlace`: the place is far from the character: the spot nearest to the place, reachable from the place.
        bool FindShore(Player* player, float x, float y, float z, float reach, SpawnPoint& stand, float& orientation, bool fromPlace = false);
        // Water the character's skill is enough for when none is near and no school of fish marks any: the shore next to
        // an inn or a flight master on its land mass, in a zone whose water its skill reaches (the nearest such). A
        // journey: a character with Fishing 1 in a zone of level 20 to 30 catches nothing there and has to start at
        // a city's canal or a beginners' lake. `name` gets the inn's or flight master's name.
        std::optional<SpawnPoint> WaterForSkill(Player* player, BotRuntime const& rt, std::string& name);
    }
}

#endif
