/*
 * Dungeon runs in the order planner (docs/orders.md "Dungeon runs", package K6). Part of OrderService; kept in a file of
 * its own.
 *
 * The order DUNGEON <dungeon> is given to a guild bot by its owner, never by the autopilot (the owner: [quote left out]). The bot leads its squad (Squads.h: the members follow it as their playerbots master in its party):
 *
 *   - outside: it walks to the dungeon's entrance (flights and routes as for any travel), waits for its members and
 *     goes in; the members catch up with it as they do after any change of map;
 *   - inside: it walks from boss to boss in the order of the dungeon's encounters; what stands in the way is fought by
 *     the group with playerbots' group play; at a boss it attacks;
 *   - the dead are brought back by the group when the fight is over; when all are dead the group starts again from the
 *     entrance, and the second time the run is over;
 *   - after the last boss (or the end of the run for another reason) the group leaves, and the bot's order is AUTO
 *     again.
 *
 * Leaving a lost fight, the pull check and the rescue of stranded bots are off inside a dungeon (OrderService.cpp).
 */

#include "OrderService.h"
#include <unordered_map>
#include "Bag.h"
#include "BotDriver.h"
#include "Storage/StorageService.h"
#include "BotSettings.h"
#include "Creature.h"
#include "CreatureGroups.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "GameObjectAI.h"
#include "GameTime.h"
#include "CellImpl.h"
#include "Chat.h"
#include "ControlSwapService.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "DungeonRoles.h"
#include "DungeonSupplies.h"
#include "FightEscape.h"
#include "ThreatManager.h"
#include "Dungeons.h"
#include "DungeonNav.h"
#include "DynamicObject.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "Group.h"
#include "Item.h"
#include "InstanceSaveMgr.h"
#include "InstanceScript.h"
#include "GroundGuard.h"
#include "GuildBotHolder.h"
#include "LootMgr.h"
#include "GuildAdventureMgr.h"
#include "GuildAdventureSettings.h"
#include "GuildAdventureUtil.h"
#include "GuildEvents.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PathGenerator.h"
#include "PathProbe.h"
#include "Pet.h"
#include "Readiness.h"
#include "RoadMap.h"
#include "WaypointMgr.h"
#include "SpellAuraEffects.h"
#include "ScriptMgr.h"
#include "SmartScriptMgr.h"
#include "MoveSpline.h"
#include <array>
#include <map>
#include <mutex>
#include "SpecService.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Storage/StorageSupply.h"
#include "Squads.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Timer.h"
#include "Tokenize.h"
#include "WorldIndex.h"
#include "GossipDef.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <cmath>

using namespace GuildAdventure;

namespace
{
    constexpr char const* ActivityLog = "module.ga.activity";

    // Near enough to the entrance to go in; near enough to a boss's place to look for it.
    constexpr float EntranceReached = 30.0f;
    constexpr float BossReached = 40.0f;
    // Beyond this the walk to the entrance is a travel (flights), nearer a plain walk.
    constexpr float EntranceTravelDistance = 2500.0f;
    // A member counts as with its leader within this distance.
    constexpr float GroupDistance = 80.0f;
    // The dead are brought back this long after the fight; all dead for this long is a wipe.
    constexpr uint32 ReviveAfterMs = 10 * IN_MILLISECONDS;
    constexpr uint32 WipeAfterMs = 10 * IN_MILLISECONDS;  // the owner, 2026-11-27: [quote left out]
    constexpr uint8 MaxWipes = 2;
    // A run that takes longer than this ends; a boss the group gets no nearer to for this long is passed over.
    constexpr uint32 MaxRunMs = 2 * HOUR * IN_MILLISECONDS;
    // A raid (docs/orders.md "Raid runs"): its bosses are fights a group of bots loses now and then, and the saved
    // instance keeps what fell, so a wipe costs less: five end a raid run; and a raid takes hours.
    constexpr uint8 MaxRaidWipes = 5;
    constexpr uint32 MaxRaidRunMs = 5 * HOUR * IN_MILLISECONDS;
    uint8 WipesFor(DungeonPlan const& plan) { return plan.raid ? MaxRaidWipes : MaxWipes; }
    uint32 RunMsFor(DungeonPlan const& plan) { return plan.raid ? MaxRaidRunMs : MaxRunMs; }
    constexpr uint32 BossStuckMs = 8 * MINUTE * IN_MILLISECONDS;
    // The dungeon step runs every tick or two while the leader walks; a longer gap is time it did not walk.
    constexpr uint32 StepGapMs = 5 * IN_MILLISECONDS;
    // No nearer to a boss for this long with no way on foot to it: the group is put near it (once per boss).
    constexpr uint32 HopAfterMs = 90 * IN_MILLISECONDS;
    // ... and sooner when its leader stands where its walk ends (a corner the path finder's ground ends in: Blackrock
    // Depths' Hall of Crafting, where the group stood two minutes before it was put near Ambassador Flamelash,
    // the owner 2026-11-23).
    constexpr uint32 HopStillMs = 30 * IN_MILLISECONDS;
    constexpr float StillYards = 4.0f;
    constexpr float BossProgress = 15.0f;
    // No way on foot from where the leader stands to a boss. WalkReaches counts 12 yd as there (an NPC a few yards off
    // the navmesh), so a boss on another level that near asks the path itself: Shadowfang Keep, 2026-11-24, the group
    // stood 18 minutes on a wall top 7 yd above Odo the Blindwatcher, 10 yd from him, its every walk refused, and was
    // not put on his floor (the test group, round 15).
    bool NoWayOnFoot(Player* player, float x, float y, float z)
    {
        // Inside a dungeon the whole navmesh answers (DungeonNav): the core's path finder stops after 148 polygons and
        // WalkReaches wants every leg 15 yd nearer, so a way that winds (Shadowfang Keep's 620 yd from the entrance to
        // Odo, 23 yd above it) counted as none and the group was put there by a teleport.
        if (player->GetMap()->IsDungeon() && sDungeonNav.FindWay(player->GetMapId(), player->GetPositionX(), player->GetPositionY(),
                player->GetPositionZ(), x, y, z, nullptr, nullptr, player->GetMap()))
            return false;
        if (!WalkReaches(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), x, y, z))
            return true;
        return player->GetExactDist(x, y, z) <= 12.0f && std::fabs(player->GetPositionZ() - z) > 4.0f && !PathEndsAt(player, x, y, z);
    }
    constexpr uint32 AssertMs = 5 * IN_MILLISECONDS;
    // Raid bosses pulled to a place (a hand list): Ossirian the Unscarred to his first crystal, 126 yd from where he
    // stands (his Strength, +300 % damage, holds until a crystal's weakness reaches him).
    // High Priestess Jeklik to the floor below her platform: the navmesh of Zul'Gurub has no way up to it (a part of 53
    // polygons, 12 yd above the floor round it, and no chain of steps under 9 yd), and players do not go up either:
    // when she is pulled she flies down her path (PATH_JEKLIK_INTRO, boss_jeklik) to (-12274, -1408) on the floor and
    // fights there. Her tank taunts her from a point under that flight, 26 yd from her (`from`); the owner's raid was put
    // up to her by a teleport, 2026-11-27.
    struct PullPlace
    {
        uint32 entry;
        float x, y, z;
        float fromX = 0.0f, fromY = 0.0f, fromZ = 0.0f; // where its tank pulls it from (0: it walks up to it)
    };
    PullPlace const* PullPlaceOf(uint32 entry)
    {
        static PullPlace const Places[] = {
            { 15339, -9407.72f, 1960.21f, 85.64f },
            { 14517, -12273.9f, -1407.8f, 131.0f, -12279.2f, -1399.7f, 131.5f },
        };
        for (PullPlace const& place : Places)
            if (place.entry == entry)
                return &place;
        return nullptr;
    }
    // The place a group with no way on foot to a boss is put at (a hand list): the way players come to it. Blackwing
    // Lair's Broodlord Lashlayer stands at the east end of the suppression room, and the hop round him from Vaelastrasz's
    // room put the raid of 40 behind his portcullis beside Firemaw, who came with his room (2026-11-22). The raid is put
    // in the suppression room 45 yd west of him, on his floor, and fights its way to him from there; it is also the place
    // his room is fought from.
    // A creature that whirls (a whirlwind that hits everybody near it and runs after a random member): nobody runs to
    // the tank with it on them, and the raid steps out of it (StepRaidBursts).
    bool IsWhirler(Creature const* creature)
    {
        // Battleguard Sartura, Sartura's Royal Guard, Emperor Vek'lor (his Arcane Burst on all within 10 yd of him)
        static uint32 const Whirlers[] = { 15516, 15984, 15276 };
        return creature && std::find(std::begin(Whirlers), std::end(Whirlers), creature->GetEntry()) != std::end(Whirlers);
    }
    // Adds of a raid boss that its raid kills first (the skull goes on them while one lives): Sartura's Royal Guards
    // whirl as she does, and with the four of them on it the raid of 40 died in a minute, 2026-11-22.
    bool KilledFirst(Creature const* creature)
    {
        static uint32 const Adds[] = { 15984 };  // Sartura's Royal Guard
        return creature && std::find(std::begin(Adds), std::end(Adds), creature->GetEntry()) != std::end(Adds);
    }
    PullPlace const* ApproachPlaceOf(uint32 entry)
    {
        static PullPlace const Places[] = {
            { 12017, -7619.0f, -1036.0f, 449.2f },  // Broodlord Lashlayer: 45 yd west of him in the suppression room
            // Battleguard Sartura: the corridor north of her room, out of the round she and her guards walk (a hop to
            // 25 yd before her pulled the four of them onto the raid of 40, 2026-11-22)
            { 15516, -8346.0f, 1786.0f, -2.0f },
            // Emperor Vek'nilash: the floor before the door of their room, 120 yd from both thrones (a hop to 25 yd before him
            // pulled both twins onto the raid, and so did the raid gathered on the floor between the thrones, 35 yd from his:
            // their aggro range is 27.5 yd from their middle, 2026-11-23); the halves go to their places from here
            { 15275, -8958.0f, 1280.0f, -104.6f },
            // Ouro: 33 yd east of his mound, where he comes out (within 40 yd of it, npc_ouro_spawner); on the mound the
            // raid had his Ground Rupture and Sweep under it as he came, three tanks dead in 5 s, 2026-11-23
            { 15517, -9140.0f, 2104.0f, -65.1f },
            // Viscidus: 42 yd north of him, the raid gathers there before the pull (the leader walked up to him and pulled
            // him alone, 2026-11-23)
            { 15299, -7992.0f, 950.0f, -52.6f },
            // The Eye of C'Thun: on the ramp up into its chamber, 68 yd from it but below the chamber's floor (it takes only
            // who stands higher than 100, boss_cthun); a hunter pulls it from there (StepCthunPull). (Behind the dragons'
            // place on the ledge the raid's way led through its sight, 2026-11-24.)
            { 15589, -8570.1f, 1918.8f, 93.0f },
            { 15727, -8570.1f, 1918.8f, 93.0f },  // C'Thun, the run's boss: the same place
        };
        for (PullPlace const& place : Places)
            if (place.entry == entry)
                return &place;
        return nullptr;
    }
    // The least readiness a member needs (Readiness.h).
    // After a boss the group stays for its loot: while a roll runs or a corpse of the group's near the leader still
    // holds something, for at most this long (the longer time when a character its owner plays is in the group: a
    // roll window stays open for a minute).
    constexpr uint32 LootWaitMs = 20 * IN_MILLISECONDS;
    constexpr uint32 LootWaitOwnerMs = 70 * IN_MILLISECONDS;
    constexpr float LootLookYards = 40.0f;
    // A closed door this near to the leader is opened; the look every few seconds.
    constexpr float DoorYards = 14.0f;
    constexpr uint32 DoorLookMs = 2 * IN_MILLISECONDS;
    // At the entrance the group goes in when everybody has this much health and mana (percent), or after this long.
    constexpr float ReadyHealth = 80.0f;
    constexpr float ReadyMana = 70.0f;
    constexpr uint32 ReadyWaitMs = 3 * MINUTE * IN_MILLISECONDS;
    // Between two runs gear below this durability (percent, the worst piece) is repaired; bags with fewer free
    // slots than this are cleared.
    constexpr float RepairBelow = 90.0f;
    // The reset of the dungeon before a run: it is done within a tick or two; after this long the run is given up.
    constexpr uint32 ResetWaitMs = MINUTE * IN_MILLISECONDS;
    constexpr uint32 BagSlotsWanted = 8;

    // The closed doors within reach of a character.
    class ClosedDoorCheck
    {
    public:
        ClosedDoorCheck(WorldObject const* who, float range) : _who(who), _range(range) { }
        bool operator()(GameObject* object)
        {
            // (Not a door that a boss's event opens: the High Inquisitor's Door opens when Mograine falls, and Whitemane
            // comes out to raise him; opened before, the group walked in to her and pulled the cathedral. The owner,
            // 2026-11-14.)
            constexpr uint32 HighInquisitorsDoor = 104600;
            return object->GetGoType() == GAMEOBJECT_TYPE_DOOR && object->GetGoState() == GO_STATE_READY && object->isSpawned() &&
                object->GetEntry() != HighInquisitorsDoor && _who->IsWithinDist(object, _range, false);
        }

    private:
        WorldObject const* _who;
        float _range;
    };

    // A creature of a boss's room that is not in a fight yet and that the group can fight (OrderService::RunDungeon:
    // the room is cleared before the boss is pulled).
    // The radius a boss's own script calls for help in when it is pulled (its SmartAI "on aggro: call for help"), 0
    // if it has none. (Jammal'an the Prophet calls everything within 90 yd: the group that had cleared 30 yd around him
    // had 26 creatures on it at the pull and was wiped out, Sunken Temple, dclear 2026-11-18.)
    float BossCallForHelpYards(uint32 entry)
    {
        static std::mutex lock;
        static std::unordered_map<uint32, float> known;
        std::lock_guard<std::mutex> guard(lock);
        auto const itr = known.find(entry);
        if (itr != known.end())
            return itr->second;
        float yards = 0.0f;
        for (SmartScriptHolder const& e : sSmartScriptMgr->GetScript(int32(entry), SMART_SCRIPT_TYPE_CREATURE))
            if (e.GetEventType() == SMART_EVENT_AGGRO && e.GetActionType() == SMART_ACTION_CALL_FOR_HELP)
                yards = std::max(yards, float(e.action.callHelp.range));
        // Bosses whose C++ script calls for help when it is engaged (me->CallForHelp): Emperor Dagran Thaurissan calls
        // all of his throne room (VISIBLE_RANGE), and 40 came with him, Blackrock Depths 2026-11-20.
        constexpr uint32 EmperorDagranThaurissan = 9019;
        if (entry == EmperorDagranThaurissan)
            yards = std::max(yards, 100.0f);
        known.emplace(entry, yards);
        return yards;
    }

    // Creatures a boss's own script turns on the players when it is pulled (its SmartAI "on aggro: set data" on the
    // creatures of an entry within a range, whose own script then attacks), with that range. Scholomance, 2026-11-20:
    // Vectus and Marduk Blackpool turn the 32 Scholomance Students of the Viewing Room on the group, which are neutral
    // until then, so the room was not cleared and 32 came at the pull. Pulled one at a time, a student brings only the
    // students within 5 yd of it. (The owner: [quote left out].)
    std::vector<std::pair<uint32, float>> BossScriptJoiners(uint32 entry)
    {
        static std::mutex lock;
        static std::unordered_map<uint32, std::vector<std::pair<uint32, float>>> known;
        std::lock_guard<std::mutex> guard(lock);
        auto const itr = known.find(entry);
        if (itr != known.end())
            return itr->second;
        std::vector<std::pair<uint32, float>> joiners;
        for (SmartScriptHolder const& e : sSmartScriptMgr->GetScript(int32(entry), SMART_SCRIPT_TYPE_CREATURE))
        {
            if (e.GetEventType() != SMART_EVENT_AGGRO || e.GetActionType() != SMART_ACTION_SET_DATA)
                continue;
            if (e.GetTargetType() == SMART_TARGET_CREATURE_DISTANCE && e.target.unitDistance.creature)
                joiners.emplace_back(e.target.unitDistance.creature, std::clamp(float(e.target.unitDistance.dist), 30.0f, 100.0f));
            else if (e.GetTargetType() == SMART_TARGET_CREATURE_RANGE && e.target.unitRange.creature)
                joiners.emplace_back(e.target.unitRange.creature, std::clamp(float(e.target.unitRange.maxDist), 30.0f, 100.0f));
        }
        known.emplace(entry, joiners);
        return joiners;
    }

    class RoomCreatureCheck
    {
    public:
        RoomCreatureCheck(Player* who, Creature const* boss, float range, std::vector<uint32> const& bosses, float callRange = 0.0f,
            float wideRange = 0.0f, uint32 wideType = 0, std::vector<std::pair<uint32, float>> const* joiners = nullptr) : _who(who),
            _boss(boss), _range(range), _bosses(bosses), _callRange(callRange), _wideRange(wideRange), _wideType(wideType),
            _joiners(joiners) { }
        bool operator()(Creature* creature)
        {
            // (Neutral creatures too that the boss's script turns on the group: BossScriptJoiners.)
            float joinRange = 0.0f;
            if (_joiners)
                for (auto const& [entry, yards] : *_joiners)
                    if (entry == creature->GetEntry())
                        joinRange = yards;
            // (No boss of the dungeon: Whitemane stands 48 yd from Mograine, behind the door his death opens, and the
            // group that walked to her as a creature of his room brought the cathedral on itself.)
            return creature != _boss && std::find(_bosses.begin(), _bosses.end(), creature->GetEntry()) == _bosses.end() &&
                creature->IsAlive() && !creature->IsInCombat() && !creature->IsCritter() &&
                !creature->IsTotem() && !creature->IsCivilian() && (creature->IsHostileTo(_who) || joinRange > 0.0f) &&
                _who->IsValidAttackTarget(creature) && !creature->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) &&
                ((joinRange > 0.0f && _boss->IsWithinDist(creature, joinRange, false)) ||
                // (Its room taken flat, but not a floor far below or above it out of its sight: Hakkar stands on his
                // platform 33 yd above a hall of priests and Sons within 30 yd flat, and the raid tried to clear them for
                // ten minutes before every pull, 2026-11-22.)
                (_boss->IsWithinDist(creature, _range, false) &&
                    (std::fabs(creature->GetPositionZ() - _boss->GetPositionZ()) <= 15.0f || _boss->IsWithinLOSInMap(creature))) ||
                // (Its call for help as the core makes it: in three dimensions and in its sight; Odo's 40 yd taken flat
                // held 24 creatures of the courtyard below and the tank walked at the floor for minutes.)
                (_callRange > _range && _boss->IsWithinDist(creature, _callRange, true) && _boss->IsWithinLOSInMap(creature)) ||
                (_wideType && creature->GetCreatureType() == _wideType && _boss->IsWithinDist(creature, _wideRange, false)));
        }

    private:
        Player* _who;
        Creature const* _boss;
        float _range;
        std::vector<uint32> const& _bosses;
        float _callRange;  // ... and what its call for help reaches
        float _wideRange;  // ... and creatures of one type this far (a boss whose script calls all of them)
        uint32 _wideType;
        std::vector<std::pair<uint32, float>> const* _joiners;  // ... and the creatures its script turns on the group
    };

    // Whether the character carries something to drink it can use (a mana user without one rests for minutes after
    // every pull, and its group waits for it).
    bool CarriesDrink(Player* player)
    {
        constexpr uint32 DrinkCategory = 59;
        bool found = false;
        auto look = [&](Item const* item)
        {
            ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
            found = found || (proto && proto->Class == ITEM_CLASS_CONSUMABLE && proto->Spells[0].SpellId &&
                proto->Spells[0].SpellCategory == DrinkCategory && proto->RequiredLevel <= player->GetLevel());
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            look(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = player->GetBagByPos(bagSlot))
                for (uint32 i = 0; i < bag->GetBagSize(); ++i)
                    look(bag->GetItemByPos(uint8(i)));
        return found;
    }

    enum Phase : uint8
    {
        PhaseNone = 0,
        PhaseApproach = 1,
        PhaseInside = 2
    };

    // The raid target icon the group's one target is marked with (package P5): mod-playerbots' damage dealers and tank take
    // the creature with their "rti" icon first, and that is the skull unless somebody changed it.
    constexpr uint8 SkullIcon = 7;
    constexpr uint8 CrossIcon = 6;

    // What the group still has to take here: a roll that runs, or a corpse of the group's near the leader with loot
    // in it (`corpse`, for the leader to walk to: the module's auto loot reaches a few yards).
    bool LootPending(Player* leader, Creature*& corpse, bool& rolling)
    {
        corpse = nullptr;
        Group* group = leader->GetGroup();
        rolling = group && !group->GetRolls().empty();
        std::list<Creature*> dead;
        leader->GetDeadCreatureListInGrid(dead, LootLookYards);
        float best = 0.0f;
        for (Creature* creature : dead)
        {
            if (!creature->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE) || creature->loot.loot_type == LOOT_SKINNING || creature->loot.isLooted())
                continue;
            if (creature->GetLootRecipient() != leader && !(group && creature->GetLootRecipientGroup() == group))
                continue;
            float const distance = leader->GetDistance(creature);
            if (!corpse || distance < best)
            {
                corpse = creature;
                best = distance;
            }
        }
        return rolling || corpse;
    }

    std::string RoughDistance(float distance)
    {
        return Acore::StringFormat("about {:.0f} yd", distance < 100.0f ? std::round(distance / 10.0f) * 10.0f :
            std::round(distance / 50.0f) * 50.0f);
    }

    float Dist2dTo(Player const* player, float x, float y)
    {
        float const dx = player->GetPositionX() - x;
        float const dy = player->GetPositionY() - y;
        return std::sqrt(dx * dx + dy * dy);
    }

    // A line in the chat of the character its owner plays in a dungeon group.
    void TellGuest(Player* guest, std::string const& text)
    {
        if (guest && guest->GetSession())
            ChatHandler(guest->GetSession()).SendSysMessage("|cff00ccff[Guild Adventure]|r " + text);
    }

    // The request a resurrection spell sends (Spell::EffectResurrect / SendResurrectRequest): the dead character's
    // client asks its owner, and brings it back beside 'from' when he accepts.
    void OfferResurrection(Player* from, Player* dead)
    {
        if (dead->isResurrectRequested())
            return;
        dead->setResurrectRequestData(from->GetGUID(), from->GetMapId(), from->GetPositionX(), from->GetPositionY(), from->GetPositionZ(),
            dead->GetMaxHealth() / 2, dead->GetMaxPower(POWER_MANA) / 2);
        WorldPacket data(SMSG_RESURRECT_REQUEST, 8 + 4 + 1 + 1 + 1 + 4);
        data << from->GetGUID();
        data << uint32(1);
        data << std::string();
        data << uint8(0);
        data << uint8(0);  // no resurrection sickness
        data << uint32(0); // at once
        dead->SendDirectMessage(&data);
    }

    // Alive again where the group is (beside it in any case when creatures stand at the body).
    void Revive(Player* player, Player* beside, bool besideAlways = false)
    {
        if (!player->IsAlive())
        {
            player->ResurrectPlayer(0.5f);
            player->SpawnCorpseBones();
        }
        // (Into the instance of the one it stands up beside: a teleport into a dungeon goes where the core decides.)
        if (beside && beside->GetMap()->IsDungeon() && !OrderService::SameInstance(player, beside))
            OrderService::BindToLeaderInstance(player, beside);
        if (beside && (besideAlways || player->GetMapId() != beside->GetMapId() || !player->IsWithinDistInMap(beside, 30.0f)))
            player->TeleportTo(beside->GetMapId(), beside->GetPositionX(), beside->GetPositionY(), beside->GetPositionZ(),
                beside->GetOrientation());
    }
}

bool OrderService::CheckDungeonOrder(AdventureGuild& guild, GuildBot const& bot, uint32 dungeonId, std::string& error) const
{
    DungeonPlan const* plan = sDungeons.GetPlan(dungeonId);
    DungeonInfo const* info = sReadiness.Find(dungeonId);
    if (!plan || !info)
        error = "that dungeon is not known";
    else if (!plan->supported)
        error = Acore::StringFormat("{} cannot be run yet: {}", plan->name, plan->why);
    else if (info->team < 2 && info->team != uint8(guild.team))
        error = Acore::StringFormat("{} lies in the other side's capital", plan->name);
    else if (bot.guid == guild.leader)
        error = "the guild leader takes no dungeon order (give it to the bot that leads the group)";
    else if (!sSquadService.GetLeader(bot.guid).IsEmpty())
        error = Acore::StringFormat("{} follows a squad leader: the order goes to the leader of the group", bot.name);
    else
        return true;
    return false;
}

bool OrderService::CheckDungeonReady(AdventureGuild& guild, GuildBot const& bot, uint32 dungeonId, std::string& error,
    std::string* summary) const
{
    DungeonInfo const* info = sReadiness.Find(dungeonId);
    std::vector<Player*> const group = DungeonGroup(guild, bot.guid);
    if (!info || group.empty())
    {
        error = !info ? "that dungeon is not known" : Acore::StringFormat("{} is not in the world", bot.name);
        return false;
    }
    std::string names;
    bool ready = true;
    for (Player* member : group)
    {
        uint32 const percent = Readiness::Judge(member, *info).percent;
        names += Acore::StringFormat("{}{} {}%", names.empty() ? "" : ", ", member->GetName(), percent);
        // A tank or healer by its dungeon role goes in its dungeon set: how ready that is, for the owner to see (the
        // order is not refused for it).
        if (DungeonRole const role = sRoleService.GetRole(member->GetGUID()); role != DungeonRole::None)
            names += Acore::StringFormat(" (as {} in its dungeon set {}%)", ToText(role),
                uint32(Readiness::JudgeWith(member, *info, sRoleService.RoleGearLevel(member)).percent));
        ready = ready && percent >= GetSettings().dungeonsMinReadiness;
    }
    if (summary)
        *summary = names;
    if (!ready)
        error = Acore::StringFormat("not everyone is ready for it (at least {}% each: {})", GetSettings().dungeonsMinReadiness, names);
    return ready;
}

bool OrderService::IsOnDungeonRun(AdventureGuild& guild, GuildBot const& bot)
{
    ObjectGuid const leaderGuid = bot.order.type == OrderType::Dungeon ? bot.guid : sSquadService.GetLeader(bot.guid);
    GuildBot const* leader = leaderGuid.IsEmpty() ? nullptr : guild.FindBot(leaderGuid);
    return leader && leader->order.type == OrderType::Dungeon;
}

void OrderService::LoadDungeonRuns()
{
    if (_dungeonRunsLoaded)
        return;
    _dungeonRunsLoaded = true;
    if (QueryResult result = CharacterDatabase.Query("SELECT bot_guid, runs_left, runs_done FROM ga_bot_dungeon_runs"))
    {
        do
        {
            Field* fields = result->Fetch();
            DungeonRuns& runs = _dungeonRuns[fields[0].Get<uint32>()];
            runs.left = std::max<uint8>(fields[1].Get<uint8>(), 1);
            runs.done = fields[2].Get<uint8>();
        } while (result->NextRow());
    }
}

void OrderService::SetDungeonRuns(ObjectGuid bot, uint32 runs)
{
    LoadDungeonRuns();
    if (runs != DungeonRunsEndless)
        runs = std::clamp<uint32>(runs, 1, GetSettings().dungeonsMaxRuns);
    if (runs <= 1)
    {
        if (_dungeonRuns.erase(bot.GetCounter()))
            CharacterDatabase.Execute("DELETE FROM ga_bot_dungeon_runs WHERE bot_guid = {}", bot.GetCounter());
        return;
    }
    DungeonRuns& state = _dungeonRuns[bot.GetCounter()];
    state.left = uint8(runs);
    state.done = 0;
    CharacterDatabase.Execute("REPLACE INTO ga_bot_dungeon_runs (bot_guid, runs_left, runs_done, set_at) VALUES ({}, {}, 0, {})",
        bot.GetCounter(), runs, uint32(GameTime::GetGameTime().count()));
}

uint32 OrderService::GetDungeonRuns(ObjectGuid bot)
{
    LoadDungeonRuns();
    auto itr = _dungeonRuns.find(bot.GetCounter());
    return itr != _dungeonRuns.end() ? itr->second.left : 1;
}

uint32 OrderService::GetDungeonRunsDone(ObjectGuid bot)
{
    LoadDungeonRuns();
    auto itr = _dungeonRuns.find(bot.GetCounter());
    return itr != _dungeonRuns.end() ? itr->second.done : 0;
}

namespace
{
    // A hostile ground effect (a cloud, a puddle, a fire of a creature's spell) that covers this point (the owner, 2026-11-14:
    // a character brought back into the Noxious Cloud it died in died again and again).
    bool HostileGroundEffectAt(WorldObject const* where, Player const* who)
    {
        constexpr float LookYards = 40.0f;
        std::list<WorldObject*> objects;
        Acore::AllWorldObjectsInRange check(where, LookYards);
        Acore::WorldObjectListSearcher<Acore::AllWorldObjectsInRange> searcher(where, objects, check, GRID_MAP_TYPE_MASK_DYNAMICOBJECT);
        Cell::VisitObjects(where, searcher, LookYards);
        for (WorldObject* object : objects)
        {
            DynamicObject const* effect = object->ToDynObject();
            if (!effect)
                continue;
            Unit const* caster = effect->GetCaster();
            if (caster && !caster->IsHostileTo(who))
                continue; // the group's own (a Consecration, a Blizzard)
            SpellInfo const* info = sSpellMgr->GetSpellInfo(effect->GetSpellId());
            if (!info || info->IsPositive())
                continue;
            if (where->GetExactDist2d(effect) <= effect->GetRadius() + 1.0f)
                return true;
        }
        return false;
    }

    // The hostile creatures within `range` of `center` that `who` could fight: alive, no critters, no pets.
    std::vector<Creature*> HostilesAround(WorldObject const* center, Player* who, float range)
    {
        std::list<Unit*> units;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(center, who, range);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(center, units, check);
        Cell::VisitObjects(center, searcher, range);
        std::vector<Creature*> result;
        for (Unit* unit : units)
        {
            Creature* creature = unit->ToCreature();
            if (!creature || !creature->IsAlive() || creature->IsPet() || creature->IsTotem() || creature->IsCritter() ||
                !creature->IsHostileTo(who) || !who->IsValidAttackTarget(creature))
                continue;
            result.push_back(creature);
        }
        return result;
    }

    // The creatures that fight the group: those that have one of its characters on their threat list.
    std::vector<Creature*> CreaturesOfTheFight(std::vector<Player*> const& group)
    {
        std::vector<Creature*> result;
        for (Player* member : group)
            for (auto const& [guid, ref] : member->GetThreatMgr().GetThreatenedByMeList())
            {
                Creature* creature = ref ? ref->GetOwner() : nullptr;
                // (Not the snakes and rats an area spell brought in: a group "turned to Snake that runs away".)
                if (creature && creature->IsAlive() && !creature->IsCritter() &&
                    std::find(result.begin(), result.end(), creature) == result.end())
                    result.push_back(creature);
            }
        return result;
    }

    // Creatures whose fire pulses round them all through a fight (a hand list): Molten Core's Flame Imps cast Fire Nova
    // (20602, 10 yd, every 4 to 7 s), and in a pack of twenty each character at the tank took a nova of every imp
    // round it: the owner's raid of 40 was wiped out in 15 s, 2026-11-27 ([quote left out]).
    struct Pulser
    {
        uint32 entry;
        float yards;
    };
    Pulser const* PulserOf(Creature const* creature)
    {
        static Pulser const Pulsers[] = { { 11669, 10.0f } };  // Flame Imp: Fire Nova
        if (creature)
            for (Pulser const& pulser : Pulsers)
                if (pulser.entry == creature->GetEntry())
                    return &pulser;
        return nullptr;
    }
    // The pulsing creatures in a fight within `range` of `center`.
    std::vector<Creature*> PulsersNear(WorldObject const* center, float range)
    {
        std::vector<Creature*> result;
        std::list<Creature*> found;
        center->GetCreatureListWithEntryInGrid(found, 11669, range);
        for (Creature* creature : found)
            if (creature->IsAlive() && creature->IsInCombat() && PulserOf(creature))
                result.push_back(creature);
        return result;
    }
    // Whether (x, y, z) lies within the pulse of one of `pulsers` (2 yd more than its reach).
    bool InPulse(std::vector<Creature*> const& pulsers, float x, float y, float z)
    {
        for (Creature* creature : pulsers)
            if (Pulser const* pulser = PulserOf(creature); pulser && creature->GetExactDist(x, y, z) < pulser->yards + 3.0f)
                return true;
        return false;
    }
}

bool OrderService::FindHopSpot(Player* player, float bossX, float bossY, float bossZ, float& x, float& y, float& z, float nearRadius,
    float farRadius, bool clear) const
{
    Map* map = player->GetMap();
    // From the side the group comes from, round the boss; a ring of 25 yd, then one of 35 (for a clear spot also two
    // farther out: Lower Blackrock Spire's group was put 60 yd before Highlord Omokk into the ogres' hall and wiped,
    // 2026-11-19).
    float const toGroup = std::atan2(player->GetPositionY() - bossY, player->GetPositionX() - bossX);
    float bestScore = -1.0f;
    std::vector<float> radii = { nearRadius, farRadius };
    if (clear)
    {
        radii.push_back(farRadius + 15.0f);
        radii.push_back(farRadius + 30.0f);
    }
    // Strict first: the path from the spot ends at the boss. Only when no spot passes that (a boss on a ledge the
    // navmesh does not reach), the old rule: a path that comes within 12 yd of it.
    auto search = [&](bool strict)
    {
    for (float radius : radii)
        for (int step = 0; step < 12; ++step)
        {
            // 0, +30, -30, +60, -60 ... degrees off the direction to the group.
            float const turn = float((step + 1) / 2) * float(M_PI) / 6.0f * ((step % 2) ? 1.0f : -1.0f);
            float const cx = bossX + radius * std::cos(toGroup + turn);
            float const cy = bossY + radius * std::sin(toGroup + turn);
            // The ground at the boss's height first, then up to 8 yd above it (a boss on a step): the first height under
            // 8 yd above the boss was a roof before. Shadowfang Keep, 2026-11-24: the roof over the prison and Odo the
            // Blindwatcher's room, 8 yd above his floor, has a navmesh of its own that no path leaves; the group was put
            // on it "on Odo's floor, 30 yd from him", stood there for 18 minutes, and a Shadowfang Whitescalp that went
            // for the healer up there could not reach it and held the group in a fight for 14 minutes.
            float cz = INVALID_HEIGHT;
            for (float fromZ : { bossZ + 2.0f, bossZ + 8.0f })
            {
                float const height = map->GetHeight(player->GetPhaseMask(), cx, cy, fromZ, true, 40.0f);
                if (height <= INVALID_HEIGHT || std::fabs(height - bossZ) > 15.0f)
                    continue;
                // (On the navmesh: Blackrock Depths, 2026-11-20, a spot 50 yd before Lord Incendius had a height and a
                // path that ended near him, but lay off the navmesh; the group was put there and no walk from it was let
                // through.) And the path from it ends at the boss: on the roof the path finder's walk ended right above
                // Odo, and the walk check counts 12 yd as there.
                PathGenerator probe(player);
                probe.CalculatePath(cx, cy, height + 0.5f, bossX, bossY, bossZ, false);
                if (probe.GetPathType() & (PATHFIND_FARFROMPOLY_START | PATHFIND_NOPATH))
                    continue;
                if (strict)
                {
                    Movement::PointsArray const& points = probe.GetPath();
                    if ((probe.GetPathType() & (PATHFIND_SHORTCUT | PATHFIND_NOT_USING_PATH)) || points.empty())
                        continue;
                    G3D::Vector3 const& end = points.back();
                    if (std::fabs(end.z - bossZ) > 4.0f || std::hypot(end.x - bossX, end.y - bossY) > 8.0f)
                        continue;
                }
                float length = 0.0f;
                if (!WalkReaches(player, cx, cy, height, bossX, bossY, bossZ, &length) || length > 3.0f * radius)
                    continue;
                cz = height;
                break;
            }
            if (cz <= INVALID_HEIGHT)
                continue;
            // The fewer creatures stand there the better (those of grids that are loaded: the place of the boss). For a
            // clear spot none may see the group there.
            uint32 crowd = 0;
            bool seen = false;
            for (auto const& [guid, creature] : map->GetCreatureBySpawnIdStore())
            {
                if (!creature || !creature->IsAlive() || !creature->IsHostileTo(player) || creature->IsCritter() || creature->IsInCombat())
                    continue;
                if (creature->GetExactDist2d(cx, cy) < 18.0f && std::fabs(creature->GetPositionZ() - cz) < 10.0f)
                    ++crowd;
                if (clear && creature->GetExactDist(cx, cy, cz) < creature->GetAggroRange(player) + 8.0f)
                    seen = true;
            }
            if (seen)
                continue;
            float const score = 100.0f - 10.0f * float(crowd) - float(step) - (radius > nearRadius ? 3.0f : 0.0f) -
                (radius > farRadius ? 3.0f : 0.0f);
            if (score > bestScore)
            {
                bestScore = score;
                x = cx;
                y = cy;
                z = cz + 0.5f;
            }
        }
    };
    search(true);
    if (bestScore < 0.0f)
        search(false);
    return bestScore >= 0.0f;
}

// A closed door between the group and a place it would be put at because it has no way on foot there (the Bar Door of
// the Grim Guzzler, between the bar and the anvil of Lord Incendius: the group was put on its far side, through the
// wall, the owner 2026-11-23): the leader walks to the door, the group opens it and steps through it, and goes on on foot
// from there. False when there is no such door (or the walk to it got nowhere for a minute and a half): then the hop.
bool OrderService::StepThroughDoor(GuildBot const& bot, Player* player, BotRuntime& rt, std::vector<Player*> const& group,
    DungeonPlan const& plan, float toX, float toY, float toZ, std::string const& toName)
{
    constexpr float DoorLookYards = 120.0f;   // the farthest door it looks at
    constexpr float DoorOffYards = 25.0f;     // ... beside the line to where it goes
    constexpr float BeforeDoorYards = 3.0f;   // where the leader stands to open it
    constexpr uint32 DoorWalkMs = 90 * IN_MILLISECONDS;
    BotRuntime::DungeonRun& run = rt.dungeon;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    float const lx = player->GetPositionX(), ly = player->GetPositionY(), lz = player->GetPositionZ();
    float const dx = toX - lx, dy = toY - ly;
    float const length = std::hypot(dx, dy);
    if (length < 5.0f)
        return false;
    std::list<GameObject*> doors;
    float const range = std::min(length, DoorLookYards);
    ClosedDoorCheck check(player, range);
    Acore::GameObjectListSearcher<ClosedDoorCheck> searcher(player, doors, check);
    Cell::VisitObjects(player, searcher, range);
    GameObject* door = nullptr;
    float best = 0.0f, nearX = 0.0f, nearY = 0.0f, nearZ = 0.0f;
    for (GameObject* object : doors)
    {
        float const ox = object->GetPositionX() - lx, oy = object->GetPositionY() - ly;
        float const along = (ox * dx + oy * dy) / (length * length);
        if (along <= 0.0f || along >= 1.0f || std::fabs(ox * dy - oy * dx) / length > DoorOffYards)
            continue;
        // (Before it on the leader's side, along the way from the leader to it.)
        float const toDoor = std::max(std::hypot(ox, oy), 0.1f);
        float const px = object->GetPositionX() - ox / toDoor * BeforeDoorYards, py = object->GetPositionY() - oy / toDoor * BeforeDoorYards;
        float const pz = object->GetPositionZ();
        if (!WalkReaches(player, lx, ly, lz, px, py, pz))
            continue;
        if (!door || along < best)
        {
            door = object;
            best = along;
            nearX = px;
            nearY = py;
            nearZ = pz;
        }
    }
    if (!door)
    {
        run.doorHop.Clear();
        return false;
    }
    std::string const doorName = door->GetGOInfo() ? door->GetGOInfo()->name : std::string("a door");
    if (run.doorHop != door->GetGUID())
    {
        run.doorHop = door->GetGUID();
        run.doorHopSince = now;
        LOG_INFO(ActivityLog, "activity: {}'s group finds no way on foot to {} in {}: {} stands closed between them, it walks to it first",
            bot.name, toName, plan.name, doorName);
    }
    if (getMSTimeDiff(run.doorHopSince, now) > DoorWalkMs)
        return false;
    if (player->GetExactDist2d(nearX, nearY) > 5.0f)
    {
        DungeonGoTo(player, plan.mapId, nearX, nearY, nearZ);
        rt.why = Acore::StringFormat("In {}: on the way to {} (through {})", plan.name, toName, doorName);
        return true;
    }
    // At the door: open it and step through it (a few yards beyond it, on the ground, with a way on from there).
    Map* map = player->GetMap();
    float x = 0.0f, y = 0.0f, z = 0.0f;
    bool found = false;
    for (float beyond : { 6.0f, 9.0f, 12.0f })
    {
        for (float turn : { 0.0f, 0.5f, -0.5f })
        {
            float const angle = std::atan2(door->GetPositionY() - nearY, door->GetPositionX() - nearX) + turn;
            float const cx = door->GetPositionX() + beyond * std::cos(angle), cy = door->GetPositionY() + beyond * std::sin(angle);
            float const cz = map->GetHeight(player->GetPhaseMask(), cx, cy, door->GetPositionZ() + 6.0f, true, 30.0f);
            if (cz <= INVALID_HEIGHT || std::fabs(cz - door->GetPositionZ()) > 8.0f)
                continue;
            PathGenerator probe(player);
            probe.CalculatePath(cx, cy, cz + 0.5f, toX, toY, toZ, false);
            if ((probe.GetPathType() & (PATHFIND_FARFROMPOLY_START | PATHFIND_NOPATH)) || !WalkReaches(player, cx, cy, cz, toX, toY, toZ))
                continue;
            x = cx;
            y = cy;
            z = cz + 0.5f;
            found = true;
            break;
        }
        if (found)
            break;
    }
    if (!found)
        return false;
    door->UseDoorOrButton(0, false, player);
    LOG_INFO(ActivityLog, "activity: {}'s group opens {} in {} and steps through it on its way to {}", bot.name, doorName, plan.name, toName);
    TakeGuestAlong(bot, player, x, y, z, Acore::StringFormat("through {}", doorName));
    for (Player* member : group)
    {
        if (!member->IsAlive() || member->GetMapId() != plan.mapId || !member->IsWithinDist2d(door->GetPositionX(), door->GetPositionY(), 60.0f))
            continue;
        member->NearTeleportTo(x, y, z, member->GetOrientation());
        if (BotRuntime* other = FindRuntime(member->GetGUID()))
            other->hasLastPosition = false;
    }
    run.doorHop.Clear();
    ClearTask(rt, 2 * IN_MILLISECONDS, "through a door");
    run.bossStuckSince = 0;
    return true;
}

bool OrderService::SameInstance(Player const* member, Player const* leader)
{
    return member && leader && member->GetMapId() == leader->GetMapId() && member->GetInstanceId() == leader->GetInstanceId();
}

bool OrderService::BindToLeaderInstance(Player* member, Player* leader)
{
    if (!member || !leader || !leader->IsInWorld() || !leader->GetMap()->IsDungeon() || !leader->GetInstanceId())
        return false;
    InstanceSave* save = sInstanceSaveMgr->GetInstanceSave(leader->GetInstanceId());
    if (!save)
        return false;
    Difficulty const difficulty = save->GetDifficulty();
    InstancePlayerBind* bind = sInstanceSaveMgr->PlayerGetBoundInstance(member->GetGUID(), leader->GetMapId(), difficulty);
    if (bind && bind->save == save)
        return true;
    // (A lockout of its own: a raid's. It keeps it, and the core sends it into that instance.)
    if (bind && bind->perm)
        return false;
    if (bind)
        sInstanceSaveMgr->PlayerUnbindInstance(member->GetGUID(), leader->GetMapId(), difficulty, true, member);
    sInstanceSaveMgr->PlayerBindToInstance(member->GetGUID(), save, false, member);
    return true;
}

// Creatures of a raid that burst when they die: those around them step out while one is nearly dead (docs/orders.md
// "Raid runs"). Garr's Firesworn cast Eruption at their death (boss_garr: fire around them, a knockback), and eight of
// them die beside the melee: the first Molten Core raid lost 30 characters in four seconds, 2026-11-21.
void OrderService::StepRaidBursts(BotRuntime::DungeonRun& run, std::vector<Player*> const& group, std::string const& raid)
{
    // (A creature with castSpell bursts by that spell, cast once when it falls below floorPct: from belowPct down to it
    // everybody but its tank keeps out, while it casts everybody, its tank too; below it and not casting, it has cast.)
    struct Burst { uint32 entry; float yards; float belowPct; uint32 castSpell; float floorPct; };
    static Burst const Bursts[] = {
        { 12099, 15.0f, 30.0f, 0, 0.0f },  // Firesworn (Garr, Molten Core): Eruption
        // Anubisath Defender (Temple of Ahn'Qiraj): Explode below 10 %, 4688 fire within 20 yd
        { 15277, 20.0f, 11.0f, 0, 0.0f },
        // Gurubashi Bat Rider (Zul'Gurub, Jeklik's trash): Unstable Concoction below 30 %, a cast of 3 s, 4375 fire
        // within 10 yd of it (the owner's raid of 20 stood at it and lost 15 members in one second, 2026-11-27)
        { 14750, 13.0f, 38.0f, 24024, 30.0f },
        // Unstable Corpse (Scholomance): Disease Burst at its death, 750 within 5 yd. It killed the owner's rogue with two
        // more creatures on it, the corpse at 8 %, 2026-11-26.
        { 10480, 5.0f, 20.0f, 0, 0.0f },
    };
    constexpr uint32 StepMs = 1500;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    AdventureGuild* guild = group.empty() ? nullptr : sGuildAdventureMgr.FindGuildOfMember(group.front()->GetGUID());
    auto roleOf = [&](Player* player)
    {
        GuildBot const* memberBot = guild ? guild->FindMember(player->GetGUID()) : nullptr;
        return memberBot ? sRoleService.EffectiveRole(*memberBot) : DungeonRole::None;
    };
    // (Who keeps apart from the others: a healer, and who fights from afar; not a tank.)
    auto spreads = [&](Player* player)
    {
        DungeonRole const role = roleOf(player);
        return role != DungeonRole::Tank && (role == DungeonRole::Healer || !BotDriver::FightsInMelee(player));
    };
    std::vector<Position> claimed;
    for (Player* member : group)
    {
        if (!member->IsAlive() || member->IsBeingTeleported() || !sGuildBotHolder.IsAIPlayed(member->GetGUID()))
            continue;
        uint32& next = run.burstStepAt[member->GetGUID()];
        if (now < next)
            continue;
        for (Burst const& burst : Bursts)
        {
            Creature* burster = member->FindNearestCreature(burst.entry, burst.yards + 2.0f, true);
            // (Not the one it holds: the tank of that creature stays, the others are out of the fire.)
            if (!burster)
                continue;
            bool const casting = burst.castSpell && burster->FindCurrentSpellBySpellId(burst.castSpell);
            if (!casting && (burster->GetHealthPct() > burst.belowPct || burster->GetVictim() == member ||
                (burst.castSpell && burster->GetHealthPct() <= burst.floorPct)))
                continue;
            float const angle = burster->GetAngle(member);
            Position spot = burster->GetPosition();
            burster->MovePositionToFirstCollision(spot, burst.yards + 4.0f, angle - burster->GetOrientation());
            BotDriver::PauseAI(member, StepMs);
            member->AttackStop();
            member->GetMotionMaster()->MovePoint(0, spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ(), FORCED_MOVEMENT_RUN);
            next = now + StepMs;
            if (!run.burstLogged.count(burst.entry))
            {
                run.burstLogged.insert(burst.entry);
                LOG_INFO(ActivityLog, "activity: the {} in {} steps out of the reach of {} (it bursts at its death: everybody but its tank keeps "
                    "{:.0f} yd away once it is below {:.0f}%)", group.size() > 5 ? "raid" : "group", raid, burster->GetName(), burst.yards,
                    burst.belowPct);
            }
            break;
        }
        // A bug the Twin Emperors' Vek'lor makes explode (Explode Bug: it glows for a few seconds, then bursts for 2813 fire
        // damage to all within 15 yd of it): everybody within 18 yd of one that glows steps out of it.
        if (now < next)
            continue;
        {
            std::list<Creature*> bugs;
            member->GetCreatureListWithEntryInGrid(bugs, 15316, 18.0f);  // Qiraji Scarab
            member->GetCreatureListWithEntryInGrid(bugs, 15317, 18.0f);  // Qiraji Scorpion
            Creature* glowing = nullptr;
            for (Creature* bug : bugs)
                if (bug->IsAlive() && bug->HasAura(804) && (!glowing || member->GetExactDist2d(bug) < member->GetExactDist2d(glowing)))
                    glowing = bug;
            if (glowing)
            {
                float const angle = glowing->GetAngle(member);
                Position spot = glowing->GetPosition();
                glowing->MovePositionToFirstCollision(spot, 20.0f, angle - glowing->GetOrientation());
                BotDriver::PauseAI(member, StepMs);
                member->GetMotionMaster()->MovePoint(0, spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ(), FORCED_MOVEMENT_RUN);
                next = now + StepMs;
                if (!run.burstLogged.count(804))
                {
                    run.burstLogged.insert(804);
                    LOG_INFO(ActivityLog, "activity: the raid in {} steps away from a bug that is about to explode ({}: everybody within "
                        "18 yd of it)", raid, glowing->GetName());
                }
                continue;
            }
        }
        // A member that carries a sickness that hits everybody beside it (an Anubisath Defender's Plague: 1008 nature
        // damage every 3 s to all within 5 yd of the one it lies on, for 40 s; the raid of 40 that stood stacked at its
        // tank lost 27 members to one Defender, 2026-11-23): it goes where nobody stands within 8 yd of it.
        if (now < next)
            continue;
        struct Sickness { uint32 aura; float yards; };
        static Sickness const Sicknesses[] = {
            { 26556, 8.0f },  // Plague (Anubisath Defender, Temple of Ahn'Qiraj): 5 yd
        };
        for (Sickness const& sickness : Sicknesses)
        {
            if (!member->HasAura(sickness.aura))
                continue;
            auto crowded = [&](float x, float y, float z)
            {
                for (Player* other : group)
                    if (other != member && other->IsAlive() && other->GetExactDist(x, y, z) < sickness.yards)
                        return true;
                return false;
            };
            if (!crowded(member->GetPositionX(), member->GetPositionY(), member->GetPositionZ()))
                continue;
            Map* map = member->GetMap();
            bool moved = false;
            for (float radius : { 8.0f, 12.0f, 16.0f, 20.0f, 25.0f })
            {
                for (int step = 0; step < 16 && !moved; ++step)
                {
                    float const angle = float(step) * float(M_PI) / 8.0f;
                    float const x = member->GetPositionX() + radius * std::cos(angle), y = member->GetPositionY() + radius * std::sin(angle);
                    float const z = map->GetHeight(member->GetPhaseMask(), x, y, member->GetPositionZ() + 5.0f, true, 20.0f);
                    if (z <= INVALID_HEIGHT || std::fabs(z - member->GetPositionZ()) > 6.0f || crowded(x, y, z))
                        continue;
                    PathGenerator probe(member);
                    probe.CalculatePath(x, y, z, false);
                    if (probe.GetPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY_START | PATHFIND_FARFROMPOLY_END))
                        continue;
                    BotDriver::PauseAI(member, StepMs);
                    member->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_RUN);
                    moved = true;
                }
                if (moved)
                    break;
            }
            if (!moved)
                continue;
            next = now + StepMs;
            if (!run.burstLogged.count(sickness.aura))
            {
                run.burstLogged.insert(sickness.aura);
                LOG_INFO(ActivityLog, "activity: in {} a member that carries {} goes where nobody stands within {:.0f} yd of it", raid,
                    sSpellMgr->GetSpellInfo(sickness.aura) ? sSpellMgr->GetSpellInfo(sickness.aura)->SpellName[0] : "a sickness",
                    sickness.yards);
            }
            break;
        }
        // A trap a boss puts under someone (Kurinnaxx's Sand Trap: 1750 damage and a silence of 20 s to all within 8 yd;
        // the raid that stood stacked at its tank was silenced and died in twelve seconds, 2026-11-22): everybody within
        // its reach steps out of it, its tank too.
        if (now < next)
            continue;
        struct Trap { uint32 entry; float yards; };
        static Trap const Traps[] = {
            { 180647, 9.0f },  // Sand Trap (Kurinnaxx, Ruins of Ahn'Qiraj)
        };
        for (Trap const& trap : Traps)
        {
            GameObject* object = member->FindNearestGameObject(trap.entry, trap.yards + 1.0f);
            if (!object || !object->isSpawned())
                continue;
            float const angle = object->GetAngle(member);
            Position spot = object->GetPosition();
            object->MovePositionToFirstCollision(spot, trap.yards + 4.0f, angle - object->GetOrientation());
            BotDriver::PauseAI(member, StepMs);
            member->GetMotionMaster()->MovePoint(0, spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ(), FORCED_MOVEMENT_RUN);
            next = now + StepMs;
            if (!run.burstLogged.count(trap.entry))
            {
                run.burstLogged.insert(trap.entry);
                LOG_INFO(ActivityLog, "activity: the raid in {} steps out of {} (everybody within {:.0f} yd of it)", raid,
                    object->GetGOInfo()->name, trap.yards);
            }
            break;
        }
        // A creature that whirls (Battleguard Sartura for 15 s, her Royal Guards for 8 s: a whirlwind that hits
        // everybody within 10 yd once a second and runs after a random member, whoever holds it; the raid of 40 stacked at
        // its tank lost half of its members in ten seconds, 2026-11-22): everybody within its reach steps out of it, its
        // tank too, and a member that fights from afar or heals keeps out of its reach while it lives.
        if (now < next)
            continue;
        // (Aura 0: always, while it lives and fights. Emperor Vek'lor's Arcane Burst is the twins' step's: his half keeps
        // its distances there, its warrior tank nearest to him for his brother after the swap, StepRaidTwins.)
        struct Whirl { uint32 entry; uint32 aura; float yards; };
        static Whirl const Whirls[] = {
            { 15516, 26083, 12.0f },  // Battleguard Sartura: Whirlwind (10 yd)
            { 15984, 26038, 12.0f },  // Sartura's Royal Guard: Whirlwind (10 yd)
            { 15712, 0, 11.0f },      // Ouro's Dirt Mound (Temple of Ahn'Qiraj): Quake, 675 nature within 8 yd, it runs after a random member
            { 15925, 0, 9.0f },       // Viscidus' Toxic Slime: Toxin, nature damage every 2 s and slowed within its cloud
            // Bloodlord Mandokir (Zul'Gurub): Whirlwind, weapon damage within 8 yd every 2 s (it killed three of
            // the owner's melee in one second, 2026-11-26)
            { 11382, 13736, 12.0f },
        };
        bool const fromAfar = !BotDriver::FightsInMelee(member);
        for (Whirl const& whirl : Whirls)
        {
            float const yards = fromAfar ? whirl.yards + 6.0f : whirl.yards;
            std::list<Creature*> whirlers;
            member->GetCreatureListWithEntryInGrid(whirlers, whirl.entry, yards + 1.0f);
            Creature* whirler = nullptr;
            for (Creature* creature : whirlers)
                if (creature->IsAlive() && creature->IsInCombat() && (fromAfar || !whirl.aura || creature->HasAura(whirl.aura)) &&
                    (!whirler || member->GetExactDist2d(creature) < member->GetExactDist2d(whirler)))
                    whirler = creature;
            if (!whirler)
                continue;
            float const angle = whirler->GetAngle(member);
            Position spot = whirler->GetPosition();
            whirler->MovePositionToFirstCollision(spot, yards + 6.0f, angle - whirler->GetOrientation());
            BotDriver::PauseAI(member, StepMs);
            if (!fromAfar)
                member->AttackStop();
            member->GetMotionMaster()->MovePoint(0, spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ(), FORCED_MOVEMENT_RUN);
            next = now + StepMs;
            if (!run.burstLogged.count(whirl.aura ? whirl.aura : whirl.entry))
            {
                run.burstLogged.insert(whirl.aura ? whirl.aura : whirl.entry);
                LOG_INFO(ActivityLog, "activity: the raid in {} steps out of the reach of {} (everybody within {:.0f} yd of it, "
                    "its tank too; who fights from afar or heals keeps {:.0f} yd from it)", raid, whirler->GetName(), whirl.yards,
                    whirl.yards + 6.0f);
            }
            break;
        }
        // A boss that blasts what stands in front of it (Ouro: Sand Blast, 3400 nature damage every 20 s to everybody in a
        // cone of 65 yd in front of him, and their threat on him wiped; he turns to his tank for it): who is not a tank
        // and stands in front of it goes behind it, the melee within its reach, the others at 12 yd beyond it.
        if (now < next)
            continue;
        struct Frontal { uint32 entry; float arc; };
        static Frontal const Frontals[] = {
            { 15517, float(M_PI) * 0.75f },  // Ouro (Temple of Ahn'Qiraj): Sand Blast
        };
        for (Frontal const& frontal : Frontals)
        {
            Creature* boss = member->FindNearestCreature(frontal.entry, 70.0f, true);
            if (!boss || !boss->IsInCombat() || boss->GetVictim() == member || roleOf(member) == DungeonRole::Tank ||
                !boss->HasInArc(frontal.arc, member))
                continue;
            float const reach = boss->GetCombatReach();
            float const yards = BotDriver::FightsInMelee(member) ? std::max(4.0f, reach * 0.8f) : reach + 12.0f;
            float const aside = (member->GetGUID().GetCounter() % 2) ? 0.6f : -0.6f;
            Position spot = boss->GetPosition();
            boss->MovePositionToFirstCollision(spot, yards, float(M_PI) + aside);
            BotDriver::PauseAI(member, StepMs);
            member->GetMotionMaster()->MovePoint(0, spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ(), FORCED_MOVEMENT_RUN);
            next = now + StepMs;
            if (!run.burstLogged.count(frontal.entry))
            {
                run.burstLogged.insert(frontal.entry);
                LOG_INFO(ActivityLog, "activity: the raid in {} keeps out of the front of {} (who is not a tank goes behind it)", raid,
                    boss->GetName());
            }
            break;
        }
        // A boss whose poison hits everybody near the one it lands on (Princess Huhuran: Noxious Poison on a random member,
        // 750 nature damage every 2 s and a silence of 8 s to all within 15 yd of it; her Poison Bolt hits the 15 nearest;
        // the raid of 40 stood within 20 yd of her and was silenced and killed in 80 s, 2026-11-23): a healer and who fights
        // from afar keep between a few and 34 yd from her (in reach of their spells and of her tank) and 10 yd from each
        // other while she lives, so that a poison hits few of them and the bolts fall on the melee.
        // High Priest Venoxis (Zul'Gurub) casts Holy Wrath at the one he hits in his troll form: it jumps to the
        // nearest other within 10 yd and both their sizes, up to 50 times, and does half as much again at each jump
        // (300, then 450, 675 ...). The owner's raid of 20 stood together at its tank and lost seven at once, 3085 to 55304
        // on the sixth to thirteenth jump, 2026-11-26. While he is above half his health (his snake form at 50 % casts
        // it no more) everybody but the one he hits waits 22 to 34 yd from him and 16 yd from the others, the melee too
        // (their AI is held there and they do not attack): the wrath finds nobody to jump to from his tank. Below it
        // the melee come in.
        if (now < next)
            continue;
        struct Spread { uint32 entry; float minYards; float maxYards; float apart; float keep; float holdAbovePct; };
        static Spread const Spreads[] = {
            { 15509, 23.0f, 34.0f, 10.0f, 7.0f, 0.0f },   // Princess Huhuran (Temple of Ahn'Qiraj): Noxious Poison
            { 14507, 22.0f, 34.0f, 16.0f, 14.0f, 50.0f }, // High Priest Venoxis (Zul'Gurub): Holy Wrath, troll form
        };
        for (Spread const& spread : Spreads)
        {
            Creature* boss = member->FindNearestCreature(spread.entry, 90.0f, true);
            if (!boss || !boss->IsInCombat())
                continue;
            bool const holdAll = spread.holdAbovePct > 0.0f && boss->GetHealthPct() > spread.holdAbovePct;
            // (Who keeps apart: a healer and who fights from afar; while he holds everybody, all but the one he hits.)
            auto keepsApart = [&](Player* player)
            {
                if (holdAll)
                    return boss->GetVictim() != player;
                return spreads(player);
            };
            if (!keepsApart(member))
                continue;
            // (A melee member that something else attacks fights it: a creature left beside him for the boss fight.)
            bool held = holdAll && !spreads(member);
            for (Unit* attacker : member->getAttackers())
                if (held && attacker != boss)
                    held = false;
            if (holdAll && !spreads(member) && !held)
                continue;
            std::vector<Player*> others;
            for (Player* other : group)
                if (other != member && other->IsAlive() && keepsApart(other))
                    others.push_back(other);
            auto crowded = [&](float x, float y, float z, float yards)
            {
                for (Player* other : others)
                    if (other->GetExactDist(x, y, z) < yards)
                        return true;
                for (Position const& spot : claimed)
                    if (spot.GetExactDist(x, y, z) < yards)
                        return true;
                return false;
            };
            float const fromBoss = member->GetExactDist2d(boss);
            if (fromBoss >= spread.minYards - 2.0f && fromBoss <= spread.maxYards + 4.0f &&
                !crowded(member->GetPositionX(), member->GetPositionY(), member->GetPositionZ(), spread.keep))
            {
                // (A melee member that waits out there: it does not walk in to attack him.)
                if (held)
                {
                    BotDriver::PauseAI(member, StepMs);
                    if (member->GetVictim())
                        member->AttackStop();
                    if (member->isMoving())
                    {
                        member->GetMotionMaster()->Clear();
                        member->GetMotionMaster()->MoveIdle();
                    }
                }
                continue;
            }
            // (Places around her, nearest to the member first, on her floor, in her sight, apart from the others.)
            Map* map = boss->GetMap();
            struct Spot { float x, y, z, d; };
            std::vector<Spot> spots;
            for (float radius = spread.minYards + 1.0f; radius <= spread.maxYards; radius += 3.0f)
                for (int step = 0; step < 24; ++step)
                {
                    float const angle = float(step) * float(M_PI) / 12.0f;
                    float const x = boss->GetPositionX() + radius * std::cos(angle), y = boss->GetPositionY() + radius * std::sin(angle);
                    float const z = map->GetHeight(boss->GetPhaseMask(), x, y, boss->GetPositionZ() + 5.0f, true, 20.0f);
                    if (z <= INVALID_HEIGHT || std::fabs(z - boss->GetPositionZ()) > 6.0f || crowded(x, y, z, spread.apart) ||
                        !boss->IsWithinLOS(x, y, z + 2.0f))
                        continue;
                    spots.push_back({ x, y, z, member->GetExactDist(x, y, z) });
                }
            std::sort(spots.begin(), spots.end(), [](Spot const& a, Spot const& b) { return a.d < b.d; });
            bool moved = false;
            for (std::size_t i = 0; i < spots.size() && i < 8 && !moved; ++i)
            {
                PathGenerator probe(member);
                probe.CalculatePath(spots[i].x, spots[i].y, spots[i].z, false);
                if (probe.GetPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY_START | PATHFIND_FARFROMPOLY_END))
                    continue;
                claimed.push_back(Position(spots[i].x, spots[i].y, spots[i].z));
                BotDriver::PauseAI(member, StepMs);
                if (held && member->GetVictim())
                    member->AttackStop();
                member->GetMotionMaster()->MovePoint(0, spots[i].x, spots[i].y, spots[i].z, FORCED_MOVEMENT_RUN);
                moved = true;
            }
            if (!moved)
            {
                if (held)
                    BotDriver::PauseAI(member, StepMs);
                continue;
            }
            next = now + StepMs + 500;
            if (!run.burstLogged.count(spread.entry))
            {
                run.burstLogged.insert(spread.entry);
                if (spread.holdAbovePct > 0.0f)
                    LOG_INFO(ActivityLog, "activity: the raid in {} spreads out around {} (everybody but "
                        "the one it hits keeps {:.0f} to {:.0f} yd from it and {:.0f} yd from each other while it is "
                        "above {:.0f}% health; the melee wait out there until then)", raid, boss->GetName(),
                        spread.minYards, spread.maxYards, spread.apart, spread.holdAbovePct);
                else
                    LOG_INFO(ActivityLog, "activity: the raid in {} spreads out around {} (who heals or "
                        "fights from afar keeps {:.0f} to {:.0f} yd from it and {:.0f} yd from each other)", raid,
                        boss->GetName(), spread.minYards, spread.maxYards, spread.apart);
            }
            break;
        }
    }
}

// The Twin Emperors on two sides (Temple of Ahn'Qiraj). Each heals the other 30000 every 3.6 s while they stand within
// 60 yd of each other, both are immune to taunt, Vek'nilash to magic and Vek'lor to physical damage, and every 30 to 40 s
// they swap places, after which each attacks the member nearest to where it appears. The raid fought them on one place
// and was wiped out four times with them at 82 to 100 %, 2026-11-23. So the raid is split in two halves, each at one
// throne with a tank, healers, melee and casters, and nobody crosses: the half at Vek'nilash's place fights him with
// its tank, melee and hunters (its casters wait), the half at Vek'lor's place him with its casters (its tank and melee
// wait out of his reach); after a swap the roles of the two halves change. Vek'lor's first victim is a caster of his
// own half (his half opens on him in the second the tank opens on his brother), and he stands still while his victim is
// within 45 yd, so every swap puts each twin where a half stands.
namespace
{
    constexpr uint32 TwinVeknilash = 15275, TwinVeklor = 15276;
    // The highest rank of a spell the character knows (0: none).
    uint32 KnownRankOf(Player* player, uint32 firstRank)
    {
        for (uint32 id = sSpellMgr->GetLastSpellInChain(firstRank); id; id = sSpellMgr->GetPrevSpellInChain(id))
            if (player->HasSpell(id))
                return id;
        return player->HasSpell(firstRank) ? firstRank : 0;
    }
    enum TwinKind : uint8 { TwinTank = 0, TwinHealer = 1, TwinPhysical = 2, TwinMagic = 3, TwinLockTank = 4 };

    // A protection potion out of the member's bags or the guild storage: when its effect is gone and its two minutes are
    // over (the owner's guides of 2026-11-23/24: shadow protection for the warlocks that tank Vek'lor, nature protection for
    // the hunter that takes the first beams of C'Thun's Eye).
    void DrinkProtection(Player* member, uint32& drank, uint32 now, std::initializer_list<uint32> entries,
        std::initializer_list<uint32> auras, std::string const& purpose)
    {
        for (uint32 aura : auras)
            if (member->HasAura(aura))
                return;
        if (drank && getMSTimeDiff(drank, now) < 120 * IN_MILLISECONDS)
            return;
        for (uint32 entry : entries)
        {
            std::string error;
            bool fromStorage = false;
            if (!member->HasItemCount(entry, 1))
            {
                if (!sStorageService.Withdraw(member, entry, 0, 1, error))
                    continue;
                fromStorage = true;
            }
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
            uint32 const spell = proto ? uint32(proto->Spells[0].SpellId) : 0;
            if (!spell)
                continue;
            member->CastSpell(member, spell, true);
            member->DestroyItemCount(entry, 1, true);
            drank = now;
            LOG_INFO(ActivityLog, "activity: {} drinks 1 x {}{} {}", member->GetName(), proto->Name1,
                fromStorage ? " out of the guild storage" : "", purpose);
            return;
        }
    }
    // The shadow protection of a warlock that tanks Vek'lor: before the pull and again when it is gone.
    void DrinkTwinPotion(Player* member, uint32& drank, uint32 now, Creature const* lor)
    {
        // Greater Shadow Protection Potion, Shadow Protection Potion.
        DrinkProtection(member, drank, now, { 13459u, 6048u }, { 17548u, 7242u }, "to tank " + lor->GetName());
    }
}

bool OrderService::StepTwinGuards(GuildBot const& bot, Player* leader, BotRuntime& rt)
{
    // Vek'lor's guards first: the five Anubisath Defenders of the twins' door and the hall north of it walk in Vek'lor's
    // formation (creature_formations, its members assist their leader): all of them came into the twins' fight, the
    // last from 365 yd, and killed most of the raid in every try of the day (2026-11-25). Each is fought before the
    // twins, and before the raid goes down into their room: on the floor its bugs attacked the raid whenever it walked
    // across, and the tank that went to help walked towards Vek'nilash's throne and pulled both twins (2026-11-25). The
    // leader goes to the nearest guard wherever it walks, the raid with it, and pulls it from 40 yd.
    BotRuntime::DungeonRun& run = rt.dungeon;
    Creature* lor = leader->FindNearestCreature(15276, 450.0f, true);
    Creature* nilash = leader->FindNearestCreature(15275, 450.0f, true);
    if (!lor || lor->IsInCombat() || (nilash && nilash->IsInCombat()))
        return false;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    // (One at a time: a guard is pulled only while no other walks within 45 yd of it. Two of them walk past each other
    // at the north end of the hall, and a pull there brought three and wiped the raid, 2026-11-25.)
    std::vector<Creature*> guards;
    if (CreatureGroup const* formation = lor->GetFormation())
        for (auto const& [member, info] : formation->GetMembers())
            if (member && member != lor && member != nilash && member->IsInWorld() && member->IsAlive())
                guards.push_back(member);
    uint32 const count = guards.size();
    Creature* guard = nullptr;
    Creature* nearest = nullptr;
    for (Creature* one : guards)
    {
        if (!nearest || leader->GetExactDist(one) < leader->GetExactDist(nearest))
            nearest = one;
        bool alone = true;
        for (Creature* other : guards)
            if (other != one && other->GetExactDist(one) < 45.0f)
                alone = false;
        if ((alone || one->IsInCombat()) && (!guard || leader->GetExactDist(one) < leader->GetExactDist(guard)))
            guard = one;
    }
    if (!nearest)
    {
        if (run.twinGuards)
        {
            run.twinGuards = false;
            LOG_INFO(ActivityLog, "activity: {}'s group has killed the guards of {}", bot.name, lor->GetName());
        }
        return false;
    }
    if (!run.twinGuards)
    {
        run.twinGuards = true;
        LOG_INFO(ActivityLog, "activity: {}'s group fights the guards of {} before the Twin Emperors: {} {} in his formation "
            "(they come into the twins' fight from wherever they walk)", bot.name, lor->GetName(), count, nearest->GetName());
    }
    if (!guard)
    {
        // (None alone: the raid holds where it stands, out of their reach, until their ways part.)
        PauseStuck(rt);
        if (leader->isMoving() && leader->GetExactDist(nearest) < 60.0f && !leader->IsInCombat())
        {
            leader->GetMotionMaster()->Clear();
            leader->StopMoving();
        }
        rt.why = Acore::StringFormat("In Ahn'Qiraj Temple: waiting until the guards of {} walk apart", lor->GetName());
        return true;
    }
    PauseStuck(rt);
    rt.why = Acore::StringFormat("In Ahn'Qiraj Temple: fighting the guards of {} before the Twin Emperors", lor->GetName());
    if (guard->IsInCombat() || leader->GetVictim() == guard || leader->IsInCombat())
        return true;
    if (leader->GetExactDist(guard) < 40.0f && leader->IsWithinLOSInMap(guard))
    {
        if (rt.task.kind != TaskKind::None)
            ClearTask(rt, 0, "at a guard of the Twin Emperors");
        BotDriver::Attack(leader, guard, false);
        LOG_INFO(ActivityLog, "activity: {}'s group fights {} (one of the guards of {}) at ({:.0f}, {:.0f})", bot.name,
            guard->GetName(), lor->GetName(), guard->GetPositionX(), guard->GetPositionY());
        return true;
    }
    if (getMSTimeDiff(rt.task.lastAssert, now) >= 2 * IN_MILLISECONDS)
    {
        // (Straight on the path finder's way, its AI and its task held: the walk to the twins' approach place went on
        // beside a plain walk order, and the run's way through the dungeon, which a walk of its leader follows, led it
        // towards Vek'nilash's throne and pulled both twins, 2026-11-25.)
        if (rt.task.kind != TaskKind::None)
            ClearTask(rt, 0, "to a guard of the Twin Emperors");
        BotDriver::PauseAI(leader, 2500);
        leader->GetMotionMaster()->Clear();
        leader->GetMotionMaster()->MovePoint(0, guard->GetPositionX(), guard->GetPositionY(), guard->GetPositionZ(),
            FORCED_MOVEMENT_RUN);
        rt.task.lastAssert = now;
    }
    return true;
}

bool OrderService::StepTwinsPull(AdventureGuild& guild, GuildBot const& bot, Player* leader, BotRuntime& rt,
    std::vector<Player*> const& group, Creature* nilash)
{
    BotRuntime::DungeonRun& run = rt.dungeon;
    Creature* lor = leader->FindNearestCreature(TwinVeklor, 300.0f, true);
    if (!lor || !nilash->IsAlive())
        return false;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    bool const fighting = nilash->IsInCombat() || lor->IsInCombat();
    // (The fight on two sides is StepRaidTwins'; a fight that began while the halves took their places counts as pulled.)
    if (fighting)
    {
        if (run.twinSide.empty())
            return false;
        if (!run.twinPulledAt)
        {
            run.twinPulledAt = now;
            Unit const* first = nilash->GetVictim() ? nilash->GetVictim() : lor->GetVictim();
            LOG_INFO(ActivityLog, "activity: the Twin Emperors came while {}'s group took its places: {} on {} ({:.0f} yd from {}, "
                "{:.0f} yd from {})", bot.name, nilash->GetVictim() ? nilash->GetName() : lor->GetName(), first ? first->GetName() : "nobody",
                first ? first->GetExactDist2d(nilash) : 0.0f, nilash->GetName(), first ? first->GetExactDist2d(lor) : 0.0f, lor->GetName());
        }
        return true;
    }
    // A new try after a wipe: the places again.
    if (run.twinPulledAt && getMSTimeDiff(run.twinPulledAt, now) > 20 * IN_MILLISECONDS)
    {
        run.twinPulledAt = 0;
        run.twinPlacesSince = 0;
        run.twinPlacedAt = 0;
        run.twinSwapAt = 0;
        run.twinOpened = false;
        run.twinBugsSince = 0;
        run.twinBugsDone = false;
    }
    if (run.twinPulledAt)
        return true;
    // The Anubisath Defender that walks the room first, and no placing while anybody fights (its Plague and its Swarmguards
    // came into the twins' fight and pulled them onto the raid at their thrones, 2026-11-23).
    bool anyFight = false;
    for (Player* member : group)
        if (member->IsAlive() && member->IsInCombat())
        {
            anyFight = true;
            break;
        }
    if (anyFight && (!run.twinBugsSince || run.twinBugsDone))
    {
        run.twinPlacesSince = 0;
        run.twinPlacedAt = 0;
        return true;
    }
    // The bugs of their room first, as a raid of players clears them: Vek'nilash's Mutate Bug makes one within 25 yd of
    // him hostile with 1800 % more damage, Vek'lor's Explode Bug makes one within 40 yd of him burst (spells 802, 804),
    // the bugs the raid's area spells hit attacked its tanks (up to 13 on Vek'nilash's tank, 2026-11-24), and a bug that
    // met a member on its way to its place drew the tank that went to help into Vek'nilash's reach and both twins onto
    // the raid (2026-11-24). So every Qiraji Scarab and Scorpion within 45 yd of a half's place is killed before the
    // places, at least 32 yd from both thrones, by the raid's damage dealers (nobody's way within 35 yd of a throne).
    // Those nearer to a throne are left: six mages sent to a half's place to pull them with a single spell each pulled
    // both twins, 2026-11-25. They come back after 5 minutes; the clearing ends after 2 minutes whatever is left.
    if (!run.twinPlacesSince && !run.twinBugsDone)
    {
        Position const thrones[2] = { nilash->GetHomePosition(), lor->GetHomePosition() };
        float px[2], py[2];
        for (uint8 side = 0; side < 2; ++side)
        {
            float const dx = -8930.0f - thrones[side].GetPositionX(), dy = 1344.0f - thrones[side].GetPositionY();
            float const length = std::max(1.0f, std::hypot(dx, dy));
            float const yards = side == 0 ? 33.0f : 30.0f;
            px[side] = thrones[side].GetPositionX() + yards * dx / length;
            py[side] = thrones[side].GetPositionY() + yards * dy / length;
        }
        auto throneDist = [&](float x, float y)
        {
            return std::min(std::hypot(x - thrones[0].GetPositionX(), y - thrones[0].GetPositionY()),
                std::hypot(x - thrones[1].GetPositionX(), y - thrones[1].GetPositionY()));
        };
        // (The way of a member to a point: nowhere within 35 yd of a throne, or no nearer to it than the member stands: the
        // raid waits 27 yd from Vek'nilash's throne, and with the 35 yd alone nobody went for a bug, 2026-11-25.)
        auto wayClear = [&](Player* member, float x, float y)
        {
            float const ax = member->GetPositionX(), ay = member->GetPositionY(), bx = x - ax, by = y - ay;
            float const len2 = bx * bx + by * by;
            for (Position const& home : thrones)
            {
                float t = len2 > 0.0f ? ((home.GetPositionX() - ax) * bx + (home.GetPositionY() - ay) * by) / len2 : 0.0f;
                t = std::clamp(t, 0.0f, 1.0f);
                float const closest = std::hypot(ax + t * bx - home.GetPositionX(), ay + t * by - home.GetPositionY());
                if (closest < std::min(35.0f, member->GetExactDist2d(home.GetPositionX(), home.GetPositionY()) - 1.0f))
                    return false;
            }
            return true;
        };
        std::list<Creature*> found;
        for (uint32 entry : { 15316u, 15317u })
            nilash->GetCreatureListWithEntryInGrid(found, entry, 220.0f);
        std::vector<Creature*> farBugs, nearBugs;
        for (Creature* bug : found)
        {
            if (!bug->IsAlive() || std::fabs(bug->GetPositionZ() - thrones[0].GetPositionZ()) > 15.0f)
                continue;
            float const toPlace = std::min(bug->GetExactDist2d(px[0], py[0]), bug->GetExactDist2d(px[1], py[1]));
            if (toPlace >= 45.0f)
                continue;
            if (throneDist(bug->GetPositionX(), bug->GetPositionY()) >= 32.0f)
                farBugs.push_back(bug);
            else if (toPlace < 28.0f)
                nearBugs.push_back(bug);
        }
        if (!run.twinBugsSince)
        {
            run.twinBugsSince = now;
            LOG_INFO(ActivityLog, "activity: {}'s group clears the bugs of the Twin Emperors' room first: {} on the floor, {} near "
                "their thrones (within 45 yd of the places of its halves)", bot.name, farBugs.size(), nearBugs.size());
        }
        uint32 const clearingFor = getMSTimeDiff(run.twinBugsSince, now);
        if (farBugs.empty() || clearingFor >= 120 * IN_MILLISECONDS)
        {
            run.twinBugsDone = true;
            LOG_INFO(ActivityLog, "activity: {}'s group has cleared the bugs of the Twin Emperors' room after {} s ({} left on the "
                "floor, {} near the thrones)", bot.name, clearingFor / IN_MILLISECONDS, farBugs.size(), nearBugs.size());
        }
        else
        {
            std::unordered_map<Creature*, uint32> taken;
            for (Player* member : group)
                if (Unit* victim = member->GetVictim(); victim && victim->IsAlive() && (victim->GetEntry() == 15316 ||
                    victim->GetEntry() == 15317))
                    if (Creature* bug = victim->ToCreature())
                        ++taken[bug];
            for (Player* member : group)
            {
                if (!member->IsAlive() || !sGuildBotHolder.IsAIPlayed(member->GetGUID()) || member->IsBeingTeleported())
                    continue;
                GuildBot const* memberBot = guild.FindMember(member->GetGUID());
                DungeonRole const memberRole = memberBot ? sRoleService.EffectiveRole(*memberBot) : DungeonRole::None;
                if (memberRole == DungeonRole::Healer)
                    continue;
                // (The tanks hold where they stand: the leader went to help members against bugs, 5 to 35 yd each time,
                // and was in Vek'nilash's reach 2 minutes later, 2026-11-25.)
                if (memberRole == DungeonRole::Tank || member == leader)
                {
                    BotDriver::PauseAI(member, 2000);
                    if (member->isMoving())
                    {
                        member->GetMotionMaster()->Clear();
                        member->GetMotionMaster()->MoveIdle();
                        member->StopMoving();
                    }
                    continue;
                }
                if (Unit* victim = member->GetVictim(); victim && victim->IsAlive())
                    continue;
                uint32& at = run.twinStepAt[member->GetGUID()];
                if (now < at)
                    continue;
                at = now + 1500;
                // A bug on the floor: the nearest one that two others do not fight yet, on a way clear of the thrones; a melee
                // fighter only one 45 yd from both (one 34 yd from Vek'nilash's throne, with the bugs its fight called,
                // pulled both twins 16 s into the clearing, 2026-11-25).
                bool const melee = BotDriver::FightsInMelee(member);
                Creature* nearest = nullptr;
                for (Creature* bug : farBugs)
                    if (taken[bug] < 2 && (!melee || throneDist(bug->GetPositionX(), bug->GetPositionY()) >= 45.0f) &&
                        wayClear(member, bug->GetPositionX(), bug->GetPositionY()) &&
                        (!nearest || member->GetExactDist2d(bug) < member->GetExactDist2d(nearest)))
                        nearest = bug;
                if (!nearest)
                    continue;
                ++taken[nearest];
                BotDriver::Attack(member, nearest, false);
            }
            rt.why = Acore::StringFormat("In Ahn'Qiraj Temple: the raid clears the bugs of the Twin Emperors' room ({} left)",
                farBugs.size() + nearBugs.size());
            return true;
        }
    }
    if (anyFight)
    {
        run.twinPlacesSince = 0;
        run.twinPlacedAt = 0;
        return true;
    }
    // Rested before the places: everybody at 70 % health, the healers at 60 % mana (after a wipe the raid came back at
    // half its health and mana and was pulled so, 2026-11-23); they rest by themselves meanwhile.
    for (Player* member : group)
    {
        if (!member->IsAlive())
            continue;
        GuildBot const* memberBot = guild.FindMember(member->GetGUID());
        bool const healer = memberBot && sRoleService.EffectiveRole(*memberBot) == DungeonRole::Healer;
        if (member->GetHealthPct() < 70.0f || (healer && member->GetMaxPower(POWER_MANA) > 0 &&
            member->GetPower(POWER_MANA) * 100 < member->GetMaxPower(POWER_MANA) * 60))
        {
            run.twinPlacesSince = 0;
            run.twinPlacedAt = 0;
            rt.why = Acore::StringFormat("In Ahn'Qiraj Temple: the raid rests before the Twin Emperors ({} first)", member->GetName());
            return true;
        }
    }
    // No mark on a twin before the pull: mod-playerbots' damage dealers attack the creature their icon marks as soon as
    // they see it, and a skull left on Vek'nilash after a wipe pulled both twins onto the raid the moment it came back
    // (2026-11-23).
    if (Group* party = leader->GetGroup())
        for (uint8 icon : { SkullIcon, CrossIcon })
            if (ObjectGuid const marked = party->GetTargetIcon(icon); marked == nilash->GetGUID() || marked == lor->GetGUID())
                party->SetTargetIcon(icon, leader->GetGUID(), ObjectGuid::Empty);
    if (run.skull == nilash->GetGUID() || run.skull == lor->GetGUID())
        run.skull.Clear();
    run.twinMarks = false;
    // The places: in front of each throne towards the door of their room, just out of the twins' reach (their aggro
    // range is 21.5 yd at the server's aggro rate, 27.5 yd from their middle with both sizes; at 20 yd the tank walking
    // to its place pulled both twins onto the raid, 2026-11-23): 33 yd from Vek'nilash's throne, 30 yd from Vek'lor's,
    // where the spells of his half's casters reach him (30 yd and both sizes, 36 yd), 130 yd apart.
    Position const homeA = nilash->GetHomePosition(), homeB = lor->GetHomePosition();
    float const cx = (homeA.GetPositionX() + homeB.GetPositionX()) / 2.0f, cy = (homeA.GetPositionY() + homeB.GetPositionY()) / 2.0f;
    run.twinCenterX = cx;
    run.twinCenterY = cy;
    Map* map = leader->GetMap();
    Position const homes[2] = { homeA, homeB };
    constexpr float TwinDoorX = -8930.0f, TwinDoorY = 1344.0f;
    for (uint8 side = 0; side < 2; ++side)
    {
        float const dx = TwinDoorX - homes[side].GetPositionX(), dy = TwinDoorY - homes[side].GetPositionY();
        float const length = std::max(1.0f, std::hypot(dx, dy));
        float const yards = side == 0 ? 33.0f : 30.0f;
        run.twinX[side] = homes[side].GetPositionX() + yards * dx / length;
        run.twinY[side] = homes[side].GetPositionY() + yards * dy / length;
        float const z = map->GetHeight(leader->GetPhaseMask(), run.twinX[side], run.twinY[side], homes[side].GetPositionZ() + 5.0f, true, 30.0f);
        run.twinZ[side] = z > INVALID_HEIGHT ? z : homes[side].GetPositionZ();
    }
    // The halves (the owner's guide of 2026-11-23): at each end of the room a warrior tank for Vek'nilash and a warlock that
    // tanks Vek'lor with Searing Pain; healers, melee and hunters, casters each kind in turn to the one side and the
    // other. A third tank fights at Vek'nilash's side too, to take him when his first tank dies; more tanks fight as
    // melee.
    if (run.twinSide.empty())
    {
        uint8 next[5] = { 0, 0, 0, 0, 0 };
        auto give = [&](Player* member, uint8 kind)
        {
            // (The leader is the first tank: it takes Vek'nilash's side.)
            uint8 const side = member == leader ? 0 : uint8(next[kind]++ % 2);
            if (member == leader)
                ++next[kind];
            run.twinSide[member->GetGUID()] = { side, kind };
        };
        if (std::find(group.begin(), group.end(), leader) != group.end())
            give(leader, TwinTank);
        for (Player* member : group)
        {
            if (member == leader)
                continue;
            GuildBot const* memberBot = guild.FindMember(member->GetGUID());
            DungeonRole const role = memberBot ? sRoleService.EffectiveRole(*memberBot) : DungeonRole::None;
            uint8 const playerClass = member->getClass();
            bool const magic = playerClass == CLASS_MAGE || playerClass == CLASS_WARLOCK || playerClass == CLASS_PRIEST ||
                ((playerClass == CLASS_DRUID || playerClass == CLASS_SHAMAN) && !BotDriver::FightsInMelee(member));
            uint8 kind = role == DungeonRole::Tank ? TwinTank : role == DungeonRole::Healer ? TwinHealer : magic ? TwinMagic : TwinPhysical;
            if (kind == TwinTank && next[TwinTank] >= 3)
                kind = TwinPhysical;
            if (kind == TwinMagic && playerClass == CLASS_WARLOCK && next[TwinLockTank] < 2)
                kind = TwinLockTank;
            give(member, kind);
        }
        // Shadow resistance for the warlocks that tank Vek'lor (his Shadow Bolt: 3600 shadow damage every 2.5 s on his
        // victim, boss_twinemperors; the warlock of his half died 15 s into the fight, 2026-11-24): every priest keeps
        // Shadow Protection on the raid, the first paladin healer of each half its Shadow Resistance Aura (the guide's
        // Alliance raid).
        bool auraGiven[2] = { false, false };
        for (Player* member : group)
        {
            auto const itr = run.twinSide.find(member->GetGUID());
            if (itr == run.twinSide.end() || !sGuildBotHolder.IsAIPlayed(member->GetGUID()))
                continue;
            bool shadow = member->getClass() == CLASS_PRIEST;
            if (member->getClass() == CLASS_PALADIN && itr->second.second == TwinHealer && !auraGiven[itr->second.first])
                shadow = auraGiven[itr->second.first] = true;
            if (shadow)
                BotDriver::SetStrategyEverywhere(member, "rshadow", true);
        }
    }
    if (!run.twinPlacesSince)
    {
        run.twinPlacesSince = now;
        LOG_INFO(ActivityLog, "activity: {}'s group takes the Twin Emperors on two sides: half of it at the throne of {}, half at "
            "the throne of {}, their places {:.0f} yd apart (each heals the other within 60 yd)", bot.name, nilash->GetName(), lor->GetName(),
            std::hypot(run.twinX[0] - run.twinX[1], run.twinY[0] - run.twinY[1]));
    }
    // Everybody to its place: the tanks on it, at Vek'lor's the casters in a row across it (in the reach of their spells),
    // the others in rows behind it (away from the throne).
    uint8 count[2] = { 0, 0 }, casters = 0;
    bool placed = true;
    Player* tankA = nullptr;
    Player* lockB = nullptr;
    std::vector<Player*> openers;
    for (Player* member : group)
    {
        auto const itr = run.twinSide.find(member->GetGUID());
        if (itr == run.twinSide.end() || !member->IsAlive())
            continue;
        uint8 const side = itr->second.first, kind = itr->second.second;
        float const dx = run.twinX[side] - homes[side].GetPositionX(), dy = run.twinY[side] - homes[side].GetPositionY();
        float const length = std::max(1.0f, std::hypot(dx, dy));
        float const ux = dx / length, uy = dy / length;
        float x = run.twinX[side], y = run.twinY[side];
        if (kind == TwinTank && !tankA && side == 0)
            tankA = member;
        if (kind == TwinLockTank && side == 1)
        {
            // (Vek'lor's warlock 3 yd before the row of his half's casters: Vek'lor's victim from the first second.)
            lockB = member;
            x -= 3.0f * ux;
            y -= 3.0f * uy;
        }
        else if (kind == TwinMagic && side == 1)
        {
            openers.push_back(member);
            float const across = 3.5f * (float(casters++) - 3.0f);
            x += -across * uy;
            y += across * ux;
        }
        else if (kind != TwinTank)
        {
            uint8 const index = count[side]++;
            float const back = 8.0f + 4.0f * float(index / 5), across = 4.0f * (float(index % 5) - 2.0f);
            x += back * ux - across * uy;
            y += back * uy + across * ux;
        }
        float const z = map->GetHeight(member->GetPhaseMask(), x, y, run.twinZ[side] + 5.0f, true, 30.0f);
        if (member->GetExactDist2d(x, y) <= 6.0f)
        {
            // (Held there: mod-playerbots' follow walked the members back to their leader at the door, and the casters
            // opened on Vek'lor from 120 yd, 2026-11-23; its follow movement stays on the motion master while the AI
            // pauses, so that is cleared too: the members put at Vek'lor's throne walked back to the leader at the other
            // throne and were 100 yd from their places at the pull, 2026-11-23.)
            if (sGuildBotHolder.IsAIPlayed(member->GetGUID()))
            {
                BotDriver::PauseAI(member, 2500);
                if (member->isMoving() || member->GetMotionMaster()->GetCurrentMovementGeneratorType() != IDLE_MOTION_TYPE)
                {
                    member->GetMotionMaster()->Clear();
                    member->GetMotionMaster()->MoveIdle();
                    member->StopMoving();
                }
            }
            continue;
        }
        placed = false;
        if (!sGuildBotHolder.IsAIPlayed(member->GetGUID()))
            continue;
        // (No AI on the way either: the tank's own pull strategy walked the leader 15 yd from Vek'nilash on its way to
        // its place and pulled both twins onto it, 2026-11-23.)
        BotDriver::PauseAI(member, 2500);
        uint32& at = run.twinStepAt[member->GetGUID()];
        if (now < at)
            continue;
        at = now + 2000;
        // (A member not at its place after 4 s is put there, like a hop: the way across the floor between the thrones
        // leads through their bugs, and the halves that walked were not at their places when the twins came, or were
        // caught between them, 2026-11-23.)
        // (At once when its straight way to the place passes within 35 yd of a throne: the leader came back from the
        // defender beyond the room's west side, walked past Vek'nilash on its way to its place and pulled both twins,
        // 2026-11-23.)
        auto wayNear = [&](Position const& home)
        {
            float const ax = member->GetPositionX(), ay = member->GetPositionY(), bx = x - ax, by = y - ay;
            float const len2 = bx * bx + by * by;
            float t = len2 > 0.0f ? ((home.GetPositionX() - ax) * bx + (home.GetPositionY() - ay) * by) / len2 : 0.0f;
            t = std::clamp(t, 0.0f, 1.0f);
            return std::hypot(ax + t * bx - home.GetPositionX(), ay + t * by - home.GetPositionY()) < 35.0f;
        };
        if ((getMSTimeDiff(run.twinPlacesSince, now) >= 4 * IN_MILLISECONDS || wayNear(homeA) || wayNear(homeB)) &&
            !member->IsInCombat())
        {
            BotDriver::PauseAI(member, 3000);
            member->GetMotionMaster()->Clear();
            member->GetMotionMaster()->MoveIdle();
            member->NearTeleportTo(x, y, (z > INVALID_HEIGHT ? z : run.twinZ[side]) + 0.5f, member->GetOrientation());
            if (BotRuntime* other = FindRuntime(member->GetGUID()))
                other->hasLastPosition = false;
            at = now + 1000;
            continue;
        }
        BotDriver::PauseAI(member, 2500);
        member->GetMotionMaster()->MovePoint(0, x, y, z > INVALID_HEIGHT ? z : run.twinZ[side], FORCED_MOVEMENT_RUN);
    }
    rt.why = Acore::StringFormat("In Ahn'Qiraj Temple: the raid takes its places at the thrones of the Twin Emperors");
    if (!placed && getMSTimeDiff(run.twinPlacesSince, now) < 45 * IN_MILLISECONDS)
        return true;
    // (Everybody at its place: 3 s for the last ones to stand still.)
    if (!run.twinPlacedAt)
        run.twinPlacedAt = now;
    if (getMSTimeDiff(run.twinPlacedAt, now) < 3 * IN_MILLISECONDS)
        return true;
    // The pull: the tank of Vek'nilash's half opens on him first. When one twin is engaged its brother attacks the same
    // character, unless he fights already (boss_twinemperors): with the casters first Vek'nilash came after the caster
    // across the floor, 2026-11-23. Vek'lor then walks towards that tank until he is within 45 yd of it, and the casters
    // of his half open on him the moment the twins fight (StepRaidTwins, instant spells first): from them he takes his
    // victim, and he stands still while it is within 45 yd.
    if (!tankA)
        tankA = leader;
    // (From its place, with its ranged weapon where it has one: Vek'nilash is the tank's before the casters open.)
    // (Off their mounts first: mod-playerbots mounted the halves while they held their places, and the casters' openers
    // failed for it, 2026-11-23.)
    for (Player* member : group)
        if (member->IsAlive() && member->IsMounted())
            member->RemoveAurasByType(SPELL_AURA_MOUNTED);
    // (A taunt reaches him from the place at once: with its attack the tank walked 33 yd first, and the casters opened
    // after the twins' first teleport, 3.6 s into the fight, when Vek'lor stood at the other throne, 2026-11-23.)
    std::string opener = "its attack";
    for (char const* name : { "taunt", "hand of reckoning", "growl", "throw", "shoot gun", "shoot bow", "shoot crossbow" })
        if (uint32 const spell = BotDriver::SpellIdByName(tankA, name); spell && tankA->CastSpell(nilash, spell, false) == SPELL_CAST_OK)
        {
            opener = name;
            break;
        }
    BotDriver::Attack(tankA, nilash, false);
    // In the same moment the warlock of Vek'lor's half pulls him with Searing Pain (the guide's pull: its cast timed to
    // land with the taunt, so here at once). His brother's engage made him attack the tank across the room with no
    // threat on it (boss_twinemperors' AttackStart adds none); the first damage on him makes the warlock his victim, and
    // he stands still while it is within 45 yd. (With the casters' instant DoTs as the opener, which make no threat when
    // they land, he walked towards Vek'nilash's tank 1 s after the pull in every try, 2026-11-23.)
    std::string lockOpener = "nobody";
    for (Player* member : group)
        if (auto const itr = run.twinSide.find(member->GetGUID()); itr != run.twinSide.end() && itr->second.second == TwinLockTank &&
            member->IsAlive())
            DrinkTwinPotion(member, run.twinPotionAt[member->GetGUID()], now, lor);
    if (lockB)
    {
        // (Turned to him first: the cast failed with "not in front" at its place, facing the door, 2026-11-24.)
        uint32 const spell = BotDriver::SpellIdByName(lockB, "searing pain");
        lockB->StopMoving();
        lockB->SetOrientation(lockB->GetAngle(lor));
        lockB->SetFacingToObject(lor);
        SpellCastResult const result = spell ? lockB->CastSpell(lor, spell, true) : SPELL_FAILED_NOT_KNOWN;
        BotDriver::Attack(lockB, lor, false);
        lockOpener = Acore::StringFormat("{} with Searing Pain (cast result {}, {:.0f} yd)", lockB->GetName(), uint32(result),
            lockB->GetExactDist(lor));
    }
    run.twinSwapAt = 0;
    std::string missing;
    for (Player* member : group)
        if (auto const itr = run.twinSide.find(member->GetGUID()); itr != run.twinSide.end() && member->IsAlive() &&
            member->GetExactDist2d(run.twinX[itr->second.first], run.twinY[itr->second.first]) > 25.0f)
            missing += (missing.empty() ? "" : ", ") + member->GetName();
    if (!missing.empty())
        LOG_INFO(ActivityLog, "activity: not at their places when {}'s group pulls the Twin Emperors: {}", bot.name, missing);
    LOG_INFO(ActivityLog, "activity: {} opens on {} with {}; on {}: {}", tankA->GetName(), nilash->GetName(), opener,
        lor->GetName(), lockOpener);
    // (Its walk to Vek'nilash's spawn ends: the leader walked from its place to his throne after the pull and died
    // among his bugs, 2026-11-24.)
    ClearTask(rt, 0, "the Twin Emperors pulled");
    // The healers heal the four tanks (the owner's guide: the two warriors and the two warlocks).
    std::list<ObjectGuid> tanks;
    for (auto const& [guid, place] : run.twinSide)
        if (place.second == TwinTank || place.second == TwinLockTank)
            tanks.push_back(guid);
    for (Player* member : group)
        if (auto const itr = run.twinSide.find(member->GetGUID()); itr != run.twinSide.end() && itr->second.second == TwinHealer &&
            sGuildBotHolder.IsAIPlayed(member->GetGUID()))
            BotDriver::SetFocusHeal(member, tanks);
    run.twinPulledAt = now;
    run.twinOpened = false;
    LOG_INFO(ActivityLog, "activity: {}'s group pulls the Twin Emperors on two sides: {} opens on {}, then {} caster{} open on {}{}",
        bot.name, tankA->GetName(), nilash->GetName(), openers.size(), openers.size() == 1 ? "" : "s", lor->GetName(),
        placed ? "" : " (not everybody at its place after 45 s)");
    return true;
}

void OrderService::StepRaidTwins(BotRuntime::DungeonRun& run, std::vector<Player*> const& group, std::string const& raid)
{
    if (run.twinSide.empty() || group.empty())
        return;
    Player* anyone = group.front();
    Creature* nilash = anyone->FindNearestCreature(TwinVeknilash, 300.0f, true);
    Creature* lor = anyone->FindNearestCreature(TwinVeklor, 300.0f, true);
    // (Both dead, or the raid far from them: the halves are over, and its members follow again, OrderService::UpdateBot.)
    if (!nilash && !lor)
    {
        for (Player* member : group)
            if (run.twinSide.count(member->GetGUID()) && sGuildBotHolder.IsAIPlayed(member->GetGUID()))
            {
                BotDriver::SetFocusHeal(member, {});
                BotDriver::SetStrategyEverywhere(member, "rshadow", false);
            }
        run.twinSide.clear();
        run.twinPulledAt = run.twinPlacesSince = run.twinPlacedAt = run.twinSwapAt = 0;
        run.twinBugsSince = 0;
        run.twinBugsDone = false;
        return;
    }
    if (!nilash || !lor || (!nilash->IsInCombat() && !lor->IsInCombat()))
        return;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    // A fight that began while the halves took their places (StepTwinsPull is not reached while the leader fights): it
    // counts as pulled here, and who the twins came for, where, is logged (the twins came 10 s into the placing and the
    // two-side fight never began, 2026-11-23).
    if (!run.twinPulledAt)
    {
        run.twinPulledAt = now;
        run.twinOpened = true;
        auto who = [&](Creature* twin)
        {
            Unit const* victim = twin->GetVictim();
            if (!victim)
                return std::string("nobody");
            auto const itr = run.twinSide.find(victim->GetGUID());
            float const fromPlace = itr == run.twinSide.end() ? 0.0f :
                victim->GetExactDist2d(run.twinX[itr->second.first], run.twinY[itr->second.first]);
            return Acore::StringFormat("{} ({} side, {:.0f} yd from its place, {:.0f} yd from {})", victim->GetName(),
                itr == run.twinSide.end() ? "no" : itr->second.first == 0 ? "the first" : "the second", fromPlace,
                victim->GetExactDist2d(twin), twin->GetName());
        };
        LOG_INFO(ActivityLog, "activity: in {} the Twin Emperors came while the raid took its places: {} on {}, {} on {}", raid,
            nilash->GetName(), who(nilash), lor->GetName(), who(lor));
    }
    auto sideOf = [&](Unit const* unit) -> uint8
    {
        return unit->GetExactDist2d(run.twinX[0], run.twinY[0]) <= unit->GetExactDist2d(run.twinX[1], run.twinY[1]) ? 0 : 1;
    };
    // (In the first 3 s after the pull each twin belongs to its own throne's half, whatever the way it walks.)
    bool const opening = getMSTimeDiff(run.twinPulledAt, now) < 3 * IN_MILLISECONDS;
    uint8 const nilashSide = opening ? 0 : sideOf(nilash), lorSide = opening ? 1 : sideOf(lor);
    // Their swaps: Twin Teleport 30 s into the fight, then every 30-40 s (boss_twinemperors). Each twin comes where his
    // brother stood, his threat list emptied, and after 2 s takes the nearest player with 2000 threat. So the melee of
    // Vek'nilash's half step away from him 26 s after the last swap until the next one (the owner's guide: about 5 s before
    // it), and at Vek'lor's half the warrior tank waits nearest to him, out of his Arcane Burst (within 5 yd).
    if (!run.twinSwapAt)
    {
        run.twinSwapAt = run.twinPulledAt;
        run.twinNilashSide = nilashSide;
    }
    else if (!opening && nilashSide != run.twinNilashSide)
    {
        LOG_INFO(ActivityLog, "activity: in {} the Twin Emperors swap places after {} s: {} at the {} side, {} at the {}", raid,
            getMSTimeDiff(run.twinSwapAt, now) / IN_MILLISECONDS, nilash->GetName(), nilashSide == 0 ? "first" : "second",
            lor->GetName(), lorSide == 0 ? "first" : "second");
        run.twinNilashSide = nilashSide;
        run.twinSwapAt = now;
    }
    uint32 const sinceSwap = getMSTimeDiff(run.twinSwapAt, now);
    // Who the twins fight 5 s into the fight, and where (once a try).
    if (!run.twinOpened && getMSTimeDiff(run.twinPulledAt, now) >= 5 * IN_MILLISECONDS)
    {
        run.twinOpened = true;
        auto victimOf = [&](Creature* twin)
        {
            Unit const* victim = twin->GetVictim();
            return Acore::StringFormat("{} at the {} side on {}", twin->GetName(), sideOf(twin) == 0 ? "first" : "second",
                victim ? victim->GetName() : std::string("nobody"));
        };
        LOG_INFO(ActivityLog, "activity: in {} 5 s into the fight: {}; {}", raid, victimOf(nilash), victimOf(lor));
    }
    // The fight in the log every 30 s: the twins' health and who of the raid is alive.
    if (getMSTimeDiff(run.twinNoteAt, now) >= 30 * IN_MILLISECONDS)
    {
        run.twinNoteAt = now;
        uint32 alive = 0;
        for (Player* member : group)
            alive += member->IsAlive() ? 1 : 0;
        LOG_INFO(ActivityLog, "activity: the Twin Emperors in {} at {:.0f}% and {:.0f}% health {} s into the fight: {} of the raid alive",
            raid, nilash->GetHealthPct(), lor->GetHealthPct(), getMSTimeDiff(run.twinPulledAt, now) / IN_MILLISECONDS, alive);
    }
    bool const swapSoon = sinceSwap >= 26 * IN_MILLISECONDS && sinceSwap < 45 * IN_MILLISECONDS;
    // The bugs of the room: a mutated one (Mutate Bug: its poison on every hit) is the ranged fighters' first target (the
    // guide: mages, warlocks and hunters swap to them); one that is about to explode (Explode Bug, 3 s) is left by
    // everybody near it but a tank that holds Vek'nilash.
    std::list<Creature*> bugs;
    for (uint32 entry : { 15316u, 15317u })
        anyone->GetCreatureListWithEntryInGrid(bugs, entry, 300.0f);
    std::vector<Creature*> mutated, exploding, biting;
    for (Creature* bug : bugs)
    {
        if (!bug->IsAlive())
            continue;
        if (bug->HasAura(804))
            exploding.push_back(bug);
        else if (bug->HasAura(802))
            mutated.push_back(bug);
        // (A bug attacks whoever in a fight stands within 10 yd of it: the tanks had up to 13 of them on them beside
        // Vek'nilash and died in 24 to 32 s, 2026-11-24; the ranged take those after the mutated ones.)
        else if (Unit const* victim = bug->GetVictim(); victim && victim->IsPlayer())
            biting.push_back(bug);
    }
    static std::unordered_map<ObjectGuid, uint32> castLogged;
    // Everybody stands on the door side of the twin at its side, on the line from his throne through the place: the
    // healers between the two tanks of the half (a healer that stood on the far side of Vek'lor was 50 yd from the
    // warlock it was to heal, and the warlock died 13 s into the fight, 2026-11-24), the others in rows across it.
    Position const homes[2] = { nilash->GetHomePosition(), lor->GetHomePosition() };
    uint8 slot[2] = { 0, 0 };
    for (Player* member : group)
    {
        auto const itr = run.twinSide.find(member->GetGUID());
        if (itr == run.twinSide.end() || !member->IsAlive() || !sGuildBotHolder.IsAIPlayed(member->GetGUID()) ||
            member->IsBeingTeleported())
            continue;
        uint8 const side = itr->second.first, kind = itr->second.second;
        uint8 const index = (kind == TwinTank || kind == TwinLockTank) ? 0 : slot[side]++;
        uint32& at = run.twinStepAt[member->GetGUID()];
        if (now < at)
            continue;
        at = now + 1000;
        bool const nilashHere = nilashSide == side, lorHere = lorSide == side;
        bool const hunter = member->getClass() == CLASS_HUNTER;
        Map* map = member->GetMap();
        // At a distance from a creature: away from it along the line through where the member stands, or to it (out
        // of the band yards - 2 .. yards + slack); true when it moves.
        auto keepAt = [&](Unit const* from, float yards, float slack)
        {
            float vx = member->GetPositionX() - from->GetPositionX(), vy = member->GetPositionY() - from->GetPositionY();
            float d = std::hypot(vx, vy);
            if (d >= yards - 2.0f && d <= yards + slack)
                return false;
            if (d < 1.0f)
            {
                vx = run.twinX[side] - from->GetPositionX();
                vy = run.twinY[side] - from->GetPositionY();
                d = std::max(1.0f, std::hypot(vx, vy));
            }
            float const x = from->GetPositionX() + yards * vx / d, y = from->GetPositionY() + yards * vy / d;
            float const z = map->GetHeight(member->GetPhaseMask(), x, y, run.twinZ[side] + 5.0f, true, 30.0f);
            member->GetMotionMaster()->MovePoint(0, x, y, z > INVALID_HEIGHT ? z : run.twinZ[side], FORCED_MOVEMENT_RUN);
            return true;
        };
        // On the door side of a twin: yards from him on the line from his throne through the place of this half, a row
        // and a place across it for all but the tanks; true when it moves.
        auto standAt = [&](Unit const* from, float yards)
        {
            float ux = run.twinX[side] - homes[side].GetPositionX(), uy = run.twinY[side] - homes[side].GetPositionY();
            float const length = std::max(1.0f, std::hypot(ux, uy));
            ux /= length;
            uy /= length;
            bool const tank = kind == TwinTank || kind == TwinLockTank;
            float const across = tank ? 0.0f : 3.0f * (float(index % 9) - 4.0f);
            float const back = yards + (tank ? 0.0f : 4.0f * float(index / 9));
            float const x = from->GetPositionX() + back * ux - across * uy, y = from->GetPositionY() + back * uy + across * ux;
            if (member->GetExactDist2d(x, y) <= 4.0f)
                return false;
            float const z = map->GetHeight(member->GetPhaseMask(), x, y, run.twinZ[side] + 5.0f, true, 30.0f);
            member->GetMotionMaster()->MovePoint(0, x, y, z > INVALID_HEIGHT ? z : run.twinZ[side], FORCED_MOVEMENT_RUN);
            return true;
        };
        auto toPlace = [&]()
        {
            if (member->GetExactDist2d(run.twinX[side], run.twinY[side]) > 6.0f)
                member->GetMotionMaster()->MovePoint(0, run.twinX[side], run.twinY[side], run.twinZ[side], FORCED_MOVEMENT_RUN);
        };
        auto wait = [&](Unit const* from, float yards)
        {
            BotDriver::PauseAI(member, 1500);
            if (member->GetVictim())
                member->AttackStop();
            if (from)
                standAt(from, yards);
            else
                toPlace();
        };
        auto bugFor = [&]() -> Creature*
        {
            Creature* best = nullptr;
            float bestDist = 40.0f;
            for (Creature* bug : mutated)
                if (float const dist = member->GetExactDist(bug); dist < bestDist && member->IsValidAttackTarget(bug))
                {
                    best = bug;
                    bestDist = dist;
                }
            if (best)
                return best;
            bestDist = 40.0f;
            for (Creature* bug : biting)
                if (float const dist = member->GetExactDist(bug); dist < bestDist && member->IsValidAttackTarget(bug))
                {
                    best = bug;
                    bestDist = dist;
                }
            return best;
        };
        // An exploding bug near it: away from it first.
        if (kind != TwinTank || !nilashHere)
        {
            Creature* bomb = nullptr;
            for (Creature* bug : exploding)
                if (member->GetExactDist2d(bug) < 10.0f)
                {
                    bomb = bug;
                    break;
                }
            if (bomb)
            {
                BotDriver::PauseAI(member, 1500);
                keepAt(bomb, 14.0f, 100.0f);
                continue;
            }
        }
        Unit const* here = nilashHere ? static_cast<Unit const*>(nilash) : lorHere ? static_cast<Unit const*>(lor) : nullptr;
        switch (kind)
        {
            case TwinHealer:
            {
                // (The tanks and their halves within their reach, out of Vek'lor's Arcane Burst and his brother's blows.)
                if (here && standAt(here, 20.0f))
                    BotDriver::PauseAI(member, 1500);
                break;
            }
            case TwinTank:
                if (nilashHere)
                {
                    if (member->GetVictim() != nilash)
                        BotDriver::Attack(member, nilash, false);
                    break;
                }
                // Vek'lor at its side: nearest to him, out of his Arcane Burst, for his brother after the next swap.
                if (lorHere)
                {
                    BotDriver::PauseAI(member, 1500);
                    if (member->GetVictim())
                        member->AttackStop();
                    standAt(lor, 11.0f);
                }
                else
                    wait(nullptr, 0.0f);
                break;
            case TwinLockTank:
                DrinkTwinPotion(member, run.twinPotionAt[member->GetGUID()], now, lor);
                // Demon Armor (shadow resistance) and Shadow Ward on its cooldown (the guide's warlock tank).
                for (char const* armor : { "demon armor", "shadow ward" })
                    if (uint32 const spell = BotDriver::SpellIdByName(member, armor); spell && !member->HasAura(spell) &&
                        !member->HasSpellCooldown(spell) && !member->IsNonMeleeSpellCast(false) &&
                        member->CastSpell(member, spell, false) == SPELL_CAST_OK)
                        break;
                if (lorHere)
                {
                    // Vek'lor's tank: Searing Pain on him from 26 yd, Life Tap when its mana runs low (the healers keep
                    // its health), nothing else (mod-playerbots' warlock would fight with its rotation).
                    BotDriver::PauseAI(member, 1500);
                    if (standAt(lor, 26.0f) || member->IsNonMeleeSpellCast(false))
                        break;
                    bool const lowMana = member->GetPower(POWER_MANA) * 100 < member->GetMaxPower(POWER_MANA) * 25;
                    char const* name = lowMana && member->GetHealthPct() > 60.0f ? "life tap" : "searing pain";
                    uint32 const spell = BotDriver::SpellIdByName(member, name);
                    if (!spell)
                        break;
                    // (Standing and turned to him: casts failed with "moving" and "not in front", 2026-11-24.)
                    if (member->isMoving())
                    {
                        member->StopMoving();
                        break;
                    }
                    member->SetOrientation(member->GetAngle(lor));
                    member->SetFacingToObject(lor);
                    SpellCastResult const result = member->CastSpell(name[0] == 'l' ? static_cast<Unit*>(member) : lor, spell, false);
                    if (result != SPELL_CAST_OK && result != SPELL_FAILED_NOT_READY && result != SPELL_FAILED_SPELL_IN_PROGRESS)
                        if (uint32& logged = castLogged[member->GetGUID()]; !logged || getMSTimeDiff(logged, now) > 10 * IN_MILLISECONDS)
                        {
                            logged = now;
                            LOG_INFO(ActivityLog, "activity: {} could not cast {} on {} (cast result {}, {:.0f} yd)", member->GetName(),
                                name, lor->GetName(), uint32(result), member->GetExactDist(lor));
                        }
                    break;
                }
                // Vek'nilash at its side (he takes no magic): the mutated bugs, else it waits out of his reach.
                if (Creature* bug = bugFor())
                {
                    if (member->GetVictim() != bug)
                        BotDriver::Attack(member, bug, false);
                    break;
                }
                wait(here, 22.0f);
                break;
            case TwinMagic:
                if (Creature* bug = bugFor())
                {
                    if (member->GetVictim() != bug)
                        BotDriver::Attack(member, bug, false);
                    break;
                }
                if (lorHere)
                {
                    // (Farther from Vek'lor than the tank that waits for his brother: it is the nearest at the swap.)
                    if (standAt(lor, 24.0f))
                        BotDriver::PauseAI(member, 1000);
                    else if (member->GetVictim() != lor)
                        BotDriver::Attack(member, lor, false);
                    break;
                }
                // Vek'nilash takes no magic: out of his reach.
                wait(here, 22.0f);
                break;
            default:  // TwinPhysical
                if (hunter)
                    if (Creature* bug = bugFor())
                    {
                        if (member->GetVictim() != bug)
                            BotDriver::Attack(member, bug, false);
                        break;
                    }
                if (nilashHere)
                {
                    // Away from Vek'nilash shortly before the swap (Vek'lor comes where he stands), a hunter from range.
                    if (swapSoon && !hunter)
                    {
                        BotDriver::PauseAI(member, 1500);
                        if (member->GetVictim())
                            member->AttackStop();
                        standAt(nilash, 18.0f);
                    }
                    else if (member->GetVictim() != nilash)
                        BotDriver::Attack(member, nilash, false);
                    break;
                }
                // Vek'lor takes no physical damage: out of his reach.
                wait(here, 22.0f);
                break;
        }
    }
    if (!run.burstLogged.count(TwinVeknilash))
    {
        run.burstLogged.insert(TwinVeknilash);
        LOG_INFO(ActivityLog, "activity: the raid in {} fights the Twin Emperors on two sides: at {}'s side its warrior tank, melee "
            "and hunters, at {}'s its warlock with Searing Pain and its casters, the mutated bugs first; the others wait out of "
            "their reach", raid, nilash->GetName(), lor->GetName());
    }
}

// Viscidus (Temple of Ahn'Qiraj, boss_viscidus; the owner's guide of 2026-11-23). Frost: his Poison Bolt Volley hits everybody
// within 100 yd every 10-15 s (1313 nature damage and 413 every 2 s, it stacks), and 200 hits of frost spells freeze him.
// Frozen for 15 s: 150 hits of physical damage shatter him into a glob for every 5 % of his health, which crawl to the
// middle of his room and make him again with what reaches him (the globs first: StepDungeonSkull). Here: the healers and
// every druid, paladin and shaman take the volley's poison off the raid, the mages cast the first rank of Frostbolt (a
// short cast, a hit sooner), everybody drinks a nature protection potion out of its bags or the guild storage, frozen
// everybody but the healers hits him in melee, among the globs a tank waits where he comes back, and when he does the
// healers wait 3 s so that the tank has him first. (Out of his Toxin clouds: the Toxic Slime is a whirl row.)
void OrderService::StepRaidViscidus(BotRuntime::DungeonRun& run, std::vector<Player*> const& group, std::string const& raid)
{
    constexpr uint32 ViscidusEntry = 15299, GlobEntry = 15667, VolleyAura = 25991, FrozenAura = 25937, HiddenAura = 25905,
        StunAura = 25900;
    constexpr float CenterX = -7992.36f, CenterY = 908.19f, CenterZ = -52.62f;
    if (group.empty())
        return;
    Creature* visc = group.front()->FindNearestCreature(ViscidusEntry, 250.0f, true);
    if (!visc || !visc->IsInCombat())
    {
        run.viscPhase = 0;
        run.viscFrozenAt = 0;
        return;
    }
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    bool const globs = visc->HasAura(HiddenAura) || visc->HasAura(StunAura) ||
        group.front()->FindNearestCreature(GlobEntry, 250.0f, true) != nullptr;
    // Frozen: his script's melee phase lasts 15 s from the freeze, though the Freeze aura itself breaks on the first hit
    // (the raid saw him frozen for a second and went back to frost, 2026-11-23); 150 hits in it shatter him.
    if (visc->HasAura(FrozenAura) && (!run.viscFrozenAt || getMSTimeDiff(run.viscFrozenAt, now) > 16 * IN_MILLISECONDS))
        run.viscFrozenAt = now;
    bool const frozen = !globs && run.viscFrozenAt && getMSTimeDiff(run.viscFrozenAt, now) < 14500;
    uint8 const phase = frozen ? 2 : globs ? 3 : 1;
    AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfMember(group.front()->GetGUID());
    auto roleOf = [&](Player* player)
    {
        GuildBot const* memberBot = guild ? guild->FindMember(player->GetGUID()) : nullptr;
        return memberBot ? sRoleService.EffectiveRole(*memberBot) : DungeonRole::None;
    };
    if (phase != run.viscPhase)
    {
        uint8 const was = run.viscPhase;
        run.viscPhase = phase;
        run.viscPhaseSince = now;
        static char const* const Names[] = { "", "frost", "frozen: shatter him", "his globs" };
        LOG_INFO(ActivityLog, "activity: the raid in {} fights {}: {} ({:.0f}% health)", raid, visc->GetName(), Names[phase],
            visc->GetHealthPct());
        // (He comes back out of his globs: the healers wait 3 s, so that his first victim is the tank at the middle.)
        if (was == 3 && phase == 1)
            for (Player* member : group)
                if (member->IsAlive() && sGuildBotHolder.IsAIPlayed(member->GetGUID()) && roleOf(member) == DungeonRole::Healer)
                {
                    member->InterruptNonMeleeSpells(false);
                    BotDriver::PauseAI(member, 3000);
                    run.viscStepAt[member->GetGUID()] = now + 3000;
                }
    }
    Player* middleTank = nullptr;
    for (Player* member : group)
        if (member->IsAlive() && roleOf(member) == DungeonRole::Tank && sGuildBotHolder.IsAIPlayed(member->GetGUID()))
        {
            middleTank = member;
            break;
        }
    uint32 cleansed = 0, frostbolts = 0, shatterers = 0, potions = 0;
    for (Player* member : group)
    {
        if (!member->IsAlive() || member->IsBeingTeleported() || !sGuildBotHolder.IsAIPlayed(member->GetGUID()))
            continue;
        uint32& at = run.viscStepAt[member->GetGUID()];
        if (now < at)
            continue;
        DungeonRole const role = roleOf(member);
        uint8 const playerClass = member->getClass();
        // A nature protection potion: when the fight begins, and again when it is gone and its two minutes are over.
        if (phase == 1 && !member->HasAura(17546) && !member->HasAura(7254))
        {
            uint32& drank = run.viscPotionAt[member->GetGUID()];
            if (!drank || getMSTimeDiff(drank, now) > 120 * IN_MILLISECONDS)
                for (uint32 entry : { 13458u, 6052u })  // Greater Nature Protection Potion, Nature Protection Potion
                {
                    std::string error;
                    bool fromStorage = false;
                    if (!member->HasItemCount(entry, 1))
                    {
                        if (!sStorageService.Withdraw(member, entry, 0, 1, error))
                            continue;
                        fromStorage = true;
                    }
                    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
                    uint32 const spell = proto ? uint32(proto->Spells[0].SpellId) : 0;
                    if (!spell)
                        continue;
                    member->CastSpell(member, spell, true);
                    member->DestroyItemCount(entry, 1, true);
                    drank = now;
                    ++potions;
                    LOG_INFO(ActivityLog, "activity: {} drinks 1 x {}{} against the poison of {}", member->GetName(), proto->Name1,
                        fromStorage ? " out of the guild storage" : "", visc->GetName());
                    break;
                }
        }
        // The poison off the raid: who can, on the member with the most of it within 40 yd.
        char const* cleanses[2] = { nullptr, nullptr };
        if (playerClass == CLASS_PALADIN)
            cleanses[0] = "cleanse", cleanses[1] = "purify";
        else if (playerClass == CLASS_DRUID)
            cleanses[0] = "abolish poison", cleanses[1] = "cure poison";
        else if (playerClass == CLASS_SHAMAN)
            cleanses[0] = "cure toxins", cleanses[1] = "cure poison";
        if (cleanses[0] && phase != 2 && !member->IsNonMeleeSpellCast(false) && !member->HasUnitState(UNIT_STATE_CASTING) &&
            (role == DungeonRole::Healer || role == DungeonRole::None))
        {
            uint32 spell = 0;
            for (char const* name : cleanses)
                if (!spell)
                    spell = BotDriver::SpellIdByName(member, name);
            Player* worst = nullptr;
            uint32 worstStacks = 0;
            if (spell && !member->GetGlobalCooldownMgr().HasGlobalCooldown(sSpellMgr->GetSpellInfo(spell)))
                for (Player* other : group)
                {
                    if (!other->IsAlive() || member->GetExactDist(other) > 38.0f || !member->IsWithinLOSInMap(other))
                        continue;
                    if (Aura const* poison = other->GetAura(VolleyAura))
                        if (uint32 const stacks = std::max<uint32>(1, poison->GetStackAmount()); stacks > worstStacks &&
                            !(spell == BotDriver::SpellIdByName(member, "abolish poison") && other->HasAura(spell)))
                        {
                            worst = other;
                            worstStacks = stacks;
                        }
                }
            if (worst && member->CastSpell(worst, spell, false) == SPELL_CAST_OK)
            {
                ++cleansed;
                BotDriver::PauseAI(member, 1200);
                at = now + 1300;
                continue;
            }
        }
        if (phase == 1 && playerClass == CLASS_MAGE && role != DungeonRole::Healer && member->HasSpell(116) &&
            !member->IsNonMeleeSpellCast(false) && member->GetExactDist(visc) < 30.0f + visc->GetCombatReach() &&
            member->IsWithinLOSInMap(visc))
        {
            // The first rank of Frostbolt: 1.5 s, the hit is what counts.
            if (member->CastSpell(visc, 116, false) == SPELL_CAST_OK)
            {
                ++frostbolts;
                BotDriver::PauseAI(member, 1700);
                at = now + 1600;
            }
            continue;
        }
        if (phase == 2 && role != DungeonRole::Healer)
        {
            // Frozen: physical hits shatter him, melee from everybody (a caster's staff too).
            if (member->GetVictim() != visc || !member->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
            {
                member->InterruptNonMeleeSpells(false);
                BotDriver::PauseAI(member, 2000);
                member->Attack(visc, true);
                member->GetMotionMaster()->MoveChase(visc);
                ++shatterers;
            }
            else if (!BotDriver::FightsInMelee(member))
                BotDriver::PauseAI(member, 2000);
            at = now + 1000;
            continue;
        }
        if (phase == 3 && member == middleTank)
        {
            // Among the globs: the tank waits where he comes back.
            if (member->GetExactDist2d(CenterX, CenterY) > 6.0f)
            {
                BotDriver::PauseAI(member, 2000);
                member->GetMotionMaster()->MovePoint(0, CenterX, CenterY, CenterZ, FORCED_MOVEMENT_RUN);
            }
            at = now + 1500;
            continue;
        }
    }
    if (shatterers && !run.burstLogged.count(FrozenAura))
    {
        run.burstLogged.insert(FrozenAura);
        LOG_INFO(ActivityLog, "activity: the raid in {} hits the frozen {} in melee ({} turned to him)", raid, visc->GetName(), shatterers);
    }
    if (frostbolts && !run.burstLogged.count(116))
    {
        run.burstLogged.insert(116);
        LOG_INFO(ActivityLog, "activity: the mages in {} cast the first rank of Frostbolt on {}", raid, visc->GetName());
    }
    if (cleansed && !run.burstLogged.count(VolleyAura))
    {
        run.burstLogged.insert(VolleyAura);
        LOG_INFO(ActivityLog, "activity: the raid in {} takes the poison of {}'s volley off its members", raid, visc->GetName());
    }
    (void)potions;
}

namespace
{
    // The Dark Glare's band: the half angle at a distance from the Eye's middle within which its line hits (16.5 yd to
    // either side of it and a yard and a half more; all of its front half within that). Runners off the melee ring go
    // out to 25 yd, where the band leaves room beyond its end.
    constexpr float GlareRunRadius = 25.0f;
    float GlareBand(float distance)
    {
        return distance <= 18.0f ? float(M_PI) / 2.0f : std::asin(18.0f / distance);
    }

    // The places on the rings around C'Thun's Eye, given once: the melee at 17 yd (7 places), the others at 31 yd (14) and
    // 44.5 yd (20), no two places within 13.5 yd of each other (its beam jumps 13 yd).
    void AssignCthunSlots(BotRuntime::DungeonRun& run, std::vector<Player*> const& group, std::string const& raid, Unit const* center)
    {
        AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfMember(group.front()->GetGUID());
        auto roleOf = [&](Player* player)
        {
            GuildBot const* memberBot = guild ? guild->FindMember(player->GetGUID()) : nullptr;
            return memberBot ? sRoleService.EffectiveRole(*memberBot) : DungeonRole::None;
        };
        std::vector<Player*> melee, others;
        for (Player* member : group)
            (BotDriver::FightsInMelee(member) && roleOf(member) != DungeonRole::Healer ? melee : others).push_back(member);
        struct Ring { float radius; uint32 places; float offset; };
        // (Its beam jumps from a target to the nearest other within 10 yd and both their sizes, 13 yd between two players
        // (the core's chain search, IsWithinDist), and each jump does half as much again as the one before (its chain
        // amplitude 1.5: 2625, 3900, 5900, 8900 ... 874000): a raid on rings 13 yd apart, places 12.5 yd apart on a ring,
        // lost 29 of 40 to the beams in a minute, one beam going round the outer ring through eleven, 2026-11-24. These
        // three rings, their places half a step out of line, are the widest 40 places in the Eye's reach: 13.5 yd. The
        // outer ring at 44.5 yd: the bots cast from 28.5 yd of the Eye's edge (its reach 15 yd, Playerbots.SpellDistance),
        // and at 45 yd they kept stepping in.)
        Ring const rings[3] = { { 17.0f, 7, 0.0f }, { 31.0f, 14, 0.5f }, { 44.5f, 20, 0.5f } };
        uint32 used[3] = { 0, 0, 0 };
        auto place = [&](Player* member, uint8 first)
        {
            for (uint8 ring = first; ring < 3; ++ring)
                if (used[ring] < rings[ring].places)
                {
                    float const step = 2.0f * float(M_PI) / float(rings[ring].places);
                    float const angle = step * (float(used[ring]++) + rings[ring].offset);
                    run.cthunSlot[member->GetGUID()] = { rings[ring].radius, angle };
                    return;
                }
            run.cthunSlot[member->GetGUID()] = { 50.0f, 2.0f * float(M_PI) * float(run.cthunSlot.size() % 24) / 24.0f };
        };
        for (Player* member : melee)
            place(member, 0);
        for (Player* member : others)
            place(member, 1);
        LOG_INFO(ActivityLog, "activity: the raid in {} spreads out around {}: {} melee at 17 yd, the others at 31 and 44.5 yd, "
            "13.5 yd apart or more (its beam jumps 13 yd)", raid, center->GetName(), std::min<size_t>(melee.size(), 7));
    }
}

// The pull of C'Thun's Eye (the owner's guide of 2026-11-24): the raid waits out of its sight (it takes whoever comes within
// 90 yd of it in its line of sight, boss_cthun), a hunter with a nature protection potion and Aspect of the Wild goes in
// first to its own place on the rings: the Eye's first three beams are its (3, 6 and 9 s into the fight, at the one it
// engaged), and the raid runs to its places the moment the Eye fights (StepRaidCthun). (The raid teleported into its sight
// had the first beam jump through all of it, 15 dead at once, 2026-11-23.)
bool OrderService::StepCthunPull(BotRuntime& rt, std::vector<Player*> const& group, Creature* eye, std::string const& raid)
{
    constexpr uint32 BodyEntryCthun = 15727;  // (the key of the release's log line)
    BotRuntime::DungeonRun& run = rt.dungeon;
    if (eye->IsInCombat() || group.empty())
        return false;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    // Everybody at the place out of its sight first (the hunter set off while the raid still came in from the
    // entrance, and the raid ran into the Eye's beams in a crowd, 2026-11-24). (Before the places are dealt: from then
    // on the raid holds where it stands.)
    if (PullPlace const* wait = ApproachPlaceOf(eye->GetEntry()); wait && run.cthunSlot.empty())
    {
        std::string away;
        for (Player* member : group)
            if (member->IsAlive() && member->GetExactDist2d(wait->x, wait->y) > 30.0f)
                away += (away.empty() ? "" : ", ") + member->GetName();
        if (!away.empty())
        {
            rt.why = Acore::StringFormat("In {}: the raid gathers out of the sight of {} ({} not there yet)", raid, eye->GetName(), away);
            return true;
        }
    }
    if (run.cthunSlot.empty())
        AssignCthunSlots(run, group, raid, eye);
    Player* puller = nullptr;
    for (Player* member : group)
        if (member->IsAlive() && member->GetGUID() == run.cthunPuller)
            puller = member;
    if (!puller)
    {
        for (Player* member : group)
            if (member->IsAlive() && member->getClass() == CLASS_HUNTER && sGuildBotHolder.IsAIPlayed(member->GetGUID()) &&
                run.cthunSlot.count(member->GetGUID()))
            {
                puller = member;
                break;
            }
        if (!puller)
            return false;
        run.cthunPuller = puller->GetGUID();
        run.cthunPotionAt = 0;
    }
    // The place of a member on the rings around the Eye.
    auto slotOf = [&](Player const* member, float& x, float& y, float& z)
    {
        auto const itr = run.cthunSlot.find(member->GetGUID());
        if (itr == run.cthunSlot.end())
            return false;
        x = eye->GetPositionX() + itr->second.first * std::cos(itr->second.second);
        y = eye->GetPositionY() + itr->second.first * std::sin(itr->second.second);
        float const h = eye->GetMap()->GetHeight(eye->GetPhaseMask(), x, y, eye->GetPositionZ() + 10.0f, true, 30.0f);
        z = h > INVALID_HEIGHT ? h : eye->GetPositionZ();
        return true;
    };
    // The others hold where they wait until the hunter steps onto the chamber's floor; then they run to their places,
    // so that they stand spread when the Eye's beams leave the hunter (its first three go at the one it took, 3, 6 and 9
    // s into the fight; from 12 s on at random members, jumping 12.5 yd from one to the next). (A raid that set off only
    // when the Eye fought ran up the ramp in a column, and the beams went through all of it at 12, 15 and 18 s: 39 dead
    // at once, 2026-11-24.)
    // (Not any more: they hold until the Eye fights and are then put at their places, StepRaidCthun. A raid that walked
    // to its places while the hunter went up was still on its way when the Eye took the hunter, and its first beams
    // jumped from the hunter through eight of them, 2026-11-24.)
    bool const release = false;
    for (Player* member : group)
        if (member != puller && member->IsAlive() && sGuildBotHolder.IsAIPlayed(member->GetGUID()))
        {
            BotDriver::PauseAI(member, 2500);
            float x = 0.0f, y = 0.0f, z = 0.0f;
            if (release && slotOf(member, x, y, z))
            {
                if (member->IsMounted())
                    member->RemoveAurasByType(SPELL_AURA_MOUNTED);
                if (member->GetExactDist2d(x, y) > 3.0f)
                    member->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_RUN);
                continue;
            }
            if (member->isMoving() || member->GetMotionMaster()->GetCurrentMovementGeneratorType() != IDLE_MOTION_TYPE)
            {
                member->GetMotionMaster()->Clear();
                member->GetMotionMaster()->MoveIdle();
                member->StopMoving();
            }
        }
    if (release && !run.burstLogged.count(BodyEntryCthun))
    {
        run.burstLogged.insert(BodyEntryCthun);
        LOG_INFO(ActivityLog, "activity: in {} {} steps onto the floor of the chamber: the raid runs to its places on the rings", raid,
            puller->GetName());
    }
    BotDriver::PauseAI(puller, 2500);
    if (puller->IsMounted())
        puller->RemoveAurasByType(SPELL_AURA_MOUNTED);
    // Greater Nature Protection Potion, Nature Protection Potion; then Aspect of the Wild.
    DrinkProtection(puller, run.cthunPotionAt, now, { 13458u, 6052u }, { 17546u, 7254u },
        "to take the first beams of " + eye->GetName());
    if (uint32 const aspect = BotDriver::SpellIdByName(puller, "aspect of the wild"); aspect && !puller->HasAura(aspect))
        puller->CastSpell(puller, aspect, false);
    auto const slot = run.cthunSlot[puller->GetGUID()];
    float x = 0.0f, y = 0.0f, z = 0.0f;
    slotOf(puller, x, y, z);
    if (!puller->IsNonMeleeSpellCast(false) && puller->GetExactDist2d(x, y) > 3.0f)
        puller->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_RUN);
    if (!run.burstLogged.count(eye->GetEntry()))
    {
        run.burstLogged.insert(eye->GetEntry());
        LOG_INFO(ActivityLog, "activity: in {} {} pulls {} alone from its place on the rings ({:.0f} yd from it); the raid waits out "
            "of its sight and runs to its places when it fights", raid, puller->GetName(), eye->GetName(), slot.first);
    }
    rt.why = Acore::StringFormat("In {}: {} pulls {}; the raid waits out of its sight", raid, puller->GetName(), eye->GetName());
    return true;
}

// C'Thun (Temple of Ahn'Qiraj, boss_cthun). The Eye: its Eye Beam jumps from one member to the next within 12.5 yd, up to
// 60 of them; a Claw Tentacle comes up under a random member every 8 s, eight Eye Tentacles at the room's edge every 45 s
// (Mind Flay); 46 s into the fight and every 46 s after it the Eye freezes, faces the member it beamed last and turns
// half round in 35 s, its Dark Glare killing whoever stands on its line (6.5 yd to either side, in front of it). Here:
// everybody keeps a place of its own on a ring around the Eye, 14 yd from its neighbours (the melee at 17 yd, in its
// reach, the others at 30 and 43 yd); the Eye Tentacles bear the cross (who fights from afar), a Claw Tentacle the skull;
// when the glare begins whoever is on its line steps aside, and once the way it turns is known whoever is ahead of it
// runs past the end of its half circle (the runners are faster than the line). C'Thun's body: whoever it swallows kills
// the Flesh Tentacles in its stomach and leaves through the exit (the area trigger, as a client reports it) with eight
// stacks of its acid or below half its health; outside, the Giant Claw Tentacle bears the skull, the Giant Eye Tentacle
// the cross, and C'Thun the skull while his carapace is off; the melee keep out of the 7 yd under him (he spits out who
// stands there).
void OrderService::StepRaidCthun(BotRuntime::DungeonRun& run, std::vector<Player*> const& group, std::string const& raid)
{
    constexpr uint32 EyeEntry = 15589, BodyEntry = 15727, ClawEntry = 15725, EyeTentacleEntry = 15726, GiantClawEntry = 15728,
        GiantEyeEntry = 15334, FleshEntry = 15802, AcidAura = 26476, CarapaceAura = 26156, RedAura = 22518;
    constexpr float ExitX = -8546.23f, ExitY = 1987.65f, ExitZ = -96.52f;
    constexpr uint32 ExitTrigger = 4033;
    if (group.empty())
        return;
    Player* anyone = nullptr;
    for (Player* member : group)
        if (member->IsAlive() && member->GetPositionZ() > 0.0f)
        {
            anyone = member;
            break;
        }
    if (!anyone)
        anyone = group.front();
    Creature* eye = anyone->FindNearestCreature(EyeEntry, 200.0f, true);
    Creature* body = anyone->FindNearestCreature(BodyEntry, 200.0f, true);
    bool const eyePhase = eye && eye->IsInCombat() && !eye->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE);
    bool const bodyPhase = !eyePhase && body && body->IsInCombat() && !body->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE);
    bool const giantEyeUp = bodyPhase && anyone->FindNearestCreature(GiantEyeEntry, 200.0f, true);
    bool const bodyWeak = bodyPhase && !body->HasAura(CarapaceAura);
    if (!eyePhase && !bodyPhase)
    {
        run.glareSince = 0;
        // (The places stay while the raid waits for the pull: they mark the C'Thun phase for the squads' rules, and the
        // places were dealt anew every second; a wipe clears them.)
        if (!eye && !body)
            run.cthunSlot.clear();
        run.cthunFightSince = 0;
        run.cthunPut.clear();
        return;
    }
    Unit* center = eyePhase ? static_cast<Unit*>(eye) : static_cast<Unit*>(body);
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfMember(group.front()->GetGUID());
    auto roleOf = [&](Player* player)
    {
        GuildBot const* memberBot = guild ? guild->FindMember(player->GetGUID()) : nullptr;
        return memberBot ? sRoleService.EffectiveRole(*memberBot) : DungeonRole::None;
    };
    if (run.cthunSlot.empty())
        AssignCthunSlots(run, group, raid, center);
    // The marks. The Eye's phase: the skull on the Eye. The body's phase: the cross on a Giant Eye Tentacle (its beam
    // chains like the Eye's), the skull on C'Thun while his carapace is off, else on a Giant Claw Tentacle (else a Claw
    // Tentacle). The Eye Tentacles, eight at a time on a ring of 28 to 30 yd around the Eye, each casting Mind Flay on
    // somebody every 10 to 15 s (750 a second): each carries an icon of its own, and whoever does damage takes, in this
    // order, the cross in its reach, the marked tentacle nearest to it in its reach (a caster 28 yd, a melee fighter 8 yd),
    // else the skull. (With the cross on the weakest of them, one at a time and mostly out of reach, they lived on and
    // their Mind Flays killed 28 of two tries; with an icon each the Eye died three minutes after the pull, 2026-11-24.
    // They have 3000 health.)
    if (Group* party = group.front()->GetGroup())
    {
        auto nearestOf = [&](uint32 entry, bool weakest) -> Creature*
        {
            std::list<Creature*> found;
            center->GetCreatureListWithEntryInGrid(found, entry, 120.0f);
            Creature* best = nullptr;
            for (Creature* creature : found)
                if (creature->IsAlive() && creature->GetPositionZ() > 0.0f && !creature->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) &&
                    (!best || (weakest ? creature->GetHealthPct() < best->GetHealthPct() :
                        center->GetExactDist2d(creature) < center->GetExactDist2d(best))))
                    best = creature;
            return best;
        };
        Unit* skull = nullptr;
        Unit* cross = nullptr;
        if (eyePhase)
            // (The skull on the Eye itself: with it on the nearest Claw Tentacle the melee fought the tentacles and the
            // Eye lost 70 % in five minutes, a raid's damage of 2900 a second, 2026-11-24. A Claw Tentacle is fought by
            // whom it attacks.)
            skull = eye;
        else
        {
            skull = nearestOf(GiantClawEntry, false);
            cross = nearestOf(GiantEyeEntry, true);
            if (!body->HasAura(CarapaceAura))
                skull = body;
            else if (!skull)
                skull = nearestOf(ClawEntry, false);
        }
        Player* marker = group.front();
        if (skull && party->GetTargetIcon(SkullIcon) != skull->GetGUID())
            party->SetTargetIcon(SkullIcon, marker->GetGUID(), skull->GetGUID());
        if (skull)
            run.skull = skull->GetGUID();
        if (party->GetTargetIcon(CrossIcon) != (cross ? cross->GetGUID() : ObjectGuid::Empty))
            party->SetTargetIcon(CrossIcon, marker->GetGUID(), cross ? cross->GetGUID() : ObjectGuid::Empty);
        // The tentacles' icons: all but the skull in the Eye's phase; in the body's all but the skull, the cross and the
        // square, which marks the weaker Flesh Tentacle in his stomach for those inside (their AI went for the skull up
        // top, out of their reach, and two in the stomach took 4 % a minute off a tentacle, 2026-11-24).
        static char const* const IconNames[CrossIcon + 1] = { "star", "circle", "diamond", "triangle", "moon", "square", "cross" };
        constexpr uint8 SquareIcon = 5;
        constexpr uint8 MoonIcon = 4;
        // (While he is weakened the moon is on the Giant Claw Tentacle nearest to him for the tanks: with the skull on
        // him while he was weakened the tanks hit him and the claws killed eleven, two tanks among them, 2026-11-25.)
        // (Only while he is weakened: the skull is the claw's otherwise, and with four icons for the Eye Tentacles eight
        // to twelve of them stood up at a time, 2026-11-25.)
        bool const clawMoon = bodyPhase && !body->HasAura(CarapaceAura);
        uint8 const icons = eyePhase ? CrossIcon + 1 : clawMoon ? MoonIcon : SquareIcon;
        Creature* giantClaw = clawMoon ? nearestOf(GiantClawEntry, false) : nullptr;
        if (clawMoon && party->GetTargetIcon(MoonIcon) != (giantClaw ? giantClaw->GetGUID() : ObjectGuid::Empty))
            party->SetTargetIcon(MoonIcon, marker->GetGUID(), giantClaw ? giantClaw->GetGUID() : ObjectGuid::Empty);
        if (bodyPhase)
        {
            std::list<Creature*> fleshes;
            body->GetCreatureListWithEntryInGrid(fleshes, FleshEntry, 250.0f);
            Creature* weaker = nullptr;
            for (Creature* tentacle : fleshes)
                if (tentacle->IsAlive() && (!weaker || tentacle->GetHealth() < weaker->GetHealth()))
                    weaker = tentacle;
            if (party->GetTargetIcon(SquareIcon) != (weaker ? weaker->GetGUID() : ObjectGuid::Empty))
                party->SetTargetIcon(SquareIcon, marker->GetGUID(), weaker ? weaker->GetGUID() : ObjectGuid::Empty);
        }
        // The Eye Tentacles' icons go to those the most damage dealers can reach (a caster 30 yd, a melee fighter 8 yd),
        // a tentacle keeps its icon while it is among them. (With the icons dealt in no order eight to twelve tentacles
        // stood up in the body's phase, and 27 of 33 of the raid fought C'Thun in his carapace, out of reach of every
        // marked one, 2026-11-25.)
        std::list<Creature*> found;
        center->GetCreatureListWithEntryInGrid(found, EyeTentacleEntry, 120.0f);
        std::vector<Creature*> tentacles;
        for (Creature* tentacle : found)
            if (tentacle->IsAlive() && tentacle->GetPositionZ() > 0.0f && !tentacle->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE))
                tentacles.push_back(tentacle);
        std::vector<Player*> dealers;
        for (Player* member : group)
            if (member->IsAlive() && sGuildBotHolder.IsAIPlayed(member->GetGUID()) && roleOf(member) != DungeonRole::Healer &&
                member->GetPositionZ() >= 0.0f)
                dealers.push_back(member);
        auto reachOf = [](Player* member) { return BotDriver::FightsInMelee(member) ? 8.0f : 30.0f; };
        std::unordered_map<Creature*, uint32> demand;
        for (Player* member : dealers)
        {
            Creature* nearest = nullptr;
            float nearestAt = reachOf(member);
            for (Creature* tentacle : tentacles)
                if (float const at = member->GetExactDist(tentacle); at < nearestAt)
                {
                    nearestAt = at;
                    nearest = tentacle;
                }
            if (nearest)
                ++demand[nearest];
        }
        std::vector<Creature*> order = tentacles;
        std::sort(order.begin(), order.end(), [&](Creature* one, Creature* other)
        {
            uint32 const a = demand[one], b = demand[other];
            return a != b ? a > b : one->GetHealth() < other->GetHealth();
        });
        std::vector<Creature*> chosen(order.begin(), order.begin() + std::min<size_t>(order.size(), icons));
        std::array<Creature*, CrossIcon + 1> marked = {};
        for (uint8 icon = 0; icon < icons; ++icon)
        {
            ObjectGuid const current = party->GetTargetIcon(icon);
            auto const itr = std::find_if(chosen.begin(), chosen.end(), [&](Creature* tentacle) { return tentacle->GetGUID() == current; });
            if (itr != chosen.end())
            {
                marked[icon] = *itr;
                chosen.erase(itr);
            }
        }
        for (uint8 icon = 0; icon < icons; ++icon)
            if (!marked[icon])
            {
                if (!chosen.empty())
                {
                    marked[icon] = chosen.front();
                    chosen.erase(chosen.begin());
                    party->SetTargetIcon(icon, marker->GetGUID(), marked[icon]->GetGUID());
                }
                else if (!party->GetTargetIcon(icon).IsEmpty())
                    party->SetTargetIcon(icon, marker->GetGUID(), ObjectGuid::Empty);
            }
        // In his carapace with no claw up the skull goes to the most wanted tentacle without an icon (else the weakest
        // marked one): with no skull their AI took C'Thun, whom nothing hurts then.
        if (bodyPhase && !skull && !order.empty())
        {
            Creature* extra = nullptr;
            for (Creature* tentacle : order)
                if (std::find(marked.begin(), marked.end(), tentacle) == marked.end())
                {
                    extra = tentacle;
                    break;
                }
            skull = extra ? extra : order.back();
            if (party->GetTargetIcon(SkullIcon) != skull->GetGUID())
                party->SetTargetIcon(SkullIcon, marker->GetGUID(), skull->GetGUID());
            run.skull = skull->GetGUID();
        }
        // Each Giant Claw Tentacle gets a tank of its own, the nearest one free (two and three claws stood up at once,
        // all tanks on the skull's, and the others killed 17 of a try, 2026-11-25).
        std::unordered_set<Player*> clawTanks;
        if (bodyPhase)
        {
            std::list<Creature*> claws;
            center->GetCreatureListWithEntryInGrid(claws, GiantClawEntry, 120.0f);
            for (Creature* claw : claws)
            {
                if (!claw->IsAlive() || claw->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE))
                    continue;
                Player* tank = nullptr;
                for (Player* member : dealers)
                    if (roleOf(member) == DungeonRole::Tank && !clawTanks.count(member) &&
                        (!tank || member->GetExactDist(claw) < tank->GetExactDist(claw)))
                        tank = member;
                if (!tank)
                    break;
                clawTanks.insert(tank);
                if (skull && claw->GetGUID() == skull->GetGUID())
                    BotDriver::SetRti(tank, "skull");
                else if (giantClaw && claw->GetGUID() == giantClaw->GetGUID())
                    BotDriver::SetRti(tank, "moon");
                else
                {
                    BotDriver::SetRti(tank, "none");
                    if (tank->GetVictim() != claw)
                        BotDriver::Attack(tank, claw, false);
                }
            }
        }
        for (Player* member : dealers)
        {
            if (clawTanks.count(member))
                continue;
            // The Giant Eye Tentacle in its reach first (a beam every 2.1 s that jumps: 20 of a try's deaths while it
            // lived a minute, the raid on the Eye Tentacles), then the Eye Tentacles (2300 health each, a Mind Flay of
            // 750 a second), then the skull.
            float const reach = reachOf(member);
            // (The cross from 40 yd off, as far as its edge: Giant Eye Tentacles stood a minute at 86 % with nobody in
            // reach of their middles, 25 of a try dead to their beams, 2026-11-25. Who goes for it is let off its place
            // until it is within 28 yd, below.)
            // (Only from its place, as far as the Giant Eye Tentacle's edge: casters sent to it from farther off walked to
            // it in a line, 6 yd apart, and its beam took four of them at once, 2026-11-25.)
            char const* best = cross && (BotDriver::FightsInMelee(member) ? member->GetExactDist(cross) < reach :
                member->GetDistance(cross) < 30.0f) ? "cross" : nullptr;
            float bestAt = reach;
            bool const onCross = best != nullptr;
            for (uint8 icon = 0; icon < icons && !onCross; ++icon)
                if (marked[icon] && member->GetExactDist(marked[icon]) < bestAt)
                {
                    bestAt = member->GetExactDist(marked[icon]);
                    best = IconNames[icon];
                }
            if (!best)
                best = "skull";
            BotDriver::SetRti(member, best);
        }
    }
    // The body's phase in the log: his carapace off and on again, his health every 30 s.
    if (bodyPhase)
    {
        bool const weak = !body->HasAura(CarapaceAura);
        uint32 inside = 0, alive = 0;
        for (Player* member : group)
            if (member->IsAlive())
            {
                ++alive;
                if (member->GetPositionZ() < 0.0f)
                    ++inside;
            }
        if (weak != run.cthunWeak)
        {
            run.cthunWeak = weak;
            if (!weak)
                run.cthunCycleAt = now;
            LOG_INFO(ActivityLog, "activity: C'Thun in {} {} at {:.0f}% health ({} of the raid alive, {} in his stomach)", raid,
                weak ? "is weakened (his flesh tentacles are dead): his carapace is off for 45 s" : "has his carapace again",
                body->GetHealthPct(), alive, inside);
        }
        if (getMSTimeDiff(run.cthunNoteAt, now) >= 30 * IN_MILLISECONDS)
        {
            run.cthunNoteAt = now;
            std::list<Creature*> eyes, giants;
            center->GetCreatureListWithEntryInGrid(eyes, EyeTentacleEntry, 120.0f);
            uint32 eyesUp = 0;
            for (Creature* tentacle : eyes)
                if (tentacle->IsAlive() && tentacle->GetPositionZ() > 0.0f)
                    ++eyesUp;
            std::string bigOnes;
            for (uint32 entry : { GiantEyeEntry, GiantClawEntry })
            {
                giants.clear();
                center->GetCreatureListWithEntryInGrid(giants, entry, 120.0f);
                for (Creature* tentacle : giants)
                    if (tentacle->IsAlive())
                        bigOnes += Acore::StringFormat(", {} at {:.0f}%", tentacle->GetName(), tentacle->GetHealthPct());
            }
            // Who fights what (the members' targets by kind).
            std::map<std::string, uint32> onWhat;
            for (Player* member : group)
                if (member->IsAlive())
                {
                    Unit* victim = member->GetVictim();
                    if (!victim && member->GetSelectedUnit() && member->GetSelectedUnit()->IsAlive() && !member->GetSelectedUnit()->IsPlayer())
                        victim = member->GetSelectedUnit();
                    ++onWhat[victim ? victim->GetName() : std::string("nothing")];
                }
            std::string targets;
            for (auto const& [what, n] : onWhat)
                targets += Acore::StringFormat("{}{} {}", targets.empty() ? "" : ", ", n, what);
            LOG_INFO(ActivityLog, "activity: C'Thun in {} at {:.0f}% health, {}: {} of the raid alive, {} in his stomach; {} Eye "
                "Tentacles up{}; on: {}", raid, body->GetHealthPct(), weak ? "weakened" : "in his carapace", alive, inside, eyesUp,
                bigOnes, targets);
        }
    }
    // A Giant Eye Tentacle comes 38 s after his tasks begin (the body's phase, the end of a weakened window) and every
    // 60 s after: from 5 s before it until it is dead the melee are at their places (they stood under him in a bunch
    // when it came after a window, and its beam took ten in half a minute, 2026-11-25).
    bool eyeSoon = false;
    if (bodyPhase)
    {
        if (!run.cthunCycleAt)
            run.cthunCycleAt = now;
        uint32 const cycle = getMSTimeDiff(run.cthunCycleAt, now);
        eyeSoon = !bodyWeak && cycle >= 33 * IN_MILLISECONDS && (cycle - 33 * IN_MILLISECONDS) % (60 * IN_MILLISECONDS) < 8 * IN_MILLISECONDS;
    }
    else
        run.cthunCycleAt = 0;
    // The Dark Glare: its start, then the way it turns.
    bool const glare = eyePhase && eye->HasAura(RedAura);
    if (glare && !run.glareSince)
    {
        run.glareSince = now;
        run.glareAngle = eye->GetOrientation();
        run.glareWay = 0;
        run.glareTarget.clear();
        LOG_INFO(ActivityLog, "activity: the Eye of C'Thun in {} begins its Dark Glare: who stands on its line steps aside", raid);
    }
    else if (!glare && run.glareSince)
    {
        // Its end: the raid stands where it turned to, so its places turn with it (no walk back over half a circle; the
        // beams come again 8 s after the glare).
        if (run.glareWay)
            for (auto& [guid, slot] : run.cthunSlot)
                slot.second = Position::NormalizeOrientation(slot.second + float(run.glareWay) * (run.glareTurn - run.glareStartTurn));
        run.glareSince = 0;
        run.glareWay = 0;
        run.glareTarget.clear();
    }
    if (glare && run.glareWay)
        run.glareTurn = Position::NormalizeOrientation(float(run.glareWay) * (eye->GetOrientation() - run.glareAngle));
    if (glare && !run.glareWay)
    {
        float const turned = Position::NormalizeOrientation(eye->GetOrientation() - run.glareAngle);
        if (turned > 0.03f && turned < float(M_PI))
            run.glareWay = 1;
        else if (turned < 2.0f * float(M_PI) - 0.03f && turned >= float(M_PI))
            run.glareWay = -1;
        if (run.glareWay)
        {
            // The whole raid turns with the line, each at its place's distance (the melee out to 25 yd, where the band
            // leaves room) and as fast as the line, so nobody's place changes against the others' (the beams jump 13 yd)
            // and the line never reaches anybody: whoever stands in its band then steps out of it to the nearer side
            // first. (Before, whoever stood where it would sweep ran ahead of it to places beyond its end, half the raid
            // into the other half's room: the runners stood 3 to 5 yd apart and died to the first beams after it, and
            // the walk back took 20 s, 2026-11-25.)
            run.glareTurn = Position::NormalizeOrientation(float(run.glareWay) * (eye->GetOrientation() - run.glareAngle));
            run.glareStartTurn = run.glareTurn;
            float const line = eye->GetOrientation();
            run.glareTarget.clear();
            uint32 count = 0;
            for (Player* member : group)
            {
                auto const slotItr = run.cthunSlot.find(member->GetGUID());
                if (!member->IsAlive() || member->GetPositionZ() < 0.0f || slotItr == run.cthunSlot.end())
                    continue;
                float const ring = std::max(slotItr->second.first, GlareRunRadius);
                float base = eye->GetAngle(member);
                float delta = Position::NormalizeOrientation(base - line);
                if (delta > float(M_PI))
                    delta -= 2.0f * float(M_PI);
                if (std::fabs(delta) < GlareBand(ring) + 0.3f)
                    base = line + (delta >= 0.0f ? 1.0f : -1.0f) * (GlareBand(ring) + 0.35f);
                run.glareTarget[member->GetGUID()] = { ring, base };
                ++count;
            }
            LOG_INFO(ActivityLog, "activity: the Dark Glare in {} turns {}: {} of the raid turn with it", raid,
                run.glareWay > 0 ? "anticlockwise" : "clockwise", count);
        }
    }
    Map* map = center->GetMap();
    auto ringPoint = [&](float radius, float angle, float& x, float& y, float& z)
    {
        x = center->GetPositionX() + radius * std::cos(angle);
        y = center->GetPositionY() + radius * std::sin(angle);
        float const h = map->GetHeight(center->GetPhaseMask(), x, y, center->GetPositionZ() + 10.0f, true, 30.0f);
        z = h > INVALID_HEIGHT ? h : center->GetPositionZ();
    };
    // The moment the Eye fights the hunter, the raid that waited on the ramp is put at its places on the rings, all at
    // once, as the twins' halves are put at theirs: the Eye takes whoever comes into its sight on the chamber's floor,
    // so nobody can stand at its place before the pull, and a raid that walked there from the ramp, before or after
    // the pull, was still in a crowd when the beams came (39 dead at once at 18 s, eight at 8 s, 2026-11-24). Its first
    // three beams then go at the hunter, alone at its place.
    if (eyePhase && !run.cthunFightSince)
        run.cthunFightSince = now;
    if (eyePhase && getMSTimeDiff(run.cthunFightSince, now) < 15 * IN_MILLISECONDS)
    {
        uint32 put = 0;
        for (Player* member : group)
        {
            if (!member->IsAlive() || member->IsBeingTeleported() || !sGuildBotHolder.IsAIPlayed(member->GetGUID()) ||
                run.cthunPut.count(member->GetGUID()))
                continue;
            auto const slotItr = run.cthunSlot.find(member->GetGUID());
            if (slotItr == run.cthunSlot.end())
                continue;
            run.cthunPut.insert(member->GetGUID());
            float x, y, z;
            ringPoint(slotItr->second.first, slotItr->second.second, x, y, z);
            if (member->GetExactDist2d(x, y) <= 6.0f)
                continue;
            BotDriver::PauseAI(member, 1500);
            member->GetMotionMaster()->Clear();
            member->GetMotionMaster()->MoveIdle();
            member->NearTeleportTo(x, y, z + 0.5f, member->GetAngle(center));
            if (BotRuntime* other = FindRuntime(member->GetGUID()))
                other->hasLastPosition = false;
            ++put;
        }
        if (put)
            LOG_INFO(ActivityLog, "activity: {} in {} fights: {} of the raid are put at their places on the rings", center->GetName(), raid, put);
    }
    // The Giant Claw Tentacles: one whose target is not within its melee reach and has nobody within 5 yd of it every
    // 10 s goes down and comes up whole under somebody else (npc_giant_claw_tentacle). Whom it attacks stays at it.
    std::vector<Creature*> giantClaws;
    if (bodyPhase)
    {
        std::list<Creature*> found;
        center->GetCreatureListWithEntryInGrid(found, GiantClawEntry, 120.0f);
        for (Creature* claw : found)
            if (claw->IsAlive() && !claw->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE))
                giantClaws.push_back(claw);
    }
    for (Player* member : group)
    {
        if (!member->IsAlive() || member->IsBeingTeleported() || !sGuildBotHolder.IsAIPlayed(member->GetGUID()))
            continue;
        uint32& at = run.cthunStepAt[member->GetGUID()];
        if (now < at)
            continue;
        // In the stomach: the Flesh Tentacles, then out. (Below the chamber's floor: the acid stays on a member that is
        // out again, and one that was sent to the exit from up there walked into the ground, 2026-11-24.) The navmesh
        // does not cover the stomach and the collision data takes its floor for solid ground: the ground guard refused
        // every step there, to the tentacles and to the exit, and the swallowed members digested where they fell.
        if (member->GetPositionZ() < 0.0f)
        {
            sGroundGuard.FreeFor(member->GetGUID(), 3000);
            uint32 const stacks = member->GetAuraCount(AcidAura);
            // The two Flesh Tentacles: whoever is inside fights the weaker one (each one killed alone heals nothing,
            // both dead take his carapace off for 45 s).
            std::list<Creature*> fleshes;
            member->GetCreatureListWithEntryInGrid(fleshes, FleshEntry, 80.0f);
            Creature* flesh = nullptr;
            std::string fleshNote;
            for (Creature* tentacle : fleshes)
            {
                if (!tentacle->IsAlive())
                    continue;
                fleshNote += Acore::StringFormat("{}{:.0f}%", fleshNote.empty() ? "" : " and ", tentacle->GetHealthPct());
                if (!flesh || tentacle->GetHealth() < flesh->GetHealth())
                    flesh = tentacle;
            }
            if (fleshNote.empty())
                fleshNote = "none alive";
            if (run.cthunInside.insert(member->GetGUID()).second)
                LOG_INFO(ActivityLog, "activity: {} is in the stomach of C'Thun in {} ({:.0f}% health; his flesh tentacles: {})",
                    member->GetName(), raid, member->GetHealthPct(), fleshNote);
            // (the owner's guide: a tank leaves at once, it is needed up top for the Giant Claw Tentacles; a healer heals the
            // others in the stomach, then leaves; the damage dealers stay up to about ten stacks of the acid. Here six
            // (a mage that left at ten came out at 12 % and died at the exit; two that left at seven and 23-30 % died in
            // the exit's 4 s), or below 55 % health. Five died at 45 % in a try in which the squads' catch-up took them out of the stomach
            // before its exit, the acid still on them; with 60 % those inside took 15 s each and the second tentacle
            // lived on, 2026-11-24.)
            DungeonRole const role = roleOf(member);
            // (A warlock pays its Life Taps with health: out below 35 % instead; they left at 32 % and 3 stacks.)
            float const outBelow = member->getClass() == CLASS_WARLOCK ? 35.0f : 55.0f;
            bool leave = stacks >= 6 || member->GetHealthPct() < outBelow || !flesh || role == DungeonRole::Tank;
            if (!leave && role == DungeonRole::Healer)
            {
                // (While a damage dealer is inside with it, hurt or not: the acid takes them down fast, and with the
                // healers out at once the damage dealers inside left after 15 s each, 2026-11-25.)
                bool hurt = false;
                for (Player* other : group)
                    if (other != member && other->IsAlive() && other->GetPositionZ() < 0.0f && roleOf(other) != DungeonRole::Tank &&
                        roleOf(other) != DungeonRole::Healer && member->GetExactDist(other) < 60.0f)
                        hurt = true;
                // Alone in there it fights the tentacle itself (healers that left at once, at 93 % and two stacks, gave
                // the flesh tentacles a fifth of the swallowed members' time, 2026-11-25).
                if (hurt)
                {
                    if (member->GetVictim())
                        member->AttackStop();
                    at = now + 1500;
                    continue;
                }
            }
            if (leave)
            {
                // The exit's script throws a player up and, 3 s later, puts it above him and takes the acid off, but only
                // with an Exit Trigger (15800) within 10 yd, and the world has none: the acid stayed on those that got
                // out, and others stayed inside, up to 14 stacks (2026-11-25). After those 3 s the run does what the
                // script would: the acid off, the member at its place up top.
                uint32& sentAt = run.cthunExitAt[member->GetGUID()];
                if (sentAt && getMSTimeDiff(sentAt, now) >= 3 * IN_MILLISECONDS && member->GetExactDist(ExitX, ExitY, ExitZ) > 10.0f)
                    sentAt = 0;
                if (sentAt && getMSTimeDiff(sentAt, now) >= 3 * IN_MILLISECONDS)
                {
                    sentAt = 0;
                    member->RemoveAurasDueToSpell(AcidAura);
                    float px = center->GetPositionX(), py = center->GetPositionY(), pz = center->GetPositionZ();
                    if (auto const slotItr = run.cthunSlot.find(member->GetGUID()); slotItr != run.cthunSlot.end())
                        ringPoint(slotItr->second.first, slotItr->second.second, px, py, pz);
                    member->NearTeleportTo(px, py, pz + 0.5f, member->GetAngle(center));
                    if (BotRuntime* other = FindRuntime(member->GetGUID()))
                        other->hasLastPosition = false;
                    LOG_INFO(ActivityLog, "activity: {} is thrown up out of the stomach of C'Thun in {} to its place", member->GetName(), raid);
                    at = now + 2000;
                    continue;
                }
                if (member->GetExactDist(ExitX, ExitY, ExitZ) <= 4.5f)
                {
                    sentAt = now;
                    WorldPacket packet(CMSG_AREATRIGGER, 4);
                    packet << uint32(ExitTrigger);
                    member->GetSession()->HandleAreaTriggerOpcode(packet);
                    // (It must stay within 10 yd of the exit for 3 s, then the stomach throws it up; its AI walked it back
                    // to a tentacle in that time and it stayed inside with the acid on it.)
                    member->StopMoving();
                    member->GetMotionMaster()->Clear();
                    BotDriver::PauseAI(member, 5000);
                    LOG_INFO(ActivityLog, "activity: {} leaves the stomach of C'Thun in {} ({} stacks of its acid, {:.0f}% health; "
                        "his flesh tentacles: {})", member->GetName(), raid, stacks, member->GetHealthPct(), fleshNote);
                    at = now + 5000;
                }
                else
                {
                    BotDriver::PauseAI(member, 2000);
                    member->AttackStop();
                    member->GetMotionMaster()->MovePoint(0, ExitX, ExitY, ExitZ, FORCED_MOVEMENT_RUN, 0.0f, 0.0f, false);
                    at = now + 1000;
                }
                continue;
            }
            BotDriver::SetRti(member, "square");
            if (member->GetVictim() != flesh)
                BotDriver::Attack(member, flesh, false);
            at = now + 1500;
            continue;
        }
        run.cthunInside.erase(member->GetGUID());
        auto const slotItr = run.cthunSlot.find(member->GetGUID());
        if (slotItr == run.cthunSlot.end())
            continue;
        float const radius = slotItr->second.first;
        float angle = slotItr->second.second;
        float const at2d = member->GetExactDist2d(center);
        float const own = center->GetAngle(member);
        // Along its ring, never across it: a straight way to the far side runs through the Eye's middle, where its glare
        // hits whoever stands within its body whatever way it looks (14 of the 14 glare deaths of a try stood 2 to 6 yd
        // from its middle, the last hit 25000 to 39000 from Dark Glare, 2026-11-24), and over its line. A step of at most
        // 0.6 of a radian (10 yd on the melee ring, 25 yd on the outer one) a time.
        auto stepAround = [&](float from, float to)
        {
            float turn = Position::NormalizeOrientation(to - from);
            if (turn > float(M_PI))
                turn -= 2.0f * float(M_PI);
            return from + std::clamp(turn, -0.6f, 0.6f);
        };
        // Whom a Giant Claw Tentacle attacks stays within its melee reach (its place waits): sent back to its place
        // every time, it left the claw with nobody beside it, and the claw came up whole elsewhere; 28 of the raid on
        // one took it from 28 % to 9 % in a minute and it was at 100 % again, 2026-11-25.
        if (Creature* claw = [&]() -> Creature*
            {
                for (Creature* candidate : giantClaws)
                    if (candidate->GetVictim() == member)
                        return candidate;
                return nullptr;
            }())
        {
            if (!member->IsWithinMeleeRange(claw))
            {
                Position const spot = claw->GetNearPosition(1.5f, claw->GetRelativeAngle(member));
                member->GetMotionMaster()->MovePoint(0, spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ(), FORCED_MOVEMENT_RUN);
            }
            at = now + 500;
            continue;
        }
        // Thrown up out of the stomach onto him, 10 yd above the chamber's floor: down to its place at once. (A client
        // stands in his middle's area trigger and is spat out; a bot sends none, and the ground guard refused every
        // step off him: those that came out stood there in a bunch until a Giant Eye Tentacle's beam jumped through
        // them, 2026-11-24.)
        if (bodyPhase && member->GetPositionZ() > center->GetPositionZ() + 4.0f && at2d < 15.0f)
        {
            sGroundGuard.FreeFor(member->GetGUID(), 4000);
            float x, y, z;
            ringPoint(radius, own, x, y, z);
            BotDriver::PauseAI(member, 2500);
            member->AttackStop();
            member->GetMotionMaster()->Clear();
            member->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_RUN, 0.0f, 0.0f, false);
            at = now + 1500;
            continue;
        }
        if (glare)
        {
            // The line hits whoever stands in front of the Eye within 16.5 yd of it (the Eye's reach 15 yd and the
            // member's 1.5: a "cone line", HasInLine), once a second as it turns. (Not the 5 degrees of its spell_cone row:
            // members that stepped a few yards out of such a line died to it one after the other as it turned, 28 of a
            // try, 2026-11-24.)
            float target = own;
            float keep = std::max(radius, 16.0f);
            bool move = false;
            if (!run.glareWay)
            {
                // Before the way is known: out of the band at its first angle, to the nearer side.
                float delta = Position::NormalizeOrientation(own - run.glareAngle);
                if (delta > float(M_PI))
                    delta -= 2.0f * float(M_PI);
                float const ring = std::max(radius, GlareRunRadius);
                // (Three of its steps beyond the band: who stood a step from its edge on the side it then turned to was
                // overtaken before it set off ahead, eight at once, 2026-11-24.)
                if (std::fabs(delta) < GlareBand(std::max(at2d, 1.0f)) + 0.05f)
                {
                    target = run.glareAngle + (delta >= 0.0f ? 1.0f : -1.0f) * (GlareBand(ring) + 0.35f);
                    keep = ring;
                    move = true;
                }
            }
            else if (auto const glareItr = run.glareTarget.find(member->GetGUID()); glareItr != run.glareTarget.end())
            {
                // As far on as the line has turned since its way became known, in steps of 0.45 of a radian (5 s of the
                // line's turn), each the step ahead of it: between two steps the members stand and cast and heal. (Turned
                // with it all the time, the raid's AI was held through every glare: no heals, no kills, and the Eye
                // Tentacles' Mind Flays killed 32 of a try, 2026-11-25.)
                keep = glareItr->second.first;
                constexpr float TurnStep = 0.45f;
                float const turned = std::max(run.glareTurn - run.glareStartTurn, 0.0f);
                target = glareItr->second.second + float(run.glareWay) * (std::floor(turned / TurnStep) + 1.0f) * TurnStep;
                float off = Position::NormalizeOrientation(target - own);
                if (off > float(M_PI))
                    off -= 2.0f * float(M_PI);
                move = std::fabs(off) * std::max(at2d, 1.0f) > 1.0f || std::fabs(at2d - keep) > 2.0f;
            }
            // Else it holds where it stands, at its ring's distance (its AI walked the melee in under the Eye).
            if (!move && at2d >= keep - 2.0f)
                continue;
            float x, y, z;
            ringPoint(keep, move ? (run.glareWay ? target : stepAround(own, target)) : own, x, y, z);
            BotDriver::PauseAI(member, move ? 900 : 600);
            if (move)
                member->InterruptNonMeleeSpells(false);
            member->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_RUN);
            at = now + (move ? 500 : 400);
            continue;
        }
        // (Not while it fights a Claw Tentacle beside it: it kills that first. Out of the glare's line it runs all the same:
        // two rogues that fought one on the melee ring died to the line, 2026-11-24.)
        // (In the Eye's phase only a tentacle at its own place: the melee went to Claw Tentacles that came up under
        // others, 12 yd off, and stood 11 to 12 yd apart when a beam came; it jumped through six, 2026-11-25.)
        if (Unit* victim = member->GetVictim(); victim && (victim->GetEntry() == ClawEntry || victim->GetEntry() == GiantClawEntry) &&
            member->GetExactDist2d(victim) < 12.0f)
        {
            float px, py, pz;
            ringPoint(radius, angle, px, py, pz);
            // (The body's phase the same: whom a Giant Claw Tentacle attacks stays at it, above, and a tank may go to it;
            // the others shoot it from their places. Who stood around the claws stood 8 yd apart when the beams came.)
            if (victim->GetExactDist2d(px, py) < 6.0f || (bodyPhase && (roleOf(member) == DungeonRole::Tank || (!giantEyeUp && !eyeSoon))))
                continue;
            if (BotDriver::FightsInMelee(member))
                member->AttackStop();
        }
        // The body's phase: the melee go to the tentacles where they come up (a Giant Claw Tentacle under somebody
        // anywhere), only not under him (he spits out who stands there); the others keep their places. While a Giant
        // Eye Tentacle is up the melee keep theirs too (its beam jumps 13 yd like the Eye's: seven melee around a Giant
        // Claw died to one beam, its last hit 71759, 2026-11-24).
        // A tank goes to the Giant Claw Tentacle whatever is up (it came up under a damage dealer and killed five,
        // 2026-11-24). While he is weakened the melee fight him where they stand under him: a bot sends no area
        // trigger, so he spits out nobody (only a client's character in his middle is thrown out).
        // The other melee keep their places in the body's phase while a Giant Eye Tentacle is up (around the claws they
        // stood in a bunch when one came, and its beam jumped through five and six of them at once, 2026-11-25; held at
        // their places all the time, the Giant Claw Tentacles lived on, three at a time, and killed 17).
        bool const bodyTank = bodyPhase && roleOf(member) == DungeonRole::Tank;
        bool const meleeFree = bodyTank || (!giantEyeUp && !eyeSoon);
        if (bodyPhase && BotDriver::FightsInMelee(member) && roleOf(member) != DungeonRole::Healer && meleeFree)
        {
            if (at2d >= 9.0f || bodyWeak)
                continue;
            angle = own;
        }
        // (Back to its place after a glare: around the ring too.)
        if (eyePhase && at2d > 8.0f)
            angle = stepAround(own, angle);
        float x, y, z;
        // While he is weakened and no Giant Eye Tentacle is up the outer ring steps in to 31 yd (from 44.5 yd he is out of
        // every caster's reach: his size is 1 yd, the Eye's 15).
        ringPoint(bodyWeak && !giantEyeUp && radius > 40.0f ? 31.0f : radius, angle, x, y, z);
        // In the Eye's phase a melee fighter on an outer ring cannot reach the Eye: it holds its place and fights only a
        // tentacle that comes up beside it (its AI walked it in to the Eye, among the others, and the beams jumped
        // through them). Everybody keeps within 2 yd of its place, looked at twice a second.
        if (BotDriver::FightsInMelee(member) && roleOf(member) != DungeonRole::Healer &&
            ((eyePhase && radius > 20.0f) || (bodyPhase && !meleeFree)))
        {
            Creature* tentacleNear = nullptr;
            for (uint32 entry : { ClawEntry, EyeTentacleEntry, GiantClawEntry, GiantEyeEntry })
                if (Creature* tentacle = member->FindNearestCreature(entry, 10.0f, true); tentacle && tentacle->GetExactDist2d(x, y) < 10.0f &&
                    (!tentacleNear || member->GetExactDist(tentacle) < member->GetExactDist(tentacleNear)))
                    tentacleNear = tentacle;
            if (tentacleNear)
            {
                BotDriver::Attack(member, tentacleNear, false);
                at = now + 1000;
                continue;
            }
            BotDriver::PauseAI(member, 1500);
            if (member->GetVictim())
                member->AttackStop();
        }
        // (The Eye Beam jumps 13 yd between two players, as far as 60 members, and the places are 13.5 yd apart: within
        // 0.7 yd of its place, looked at three times a second.)
        // (The body's phase the same: the Giant Eye Tentacles' beams jump 13 yd too.)
        // (A caster on its way to a Giant Eye Tentacle beyond its reach is let go until it is in reach.)
        if (Unit* victim = member->GetVictim(); bodyPhase && victim && victim->GetEntry() == GiantEyeEntry &&
            !BotDriver::FightsInMelee(member) && member->GetDistance(victim) > 28.0f)
        {
            at = now + 500;
            continue;
        }
        if (member->GetExactDist2d(x, y) > 0.7f)
        {
            // (A short pause: a pause of a second at each step back held the casters' AI so often that they hardly cast.)
            BotDriver::PauseAI(member, 400);
            member->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_RUN);
        }
        at = now + 300;
    }
}

// The C'Thun step four times a second for the bot that leads the raid (the run's own step comes once a second, and the
// glare's line moves 5 degrees a second: runners that set off a second late were overtaken).
void OrderService::StepRaidCthunFast(Player* lead, BotRuntime& rt)
{
    if (rt.dungeon.phase != PhaseInside || rt.dungeon.cthunSlot.empty() || lead->GetMapId() != 531)
        return;
    AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfMember(lead->GetGUID());
    DungeonPlan const* plan = sDungeons.GetPlan(rt.dungeon.id);
    if (!guild || !plan)
        return;
    StepRaidCthun(rt.dungeon, DungeonGroup(*guild, lead->GetGUID()), plan->name);
}

// Creatures that burn everybody within their reach while they channel (Blackrock Depths' Ironhand Guardians in Magmus'
// hall: Gout of Flame, 160 fire damage a second to everybody within 30 yd of each of the six statues while it channels;
// they go on after Magmus falls, and the owner's groups died standing in them, 2026-11-23): a member within reach of one
// that channels goes to the nearest place out of the reach of all of them that channel.
// Area triggers a dungeon's way needs that only a client reports: a player's client sends CMSG_AREATRIGGER when it walks
// into one, a bot has no client. When a member the AI plays stands in one of these, the module sends that report for it,
// once per run, as a client does (a hand-played member's client reports it itself). Upper Blackrock Spire's script
// counts the guards of the Dragonspire Hall's seven runes only after a player walked into trigger 2046, and opens the
// two doors to Pyroguard Emberseer when they are dead: without it the doors stayed shut and the group was put beside
// the Blackrock Altar (2026-11-26). Not every scripted trigger of a dungeon: the Ring of Law's starts the arena's
// waves, Mograine's room has the run's own clearing, the stadium's is the event of Rend Blackhand's row.
void OrderService::StepWayTriggers(BotRuntime::DungeonRun& run, std::vector<Player*> const& group, std::string const& dungeon)
{
    static uint32 const Triggers[] = {
        2046, // Upper Blackrock Spire, the Dragonspire Hall (at_dragonspire_hall)
    };
    for (uint32 id : Triggers)
    {
        if (run.waySent.count(id))
            continue;
        AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(id);
        if (!trigger)
            continue;
        for (Player* member : group)
        {
            if (!member->IsAlive() || member->IsBeingTeleported() || !member->FindMap() || member->GetMapId() != trigger->map ||
                !sGuildBotHolder.IsAIPlayed(member->GetGUID()) || !member->IsInAreaTriggerRadius(trigger))
                continue;
            WorldPacket packet(CMSG_AREATRIGGER, 4);
            packet << uint32(id);
            member->GetSession()->HandleAreaTriggerOpcode(packet);
            run.waySent.insert(id);
            LOG_INFO(ActivityLog, "activity: {} walks into area trigger {} in {}: reported as a client does", member->GetName(), id,
                dungeon);
            break;
        }
    }
}

void OrderService::StepDungeonFlames(BotRuntime::DungeonRun& run, std::vector<Player*> const& group, std::string const& dungeon)
{
    struct Flame { uint32 entry; uint32 aura; float yards; };
    static Flame const Flames[] = {
        { 8982, 15529, 30.0f },  // Ironhand Guardian (Blackrock Depths, Magmus' hall): Gout of Flame
    };
    constexpr uint32 StepMs = 2000;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    for (Player* member : group)
    {
        if (!member->IsAlive() || member->IsBeingTeleported() || !sGuildBotHolder.IsAIPlayed(member->GetGUID()) || !member->FindMap())
            continue;
        uint32& next = run.burstStepAt[member->GetGUID()];
        if (now < next)
            continue;
        for (Flame const& flame : Flames)
        {
            std::list<Creature*> found;
            member->GetCreatureListWithEntryInGrid(found, flame.entry, 90.0f);
            std::vector<Creature*> burning;
            for (Creature* creature : found)
                if (creature->IsAlive() && creature->HasAura(flame.aura))
                    burning.push_back(creature);
            auto inReach = [&](float x, float y, float z)
            {
                for (Creature* creature : burning)
                    if (creature->GetExactDist(x, y, z) < flame.yards + 2.0f)
                        return true;
                return false;
            };
            if (burning.empty() || !inReach(member->GetPositionX(), member->GetPositionY(), member->GetPositionZ()))
                continue;
            Map* map = member->GetMap();
            bool found2 = false;
            float x = 0.0f, y = 0.0f, z = 0.0f;
            for (float radius : { 6.0f, 10.0f, 15.0f, 20.0f, 26.0f, 32.0f, 40.0f })
            {
                for (int step = 0; step < 16 && !found2; ++step)
                {
                    float const angle = float(step) * float(M_PI) / 8.0f;
                    float const cx = member->GetPositionX() + radius * std::cos(angle), cy = member->GetPositionY() + radius * std::sin(angle);
                    float const cz = map->GetHeight(member->GetPhaseMask(), cx, cy, member->GetPositionZ() + 5.0f, true, 20.0f);
                    if (cz <= INVALID_HEIGHT || std::fabs(cz - member->GetPositionZ()) > 6.0f || inReach(cx, cy, cz))
                        continue;
                    PathGenerator probe(member);
                    probe.CalculatePath(cx, cy, cz, false);
                    if (probe.GetPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY_START | PATHFIND_FARFROMPOLY_END))
                        continue;
                    x = cx;
                    y = cy;
                    z = cz;
                    found2 = true;
                }
                if (found2)
                    break;
            }
            if (!found2)
                continue;
            BotDriver::PauseAI(member, StepMs);
            member->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_RUN);
            next = now + StepMs;
            if (!run.burstLogged.count(flame.aura))
            {
                run.burstLogged.insert(flame.aura);
                LOG_INFO(ActivityLog, "activity: the group in {} steps out of the flames of {} (everybody within {:.0f} yd of one that "
                    "channels goes out of the reach of all of them)", dungeon, burning.front()->GetName(), flame.yards);
            }
            break;
        }
    }
}

// Creatures whose fire pulses round them all through a fight (Molten Core's Flame Imps, `PulserOf`): only a tank stands
// among them. Everybody else keeps out of their pulse, a melee damage dealer holds back while it has no creature on it
// and they fight, and who stands in a pulse without the creature on it steps out to the nearest place out of reach of
// all of them (the owner, 2026-11-27: [quote left out]; his raid had walked into twenty imps and died).
void OrderService::StepDungeonPulsers(BotRuntime::DungeonRun& run, std::vector<Player*> const& group, std::string const& dungeon)
{
    constexpr uint32 StepMs = 1500;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    AdventureGuild* guild = group.empty() ? nullptr : sGuildAdventureMgr.FindGuildOfMember(group.front()->GetGUID());
    for (Player* member : group)
    {
        if (!member->IsAlive() || member->IsBeingTeleported() || !sGuildBotHolder.IsAIDriven(member->GetGUID()) || !member->FindMap())
            continue;
        GuildBot const* memberBot = guild ? guild->FindMember(member->GetGUID()) : nullptr;
        DungeonRole const role = memberBot ? sRoleService.EffectiveRole(*memberBot) : DungeonRole::None;
        if (role == DungeonRole::Tank)
            continue;
        std::vector<Creature*> const pulsers = PulsersNear(member, 45.0f);
        if (pulsers.empty())
            continue;
        uint32& next = run.burstStepAt[member->GetGUID()];
        if (now < next)
            continue;
        bool attacked = false;
        for (Creature* creature : pulsers)
            if (creature->GetVictim() == member)
                attacked = true;
        bool const melee = role != DungeonRole::Healer && BotDriver::FightsInMelee(member);
        bool const inPulse = InPulse(pulsers, member->GetPositionX(), member->GetPositionY(), member->GetPositionZ());
        if (!inPulse)
        {
            // Out of their reach: a melee damage dealer with no creature on it does not go in; its victim, if a pulser,
            // is let go (the ranged and the tank take them).
            if (melee && !attacked && member->GetVictim() && PulserOf(member->GetVictim()->ToCreature()))
            {
                member->AttackStop();
                BotDriver::PauseAI(member, StepMs);
                member->GetMotionMaster()->Clear();
                member->StopMoving();
                next = now + StepMs;
            }
            else if (melee && !attacked && member->isMoving())
            {
                // On its way into them: stop at the edge.
                float const tx = member->movespline->Finalized() ? member->GetPositionX() : member->movespline->FinalDestination().x;
                float const ty = member->movespline->Finalized() ? member->GetPositionY() : member->movespline->FinalDestination().y;
                float const tz = member->movespline->Finalized() ? member->GetPositionZ() : member->movespline->FinalDestination().z;
                if (InPulse(pulsers, tx, ty, tz))
                {
                    BotDriver::PauseAI(member, StepMs);
                    member->GetMotionMaster()->Clear();
                    member->StopMoving();
                    next = now + StepMs;
                }
            }
            continue;
        }
        if (attacked && !melee)
            continue;  // a ranged member with an imp on it is the tank's to help; it does not drag the imp about
        Map* map = member->GetMap();
        bool found = false;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        for (float radius : { 6.0f, 10.0f, 15.0f, 20.0f, 26.0f, 32.0f, 40.0f })
        {
            for (int step = 0; step < 16 && !found; ++step)
            {
                float const angle = float(step) * float(M_PI) / 8.0f;
                float const cx = member->GetPositionX() + radius * std::cos(angle), cy = member->GetPositionY() + radius * std::sin(angle);
                float const cz = map->GetHeight(member->GetPhaseMask(), cx, cy, member->GetPositionZ() + 5.0f, true, 20.0f);
                if (cz <= INVALID_HEIGHT || std::fabs(cz - member->GetPositionZ()) > 6.0f || InPulse(pulsers, cx, cy, cz))
                    continue;
                PathGenerator probe(member);
                probe.CalculatePath(cx, cy, cz, false);
                if (probe.GetPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY_START | PATHFIND_FARFROMPOLY_END))
                    continue;
                x = cx;
                y = cy;
                z = cz;
                found = true;
            }
            if (found)
                break;
        }
        if (!found)
            continue;
        if (melee)
            member->AttackStop();
        BotDriver::PauseAI(member, StepMs);
        member->GetMotionMaster()->Clear();
        member->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_RUN);
        next = now + StepMs;
        uint32 const key = pulsers.front()->GetEntry();
        if (!run.burstLogged.count(key))
        {
            run.burstLogged.insert(key);
            LOG_INFO(ActivityLog, "activity: the group in {} keeps out of the pulse of {} (only its tank stands among them; melee "
                "damage dealers hold back while they fight)", dungeon, pulsers.front()->GetName());
        }
    }
}

// A raid boss whose wound stacks on its tank (each stack less healing): at that many stacks an assist tank of the raid
// with no more than one taunts it and is the main tank from then on (the flag mod-playerbots' tanks follow), and so back
// and forth. Kurinnaxx's Mortal Wound (10 % less healing a stack, 15 s) brought its tank down in a minute, 2026-11-21.
void OrderService::StepRaidTankSwap(BotRuntime::DungeonRun& run, std::vector<Player*> const& group, std::string const& raid)
{
    struct Swap { uint32 boss; uint32 wound; uint8 stacks; };
    static Swap const Swaps[] = {
        { 15348, 25646, 5 },  // Kurinnaxx (Ruins of Ahn'Qiraj): Mortal Wound
        { 15510, 25646, 5 },  // Fankriss the Unyielding (Temple of Ahn'Qiraj): the same Mortal Wound
    };
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    if (run.tankSwapAt && getMSTimeDiff(run.tankSwapAt, now) < 8 * IN_MILLISECONDS)
        return;
    for (Swap const& swap : Swaps)
    {
        Creature* boss = nullptr;
        for (Player* member : group)
            if (member->IsAlive() && member->FindMap() && (boss = member->FindNearestCreature(swap.boss, 80.0f, true)))
                break;
        if (!boss || !boss->IsInCombat())
            continue;
        Player* victim = boss->GetVictim() ? boss->GetVictim()->ToPlayer() : nullptr;
        if (!victim || std::find(group.begin(), group.end(), victim) == group.end() || victim->GetAuraCount(swap.wound) < swap.stacks)
            continue;
        for (Player* member : group)
        {
            Group const* party = member->GetGroup();
            if (member == victim || !member->IsAlive() || !party || !party->IsAssistant(member->GetGUID()) ||
                !sGuildBotHolder.IsAIPlayed(member->GetGUID()) || member->GetAuraCount(swap.wound) > 1 || member->GetDistance(boss) > 25.0f ||
                member->HasUnitState(UNIT_STATE_CONTROLLED))
                continue;
            uint32 taunt = 0;
            switch (member->getClass())
            {
                case CLASS_WARRIOR: taunt = 355; break;
                case CLASS_PALADIN: taunt = 62124; break;
                case CLASS_DRUID: taunt = 6795; break;
                case CLASS_DEATH_KNIGHT: taunt = 56222; break;
                default: break;
            }
            if (!taunt || !member->HasSpell(taunt) || member->HasSpellCooldown(taunt))
                continue;
            BotDriver::PauseAI(member, 800);
            SpellCastResult const result = member->CastSpell(boss, taunt, false);
            BotDriver::Attack(member, boss, false);
            run.tankSwapAt = now;
            if (result != SPELL_CAST_OK)
                continue;
            run.raidMainTank = member->GetGUID();
            run.raidLookAt = 0;
            LOG_INFO(ActivityLog, "activity: the raid in {} swaps its tank on {}: {} taunts it from {} ({} stacks of {})", raid,
                boss->GetName(), member->GetName(), victim->GetName(), uint32(victim->GetAuraCount(swap.wound)),
                sSpellMgr->GetSpellInfo(swap.wound) ? sSpellMgr->GetSpellInfo(swap.wound)->SpellName[0] : "its wound");
            return;
        }
    }
}

// Ossirian the Unscarred (Ruins of Ahn'Qiraj): his Strength of Ossirian makes him all but unkillable until a crystal
// next to him is used, which leaves him weak to one school for 45 s, and the next crystal stands elsewhere. mod-playerbots'
// aq20 strategy uses the crystal once he is within 25 yd of it; nobody brought him there, and the raid died with him at
// 88 %, 2026-11-22. His tank walks him to the nearest crystal while his Strength is on or his weakness ends in 10 s.
void OrderService::StepRaidOssirian(BotRuntime::DungeonRun& run, std::vector<Player*> const& group, std::string const& raid)
{
    constexpr uint32 OssirianEntry = 15339, CrystalEntry = 180619, Strength = 25176;
    static uint32 const Weakness[] = { 25177, 25178, 25180, 25181, 25183 };
    Creature* boss = nullptr;
    for (Player* member : group)
        if (member->IsAlive() && member->FindMap() && (boss = member->FindNearestCreature(OssirianEntry, 120.0f, true)))
            break;
    if (!boss || !boss->IsInCombat())
        return;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    // A crystal within his reach is used while his Strength is on or his weakness ends: mod-playerbots' strategy uses one
    // only from 25 yd and late, and his Strength killed the raid's tank and healers before (2026-11-22). The member
    // nearest to it that is no tank of his walks up and uses it (the crystal's script: its trigger casts a weakness
    // within 30 yd).
    {
        int32 weakLeft = 0;
        for (uint32 aura : Weakness)
            if (Aura const* weak = boss->GetAura(aura))
                weakLeft = std::max(weakLeft, weak->GetDuration());
        GameObject* nearCrystal = boss->FindNearestGameObject(CrystalEntry, 28.0f);
        if (nearCrystal && nearCrystal->isSpawned() && (boss->HasAura(Strength) || weakLeft < 3 * IN_MILLISECONDS) && now >= run.ossirianUseAt)
        {
            Player* user = nullptr;
            for (Player* member : group)
                if (member->IsAlive() && member != boss->GetVictim() && sGuildBotHolder.IsAIPlayed(member->GetGUID()) &&
                    member->GetExactDist(nearCrystal) < 45.0f && (!user || member->GetExactDist(nearCrystal) < user->GetExactDist(nearCrystal)))
                    user = member;
            if (user)
            {
                BotDriver::PauseAI(user, 1500);
                if (user->GetExactDist(nearCrystal) > 4.0f)
                    user->GetMotionMaster()->MovePoint(0, nearCrystal->GetPositionX(), nearCrystal->GetPositionY(), nearCrystal->GetPositionZ(), FORCED_MOVEMENT_RUN);
                else if (GameObjectAI* ai = nearCrystal->AI())
                {
                    user->StopMoving();
                    ai->GossipHello(user, true);
                    run.ossirianUseAt = now + 5 * IN_MILLISECONDS;
                    LOG_INFO(ActivityLog, "activity: {} uses the crystal {:.0f} yd from Ossirian the Unscarred in {} ({})", user->GetName(),
                        boss->GetDistance(nearCrystal), raid, boss->HasAura(Strength) ? "his Strength is on" : "his weakness ends in a few seconds");
                }
            }
        }
    }
    // While his Strength is on and no crystal is within its reach of him, the raid holds where it stands (at the crystal
    // his tank brings him to) and nobody goes to help: a priest and a second tank that ran to him died within seconds,
    // 2026-11-22. Who stands within 15 yd of him fights on, and the healers heal from where they stand (held as well, the
    // tank died 35 yd from them).
    if (boss->HasAura(Strength) && !boss->FindNearestGameObject(CrystalEntry, 30.0f))
    {
        run.raidHoldUntil = now + 1000;
        for (Player* member : group)
            if (member->IsAlive() && member != boss->GetVictim() && sGuildBotHolder.IsAIPlayed(member->GetGUID()) &&
                member->GetExactDist(boss) > 15.0f && sRoleService.GetRole(member->GetGUID()) != DungeonRole::Healer)
            {
                BotDriver::PauseAI(member, 800);
                if (member->GetVictim() == boss)
                    member->AttackStop();
                if (member->isMoving())
                    member->StopMoving();
            }
    }
    Player* tank = boss->GetVictim() ? boss->GetVictim()->ToPlayer() : nullptr;
    if (!tank || std::find(group.begin(), group.end(), tank) == group.end() || !sGuildBotHolder.IsAIPlayed(tank->GetGUID()))
        return;
    if (BotRuntime const* tankRuntime = GetRuntime(tank->GetGUID()))
        if (tankRuntime->pull.active)
            return; // (Its pull brings him to the crystal.)
    int32 weakLeft = 0;
    for (uint32 aura : Weakness)
        if (Aura const* weak = boss->GetAura(aura))
            weakLeft = std::max(weakLeft, weak->GetDuration());
    if (!boss->HasAura(Strength) && weakLeft > 10 * IN_MILLISECONDS)
        return;
    // The crystal nearest to where the raid stands (not to him or his tank: the tank took him to one 52 yd from its
    // raid, which then ran after it, 2026-11-22).
    float cx = 0.0f, cy = 0.0f;
    uint32 standing = 0;
    for (Player* member : group)
        if (member->IsAlive() && member != tank)
        {
            cx += member->GetPositionX();
            cy += member->GetPositionY();
            ++standing;
        }
    GameObject* crystal = nullptr;
    std::list<GameObject*> crystals;
    boss->GetGameObjectListWithEntryInGrid(crystals, CrystalEntry, 200.0f);
    for (GameObject* candidate : crystals)
    {
        if (!candidate->isSpawned())
            continue;
        float const from = standing ? candidate->GetExactDist2d(cx / standing, cy / standing) : candidate->GetExactDist2d(tank);
        if (!crystal || from < (standing ? crystal->GetExactDist2d(cx / standing, cy / standing) : crystal->GetExactDist2d(tank)))
            crystal = candidate;
    }
    if (!crystal || !crystal->isSpawned() || boss->GetDistance(crystal) <= 10.0f)
        return;
    uint32& next = run.burstStepAt[tank->GetGUID()];
    if (now < next)
        return;
    Position spot = crystal->GetPosition();
    crystal->MovePositionToFirstCollision(spot, 4.0f, crystal->GetAngle(tank) - crystal->GetOrientation());
    BotDriver::PauseAI(tank, 1200);
    tank->GetMotionMaster()->MovePoint(0, spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ(), FORCED_MOVEMENT_RUN);
    next = now + 1000;
    if (run.ossirianCrystal != crystal->GetGUID())
    {
        run.ossirianCrystal = crystal->GetGUID();
        LOG_INFO(ActivityLog, "activity: {} takes Ossirian the Unscarred in {} to the crystal {:.0f} yd away ({})", tank->GetName(), raid,
            boss->GetDistance(crystal), boss->HasAura(Strength) ? "his Strength is on" : "his weakness ends in a few seconds");
    }
}

// Hakkar (Zul'Gurub): his Blood Siphon, every 90 s from 90 s into the fight, heals him from each character in 100 yd
// that has no Poisonous Blood and hurts him from each that has it. The poison (90 s) comes from the cloud a Son of Hakkar
// leaves where it dies (8 yd, 10 s; spell 24320, whose effect the core does not apply: spell_ga_poisonous_blood below).
// In the 60 s before a siphon the raid's assist tanks and ranged kill a Son that comes within 60 yd of Hakkar (an assist
// tank holds it: on the ranged alone it killed four of them, 2026-11-22), and everybody without the poison but Hakkar's
// own tank steps into its cloud (docs/orders.md "Raid runs"). Two raids took Hakkar to 18 % and
// wiped on his heals, 2026-11-21.
void OrderService::StepRaidHakkar(BotRuntime::DungeonRun& run, std::vector<Player*> const& group, std::string const& raid)
{
    constexpr uint32 HakkarEntry = 14834, SonEntry = 11357, CloudEntry = 14989, SiphonSpell = 24324, PoisonAura = 24321;
    constexpr uint32 SiphonMs = 90 * IN_MILLISECONDS, WindowMs = 60 * IN_MILLISECONDS;
    Creature* hakkar = nullptr;
    for (Player* member : group)
        if (member->IsAlive() && member->FindMap())
            if ((hakkar = member->FindNearestCreature(HakkarEntry, 150.0f, true)))
                break;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    if (!hakkar || !hakkar->IsInCombat())
    {
        run.hakkarSince = run.hakkarSiphonAt = run.hakkarSiphonSeen = 0;
        return;
    }
    if (!run.hakkarSince)
    {
        run.hakkarSince = now;
        run.hakkarSiphonAt = now + SiphonMs;
        run.hakkarWindow = 0;
    }
    // (The siphon as it is cast: its time is the next one's start.)
    if (Spell const* channel = hakkar->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
        if (channel->GetSpellInfo()->Id == SiphonSpell && getMSTimeDiff(run.hakkarSiphonSeen, now) > 20 * IN_MILLISECONDS)
        {
            run.hakkarSiphonSeen = now;
            run.hakkarSiphonAt = now + SiphonMs;
        }
    if (now + WindowMs < run.hakkarSiphonAt)
        return;
    std::vector<Player*> need;
    for (Player* member : group)
        if (member->IsAlive() && !member->IsBeingTeleported() && sGuildBotHolder.IsAIPlayed(member->GetGUID()) &&
            hakkar->GetVictim() != member && !member->HasAura(PoisonAura) && member->GetDistance(hakkar) < 100.0f)
            need.push_back(member);
    if (need.empty())
        return;
    uint32 const window = run.hakkarSiphonAt;
    if (Creature* cloud = hakkar->FindNearestCreature(CloudEntry, 80.0f, true))
    {
        for (Player* member : need)
        {
            uint32& next = run.burstStepAt[member->GetGUID()];
            if (now < next || member->GetDistance(cloud) < 3.0f)
                continue;
            Position spot = cloud->GetPosition();
            cloud->MovePositionToFirstCollision(spot, frand(0.0f, 3.0f), frand(0.0f, 2 * float(M_PI)));
            BotDriver::PauseAI(member, 2000);
            member->AttackStop();
            member->GetMotionMaster()->MovePoint(0, spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ(), FORCED_MOVEMENT_RUN);
            next = now + 1000;
        }
        if (run.hakkarWindow != window)
        {
            run.hakkarWindow = window;
            LOG_INFO(ActivityLog, "activity: the raid in {} steps into the poison cloud of a Son of Hakkar before Blood Siphon "
                "({} characters without Poisonous Blood, the siphon in {} s)", raid, need.size(),
                run.hakkarSiphonAt > now ? (run.hakkarSiphonAt - now) / IN_MILLISECONDS : 0);
        }
        return;
    }
    // (A Son on his level or on the stairs, in his sight: not one of the hall 33 yd below his platform, which the raid
    // shot at for minutes, 2026-11-22.)
    Creature* son = nullptr;
    {
        std::list<Creature*> sons;
        hakkar->GetCreatureListWithEntryInGrid(sons, SonEntry, 70.0f);
        for (Creature* candidate : sons)
            if (candidate->IsAlive() && !candidate->IsInEvadeMode() && std::fabs(candidate->GetPositionZ() - hakkar->GetPositionZ()) <= 20.0f &&
                hakkar->IsWithinLOSInMap(candidate) && (!son || hakkar->GetExactDist(candidate) < hakkar->GetExactDist(son)))
                son = candidate;
    }
    if (!son)
        return;
    for (Player* member : group)
    {
        uint8 const cls = member->getClass();
        Group const* party = member->GetGroup();
        bool const assistTank = party && party->IsAssistant(member->GetGUID()) && hakkar->GetVictim() != member;
        if ((!assistTank && cls != CLASS_HUNTER && cls != CLASS_MAGE && cls != CLASS_WARLOCK) || !member->IsAlive() ||
            !sGuildBotHolder.IsAIPlayed(member->GetGUID()) || member->GetDistance(son) > 60.0f)
            continue;
        uint32& next = run.burstStepAt[member->GetGUID()];
        if (now < next)
            continue;
        if (member->GetVictim() != son)
            BotDriver::Attack(member, son, false);
        next = now + 2000;
    }
    if (run.hakkarSonWindow != window)
    {
        run.hakkarSonWindow = window;
        LOG_INFO(ActivityLog, "activity: the raid's ranged in {} kill a Son of Hakkar for its poison cloud ({} yd from Hakkar, "
            "Blood Siphon in {} s)", raid, uint32(son->GetDistance(hakkar)),
            run.hakkarSiphonAt > now ? (run.hakkarSiphonAt - now) / IN_MILLISECONDS : 0);
    }
}

// Razorgore the Untamed (Blackwing Lair). His first phase is the 30 Black Dragon Eggs of his room, which only he breaks:
// a member uses the Orb of Domination and possesses him (90 s; Mind Exhaustion 60 s after), and he casts Destroy Egg
// (3 s, in his reach) at one egg after the other while waves of his troops come at the raid. Damage that would kill him
// in that phase kills the raid (Explosion), so the run does not pull him while eggs are left (RazorgoreEggsLeft). A
// bot has no client to steer him: the module walks him to each egg and casts for him. Grethok the Controller and his
// guards stand at the orb and are fought first. The last egg begins the second phase: a boss like any other.
bool OrderService::RazorgoreEggsLeft(Creature const* razorgore)
{
    InstanceScript* instance = razorgore ? razorgore->GetInstanceScript() : nullptr;
    return instance && instance->GetData(2 /*DATA_EGG_EVENT*/) != DONE;
}

void OrderService::StepRaidRazorgore(BotRuntime::DungeonRun& run, std::vector<Player*> const& group, DungeonPlan const& plan,
    Player* leader)
{
    constexpr uint32 RazorgoreEntry = 12435, OrbEntry = 177808, EggEntry = 177807, Exhaustion = 23958, DestroyEgg = 19873;
    if (run.boss >= plan.bosses.size() || plan.bosses[run.boss].entry != RazorgoreEntry || !leader || !leader->IsAlive())
        return;
    Creature* razor = leader->FindNearestCreature(RazorgoreEntry, 150.0f, true);
    if (!razor || !RazorgoreEggsLeft(razor))
        return;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    Player* controller = nullptr;
    if (Unit* charmer = razor->GetCharmer())
        if (Player* possessor = charmer->ToPlayer())
            if (std::find(group.begin(), group.end(), possessor) != group.end())
                controller = possessor;
    // (Not before the group stands in his room with the room cleared, unless the eggs are under way.)
    if (!controller && !razor->IsInCombat() && !(run.roomLogged == run.boss + 1 && run.roomDone))
        return;
    if (!controller)
    {
        GameObject* orb = razor->FindNearestGameObject(OrbEntry, 150.0f);
        if (!orb)
            return;
        // Grethok and his guards at the orb first: the leader turns to the nearest that is in no fight.
        std::list<Creature*> guards;
        orb->GetCreatureListWithEntryInGrid(guards, std::vector<uint32>{ 12557, 14456 }, 25.0f);
        for (Creature* guard : guards)
            if (guard->IsAlive() && !guard->IsInCombat() && leader->IsValidAttackTarget(guard))
            {
                if (!leader->IsInCombat() && now >= run.razorStepAt)
                {
                    BotDriver::Attack(leader, guard, false);
                    run.razorStepAt = now + 2000;
                }
                return;
            }
        // Who uses it: a mage, rogue, warrior or priest of the raid without a dungeon role and not flagged as a tank,
        // alive, AI-driven, without a pet and without Mind Exhaustion; the one nearest to the orb.
        Group* raidGroup = leader->GetGroup();
        Player* user = nullptr;
        for (Player* member : group)
        {
            uint8 const cls = member->getClass();
            if (!member->IsAlive() || member->IsBeingTeleported() || !sGuildBotHolder.IsAIPlayed(member->GetGUID()) || member->GetPet() ||
                member->HasAura(Exhaustion) || (cls != CLASS_MAGE && cls != CLASS_ROGUE && cls != CLASS_WARRIOR && cls != CLASS_PRIEST) ||
                sRoleService.GetRole(member->GetGUID()) != DungeonRole::None ||
                (raidGroup && raidGroup->IsAssistant(member->GetGUID())))
                continue;
            if (!user || member->GetExactDist(orb) < user->GetExactDist(orb))
                user = member;
        }
        if (!user)
            return;
        BotDriver::PauseAI(user, 1500);
        if (user->GetExactDist(orb) > 3.0f)
        {
            if (now >= run.razorStepAt)
            {
                Position spot = orb->GetPosition();
                orb->MovePositionToFirstCollision(spot, 1.5f, orb->GetAngle(user) - orb->GetOrientation());
                user->GetMotionMaster()->MovePoint(0, spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ(), FORCED_MOVEMENT_RUN);
                run.razorStepAt = now + 1000;
            }
            return;
        }
        if (now < run.razorUseAt)
            return;
        user->StopMoving();
        user->AttackStop();
        orb->Use(user);
        run.razorUseAt = now + 3000;
        ++run.razorUses;
        LOG_INFO(ActivityLog, "activity: {} uses the Orb of Domination in {}: it takes Razorgore the Untamed over to break the eggs "
            "({}. time; {} eggs broken by him so far)", user->GetName(), plan.name, run.razorUses, run.razorEggs);
        return;
    }
    // Possessed: his possessor stands at the orb, he goes from egg to egg.
    BotDriver::PauseAI(controller, 1500);
    if (razor->HasUnitState(UNIT_STATE_CASTING) || now < run.razorStepAt)
        return;
    run.razorStepAt = now + 500;
    std::list<GameObject*> eggs;
    razor->GetGameObjectListWithEntryInGrid(eggs, EggEntry, 150.0f);
    GameObject* egg = nullptr;
    for (GameObject* candidate : eggs)
    {
        if (!candidate->isSpawned() || candidate->GetGoState() != GO_STATE_READY)
            continue;
        auto const tried = run.razorEggTried.find(candidate->GetGUID());
        if (tried != run.razorEggTried.end() && getMSTimeDiff(tried->second, now) < 15 * IN_MILLISECONDS)
            continue;
        if (!egg || razor->GetExactDist2d(candidate) < razor->GetExactDist2d(egg))
            egg = candidate;
    }
    if (!egg)
        return;
    if (razor->GetExactDist2d(egg) > 3.0f)
    {
        razor->GetMotionMaster()->MovePoint(0, egg->GetPositionX(), egg->GetPositionY(), egg->GetPositionZ(), FORCED_MOVEMENT_RUN);
        return;
    }
    razor->StopMoving();
    SpellCastResult const result = razor->CastSpell(egg, DestroyEgg, false);
    run.razorEggTried[egg->GetGUID()] = now;
    if (result == SPELL_CAST_OK)
    {
        if (++run.razorEggs % 5 == 0)
            LOG_INFO(ActivityLog, "activity: Razorgore the Untamed, under the Orb of Domination of {}, breaks his {}. egg in {}",
                controller->GetName(), run.razorEggs, plan.name);
    }
    else if (run.razorFailLogged != uint32(result) + 1)
    {
        run.razorFailLogged = uint32(result) + 1;
        LOG_INFO(ActivityLog, "activity: Razorgore the Untamed cannot cast Destroy Egg at the egg {:.0f} yd from him in {} (cast result {})",
            razor->GetExactDist(egg), plan.name, uint32(result));
    }
}

// Poisonous Blood (24320): the cloud of a dead Son of Hakkar casts it once a second on everybody within 8 yd; its
// effect (a script effect) is to give them Poisonous Blood (24321, 90 s), which the core does not do.
class spell_ga_poisonous_blood : public SpellScript
{
    PrepareSpellScript(spell_ga_poisonous_blood);

    void HandleScript(SpellEffIndex /*effIndex*/)
    {
        if (Player* target = GetHitPlayer())
            target->CastSpell(target, 24321, true);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_ga_poisonous_blood::HandleScript, EFFECT_0, SPELL_EFFECT_SCRIPT_EFFECT);
    }
};

// The last hit a player took inside a dungeon or raid: the death line names it (what killed a raid member is not in the
// fight's own numbers: "last fought" is the creature it fought, not what dealt the blow). A spell's direct damage is
// seen first by ModifySpellDamageTaken (its spell), then by OnDamage (its amount); melee and damage over time come by
// OnDamage alone. Map threads write, the world thread reads.
namespace
{
    struct LastHit
    {
        uint32 attacker = 0; // creature entry, 0 for a player or nothing
        uint32 spell = 0;    // 0: melee or damage over time
        uint32 damage = 0;
        uint32 at = 0;
    };

    std::mutex lastHitLock;
    std::unordered_map<ObjectGuid, LastHit> lastHits;
    std::unordered_map<ObjectGuid, std::pair<uint32, uint32>> pendingSpells; // spell, time

    bool TracksHitsOn(Unit const* victim)
    {
        return victim && victim->IsPlayer() && victim->FindMap() && victim->FindMap()->IsDungeon();
    }
}

class GuildAdventureLastHitScript : public UnitScript
{
public:
    GuildAdventureLastHitScript() : UnitScript("GuildAdventureLastHitScript", true,
        { UNITHOOK_ON_DAMAGE, UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN }) { }

    void ModifySpellDamageTaken(Unit* target, Unit* /*attacker*/, int32& /*damage*/, SpellInfo const* spellInfo) override
    {
        if (!spellInfo || !TracksHitsOn(target))
            return;
        std::lock_guard<std::mutex> guard(lastHitLock);
        pendingSpells[target->GetGUID()] = { spellInfo->Id, GameTime::GetGameTimeMS().count() };
    }

    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        if (!damage || !TracksHitsOn(victim))
            return;
        uint32 const now = GameTime::GetGameTimeMS().count();
        std::lock_guard<std::mutex> guard(lastHitLock);
        LastHit& hit = lastHits[victim->GetGUID()];
        hit.attacker = attacker && attacker->IsCreature() ? attacker->GetEntry() : 0;
        hit.damage = damage;
        hit.at = now;
        hit.spell = 0;
        if (auto const itr = pendingSpells.find(victim->GetGUID()); itr != pendingSpells.end())
        {
            if (now - itr->second.second < 200)
                hit.spell = itr->second.first;
            pendingSpells.erase(itr);
        }
    }
};

std::string OrderService::DescribeLastHit(Player const* player)
{
    LastHit hit;
    {
        std::lock_guard<std::mutex> guard(lastHitLock);
        auto const itr = lastHits.find(player->GetGUID());
        if (itr == lastHits.end())
            return {};
        hit = itr->second;
        lastHits.erase(itr);
    }
    uint32 const ago = GameTime::GetGameTimeMS().count() - hit.at;
    if (ago > 10 * IN_MILLISECONDS)
        return {};
    CreatureTemplate const* by = hit.attacker ? sObjectMgr->GetCreatureTemplate(hit.attacker) : nullptr;
    SpellInfo const* spell = hit.spell ? sSpellMgr->GetSpellInfo(hit.spell) : nullptr;
    return Acore::StringFormat(", the last hit {} from {} ({})", hit.damage, by ? by->Name : std::string("a player or nothing"),
        spell ? Acore::StringFormat("{} {}", spell->SpellName[0], spell->Id) : std::string("melee or over time"));
}

void AddGuildAdventureRaidScripts()
{
    RegisterSpellScript(spell_ga_poisonous_blood);
    new GuildAdventureLastHitScript();
}

bool OrderService::LeadsRaidRun(Player const* player) const
{
    if (!player)
        return false;
    AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfMember(player->GetGUID());
    GuildBot const* bot = guild ? guild->FindBot(player->GetGUID()) : nullptr;
    if (!bot || bot->order.type != OrderType::Dungeon)
        return false;
    DungeonPlan const* plan = sDungeons.GetPlan(bot->order.zoneId);
    // (Also a dungeon whose group is bigger than a party: Blackrock Spire takes 15, and a party holds 5.)
    return plan && (plan->raid || sSquadService.GetMembers(bot->guid).size() + 1 > MAXGROUPSIZE);
}

bool OrderService::KeepRaidGroup(AdventureGuild& guild, Player* leader, std::vector<Player*> const& group, bool force)
{
    constexpr uint32 LookMs = 5 * IN_MILLISECONDS;
    BotRuntime* rt = FindRuntime(leader->GetGUID());
    Group* party = leader->GetGroup();
    if (!party || !party->IsLeader(leader->GetGUID()) || party->isBGGroup() || party->isBFGroup() || party->isLFGGroup())
        return false;
    if (!party->isRaidGroup())
    {
        party->ConvertToRaid();
        LOG_INFO(ActivityLog, "activity: {}'s party is a raid group now ({} members)", leader->GetName(), party->GetMembersCount());
    }
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    if (rt && !force && rt->dungeon.raidLookAt && getMSTimeDiff(rt->dungeon.raidLookAt, now) < LookMs)
        return true;
    if (rt)
        rt->dungeon.raidLookAt = now;
    // The tanks of the group by their dungeon role (else their talents): the first one (the leader when it tanks) the main
    // tank, the others assistants; nobody else carries either flag.
    std::vector<ObjectGuid> tanks;
    for (Player* member : group)
        if (GuildBot const* memberBot = guild.FindMember(member->GetGUID()))
            if (sRoleService.EffectiveRole(*memberBot) == DungeonRole::Tank && party->IsMember(member->GetGUID()))
            {
                if (member == leader)
                    tanks.insert(tanks.begin(), member->GetGUID());
                else
                    tanks.push_back(member->GetGUID());
            }
    // (The tank that took a boss over in a tank swap stays the main tank: StepRaidTankSwap.)
    if (rt && !rt->dungeon.raidMainTank.IsEmpty())
        if (auto const over = std::find(tanks.begin(), tanks.end(), rt->dungeon.raidMainTank); over != tanks.end())
            std::rotate(tanks.begin(), over, over + 1);
    for (auto const& slot : party->GetMemberSlots())
    {
        auto const at = std::find(tanks.begin(), tanks.end(), slot.guid);
        bool const main = at == tanks.begin() && at != tanks.end();
        bool const assist = at != tanks.end() && !main;
        if (bool(slot.flags & MEMBER_FLAG_MAINTANK) != main)
            party->SetGroupMemberFlag(slot.guid, main, MEMBER_FLAG_MAINTANK);
        if (party->IsAssistant(slot.guid) != assist && slot.guid != leader->GetGUID())
            party->SetGroupMemberFlag(slot.guid, assist, MEMBER_FLAG_ASSISTANT);
    }
    // The reagents of the raid buffs out of the guild storage (Prayer of Fortitude, Gift of the Wild, Arcane Brilliance,
    // the greater blessings): mod-playerbots casts the raid-wide buff only when the reagent is in the bags
    // (GenericBuffUtils HasRequiredReagents), else the single one on each of 40 characters. The priests of the first
    // Molten Core raid went to 0 % mana buffing during the rests, 2026-11-21.
    struct Reagent { uint8 classId; uint32 item; uint32 level; };
    static Reagent const Reagents[] = {
        { CLASS_PRIEST, 17029, 56 },  // Sacred Candle (Prayer of Fortitude, Spirit, Shadow Protection, the last ranks)
        { CLASS_PRIEST, 17028, 48 },  // Holy Candle (their first ranks)
        { CLASS_DRUID, 17026, 60 },   // Wild Thornroot (Gift of the Wild, rank 2)
        { CLASS_DRUID, 17021, 50 },   // Wild Berries (rank 1)
        { CLASS_MAGE, 17020, 56 },    // Arcane Powder (Arcane Brilliance)
        { CLASS_PALADIN, 21177, 52 }, // Symbol of Kings (the greater blessings)
    };
    constexpr uint32 ReagentStack = 20;
    if (uint32 const owner = sStorageService.GetOwnerOf(leader))
        for (Player* member : group)
            for (Reagent const& reagent : Reagents)
            {
                // (The reagent of the highest rank its level has: the first of its class in the list it is level for.)
                if (member->getClass() != reagent.classId || member->GetLevel() < reagent.level)
                    continue;
                uint32 const carried = member->GetItemCount(reagent.item, false);
                uint32 const stored = carried >= ReagentStack / 4 ? 0 : sStorageService.GetAmount(owner, reagent.item, 0);
                uint32 const want = std::min(stored, ReagentStack - std::min(carried, ReagentStack));
                std::string error;
                if (want && sStorageService.Withdraw(member, reagent.item, 0, want, error))
                {
                    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(reagent.item);
                    LOG_INFO(ActivityLog, "activity: {} took {} x {} out of the guild storage for the raid buffs ({} left there)",
                        member->GetName(), want, proto ? proto->Name1 : "?", sStorageService.GetAmount(owner, reagent.item, 0));
                }
                break;
            }
    return true;
}

// The group into its leader's instance (2026-11-14). The start sends the leader in alone (RunDungeon); here, every tick
// of the leader inside, each member that is still to go in is bound to the leader's instance and taken in beside it,
// and a member that stands in another instance of the map (from before this rule, or after a way in the core decided
// for it) is taken out to the entrance and then in. Not a hand-played character in a fight.
void OrderService::StepDungeonEnter(GuildBot const& bot, Player* leader, BotRuntime::DungeonRun& run, DungeonPlan const& plan,
    std::vector<Player*> const& group, Player* guest)
{
    constexpr uint32 EnterLookMs = 2 * IN_MILLISECONDS;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    if (now < run.nextEnterLook || !leader->IsInWorld() || leader->IsBeingTeleported() || leader->GetMapId() != plan.mapId ||
        !leader->GetInstanceId())
        return;
    run.nextEnterLook = now + EnterLookMs;
    std::vector<Player*> movers = group;
    if (guest && std::find(movers.begin(), movers.end(), guest) == movers.end())
        movers.push_back(guest);
    // The party: the core takes a character into the instance of its party's leader (else into a new one, whatever it
    // is bound to: InstanceSaveMgr::PlayerGetDestinationInstanceId), and lets nobody in who is not in the party of those
    // inside (InstanceMap::CannotEnter). So the run's leader leads the party, and the group's characters are in it; a
    // character of an old party that is no part of this group makes room. (The healer of a Razorfen Downs group stood
    // outside: its leader's party was full with a bot of an earlier group, and that bot led it.)
    std::vector<ObjectGuid> keep;
    for (Player* member : movers)
        keep.push_back(member->GetGUID());
    if (Player* anyGuest = DungeonGuest(bot.guid))
        keep.push_back(anyGuest->GetGUID());
    auto makeRoom = [leader, &keep]()
    {
        Group* party = leader->GetGroup();
        if (!party || !party->IsFull())
            return;
        std::vector<ObjectGuid> others;
        for (auto const& slot : party->GetMemberSlots())
            if (slot.guid != leader->GetGUID() && std::find(keep.begin(), keep.end(), slot.guid) == keep.end())
                others.push_back(slot.guid);
        if (!others.empty())
            party->RemoveMember(others.front());
    };
    for (Player* member : movers)
    {
        if (member == leader || !member->IsInWorld() || member->IsBeingTeleported() || member->IsDuringRemoveFromWorld())
            continue;
        ObjectGuid const guid = member->GetGUID();
        auto const pending = std::find(run.enterPending.begin(), run.enterPending.end(), guid);
        if (SameInstance(member, leader))
        {
            if (pending != run.enterPending.end())
                run.enterPending.erase(pending);
            continue;
        }
        // (Not the hand-played character: the run puts it into the bots' party itself, RunDungeon.)
        bool const ownCharacter = member == guest && (!BotDriver::GetAI(member) || !sGuildBotHolder.IsAIPlayed(guid));
        if (!ownCharacter && (!leader->GetGroup() || member->GetGroup() != leader->GetGroup()))
        {
            // (A raid, or a group bigger than a party: the party is a raid group, which holds the whole group.)
            if ((plan.raid || movers.size() > MAXGROUPSIZE) && leader->GetGroup() && !leader->GetGroup()->isRaidGroup() && leader->GetGroup()->IsLeader(leader->GetGUID()))
                leader->GetGroup()->ConvertToRaid();
            else
                makeRoom();
            if (!BotDriver::JoinGroupOf(member, leader))
                continue;
        }
        if (Group* party = leader->GetGroup(); party && party->GetLeaderGUID() != leader->GetGUID() && party->IsMember(leader->GetGUID()))
            party->ChangeLeader(leader->GetGUID());
        bool const handPlayed = ownCharacter;
        if (member->GetMapId() == plan.mapId)
        {
            // In another instance of this map: a teleport on the same map stays in that instance, so out first.
            if (handPlayed && member->IsInCombat())
                continue;
            if (!BindToLeaderInstance(member, leader))
                continue;
            if (std::find(run.strayLogged.begin(), run.strayLogged.end(), guid) == run.strayLogged.end())
            {
                run.strayLogged.push_back(guid);
                LOG_INFO(ActivityLog, "activity: {} stands in another instance of {} than its group (instance {}, {}'s is {}): it is taken "
                    "out to the entrance and in to its group", member->GetName(), plan.name, member->GetInstanceId(), bot.name,
                    leader->GetInstanceId());
            }
            if (!member->IsAlive())
            {
                member->ResurrectPlayer(0.5f);
                member->SpawnCorpseBones();
            }
            if (pending == run.enterPending.end())
                run.enterPending.push_back(guid);
            if (handPlayed)
                TellGuest(member, Acore::StringFormat("You stood in another instance of {} than your group: you are taken to it.", plan.name));
            member->TeleportTo(plan.outMap, plan.outX, plan.outY, plan.outZ, member->GetOrientation());
            continue;
        }
        // Outside: a bot of the group always (also after a restart of the server, which forgets who was to go in, or
        // when it died and came back outside), the hand-played character only when it is to go in (the start, or
        // one taken out of a stray instance) and not in a fight.
        if ((handPlayed && pending == run.enterPending.end()) || (handPlayed && member->IsInCombat()) || member->IsInFlight())
            continue;
        if (!BindToLeaderInstance(member, leader))
        {
            if (pending != run.enterPending.end())
            {
                run.enterPending.erase(pending);
                LOG_INFO(ActivityLog, "activity: {} cannot follow its group into {}: it is locked to another instance of it",
                    member->GetName(), plan.name);
            }
            continue;
        }
        if (pending == run.enterPending.end() &&
            std::find(run.strayLogged.begin(), run.strayLogged.end(), guid) == run.strayLogged.end())
        {
            run.strayLogged.push_back(guid);
            LOG_INFO(ActivityLog, "activity: {} is outside {} while its group is inside{}: it is taken in to it", member->GetName(),
                plan.name, member->IsAlive() ? "" : " (dead)");
        }
        if (!member->IsAlive())
        {
            member->ResurrectPlayer(0.5f);
            member->SpawnCorpseBones();
        }
        Position const spot = leader->GetNearPosition(2.0f, float(M_PI));
        member->TeleportTo(plan.mapId, spot.GetPositionX(), spot.GetPositionY(), leader->GetPositionZ(), leader->GetOrientation());
        if (BotRuntime* other = FindRuntime(guid))
            other->hasLastPosition = false;
    }
}

// A member under attack (the owner, 2026-11-14, from his Uldaman run: [quote left out]). The log of that run: the mage and the healer fought three Stonevault
// Oracles 55 yd behind the tank, the mage died, and the tank stood "waiting for its squad: Magefrost is dead" while the
// healer fought on alone for three minutes. Now every second of a fight: the creatures that attack a member who is not
// the tank are the group's first targets. The tank goes to them at once, the healer's first, then the member with the
// least health, also out of its sight (its own movement walks there) and away from the creatures on itself (they
// follow it); the damage dealers turn to them too (not while they hold back for the tank: they come when the hold is
// over). A damage dealer already on such a creature stays on it, unless the healer is attacked and it is not on the
// healer's.
namespace
{
    // The first of these spells the character knows and can cast at `target` now (in reach, ready, the power for it):
    // true when it was cast.
    bool CastFirstOf(Player* caster, Unit* target, std::initializer_list<char const*> names)
    {
        for (char const* name : names)
        {
            uint32 const spell = BotDriver::SpellIdByName(caster, name);
            SpellInfo const* info = spell ? sSpellMgr->GetSpellInfo(spell) : nullptr;
            if (!info || caster->HasSpellCooldown(spell))
                continue;
            if (target != caster && (!caster->IsWithinDistInMap(target, info->GetMaxRange(false)) || !caster->IsWithinLOSInMap(target)))
                continue;
            if (caster->CastSpell(target, spell, false) == SPELL_CAST_OK)
                return true;
        }
        return false;
    }
}

void OrderService::StepDungeonRescue(AdventureGuild& guild, GuildBot const& bot, Player* player, BotRuntime::DungeonRun& run,
    std::vector<Player*> const& group, Player* tank, bool pulling, bool held, uint32 now)
{
    constexpr float TankHelpYards = 100.0f;   // a tank goes this far for a member
    constexpr float DamageHelpYards = 60.0f;  // ... a damage dealer
    // (Not while a raid mechanic holds the raid where it stands: Ossirian with his Strength on; nor at the Twin Emperors,
    // where each half fights at its own throne: the tank of Vek'lor's half ran across the room to a hunter Vek'nilash
    // attacked, 2026-11-24.)
    if ((run.raidHoldUntil && now < run.raidHoldUntil) || (!run.twinSide.empty() && run.twinPulledAt) || !run.cthunSlot.empty() ||
        (run.twinBugsSince && !run.twinBugsDone) || run.twinGuards)
        return;
    Player* healer = nullptr;
    for (Player* member : group)
        if (GuildBot const* memberBot = guild.FindMember(member->GetGUID()))
            if (!healer && member->IsAlive() && sRoleService.EffectiveRole(*memberBot) == DungeonRole::Healer)
                healer = member;
    // (A group without a tank: the bot that leads goes.)
    Player* const protector = tank ? tank : player;
    // A tank that cannot act (dead, or crowd-controlled: a polymorph, a stun, a fear): another member with a taunt takes
    // what attacks the healer off it. Lower Blackrock Spire, 2026-11-26: Shadow Hunter Vosh'gajin hexed the death knight
    // tank 8 s into his fight (Hex, magic, 10 s; nobody of the group dispels magic), he and his two shadow priests went
    // for the druid healer, and it died in 11 s beside the frog while the warrior of the group, Mocking Blow in its
    // stance, fought on the boss.
    // (What attacks the healer first, the boss first; else the boss on another member: on the j5 instance the hexed
    // tank's Vosh'gajin went for the warlock, which lived with a potion at 32 %.)
    if (now >= run.nextTaunt &&
        (!protector->IsAlive() || protector->HasUnitState(UNIT_STATE_CONFUSED | UNIT_STATE_STUNNED | UNIT_STATE_FLEEING)))
    {
        Creature* off = nullptr;
        if (healer)
            for (Unit* unit : healer->getAttackers())
                if (Creature* creature = unit ? unit->ToCreature() : nullptr; creature && creature->IsAlive() && creature->GetVictim() == healer &&
                    (!off || (creature->IsDungeonBoss() && !off->IsDungeonBoss())))
                    off = creature;
        if (!off)
            for (Player* member : group)
                if (member != protector && member->IsAlive())
                    for (Unit* unit : member->getAttackers())
                        if (Creature* creature = unit ? unit->ToCreature() : nullptr; !off && creature && creature->IsAlive() &&
                            creature->IsDungeonBoss() && creature->GetVictim() == member)
                            off = creature;
        Unit* const victim = off ? off->GetVictim() : nullptr;
        if (off && victim)
            for (Player* member : group)
            {
                if (member == victim || member == healer || member == protector || !member->IsAlive() ||
                    member->HasUnitState(UNIT_STATE_CONFUSED | UNIT_STATE_STUNNED | UNIT_STATE_FLEEING))
                    continue;
                if (CastFirstOf(member, off, { "mocking blow", "dark command", "hand of reckoning", "taunt", "growl" }))
                {
                    run.nextTaunt = now + 2 * IN_MILLISECONDS;
                    ++run.metrics.taunts;
                    DungeonPlan const* plan = sDungeons.GetPlan(run.id);
                    LOG_INFO(ActivityLog, "activity: {} takes {} off {}{} in {}: its tank {} {}", member->GetName(), off->GetName(),
                        victim->GetName(), victim == healer ? " (its healer)" : "", plan ? plan->name : "a dungeon", protector->GetName(),
                        protector->IsAlive() ? "cannot act (crowd-controlled)" : "is dead");
                    break;
                }
            }
    }
    if (!protector->IsAlive())
        return;
    // A tank that holds a boss stays with it: it goes 30 yd at most, a member farther off runs to it (Dire Maul West,
    // 2026-11-21: the tank left Tendris Warpwood and his treants for an Ironbark Protector 93 yd away on the healer, and
    // the group was wiped out).
    bool holdsBoss = false;
    if (Unit const* victim = protector->GetVictim(); victim && victim->ToCreature() && victim->ToCreature()->IsDungeonBoss())
        holdsBoss = true;
    for (Unit* unit : protector->getAttackers())
        holdsBoss = holdsBoss || (unit && unit->ToCreature() && unit->ToCreature()->IsDungeonBoss() && unit->IsAlive());
    float const tankReach = holdsBoss ? 30.0f : TankHelpYards;
    struct Threat
    {
        Creature* creature;
        Player* victim;
    };
    std::vector<Threat> threats;
    for (Player* member : group)
    {
        if (!member->IsAlive() || member == protector)
            continue;
        for (Unit* unit : member->getAttackers())
            if (Creature* creature = unit ? unit->ToCreature() : nullptr; creature && creature->IsAlive() && creature->GetVictim() == member &&
                creature->IsWithinDistInMap(protector, TankHelpYards) && !IsTributeSpared(run, creature))
                threats.push_back({ creature, member });
    }
    if (threats.empty())
        return;
    std::sort(threats.begin(), threats.end(), [healer, protector](Threat const& a, Threat const& b)
    {
        bool const aHealer = a.victim == healer;
        bool const bHealer = b.victim == healer;
        if (aHealer != bHealer)
            return aHealer;
        if (a.victim != b.victim)
            return a.victim->GetHealthPct() < b.victim->GetHealthPct();
        return protector->GetDistance(a.creature) < protector->GetDistance(b.creature);
    });
    bool const healerAttacked = threats.front().victim == healer;
    auto threatOf = [&threats](Unit const* unit) -> Threat const*
    {
        for (Threat const& threat : threats)
            if (threat.creature == unit)
                return &threat;
        return nullptr;
    };
    // Per member helped: the creature, whether the tank went, how many damage dealers turned to it.
    struct Help
    {
        Creature* creature = nullptr;
        bool tankWent = false;
        bool outOfSight = false;
        bool taunted = false;
        bool cameToTank = false;
        uint32 turned = 0;
    };
    std::unordered_map<Player*, Help> helped;

    // The tank (not in the middle of a careful pull), package P5: two or more creatures on others beside it, its area
    // threat; a creature on another member within its taunt's reach, the taunt; it walks there only for its healer or a
    // creature within 15 yd (the others come to it, below).
    constexpr float AreaThreatYards = 8.0f;
    constexpr float WalkToYards = 15.0f;
    if (!pulling)
    {
        uint32 beside = 0;
        for (Threat const& threat : threats)
            beside += threat.creature->IsWithinDistInMap(protector, AreaThreatYards) ? 1 : 0;
        if (beside >= 2 && now >= run.nextAreaThreat &&
            CastFirstOf(protector, protector, { "thunder clap", "consecration", "swipe (bear)", "death and decay", "challenging shout",
                "challenging roar" }))
        {
            run.nextAreaThreat = now + 6 * IN_MILLISECONDS;
            ++run.metrics.areaThreats;
        }
        else if (now >= run.nextTaunt)
        {
            // The taunt goes to the first creature on another member it reaches (in range and sight), the healer's
            // first, also when it is the tank's target already: a creature the tank attacks may still attack the healer.
            // Lower Blackrock Spire, 2026-11-26: the death knight tank had a Firebrand Invoker that cast at the druid
            // healer as its target and so taunted nothing, two Firebrand Grunts came onto the healer beside it, and the
            // healer died in 11 s while the tank fought the four on itself.
            for (Threat const& threat : threats)
            {
                if (threat.creature->GetVictim() == protector)
                    continue;
                if (CastFirstOf(protector, threat.creature, { "taunt", "hand of reckoning", "growl", "dark command" }))
                {
                    run.nextTaunt = now + 1500;
                    ++run.metrics.taunts;
                    Help& help = helped[threat.victim];
                    help.creature = threat.creature;
                    help.taunted = true;
                    break;
                }
            }
        }
        // The tank walks to one on its healer, to one within 15 yd, and to one that stays off: 3 s on a member and
        // still more than 15 yd from the tank (a caster shoots from where it stands; a member more than 40 yd away does
        // not run to the tank). Dire Maul East, 2026-11-25: a Satyr patrol caught the hunter, the mage and the
        // paladin 60 yd behind the tank, its Felsworn cast from there, and the tank stood "in a fight against 0 foes"
        // for 25 s while the mage died with three on it.
        constexpr uint32 StaysOffMs = 3 * IN_MILLISECONDS;
        std::erase_if(run.offTankSince, [&](auto const& entry)
        {
            for (Threat const& threat : threats)
                if (threat.creature->GetGUID() == entry.first)
                    return threat.creature->IsWithinDistInMap(protector, WalkToYards);
            return true;
        });
        for (Threat const& threat : threats)
            if (!threat.creature->IsWithinDistInMap(protector, WalkToYards))
                run.offTankSince.emplace(threat.creature->GetGUID(), now);
        Threat const* go = nullptr;
        bool staysOff = false;
        for (Threat const& threat : threats)
        {
            if (!protector->IsWithinDistInMap(threat.creature, tankReach))
                continue;
            auto const since = run.offTankSince.find(threat.creature->GetGUID());
            bool const off = since != run.offTankSince.end() && getMSTimeDiff(since->second, now) >= StaysOffMs;
            if (threat.victim == healer || off || protector->IsWithinDistInMap(threat.creature, WalkToYards))
            {
                go = &threat;
                staysOff = off && threat.victim != healer;
                break;
            }
        }
        Threat const* current = threatOf(protector->GetVictim());
        bool const better = go && (!current || (go->victim == healer && current->victim != healer) ||
            (staysOff && !current->creature->IsWithinDistInMap(protector, WalkToYards)));
        if (better && protector->GetVictim() != go->creature)
        {
            bool const inSight = protector->IsWithinLOSInMap(go->creature);
            if (BotDriver::Attack(protector, go->creature, false))
            {
                Help& help = helped[go->victim];
                help.creature = go->creature;
                help.tankWent = true;
                help.outOfSight = !inSight;
            }
        }
    }

    // Aggro means: to the tank (package P5). A healer or a ranged member with a creature on it runs to a point right in
    // front of the tank, where the tank takes the creature; a melee member only when its creature is not the group's
    // target (the skull).
    constexpr float FrontYards = 2.5f;
    constexpr uint32 ToTankEveryMs = 4 * IN_MILLISECONDS;
    if (!pulling && tank && tank->IsAlive())
        for (Threat const& threat : threats)
        {
            Player* member = threat.victim;
            if (member == tank || !sGuildBotHolder.IsAIPlayed(member->GetGUID()) || member->IsWithinDistInMap(tank, 6.0f) ||
                !member->IsWithinDistInMap(tank, 40.0f))
                continue;
            bool const isHealer = member == healer;
            bool const melee = !isHealer && BotDriver::FightsInMelee(member);
            if ((melee && threat.creature->GetGUID() == run.skull) || IsWhirler(threat.creature))
                continue;
            // Not into the fire round the tank: a creature whose pulse burns all round it, or a tank that stands among
            // such creatures (the pulsers' step keeps the member out of their reach).
            if (PulserOf(threat.creature) || !PulsersNear(tank, 15.0f).empty())
                continue;
            uint32& at = run.toTankAt[member->GetGUID()];
            if (at && getMSTimeDiff(at, now) < ToTankEveryMs)
                continue;
            at = now;
            float const o = tank->GetOrientation();
            float x = tank->GetPositionX() + std::cos(o) * FrontYards;
            float y = tank->GetPositionY() + std::sin(o) * FrontYards;
            float const z = tank->GetPositionZ();
            if (!tank->IsWithinLOS(x, y, z + 1.5f))
            {
                x = tank->GetPositionX();
                y = tank->GetPositionY();
            }
            uint32 const runMs = std::min<uint32>(3500, uint32(member->GetDistance(x, y, z) / 7.0f * 1000.0f) + 300);
            BotDriver::PauseAI(member, runMs);
            member->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_RUN);
            ++run.metrics.toTank;
            Help& help = helped[member];
            if (!help.creature)
                help.creature = threat.creature;
            help.cameToTank = true;
        }

    // The damage dealers: to the creatures on the healer (the owner's DP6); the rest of the time they follow the skull, the
    // group's one target (package P5).
    if (!held && healerAttacked)
        for (Player* member : group)
        {
            GuildBot const* memberBot = guild.FindMember(member->GetGUID());
            if (!memberBot || member == protector || member == healer || !member->IsAlive())
                continue;
            DungeonRole const role = sRoleService.EffectiveRole(*memberBot);
            if (role == DungeonRole::Tank || role == DungeonRole::Healer)
                continue;
            Threat const* current = threatOf(member->GetVictim());
            if (current && (current->victim == healer || !healerAttacked))
                continue;
            Threat const* pick = nullptr;
            bool const meleeMember = BotDriver::FightsInMelee(member);
            for (Threat const& threat : threats)
                if (threat.victim != member && member->IsWithinDistInMap(threat.creature, DamageHelpYards) &&
                    !(meleeMember && PulserOf(threat.creature)))
                {
                    pick = &threat;
                    break;
                }
            if (!pick || member->GetVictim() == pick->creature)
                continue;
            // A ranged character shoots from where it can see it; out of its sight it walks there first when it has
            // no fight of its own.
            bool const melee = BotDriver::FightsInMelee(member);
            bool const inSight = member->IsWithinLOSInMap(pick->creature);
            bool went = false;
            if (melee || inSight)
                went = BotDriver::Attack(member, pick->creature, false);
            else if (!member->IsInCombat())
            {
                DungeonGoTo(member, member->GetMapId(), pick->victim->GetPositionX(), pick->victim->GetPositionY(),
                    pick->victim->GetPositionZ());
                went = true;
            }
            if (went)
            {
                Help& help = helped[pick->victim];
                if (!help.creature)
                    help.creature = pick->creature;
                ++help.turned;
            }
        }

    for (auto const& [victim, help] : helped)
    {
        if (run.rescueLogged.count(victim->GetGUID()))
            continue;
        run.rescueLogged[victim->GetGUID()] = now;
        ++run.rescues;
        LOG_INFO(ActivityLog, "activity: {}'s group goes to help {}{} ({}% health) against {} (level {}, {:.0f} yd from {}): {}{}{}",
            bot.name, victim->GetName(), victim == healer ? " (its healer)" : "", uint32(victim->GetHealthPct()), help.creature->GetName(),
            uint32(help.creature->GetLevel()), protector->GetDistance(help.creature), protector->GetName(),
            help.tankWent ? Acore::StringFormat("{} {} goes to it{}", tank ? "the tank" : "its leader", protector->GetName(),
                help.outOfSight ? " (out of its sight: it walks there)" : "") : help.taunted ?
                Acore::StringFormat("{} {} taunts it", tank ? "the tank" : "its leader", protector->GetName()) : std::string(),
            (help.tankWent || help.taunted) && (help.turned || help.cameToTank) ? ", " : "",
            help.turned ? Acore::StringFormat("{} damage dealer{} turned to it", help.turned, help.turned == 1 ? "" : "s") :
                help.cameToTank ? Acore::StringFormat("{} runs to the tank", victim->GetName()) : std::string());
    }
}

namespace
{
    uint32 Percent(uint32 part, uint32 whole)
    {
        return whole ? (part * 100 + whole / 2) / whole : 0;
    }
}

namespace
{
    // Something to drink for a dungeon member: in its bags, in the guild storage, or with a mage of its group.
    bool HasDrinkAnywhere(Player* player)
    {
        constexpr uint32 DrinkCategory = 59;
        auto drink = [player](Item const* item)
        {
            ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
            return proto && proto->Spells[0].SpellCategory == DrinkCategory && proto->RequiredLevel <= player->GetLevel();
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (drink(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot)))
                return true;
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag const* bag = player->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    if (drink(bag->GetItemByPos(uint8(slot))))
                        return true;
        if (StorageSupply::Has(player, StorageSupply::Kind::Drink))
            return true;
        if (Group* group = player->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource(); member && member != player && member->getClass() == CLASS_MAGE &&
                    member->IsInWorld() && member->IsAlive())
                    return true;
        return false;
    }

    // In a fight that is a fight: somebody attacks it or it has a living foe (a combat flag that a creature which ran
    // off left behind does not hold a gate).
    bool InRealFight(Player* player)
    {
        if (!player->IsInCombat())
            return false;
        // (A creature on it from far off is no fight of it: Blackrock Depths, 2026-11-20, a Blazing Fireguard 72 yd away
        // and 22 yd higher, which could not get to the hunter it was "on", held the group at Ambassador Flamelash for
        // ten minutes.)
        for (Unit const* attacker : player->getAttackers())
            if (attacker->IsAlive() && player->IsWithinDistInMap(attacker, 40.0f))
                return true;
        Unit const* victim = player->GetVictim();
        return victim && victim->IsAlive() && player->IsWithinDistInMap(victim, 40.0f);
    }
}

DungeonBoss const& OrderService::RunBoss(BotRuntime::DungeonRun const& run, DungeonPlan const& plan, std::size_t at)
{
    return plan.bosses[run.order.empty() ? at : run.order[at]];
}

std::size_t OrderService::RunBossCount(BotRuntime::DungeonRun const& run, DungeonPlan const& plan)
{
    return run.order.empty() ? plan.bosses.size() : run.order.size();
}

std::vector<uint8> OrderService::BossOrder(Player* leader, DungeonPlan const& plan)
{
    // Once per dungeon (the way does not change), from the leader inside: the path finder needs the instance's map.
    if (auto const known = _bossOrders.find(plan.id); known != _bossOrders.end() && known->second.size() == plan.bosses.size())
        return known->second;
    std::size_t const count = plan.bosses.size();
    std::vector<uint8> order;
    // Raids whose levels lie over each other keep the list's order, the order their encounters come in (Blackwing Lair:
    // Firemaw's hall lies 17 yd above the suppression room before Broodlord Lashlayer, and the way's order sent the
    // raid to him first, through the room's packs, 2026-11-22; the Temple of Ahn'Qiraj likewise goes down level by level).
    bool const listOrder = plan.raid && (plan.id == 50 || plan.id == 161);
    if (count < 2 || count > 255 || listOrder)
    {
        if (listOrder)
        {
            std::string listed;
            for (DungeonBoss const& boss : plan.bosses)
                listed += (listed.empty() ? "" : ", ") + boss.name;
            LOG_INFO(ActivityLog, "activity: the bosses of {} in the list's order (its levels lie over each other): {}", plan.name, listed);
            for (std::size_t i = 0; i < count; ++i)
                order.push_back(uint8(i));
            _bossOrders[plan.id] = order;
            return order;
        }
        for (std::size_t i = 0; i < count; ++i)
            order.push_back(uint8(i));
        return order;
    }
    // Chains: a stop that must come right before a boss (an event, a hand-listed boss; DungeonBoss::before) goes with it,
    // in the list's order; an event stop that comes after every boss (Mutanus: the Disciple walks once the Fanglords are
    // dead) stays at the end, and so does the boss that ends the dungeon.
    std::vector<std::vector<uint8>> chains;
    std::vector<int32> chainOf(count, -1);
    for (std::size_t i = 0; i < count; ++i)
    {
        if (chainOf[i] >= 0)
            continue;
        DungeonBoss const& boss = plan.bosses[i];
        if (boss.before)
            continue; // it joins the chain of the boss it comes before
        chainOf[i] = int32(chains.size());
        chains.push_back({ uint8(i) });
    }
    std::vector<uint8> tail; // stops that stay at the end, in the list's order
    for (std::size_t i = 0; i < count; ++i)
    {
        DungeonBoss const& boss = plan.bosses[i];
        if (!boss.before)
            continue;
        // The boss it comes before: the next one of that entry in the list.
        int32 target = -1;
        for (std::size_t j = i + 1; j < count && target < 0; ++j)
            if (plan.bosses[j].entry == boss.before && !plan.bosses[j].before)
                target = int32(j);
        if (target < 0 || chainOf[target] < 0)
        {
            tail.push_back(uint8(i));
            continue;
        }
        std::vector<uint8>& chain = chains[chainOf[target]];
        chain.insert(chain.end() - 1, uint8(i)); // before the boss, after the stops glued before it already
        chainOf[i] = chainOf[target];
    }
    // The chain of the boss that ends the dungeon, and event stops that come after every boss: last, as they are.
    std::vector<std::size_t> free;
    std::vector<std::size_t> end;
    for (std::size_t c = 0; c < chains.size(); ++c)
    {
        DungeonBoss const& head = plan.bosses[chains[c].back()];
        bool const atEnd = head.last || head.IsEvent();
        (atEnd ? end : free).push_back(c);
    }

    // The way between two points inside: along the whole navmesh (its drops and swims included, DungeonNav); without
    // one along the path finder's path; with none, three times the straight line. (Blackfathom Deeps, 2026-11-25: the
    // path finder's 148 polygons do not reach Ghamoo-ra from the entrance, and the group went to Gelihast first, came
    // back past the entrance after Old Serra'kis and swam to Ghamoo-ra and Lady Sarevess last; players take the turtle
    // in the first pool first.)
    std::string lengths;
    auto way = [&](float x, float y, float z, float toX, float toY, float toZ)
    {
        float navLength = 0.0f;
        if (sDungeonNav.FindWay(leader->GetMapId(), x, y, z, toX, toY, toZ, nullptr, &navLength, leader->GetMap()))
            return navLength;
        PathReport const report = ProbePath(leader, x, y, z, toX, toY, toZ);
        float const straight = std::sqrt((toX - x) * (toX - x) + (toY - y) * (toY - y) + (toZ - z) * (toZ - z));
        if (!report.valid || report.straightLine || (report.type & PATHFIND_NOPATH))
            return straight * 3.0f;
        float const left = std::sqrt((toX - report.endX) * (toX - report.endX) + (toY - report.endY) * (toY - report.endY) +
            (toZ - report.endZ) * (toZ - report.endZ));
        return report.length + 2.0f * left;
    };
    // The shortest order of the bosses from the entrance (Held-Karp over the ways between them, the way on to the boss
    // that ends the dungeon counted; one way may differ from its way back: drops lead down only). The nearest boss each
    // time is not enough: in Blackfathom Deeps it took Lorgus Jett before Gelihast, whose murloc cave lies on the way up
    // to him, and walked back through it (2934 yd against 2713).
    std::size_t const m = free.size();
    auto headOf = [&](std::size_t k) -> DungeonBoss const& { return plan.bosses[chains[free[k]].front()]; };
    auto tailOf = [&](std::size_t k) -> DungeonBoss const& { return plan.bosses[chains[free[k]].back()]; };
    std::vector<float> fromIn(m, 0.0f), toEnd(m, 0.0f);
    std::vector<std::vector<float>> between(m, std::vector<float>(m, 0.0f));
    DungeonBoss const* endBoss = end.empty() ? nullptr : &plan.bosses[chains[end.front()].front()];
    for (std::size_t k = 0; k < m; ++k)
    {
        DungeonBoss const& head = headOf(k);
        DungeonBoss const& tail = tailOf(k);
        fromIn[k] = way(plan.inX, plan.inY, plan.inZ, head.x, head.y, head.z);
        if (endBoss)
            toEnd[k] = way(tail.x, tail.y, tail.z, endBoss->x, endBoss->y, endBoss->z);
        for (std::size_t j = 0; j < m; ++j)
            if (j != k)
            {
                DungeonBoss const& next = headOf(j);
                between[k][j] = way(tail.x, tail.y, tail.z, next.x, next.y, next.z);
            }
    }
    std::vector<std::size_t> tour;
    if (m > 0 && m <= 14)
    {
        std::size_t const full = (std::size_t(1) << m) - 1;
        std::vector<std::vector<float>> best(full + 1, std::vector<float>(m, 1e30f));
        std::vector<std::vector<int8>> came(full + 1, std::vector<int8>(m, -1));
        for (std::size_t k = 0; k < m; ++k)
            best[std::size_t(1) << k][k] = fromIn[k];
        for (std::size_t set = 1; set <= full; ++set)
            for (std::size_t k = 0; k < m; ++k)
            {
                if (!(set & (std::size_t(1) << k)) || best[set][k] >= 1e29f)
                    continue;
                for (std::size_t j = 0; j < m; ++j)
                {
                    if (set & (std::size_t(1) << j))
                        continue;
                    std::size_t const next = set | (std::size_t(1) << j);
                    float const cost = best[set][k] + between[k][j];
                    if (cost < best[next][j])
                    {
                        best[next][j] = cost;
                        came[next][j] = int8(k);
                    }
                }
            }
        std::size_t last = 0;
        float lastCost = 1e30f;
        for (std::size_t k = 0; k < m; ++k)
            if (best[full][k] + toEnd[k] < lastCost)
            {
                lastCost = best[full][k] + toEnd[k];
                last = k;
            }
        std::size_t set = full;
        for (int k = int(last); k >= 0;)
        {
            tour.push_back(std::size_t(k));
            int const before = came[set][k];
            set &= ~(std::size_t(1) << k);
            k = before;
        }
        std::reverse(tour.begin(), tour.end());
    }
    else
    {
        // Many bosses: the nearest each time.
        std::vector<bool> taken(m, false);
        int at = -1;
        for (std::size_t step = 0; step < m; ++step)
        {
            std::size_t pick = 0;
            float pickWay = 1e30f;
            for (std::size_t k = 0; k < m; ++k)
            {
                if (taken[k])
                    continue;
                float const length = at < 0 ? fromIn[k] : between[std::size_t(at)][k];
                if (length < pickWay)
                {
                    pickWay = length;
                    pick = k;
                }
            }
            taken[pick] = true;
            tour.push_back(pick);
            at = int(pick);
        }
    }
    // A boss the way to the next one passes is taken on the way, as players do (the shortest order of Blackfathom Deeps
    // goes to Lady Sarevess first, past Ghamoo-ra's pool, and comes back for him: the way there is the way to Ghamoo-ra
    // and on). "Passes": it is nearer, and the way through it is at most a tenth and 20 yd longer than the way straight on.
    auto legOf = [&](int from, std::size_t to) { return from < 0 ? fromIn[to] : between[std::size_t(from)][to]; };
    for (std::size_t guard = 0, i = 0; i < tour.size() && guard < m * m + m; ++guard)
    {
        int const from = i == 0 ? -1 : int(tour[i - 1]);
        float const straightOn = legOf(from, tour[i]);
        std::size_t passed = tour.size();
        for (std::size_t j = i + 1; j < tour.size() && passed == tour.size(); ++j)
            if (legOf(from, tour[j]) < straightOn && legOf(from, tour[j]) + between[tour[j]][tour[i]] <= straightOn * 1.1f + 20.0f)
                passed = j;
        if (passed == tour.size())
        {
            ++i;
            continue;
        }
        std::size_t const onTheWay = tour[passed];
        tour.erase(tour.begin() + passed);
        tour.insert(tour.begin() + i, onTheWay);
    }
    float total = 0.0f;
    for (std::size_t i = 0; i < tour.size(); ++i)
    {
        std::vector<uint8> const& chain = chains[free[tour[i]]];
        order.insert(order.end(), chain.begin(), chain.end());
        float const leg = i == 0 ? fromIn[tour[i]] : between[tour[i - 1]][tour[i]];
        total += leg;
        lengths += Acore::StringFormat("{}{} ({:.0f} yd)", lengths.empty() ? "" : ", ", plan.bosses[chain.back()].name, leg);
    }
    for (std::size_t c : end)
        order.insert(order.end(), chains[c].begin(), chains[c].end());
    order.insert(order.end(), tail.begin(), tail.end());
    std::string listed;
    for (DungeonBoss const& boss : plan.bosses)
        listed += (listed.empty() ? "" : ", ") + boss.name;
    std::string ordered;
    for (uint8 index : order)
        ordered += (ordered.empty() ? "" : ", ") + plan.bosses[index].name;
    LOG_INFO(ActivityLog, "activity: the bosses of {} in the order of the way: {} ({}; {:.0f} yd from boss to boss before the last). "
        "The list's order was: {}", plan.name, ordered, lengths.empty() ? "no boss to order" : lengths, total, listed);
    _bossOrders[plan.id] = order;
    return order;
}

void OrderService::StepDungeonHealer(Player* leader, BotRuntime& rt)
{
    // the owner's E5: a healer spends no mana on damage in a dungeon, its wand is fine; it heals through when the fight is
    // heavy: the tank below 70 % and 15 % gone within 3 s, four or more creatures on the tank, a boss fight, or anybody
    // below 40 %. Then its strongest heal without a pause, no damage and no saving of mana.
    constexpr uint32 WindowMs = 3 * IN_MILLISECONDS;
    constexpr float WandYards = 28.0f;
    BotRuntime::DungeonRun& run = rt.dungeon;
    if (!run.id || run.phase != PhaseInside || !leader->GetMap()->IsDungeon())
        return;
    AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfMember(leader->GetGUID());
    if (!guild)
        return;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    Player* healer = nullptr;
    Player* tank = nullptr;
    std::vector<Player*> members;
    for (ObjectGuid const& guid : run.groupGuids)
    {
        Player* member = ObjectAccessor::FindConnectedPlayer(guid);
        if (!member || !member->IsInWorld() || !SameInstance(member, leader))
            continue;
        members.push_back(member);
        GuildBot const* memberBot = guild->FindMember(guid);
        if (!memberBot || !member->IsAlive())
            continue;
        DungeonRole const role = sRoleService.EffectiveRole(*memberBot);
        if (role == DungeonRole::Healer && !healer && sGuildBotHolder.IsAIPlayed(guid))
            healer = member;
        if (role == DungeonRole::Tank && !tank)
            tank = member;
    }
    if (Player* guest = DungeonGuest(leader->GetGUID()); guest && SameInstance(guest, leader))
        members.push_back(guest);
    if (!healer)
        return;
    // No damage spells in a dungeon (mod-playerbots' "healer dps"): looked at every step, not once per run (a level-up
    // inside the run resets the strategies by the talents and gives "healer dps" back: a priest cast Smite, Holy Fire
    // and Shadow Word: Pain for the rest of a Scarlet Monastery run after it, 2026-11-25).
    if (BotDriver::SetCombatStrategy(healer, "healer dps", false))
    {
        if (run.healerDpsOff == healer->GetGUID())
            LOG_INFO(ActivityLog, "activity: {} had \"healer dps\" back in {} (its strategies were reset): it is taken off again",
                healer->GetName(), leader->GetMap()->GetMapName());
        run.healerDpsOff = healer->GetGUID();
    }
    if (!tank)
        tank = leader;

    bool fighting = false;
    for (Player* member : members)
        fighting = fighting || (member->IsAlive() && member->IsInCombat());
    if (!fighting)
    {
        run.tankHealth.clear();
        if (run.pumping && run.saveManaOff == healer->GetGUID())
            BotDriver::SetCombatStrategy(healer, "save mana", true);
        run.pumping = false;
        run.pumpLogged = false;
        run.saveManaOff.Clear();
        return;
    }

    // The tank's health of the last seconds.
    run.tankHealth.emplace_back(now, tank->GetHealthPct());
    while (!run.tankHealth.empty() && getMSTimeDiff(run.tankHealth.front().first, now) > WindowMs)
        run.tankHealth.pop_front();
    float const tankNow = tank->GetHealthPct();
    float tankMax = tankNow;
    for (auto const& [at, pct] : run.tankHealth)
        tankMax = std::max(tankMax, pct);
    uint32 onTank = 0;
    for (Unit* attacker : tank->getAttackers())
        onTank += attacker && attacker->IsAlive() && attacker->IsCreature() ? 1 : 0;
    bool bossFight = false;
    if (DungeonPlan const* plan = sDungeons.GetPlan(run.id); plan && run.boss < RunBossCount(run, *plan))
        if (Creature const* boss = leader->FindNearestCreature(RunBoss(run, *plan, run.boss).entry, 60.0f, true))
            bossFight = boss->IsInCombat();
    Player* lowest = nullptr;
    for (Player* member : members)
        if (member->IsAlive() && (!lowest || member->GetHealthPct() < lowest->GetHealthPct()))
            lowest = member;
    std::string why;
    if (tank->IsAlive() && tankNow < 70.0f && tankMax - tankNow > 15.0f)
        why = Acore::StringFormat("{} lost {:.0f}% in 3 s", tank->GetName(), tankMax - tankNow);
    else if (onTank >= 4)
        why = Acore::StringFormat("{} creatures on {}", onTank, tank->GetName());
    else if (bossFight)
        why = "a boss fight";
    else if (lowest && lowest->GetHealthPct() < 40.0f)
        why = Acore::StringFormat("{} at {:.0f}%", lowest->GetName(), lowest->GetHealthPct());
    bool const pump = !why.empty();
    // (Every step while it heals through: a reset of its strategies gives "save mana" back.)
    if (pump && BotDriver::SetCombatStrategy(healer, "save mana", false))
        run.saveManaOff = healer->GetGUID();
    if (pump && !run.pumping)
    {
        if (!run.pumpLogged)
        {
            run.pumpLogged = true;
            ++run.metrics.pumps;
            LOG_INFO(ActivityLog, "activity: {} heals through in the fight ({}): its strongest heal without a pause, no saving of mana",
                healer->GetName(), why);
        }
    }
    run.pumping = pump;

    if (healer->IsNonMeleeSpellCast(false, false, true) || healer->isMoving() || healer->HasUnitState(UNIT_STATE_CASTING))
        return;
    // A healer that does not see its tank, or stands too far from it, walks to a place that does (package P6b): its
    // heals do not reach a tank it lost from sight, and mod-playerbots takes such a member off its list.
    constexpr float HealYards = 36.0f;
    constexpr float SeeFromYards = 10.0f;
    if (tank->IsAlive() && tank != healer && now >= run.nextHealerMove &&
        (!healer->IsWithinDistInMap(tank, HealYards) || !healer->IsWithinLOSInMap(tank)))
    {
        float const dx = healer->GetPositionX() - tank->GetPositionX();
        float const dy = healer->GetPositionY() - tank->GetPositionY();
        float const length = std::max(0.1f, std::sqrt(dx * dx + dy * dy));
        float x = tank->GetPositionX() + dx / length * SeeFromYards;
        float y = tank->GetPositionY() + dy / length * SeeFromYards;
        float z = tank->GetPositionZ();
        float const ground = tank->GetMap()->GetHeight(tank->GetPhaseMask(), x, y, z + 3.0f, true, 8.0f);
        if (ground <= INVALID_HEIGHT || std::fabs(ground - z) > 4.0f || !tank->IsWithinLOS(x, y, ground + 1.5f))
        {
            Position spot = tank->GetPosition();
            tank->MovePositionToFirstCollision(spot, 4.0f, float(M_PI));
            x = spot.GetPositionX();
            y = spot.GetPositionY();
            z = spot.GetPositionZ();
        }
        else
            z = ground;
        run.nextHealerMove = now + 3 * IN_MILLISECONDS;
        ++run.metrics.healerMoves;
        BotDriver::PauseAI(healer, std::min<uint32>(3000, uint32(healer->GetDistance(x, y, z) / 7.0f * 1000.0f) + 300));
        healer->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_RUN);
        return;
    }
    if (pump)
    {
        // The one who needs it most: the tank first while it falls, else the lowest.
        Player* target = tank->IsAlive() && tankNow < 70.0f ? tank : lowest;
        if (!target || target->GetHealthPct() >= 92.0f || !healer->IsWithinDistInMap(target, 40.0f) || !healer->IsWithinLOSInMap(target))
            return;
        if (healer->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
            healer->InterruptSpell(CURRENT_AUTOREPEAT_SPELL);
        CastFirstOf(healer, target, { "greater heal", "heal", "flash heal", "lesser heal", "holy light", "flash of light", "healing touch",
            "regrowth", "healing wave", "lesser healing wave" });
        return;
    }
    // Nobody needs a heal: the wand at the group's target (a healer without a wand does no damage).
    if (lowest && lowest->GetHealthPct() < 85.0f)
        return;
    Item const* wand = healer->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
    if (!wand || wand->GetTemplate()->Class != ITEM_CLASS_WEAPON || wand->GetTemplate()->SubClass != ITEM_SUBCLASS_WEAPON_WAND ||
        healer->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
        return;
    Unit* target = run.skull.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*healer, run.skull);
    if (!target)
        target = tank->GetVictim();
    if (!target || !target->IsAlive() || !healer->IsValidAttackTarget(target) || healer->GetExactDist(target) > WandYards ||
        !healer->IsWithinLOSInMap(target))
        return;
    healer->SetFacingToObject(target);
    constexpr uint32 ShootSpell = 5019;
    healer->CastSpell(target, ShootSpell, false);
}

void OrderService::StepDungeonConductor(Player* leader, BotRuntime& rt)
{
    // The conductor of a dungeon group (package P2), four times a second: the tank's way inside is recorded, and while
    // the tank walks out of any fight the members walk it behind the tank, each at its place (melee 3 yd, ranged 6 yd,
    // the healer 8 yd behind, side by side when several), instead of mod-playerbots' follow, which aims at a point
    // beside the leader and cuts corners through the next pack.
    constexpr float TrailStep = 1.5f;
    constexpr std::size_t TrailMax = 200;  // 300 yd: the rest looks that far back for a quiet place
    constexpr float JumpYards = 20.0f;
    constexpr float SlotTolerance = 2.5f;
    constexpr float SpreadYards = 15.0f;
    constexpr uint32 ResendMs = 1500;
    constexpr uint32 HoldMs = 700;
    BotRuntime::DungeonRun& run = rt.dungeon;
    if (!run.id || run.phase != PhaseInside || !leader->IsAlive() || !leader->GetMap()->IsDungeon() || leader->IsBeingTeleported())
        return;
    uint32 const now = std::max<uint32>(getMSTime(), 1);

    // The way: a point every 1.5 yd, the newest first; a teleport or hop starts it anew.
    BotRuntime::DungeonRun::TrailPoint const here { leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ() };
    auto dist = [](BotRuntime::DungeonRun::TrailPoint const& a, BotRuntime::DungeonRun::TrailPoint const& b)
    {
        return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
    };
    if (run.trail.empty() || dist(run.trail.front(), here) > JumpYards)
    {
        run.trail.clear();
        run.trail.push_front(here);
    }
    else if (dist(run.trail.front(), here) >= TrailStep)
    {
        run.trail.push_front(here);
        if (run.trail.size() > TrailMax)
            run.trail.pop_back();
    }

    // While the tank pulls (carefully, or walks into what stands on its way) and is not in the fight yet, the others
    // wait: a member out of the fight and not attacked does nothing, its pet neither (round 9: a hunter, its pet or the
    // healer was in a fight before the tank in 10 of 54 Deadmines fights).
    // (Also while the leader of a tribute run walks to Captain Kromcrush in the ogre suit: the others are no ogres, and
    // he came at the member that followed it, 2026-11-21; OrderTribute.cpp.)
    bool const tankPulls = rt.pull.active || (!run.wayBlocker.IsEmpty() && getMSTimeDiff(run.wayBlockerAt, now) < 20 * IN_MILLISECONDS) ||
        (run.tributeHoldUntil && now < run.tributeHoldUntil);
    // (While the tank gathers a big pull, also once it fights: the others wait for it where they stand. The healer is
    // the fight step's: it heals the tank.)
    if ((tankPulls && !leader->IsInCombat()) || run.gather.active)
        for (ObjectGuid const& guid : run.groupGuids)
        {
            Player* member = guid == leader->GetGUID() ? nullptr : ObjectAccessor::FindConnectedPlayer(guid);
            if (!member || !member->IsInWorld() || !member->IsAlive() || member->IsInCombat() || !member->getAttackers().empty() ||
                !sGuildBotHolder.IsAIPlayed(guid))
                continue;
            if (run.gather.active)
                if (AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfMember(guid))
                    if (GuildBot const* memberBot = guild->FindMember(guid); memberBot && sRoleService.EffectiveRole(*memberBot) == DungeonRole::Healer)
                        continue;
            BotDriver::PauseAI(member, HoldMs);
            if (Pet* pet = member->GetPet(); pet && pet->IsAlive() && !pet->IsInCombat() && pet->GetVictim())
                pet->AttackStop();
        }

    // Not in a fight of the group, not while the tank pulls carefully (the group waits where it is), and only while the
    // tank walks: a tank that stands (a gate, a rest, a wait) leaves its members to their own AI (buffs, drinks, pets).
    if (rt.pull.active || run.gather.active || !leader->isMoving() || leader->IsInCombat() || (run.tributeHoldUntil && now < run.tributeHoldUntil))
    {
        run.slotSent.clear();
        return;
    }
    std::vector<Player*> members;
    for (ObjectGuid const& guid : run.groupGuids)
    {
        if (guid == leader->GetGUID())
            continue;
        Player* member = ObjectAccessor::FindConnectedPlayer(guid);
        if (!member || !member->IsInWorld())
            continue;
        if (member->IsAlive() && member->IsInCombat())
        {
            run.slotSent.clear();
            return;
        }
        members.push_back(member);
    }

    // The point `back` yards behind the tank along its way, and the way's direction there.
    auto along = [&](float back, float& x, float& y, float& z, float& dx, float& dy)
    {
        BotRuntime::DungeonRun::TrailPoint previous = here;
        float left = back;
        for (BotRuntime::DungeonRun::TrailPoint const& point : run.trail)
        {
            float const step = dist(previous, point);
            if (step >= left && step > 0.01f)
            {
                float const f = left / step;
                x = previous.x + (point.x - previous.x) * f;
                y = previous.y + (point.y - previous.y) * f;
                z = previous.z + (point.z - previous.z) * f;
                dx = (previous.x - point.x) / step;
                dy = (previous.y - point.y) / step;
                return;
            }
            left -= step;
            previous = point;
        }
        // (The way is shorter than that: the start of it, behind the tank as it faces.)
        float const o = leader->GetOrientation();
        x = previous.x - std::cos(o) * std::max(0.0f, left);
        y = previous.y - std::sin(o) * std::max(0.0f, left);
        z = previous.z;
        dx = std::cos(o);
        dy = std::sin(o);
    };

    uint32 melee = 0, ranged = 0, healers = 0;
    AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfMember(leader->GetGUID());
    for (Player* member : members)
    {
        if (!member->IsAlive() || !SameInstance(member, leader) || member->IsBeingTeleported() || !sGuildBotHolder.IsAIPlayed(member->GetGUID()))
            continue;
        BotRuntime const* other = FindRuntime(member->GetGUID());
        if (other && other->rest.active)
            continue;
        GuildBot const* memberBot = guild ? guild->FindMember(member->GetGUID()) : nullptr;
        bool const healer = memberBot && sRoleService.EffectiveRole(*memberBot) == DungeonRole::Healer;
        bool const close = !healer && BotDriver::FightsInMelee(member);
        uint32& count = healer ? healers : close ? melee : ranged;
        // (Five places a row; a raid's group walks in rows of five behind each other, 2.5 yd apart.)
        float const back = (healer ? 8.0f : close ? 3.0f : 6.0f) + 2.5f * float(count / 5);
        static float const Sides[] = { 0.0f, 1.6f, -1.6f, 3.2f, -3.2f };
        float const side = Sides[count % 5];
        ++count;
        float x = 0.0f, y = 0.0f, z = 0.0f, dx = 0.0f, dy = 0.0f;
        along(back, x, y, z, dx, dy);
        float sx = x - dy * side, sy = y + dx * side, sz = z;
        if (side != 0.0f)
        {
            float const ground = leader->GetMap()->GetHeight(leader->GetPhaseMask(), sx, sy, z + 2.0f, true, 6.0f);
            if (ground <= INVALID_HEIGHT || std::fabs(ground - z) > 2.5f ||
                !leader->GetMap()->isInLineOfSight(x, y, z + 1.5f, sx, sy, ground + 1.5f, leader->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS,
                    VMAP::ModelIgnoreFlags::Nothing))
            {
                sx = x;
                sy = y;
            }
            else
                sz = ground;
        }
        if (member->GetExactDist(sx, sy, sz) <= SlotTolerance)
            continue;
        // (Far behind, it runs to the tank: the spread rule holds the tank for it, StepDungeonSpread.)
        if (!member->IsWithinDistInMap(leader, 2.0f * SpreadYards))
        {
            sx = here.x;
            sy = here.y;
            sz = here.z;
        }
        BotDriver::PauseAI(member, HoldMs);
        auto const sent = run.slotSent.find(member->GetGUID());
        if (sent != run.slotSent.end() && member->isMoving() && dist(sent->second, { sx, sy, sz }) < 1.5f &&
            getMSTimeDiff(run.slotSentAt[member->GetGUID()], now) < ResendMs)
            continue;
        member->GetMotionMaster()->MovePoint(0, sx, sy, sz, FORCED_MOVEMENT_RUN);
        run.slotSent[member->GetGUID()] = { sx, sy, sz };
        run.slotSentAt[member->GetGUID()] = now;
    }
}

void OrderService::StepDungeonBuffPlan(AdventureGuild& guild, GuildBot const& bot, Player* leader, BotRuntime& rt,
    DungeonPlan const& plan, std::vector<Player*> const& group, Player* guest)
{
    // the owner's E7: one provider per kind of buff that does not stack, decided by what brings the group more, the
    // talents counted. mod-playerbots' plan of the paladins' blessings does that already (the strongest Might and
    // Wisdom by rank and Improved talent, Kings and Sanctuary by role, a greater blessing per class) and counts a
    // paladin its owner plays by hand as one of its providers (its placeholder AI): he is told what it leaves to him
    // (DB8). Warriors and death knights do not shout over a stronger Might. What is left here: several shamans set
    // the same totems, and nobody sees who gives what.
    constexpr uint32 LookMs = 5 * IN_MILLISECONDS;
    BotRuntime::DungeonRun& run = rt.dungeon;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    if (now < run.nextBuffLook)
        return;
    run.nextBuffLook = now + LookMs;
    (void)guild;

    // (The whole group by its members, not only those already inside: they go in after the leader.)
    std::vector<Player*> characters;
    uint32 expected = 0;
    for (ObjectGuid const& guid : run.groupGuids)
    {
        ++expected;
        Player* member = ObjectAccessor::FindConnectedPlayer(guid);
        if (member && member->IsInWorld() && member->IsAlive() && SameInstance(member, leader))
            characters.push_back(member);
    }
    for (Player* member : group)
        if (member->IsAlive() && SameInstance(member, leader) && std::find(characters.begin(), characters.end(), member) == characters.end())
            characters.push_back(member);
    if (guest && guest->IsAlive() && SameInstance(guest, leader) && std::find(characters.begin(), characters.end(), guest) == characters.end())
        characters.push_back(guest);
    bool const byHand = guest && std::find(characters.begin(), characters.end(), guest) != characters.end() &&
        !sGuildBotHolder.IsAIPlayed(guest->GetGUID());

    // Two or more shamans played by the AI: the second sets the other totem of each element (inside only, where they
    // set the totems of their talents; BotDriver::KeepTotems).
    std::vector<Player*> shamans;
    for (Player* member : characters)
        if (member->getClass() == CLASS_SHAMAN && sGuildBotHolder.IsAIPlayed(member->GetGUID()))
            shamans.push_back(member);
    std::sort(shamans.begin(), shamans.end(), [](Player const* a, Player const* b) { return a->GetGUID() < b->GetGUID(); });
    for (std::size_t i = 1; i < shamans.size(); ++i)
    {
        std::string const changed = BotDriver::ComplementTotems(shamans[0], shamans[i]);
        if (!changed.empty())
            LOG_INFO(ActivityLog, "activity: {} sets other totems than {} in {}: {}", shamans[i]->GetName(), shamans[0]->GetName(),
                plan.name, changed);
    }

    // The paladin its owner plays by hand: what the bots leave to him, named once per blessing and run.
    if (byHand && guest->getClass() == CLASS_PALADIN)
    {
        ObjectGuid target;
        if (uint32 const spellId = BotDriver::BlessingLeftTo(guest, target))
            if (std::find(run.blessingsTold.begin(), run.blessingsTold.end(), spellId) == run.blessingsTold.end())
            {
                run.blessingsTold.push_back(spellId);
                SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
                Unit const* on = target.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*guest, target);
                std::string const name = info ? info->SpellName[0] : std::to_string(spellId);
                TellGuest(guest, Acore::StringFormat("Your group leaves {} to you{}: its paladins do not cast your share of the blessings.",
                    name, on ? " (next on " + on->GetName() + ")" : std::string()));
                LOG_INFO(ActivityLog, "activity: {}'s group in {} leaves {} to {}, whom its owner plays by hand", bot.name, plan.name, name,
                    guest->GetName());
            }
    }

    // Who gives what, once per run, when the whole group is inside (the members go in after the leader).
    if (run.buffsLogged)
        return;
    // (And not before half a minute into the run: at its first steps the members are not inside yet, and the line of
    // the first build named the tank alone in every run.)
    if (characters.size() < std::max<std::size_t>(expected, group.size()) || getMSTimeDiff(run.startedAt, now) < 30 * IN_MILLISECONDS)
        return;
    run.buffsLogged = true;
    std::string providers;
    auto add = [&](Player* member, std::string const& what)
    {
        providers += (providers.empty() ? "" : "; ") + member->GetName() + " " + what;
    };
    for (Player* member : characters)
    {
        bool const hand = member == guest && byHand;
        switch (member->getClass())
        {
            case CLASS_PALADIN: add(member, hand ? "blessings (by hand)" : "blessings by mod-playerbots' plan (strongest Might and Wisdom, Kings, Sanctuary)"); break;
            case CLASS_PRIEST: add(member, "Power Word: Fortitude"); break;
            case CLASS_MAGE: add(member, "Arcane Intellect"); break;
            case CLASS_DRUID: add(member, "Mark of the Wild"); break;
            case CLASS_WARRIOR: add(member, "Battle Shout unless a stronger Blessing of Might is on"); break;
            case CLASS_SHAMAN: add(member, "the totems of its talents" + std::string(shamans.size() > 1 && member != shamans[0] ? ", apart from the first shaman's" : "")); break;
            case CLASS_WARLOCK: add(member, "its pet"); break;
            default: break;
        }
    }
    LOG_INFO(ActivityLog, "activity: the buffs of {}'s group in {}: {}", bot.name, plan.name, providers.empty() ? "none" : providers);
}

namespace
{
    constexpr float HazardYards = 25.0f;

    // Whether an idle creature would come at a place: within its aggro range and `extra` yards of where it stands, or,
    // for one that walks about its spawn, of anywhere of its walk. A creature in a fight calls those of its kind within
    // 10 yd of it every 3 s (CreatureFamilyAssistanceRadius / Period), so a group fighting beside a dense hall has the
    // hall on it. (Scholomance, 2026-11-20: a Risen Aberration walking 5 yd about its spawn came into the group that
    // rested at the edge of the hall before the Viewing Room, and 13 of the hall's 43 followed.)
    bool IdleSees(Creature const* creature, Player const* who, float x, float y, float z, float extra)
    {
        float const reach = creature->GetAggroRange(who) + extra;
        if (creature->GetExactDist(x, y, z) < reach)
            return true;
        // A creature on a round of waypoints, or one that walks in the formation of such a creature, comes wherever its
        // round leads. (Scholomance, 2026-11-21: the group rested in the hall at the foot of the entrance stairs 32 yd
        // from a Risen Protector on its round, which came through the group with six others and wiped it out.)
        Creature const* walker = creature;
        float formation = 0.0f;
        if (CreatureGroup const* group = creature->GetFormation())
            if (Creature const* leader = group->GetLeader(); leader && leader != creature)
            {
                walker = leader;
                formation = std::min(10.0f, creature->GetExactDist2d(leader)) + 2.0f;
            }
        if (uint32 const pathId = walker->GetWaypointPath())
            if (WaypointPath const* path = sWaypointMgr->GetPath(pathId); path && path->Nodes.size() > 1)
            {
                float const nearYards = reach + formation + 3.0f;
                for (std::size_t i = 0; i < path->Nodes.size(); ++i)
                {
                    WaypointNode const& a = path->Nodes[i];
                    WaypointNode const& b = path->Nodes[(i + 1) % path->Nodes.size()];
                    float const length = std::hypot(b.X - a.X, b.Y - a.Y);
                    int const steps = std::max(1, int(length / 4.0f));
                    for (int step = 0; step <= steps; ++step)
                    {
                        float const t = float(step) / float(steps);
                        float const px = a.X + (b.X - a.X) * t, py = a.Y + (b.Y - a.Y) * t, pz = a.Z + (b.Z - a.Z) * t;
                        if (std::fabs(pz - z) < 10.0f && std::hypot(px - x, py - y) < nearYards)
                            return true;
                    }
                }
            }
        float const wander = creature->GetWanderDistance();
        if (wander <= 0.0f && creature->GetDefaultMovementType() != RANDOM_MOTION_TYPE)
            return false;
        return creature->GetHomePosition().GetExactDist(x, y, z) < reach + std::max(wander, 5.0f);
    }

    // A trap the group learned of lies within HazardYards of the place.
    bool NearHazard(BotRuntime::DungeonRun const& run, float x, float y, float z)
    {
        for (BotRuntime::DungeonRun::Hazard const& hazard : run.hazards)
            if (hazard.hits >= 2 && (hazard.x - x) * (hazard.x - x) + (hazard.y - y) * (hazard.y - y) < HazardYards * HazardYards &&
                std::fabs(hazard.z - z) < 10.0f)
                return true;
        return false;
    }

    // Blackrock Depths, 2026-11-20: the Ironhand Guardians of the Iron Hall breathe fire on a timer while Magmus fights
    // and, in this core, on after he is dead; they put nobody in a fight. The group rested in the hall and brought its
    // dead back there, and died there over and over ("not in a fight, last fought nothing"), twice a wipe. A member
    // that loses health out of a fight (not by a fall or under water, not a warlock by its own Life Tap or a warrior by
    // its Bloodrage, not by a lower maximum health) twice within a minute at one place, or dies there, marks a trap.
    void NoteHazards(GuildBot const& bot, DungeonPlan const& plan, BotRuntime::DungeonRun& run, std::vector<Player*> const& group,
        Player* guest, uint32 now)
    {
        constexpr uint32 KeepMs = 3 * MINUTE * IN_MILLISECONDS;
        constexpr uint32 LogEveryMs = 30 * IN_MILLISECONDS;
        constexpr uint32 SecondHitMs = MINUTE * IN_MILLISECONDS;
        constexpr uint32 LookGapMs = 2500;
        constexpr uint32 SpellBloodrage = 29131;
        std::erase_if(run.hazards, [now](BotRuntime::DungeonRun::Hazard const& hazard)
        {
            return getMSTimeDiff(hazard.at, now) >= (hazard.hits >= 2 ? KeepMs : SecondHitMs);
        });
        auto look = [&](Player* member)
        {
            BotRuntime::DungeonRun::HazardLook& last = run.hazardLooks[member->GetGUID()];
            BotRuntime::DungeonRun::HazardLook const next { member->IsAlive() ? member->GetHealth() : 0, member->GetMaxHealth(),
                member->IsAlive(), member->IsInCombat(), now };
            // (Two looks in a row out of a fight: the leader looks only while it is in no fight itself, and a member's
            // last blows of a fight fell between a look before the leader's fight and one after it: "traps" beside
            // every fight, 2026-11-20. And no damage over time of an enemy that fights: a DoT of the Iron Hall's
            // guardians, which are in no fight, still counts; ruling every DoT out let the group rest in the hall
            // after Magmus and die there, the same evening.)
            auto enemyDot = [&]()
            {
                for (AuraType type : { SPELL_AURA_PERIODIC_DAMAGE, SPELL_AURA_PERIODIC_DAMAGE_PERCENT, SPELL_AURA_PERIODIC_LEECH })
                    for (AuraEffect const* effect : member->GetAuraEffectsByType(type))
                        if (Unit const* caster = effect->GetCaster())
                            if (caster != member && caster->IsInCombat())
                                return true;
                return false;
            };
            bool const lost = last.alive && !last.fight && !next.fight && last.at && getMSTimeDiff(last.at, now) <= LookGapMs &&
                next.maxHealth >= last.maxHealth && last.maxHealth && next.health + last.maxHealth / 25 <= last.health && !enemyDot();
            if (lost && member->GetMapId() == plan.mapId && member->getClass() != CLASS_WARLOCK && !member->HasAura(SpellBloodrage) &&
                !member->IsUnderWater() && !member->HasUnitMovementFlag(MOVEMENTFLAG_FALLING))
            {
                float const x = member->GetPositionX(), y = member->GetPositionY(), z = member->GetPositionZ();
                uint8 hits = next.alive ? 1 : 2;
                bool known = false;
                for (BotRuntime::DungeonRun::Hazard& hazard : run.hazards)
                    if ((hazard.x - x) * (hazard.x - x) + (hazard.y - y) * (hazard.y - y) < 64.0f && std::fabs(hazard.z - z) < 10.0f)
                    {
                        hazard.at = now;
                        hazard.hits = std::min<uint8>(hazard.hits + hits, 9);
                        hits = hazard.hits;
                        known = true;
                    }
                if (!known)
                    run.hazards.push_back({ x, y, z, now, hits });
                if (hits >= 2 && (!run.hazardLoggedAt || getMSTimeDiff(run.hazardLoggedAt, now) >= LogEveryMs))
                {
                    run.hazardLoggedAt = now;
                    LOG_INFO(ActivityLog, "activity: {} of {}'s group loses health out of a fight in {} at ({:.0f}, {:.0f}, {:.0f}) ({} -> {}{}): "
                        "a trap; the group neither rests nor brings anybody back within {:.0f} yd of it", member->GetName(), bot.name,
                        plan.name, x, y, z, last.health, next.health, next.alive ? "" : ", dead", HazardYards);
                }
            }
            last = next;
        };
        for (Player* member : group)
            look(member);
        if (guest)
            look(guest);
    }
}

bool OrderService::StepDungeonGroupRest(AdventureGuild& guild, GuildBot const& bot, Player* leader, BotRuntime& rt,
    DungeonPlan const& plan, std::vector<Player*> const& group)
{
    // the owner's E2: the group goes on until a member is below its trigger, and then all of it rests up together ([quote left out]). Members do not rest on their own in a dungeon run (StepRest); what they rest to
    // here is asked through the gate's fields (BotRuntime::gateHealth / gateMana).
    constexpr uint8 TriggerHealth = 50;
    constexpr uint8 TriggerHealerMana = 40;
    constexpr uint8 TriggerDamageMana = 20;
    constexpr uint8 FullHealth = 95;
    constexpr uint8 FullMana = 95;
    constexpr uint32 AskMs = 6 * IN_MILLISECONDS;
    constexpr uint32 MaxMs = 3 * MINUTE * IN_MILLISECONDS;
    BotRuntime::DungeonRun& run = rt.dungeon;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    for (Player* member : group)
        if (member->IsAlive() && InRealFight(member))
        {
            run.fightSeenAt = now;
            return false;
        }
    // After a fight, out of the sight of creatures that are in none yet: back along the tank's way to the first place
    // none of them would come at (IdleSees), at most 60 yd, once per fight. (Scholomance, 2026-11-20: after each fight at
    // the edge of the hall before the Viewing Room the group stood among 36 creatures within 30 yd, and the next came at
    // it before it pulled anything; the fourth time 13 came at once.)
    if (!run.restSpot && leader->IsAlive() && run.fightSeenAt && run.regroupFor != run.fightSeenAt &&
        getMSTimeDiff(run.fightSeenAt, now) < 8 * IN_MILLISECONDS)
    {
        run.regroupFor = run.fightSeenAt;
        std::vector<Creature*> idle;
        for (Creature* creature : HostilesAround(leader, leader, 60.0f))
            if (!creature->IsInCombat() && !creature->IsCritter() && !creature->IsCivilian())
                idle.push_back(creature);
        auto seenBy = [&](float x, float y, float z)
        {
            for (Creature* creature : idle)
                if (IdleSees(creature, leader, x, y, z, 2.0f))
                    return creature;
            return static_cast<Creature*>(nullptr);
        };
        if (Creature const* watcher = seenBy(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ()))
        {
            float back = 0.0f;
            BotRuntime::DungeonRun::TrailPoint previous { leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ() };
            for (BotRuntime::DungeonRun::TrailPoint const& point : run.trail)
            {
                back += std::sqrt((point.x - previous.x) * (point.x - previous.x) + (point.y - previous.y) * (point.y - previous.y));
                previous = point;
                if (back > 60.0f)
                    break;
                if (!seenBy(point.x, point.y, point.z))
                {
                    run.restSpot = true;
                    run.restX = point.x;
                    run.restY = point.y;
                    run.restZ = point.z;
                    run.restWalkSince = now;
                    run.restWalkLimit = 30 * IN_MILLISECONDS;
                    if (!run.groupRestSince)
                        run.groupRestSince = now;
                    LOG_INFO(ActivityLog, "activity: {}'s group goes {:.0f} yd back after its fight in {}: {} (level {}) would come at it "
                        "where it stands", bot.name, back, plan.name, watcher->GetName(), uint32(watcher->GetLevel()));
                    break;
                }
            }
        }
    }
    // Out of a trap first (NoteHazards): back along the tank's way to the first place clear of it, at most 80 yd. Only
    // when the group rests: a trap beside the way is no reason to turn round (Dire Maul East, 2026-11-25: after Choking
    // Spores hit the hunter the group went back out of it whenever its tank came past the place, five times in 70 s and
    // "rested after 0 s" each time, beside a patrol's round, which then caught the hunter 60 yd behind the tank).
    auto outOfTrap = [&]()
    {
        if (!leader->IsAlive() || !NearHazard(run, leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ()))
            return false;
        float back = 0.0f;
        BotRuntime::DungeonRun::TrailPoint previous { leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ() };
        for (BotRuntime::DungeonRun::TrailPoint const& point : run.trail)
        {
            back += std::sqrt((point.x - previous.x) * (point.x - previous.x) + (point.y - previous.y) * (point.y - previous.y));
            previous = point;
            if (back > 80.0f)
                break;
            if (!NearHazard(run, point.x, point.y, point.z))
            {
                run.restSpot = true;
                run.restX = point.x;
                run.restY = point.y;
                run.restZ = point.z;
                run.restWalkSince = now;
                run.restWalkLimit = 30 * IN_MILLISECONDS;
                LOG_INFO(ActivityLog, "activity: {}'s group goes {:.0f} yd back out of a trap in {} and rests there", bot.name, back, plan.name);
                return true;
            }
        }
        return false;
    };
    auto manaOf = [](Player* member) -> int32
    {
        if (member->getPowerType() != POWER_MANA || !member->GetMaxPower(POWER_MANA))
            return -1;
        return int32(uint64(member->GetPower(POWER_MANA)) * 100 / member->GetMaxPower(POWER_MANA));
    };
    if (!run.groupRestSince)
    {
        std::string low;
        for (Player* member : group)
        {
            if (!member->IsAlive() || !SameInstance(member, leader) || !sGuildBotHolder.IsAIPlayed(member->GetGUID()))
                continue;
            GuildBot const* memberBot = guild.FindMember(member->GetGUID());
            bool const healer = memberBot && sRoleService.EffectiveRole(*memberBot) == DungeonRole::Healer;
            int32 const mana = manaOf(member);
            if (member->GetHealthPct() < TriggerHealth)
                low = Acore::StringFormat("{} health {:.0f}%", member->GetName(), member->GetHealthPct());
            else if (mana >= 0 && mana < (healer ? TriggerHealerMana : TriggerDamageMana) && (healer || HasDrinkAnywhere(member)))
                low = Acore::StringFormat("{} mana {}%", member->GetName(), mana);
            if (!low.empty())
                break;
        }
        if (low.empty())
            return false;
        run.groupRestSince = now;
        run.groupRestBegan = now;
        ++run.metrics.groupRests;
        LOG_INFO(ActivityLog, "activity: {}'s group rests together in {}: {}", bot.name, plan.name, low);
        if (!run.restSpot && outOfTrap())
            return true;
        // Not where creatures that are in no fight would see the group: back along the tank's way to the first place
        // none of them sees, at most 80 yd. (Stratholme's group rested beside 11 ghouls of the street, which came at
        // it, and wiped, 2026-11-19.)
        std::vector<Creature*> idle;
        for (Creature* creature : HostilesAround(leader, leader, 120.0f))
            if (!creature->IsInCombat() && !creature->IsCritter() && !creature->IsCivilian())
                idle.push_back(creature);
        auto seen = [&](float x, float y, float z)
        {
            for (Creature* creature : idle)
                if (IdleSees(creature, leader, x, y, z, 8.0f))
                    return creature;
            return static_cast<Creature*>(nullptr);
        };
        // (Not after the walk out of a trap above has begun: that is where it rests.)
        if (run.restSpot)
            idle.clear();
        run.restSpot = false;
        if (Creature const* watcher = seen(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ()))
        {
            // Back along the trail to the first place none of them sees, up to 220 yd; with none, to the place fewest
            // see. (Scholomance's hall: within 80 yd of the group every place was in the walk of a wandering Risen
            // Aberration or Diseased Ghoul, the group rested where it stood, and 18 came at it, 2026-11-21.)
            auto watchers = [&](float x, float y, float z)
            {
                uint32 count = 0;
                for (Creature* creature : idle)
                    count += IdleSees(creature, leader, x, y, z, 8.0f) ? 1 : 0;
                return count;
            };
            uint32 const hereCount = watchers(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ());
            float back = 0.0f, bestBack = 0.0f;
            uint32 bestCount = hereCount;
            BotRuntime::DungeonRun::TrailPoint best {};
            BotRuntime::DungeonRun::TrailPoint previous { leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ() };
            for (BotRuntime::DungeonRun::TrailPoint const& point : run.trail)
            {
                back += std::sqrt((point.x - previous.x) * (point.x - previous.x) + (point.y - previous.y) * (point.y - previous.y));
                previous = point;
                if (back > 220.0f)
                    break;
                uint32 const count = watchers(point.x, point.y, point.z);
                if (count < bestCount)
                {
                    bestCount = count;
                    best = point;
                    bestBack = back;
                    if (!count)
                        break;
                }
            }
            if (bestCount < hereCount)
            {
                run.restSpot = true;
                run.restX = best.x;
                run.restY = best.y;
                run.restZ = best.z;
                run.restWalkSince = now;
                run.restWalkLimit = std::max<uint32>(30 * IN_MILLISECONDS, uint32(bestBack * 250.0f));
                LOG_INFO(ActivityLog, "activity: {}'s group goes {:.0f} yd back to rest in {}: {} (level {}) would see it where it stands{}",
                    bot.name, bestBack, plan.name, watcher->GetName(), uint32(watcher->GetLevel()),
                    bestCount ? Acore::StringFormat(" ({} of {} creatures still see the place it goes to)", bestCount, hereCount) : std::string());
            }
        }
    }
    // On its way back to the quiet place (at most 30 s).
    if (run.restSpot)
    {
        if (leader->GetExactDist(run.restX, run.restY, run.restZ) > 4.0f && getMSTimeDiff(run.restWalkSince, now) < run.restWalkLimit)
        {
            if (BotDriver::GetRpgState(leader) != RpgState::GoCamp || getMSTimeDiff(rt.task.lastAssert, now) >= AssertMs)
            {
                DungeonGoTo(leader, plan.mapId, run.restX, run.restY, run.restZ);
                rt.task.lastAssert = now;
            }
            PauseStuck(rt);
            run.bossStuckSince = now;
            run.groupRestSince = now;
            rt.why = Acore::StringFormat("In {}: the group goes back to rest out of sight", plan.name);
            return true;
        }
        run.restSpot = false;
    }
    std::string still;
    for (Player* member : group)
    {
        if (!member->IsAlive() || !SameInstance(member, leader) || !sGuildBotHolder.IsAIPlayed(member->GetGUID()))
            continue;
        int32 const mana = manaOf(member);
        bool const shortHealth = member->GetHealthPct() < FullHealth;
        // A healer with nothing to drink is waited for too, as its low mana began the rest (its mana comes back by
        // itself): Ragefire Chasm's group "rested together" for its healer at 4 % and was "rested after 0 s" three times
        // in a second (2026-11-24).
        GuildBot const* memberBot = guild.FindMember(member->GetGUID());
        bool const healer = memberBot && sRoleService.EffectiveRole(*memberBot) == DungeonRole::Healer;
        bool const shortMana = mana >= 0 && mana < FullMana && (healer || HasDrinkAnywhere(member));
        if (!shortHealth && !shortMana)
            continue;
        if (BotRuntime* other = member == leader ? &rt : FindRuntime(member->GetGUID()))
        {
            other->gateHealth = shortHealth ? FullHealth : 0;
            other->gateMana = shortMana ? FullMana : 0;
            other->gateUntil = now + AskMs;
        }
        if (still.size() < 160)
            still += (still.empty() ? "" : ", ") + member->GetName();
    }
    if (still.empty() || getMSTimeDiff(run.groupRestSince, now) >= MaxMs)
    {
        LOG_INFO(ActivityLog, "activity: {}'s group is rested in {} after {} s{}", bot.name, plan.name,
            getMSTimeDiff(run.groupRestSince, now) / IN_MILLISECONDS, still.empty() ? "" : Acore::StringFormat(" (time up: {} not full)", still));
        run.groupRestSince = 0;
        // The clocks of the walk to an object that wakes a boss stand still while the group rests: Upper Blackrock Spire's
        // group rested 125 s in front of Pyroguard Emberseer's hall, set off for the Blackrock Altar along a way of 208 yd
        // and was put beside it 7 s later, "no nearer in a minute" (2026-11-26, the owner: [quote left out]). A group that fights all the time still has its wall clock.
        if (run.groupRestBegan)
        {
            uint32 const restMs = getMSTimeDiff(run.groupRestBegan, now);
            if (run.objectStillSince)
                run.objectStillSince = std::min(run.objectStillSince + restMs, now);
            if (run.objectSince)
                run.objectSince = std::min(run.objectSince + restMs, now);
            run.groupRestBegan = 0;
        }
        return false;
    }
    if (leader->isMoving())
        BotDriver::Rest(leader);
    PauseStuck(rt);
    run.bossStuckSince = now;
    rt.strandSince = 0;
    rt.why = Acore::StringFormat("In {}: the group rests ({})", plan.name, still);
    return true;
}

bool OrderService::StepDungeonSpread(GuildBot const& bot, Player* leader, BotRuntime& rt, DungeonPlan const& plan,
    std::vector<Player*> const& group)
{
    // The spread limit (package P2): the tank stands while a member of its group is farther than 15 yd from it (was:
    // 60 yd), and goes on when all are back; a member that has not come for a minute is put beside the tank.
    constexpr float SpreadYards = 15.0f;
    constexpr uint32 FetchAfterMs = MINUTE * IN_MILLISECONDS;
    constexpr uint32 LogEveryMs = 30 * IN_MILLISECONDS;
    BotRuntime::DungeonRun& run = rt.dungeon;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    // (Not at the Twin Emperors: their halves stand 130 yd apart, and the half at Vek'lor's throne was put beside the
    // tank at the other at the pull, 2026-11-24.)
    if (rt.pull.active || leader->IsInCombat() || !leader->IsAlive() ||
        (!run.twinSide.empty() && (run.twinPlacesSince || run.twinPulledAt)) || !run.cthunSlot.empty() ||
        (run.twinBugsSince && !run.twinBugsDone))
    {
        run.spreadSince = 0;
        run.strayedSince.clear();
        return false;
    }
    std::string behind;
    float farthest = 0.0f;
    for (Player* member : group)
    {
        if (member == leader)
            continue;
        // (Dead, in another instance, in a fight or on its way in: other rules see to it.)
        if (!member->IsAlive() || !SameInstance(member, leader) || member->IsInCombat() || member->IsBeingTeleported() ||
            !sGuildBotHolder.IsAIPlayed(member->GetGUID()))
        {
            run.strayedSince.erase(member->GetGUID());
            continue;
        }
        float const distance = member->GetDistance(leader);
        // (A raid walks in rows: its last ones are farther back.)
        if (distance <= (plan.raid ? SpreadYards + 10.0f : SpreadYards))
        {
            run.strayedSince.erase(member->GetGUID());
            continue;
        }
        uint32& since = run.strayedSince[member->GetGUID()];
        if (!since)
            since = now;
        // A way on foot to the tank that the member's follow does not walk (long, or down a drop): it walks it itself,
        // for up to three minutes, before it is put beside the tank (the owner: no porting in dungeons).
        else if (getMSTimeDiff(since, now) >= FetchAfterMs / 4 && getMSTimeDiff(since, now) < 3 * FetchAfterMs &&
            WalkDungeonWay(member, leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ()))
        {
            if (getMSTimeDiff(since, now) < FetchAfterMs / 4 + 1500)
                LOG_INFO(ActivityLog, "activity: {} walks the navmesh's way to its tank {} in {} ({:.0f} yd away)", member->GetName(),
                    leader->GetName(), plan.name, distance);
        }
        else if (getMSTimeDiff(since, now) >= FetchAfterMs)
        {
            Position spot = leader->GetPosition();
            leader->MovePositionToFirstCollision(spot, 3.0f, float(M_PI) + frand(-0.6f, 0.6f));
            LOG_INFO(ActivityLog, "activity: {} did not come to its tank {} in {} for {} s ({:.0f} yd away): it is put beside it",
                member->GetName(), leader->GetName(), plan.name, FetchAfterMs / IN_MILLISECONDS, distance);
            member->NearTeleportTo(spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ(), leader->GetOrientation());
            if (BotRuntime* other = FindRuntime(member->GetGUID()))
                other->hasLastPosition = false;
            ++run.metrics.fetched;
            since = now;
            continue;
        }
        if (distance > farthest)
        {
            farthest = distance;
            behind = Acore::StringFormat("{} is {:.0f} yd behind", member->GetName(), distance);
        }
    }
    if (behind.empty())
    {
        run.spreadSince = 0;
        return false;
    }
    if (!run.spreadSince)
    {
        run.spreadSince = now;
        ++run.metrics.spreadHalts;
        if (now >= run.spreadNextLog)
        {
            run.spreadNextLog = now + LogEveryMs;
            LOG_INFO(ActivityLog, "activity: {} stands for its group in {}: {}", bot.name, plan.name, behind);
        }
    }
    if (leader->isMoving())
        BotDriver::Rest(leader);
    PauseStuck(rt);
    run.bossStuckSince = now;
    rt.strandSince = 0;
    rt.why = Acore::StringFormat("In {}: waiting for its group ({})", plan.name, behind);
    return true;
}

bool OrderService::StepDungeonWay(AdventureGuild& guild, GuildBot const& bot, Player* leader, BotRuntime& rt, DungeonPlan const& plan,
    std::vector<Player*> const& group, Player* guest, float toX, float toY, float toZ, float& goX, float& goY, float& goZ)
{
    // Package P3 of the plan, with the owner's E3 and E4: the tank goes in stretches of at most 35 yd; before each it looks
    // at what stands on the stretch within the reach of the group's lowest member (the core's aggro range for that
    // member) plus a margin of 8 yd, at least 15 yd: the tank engages it before anybody walks past it.
    constexpr float SectionYards = 35.0f;
    float const Margin = float(sBotSettings.Get(leader, Setting::DungeonsEngageMargin));
    constexpr float MinEngage = 15.0f;
    constexpr float PackYards = 10.0f;
    constexpr float CampBackYards = 25.0f;
    constexpr float BossRoomYards = 25.0f;
    BotRuntime::DungeonRun& run = rt.dungeon;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    goX = toX;
    goY = toY;
    goZ = toZ;
    if (run.gather.active)
        return true;
    if (rt.pull.active || leader->IsInCombat() || !leader->IsAlive())
        return false;
    // A drop or swim under way: on to its landing, the stretch is not planned anew meanwhile (ViaDungeonPath holds the
    // same; here it was planned from half way down a jump before Ghamoo-ra, began with the swim back, and the leader
    // went to and fro between the two parts, Blackfathom Deeps, 2026-11-25).
    if (rt.dropUntil && now < rt.dropUntil)
    {
        if (leader->GetExactDist(rt.dropX, rt.dropY, rt.dropZ) > 2.0f)
        {
            KeepDropMoving(leader, rt);
            return true;
        }
        rt.dropUntil = 0;
    }
    else if (rt.dropUntil)
        DropDone(leader, rt);
    // The creature the tank walked into stays its target until it fights: one decision, not one each second (the
    // first round walked the tank in, let the gate call it back when it was 15 yd from its group, and in again).
    if (!run.wayBlocker.IsEmpty())
    {
        constexpr uint32 EngageMs = 20 * IN_MILLISECONDS;
        Creature* engaged = ObjectAccessor::GetCreature(*leader, run.wayBlocker);
        if (engaged && engaged->IsAlive() && !engaged->IsInCombat() && getMSTimeDiff(run.wayBlockerAt, now) < EngageMs &&
            leader->IsWithinDistInMap(engaged, 60.0f))
        {
            GoForWalkIn(leader, rt, engaged);
            rt.why = Acore::StringFormat("In {}: going for {}", plan.name, engaged->GetName());
            return true;
        }
        if (run.walkInWalking)
        {
            run.walkInWalking = false;
            BotDriver::PauseAI(leader, 0);
        }
        run.wayBlocker.Clear();
    }
    float sx = 0.0f, sy = 0.0f, sz = 0.0f;
    // The stretch lies along the navmesh's way (DungeonNav): the core's path finder walks only 148 polygons towards the
    // boss and ends below a plateau the way reaches through the caves and down a drop (the Wailing Caverns, 2026-11-24:
    // the stretch to Verdan the Everliving led the tank back to the foot of his plateau every few seconds, 60 yd from
    // him and 1950 yd by the way, for eight minutes).
    NavPoint navPoint, navTop;
    float fromX, fromY, fromZ;
    bool const fromLanding = NavFrom(leader, rt, fromX, fromY, fromZ);
    if (sDungeonNav.PointAlong(leader->GetMapId(), fromX, fromY, fromZ, toX, toY, toZ, SectionYards, navPoint, nullptr, &navTop,
            leader->GetMap()))
    {
        if (navPoint.drop)
        {
            // At the edge of a drop on the way: down it, as a player jumps down a ledge.
            float const toTop = std::hypot(leader->GetPositionX() - navTop.x, leader->GetPositionY() - navTop.y);
            float const topUp = std::fabs(leader->GetPositionZ() - navTop.z);
            if (toTop <= 2.5f && topUp <= 3.0f)
            {
                rt.task.dropStallSince = 0;
                DropDown(leader, navPoint.x, navPoint.y, navPoint.z, navPoint.swim);
                return true;
            }
            // A few yards from that edge and no nearer to it in 5 s: the drop from where it stands. (Blackfathom Deeps,
            // 2026-11-25: before Ghamoo-ra the walk to the edge of a swim 5 yd off took the leader up onto the part a
            // yard above it, whose way began with the swim back, and it went to and fro between the two for minutes.)
            if (toTop <= 6.0f && topUp <= 4.0f && leader->GetExactDist(navPoint.x, navPoint.y, navPoint.z) <= 15.0f)
            {
                BotTask& task = rt.task;
                if (!task.dropStallSince || std::hypot(leader->GetPositionX() - task.dropStallX, leader->GetPositionY() - task.dropStallY) > 1.0f)
                {
                    task.dropStallSince = now;
                    task.dropStallX = leader->GetPositionX();
                    task.dropStallY = leader->GetPositionY();
                }
                else if (getMSTimeDiff(task.dropStallSince, now) >= 5 * IN_MILLISECONDS)
                {
                    LOG_INFO(ActivityLog, "activity: {} does not get to the edge at ({:.0f}, {:.0f}, {:.0f}) in {}, {:.0f} yd away: it {} from "
                        "where it stands", leader->GetName(), navTop.x, navTop.y, navTop.z, plan.name, toTop, navPoint.swim ? "swims" : "jumps");
                    task.dropStallSince = 0;
                    DropDown(leader, navPoint.x, navPoint.y, navPoint.z, navPoint.swim);
                    return true;
                }
            }
            else
                rt.task.dropStallSince = 0;
            navPoint = navTop;
        }
        else if (fromLanding && leader->GetPositionZ() - fromZ > 2.5f)
        {
            SwimAlong(leader, rt, navPoint.x, navPoint.y, navPoint.z);
            return true;
        }
        goX = navPoint.x;
        goY = navPoint.y;
        goZ = navPoint.z;
    }
    else if (PathPointAhead(leader, leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ(), toX, toY, toZ, SectionYards, sx, sy, sz))
    {
        goX = sx;
        goY = sy;
        goZ = sz;
    }
    // A patrol that crosses the stretch ahead (the plan: wait up to 8 s, or pull it): while the group waits, the tank
    // stands; between two looks too.
    constexpr uint32 PatrolWaitMs = 8 * IN_MILLISECONDS;
    if (!run.patrol.IsEmpty() && getMSTimeDiff(run.patrolSince, now) < PatrolWaitMs)
    {
        Creature* patrol = ObjectAccessor::GetCreature(*leader, run.patrol);
        if (patrol && patrol->IsAlive() && !patrol->IsInCombat())
        {
            if (leader->isMoving())
                BotDriver::Rest(leader);
            rt.why = Acore::StringFormat("In {}: waiting for {} on its round to pass", plan.name, patrol->GetName());
            return true;
        }
        run.patrol.Clear();
    }
    if (now < run.nextWayLook)
        return false;
    run.nextWayLook = now + IN_MILLISECONDS;

    // The group's lowest member (the character its owner plays by hand counts).
    Player* lowest = leader;
    for (Player* member : group)
        if (member->IsAlive() && SameInstance(member, leader) && member->GetLevel() < lowest->GetLevel())
            lowest = member;
    if (guest && guest->IsAlive() && SameInstance(guest, leader) && guest->GetLevel() < lowest->GetLevel())
        lowest = guest;

    // The band around the way by the run window's mode (the owner, 2026-11-19): the bosses, only what would attack the group
    // on its way (the reach of its lowest member and 3 yd); the rooms, also everything in sight on the way's level
    // within 35 yd of it ([quote left out]).
    DungeonFightRules const rules = GetFightRules(bot.guid);
    constexpr float BossesMargin = 3.0f;
    constexpr float ClearYards = 35.0f;
    auto engageOf = [&](Creature const* creature)
    {
        // (A creature that walks about comes its walk nearer: Scholomance's hall at the foot of the entrance stairs is
        // full of Risen Aberrations and Diseased Ghouls that wander 3 to 5 yd, and the group that walked past them at
        // their reach had 18 on it, 2026-11-21.)
        float const reach = creature->GetAggroRange(lowest) + (creature->GetDefaultMovementType() == RANDOM_MOTION_TYPE ?
            std::max(creature->GetWanderDistance(), 3.0f) : 0.0f);
        if (rules.mode == ModeBosses || rules.mode == ModeTribute)
            return std::max(10.0f, reach + std::min(Margin, BossesMargin));
        return std::max({ MinEngage, reach + Margin, ClearYards });
    };
    // What stands on the stretch: the nearest such creature to the tank.
    float const ax = leader->GetPositionX(), ay = leader->GetPositionY(), az = leader->GetPositionZ();
    float const bx = goX, by = goY;
    float const abx = bx - ax, aby = by - ay;
    float const ab2 = std::max(0.01f, abx * abx + aby * aby);
    // How far a creature lies above or below the stretch where it comes nearest: on stairs the tank's own height says
    // little. (Scholomance, 2026-11-20: the creatures of the hall at the foot of the entrance stairs lay 30 yd below
    // the tank until it stood among them, and 14 came at it.)
    auto offStretchZ = [&](Creature const* creature)
    {
        float const t = std::clamp(((creature->GetPositionX() - ax) * abx + (creature->GetPositionY() - ay) * aby) / ab2, 0.0f, 1.0f);
        return std::fabs(creature->GetPositionZ() - (az + (goZ - az) * t));
    };
    Creature* blocker = nullptr;
    std::vector<Creature*> around = HostilesAround(leader, leader, SectionYards + MinEngage + 25.0f);
    // (Not a named ogre a tribute run leaves alive: OrderTribute.cpp.)
    around.erase(std::remove_if(around.begin(), around.end(), [&](Creature* creature) { return IsTributeSpared(run, creature); }),
        around.end());
    // Patrols: a creature on a round of waypoints whose place, or the end of the movement it makes, comes within its
    // reach of the stretch. (Its place alone says little: it walks onto the stretch while the group does.)
    auto toStretch = [&](float x, float y)
    {
        float const t = std::clamp(((x - ax) * abx + (y - ay) * aby) / ab2, 0.0f, 1.0f);
        return std::hypot(ax + abx * t - x, ay + aby * t - y);
    };
    bool patrolSeen = false;
    for (Creature* creature : around)
    {
        // (No boss: a boss on a round, Rhahk'Zor, is the boss logic's.)
        if (creature->IsInCombat() || offStretchZ(creature) > 10.0f ||
            creature->GetMotionMaster()->GetCurrentMovementGeneratorType() != WAYPOINT_MOTION_TYPE || creature->IsDungeonBoss() ||
            std::any_of(plan.bosses.begin(), plan.bosses.end(), [&](DungeonBoss const& boss) { return boss.entry == creature->GetEntry(); }))
            continue;
        float const engage = std::max(MinEngage, creature->GetAggroRange(lowest) + Margin);
        float distance = toStretch(creature->GetPositionX(), creature->GetPositionY());
        if (!creature->movespline->Finalized())
        {
            G3D::Vector3 const end = creature->movespline->FinalDestination();
            distance = std::min(distance, toStretch(end.x, end.y));
        }
        if (distance > engage || !leader->IsWithinLOSInMap(creature))
            continue;
        patrolSeen = true;
        if (run.patrol != creature->GetGUID())
        {
            run.patrol = creature->GetGUID();
            run.patrolSince = now;
            LOG_INFO(ActivityLog, "activity: {}'s group waits for {} (level {}) on its round to pass in {}: it crosses the way {:.0f} yd "
                "ahead", bot.name, creature->GetName(), uint32(creature->GetLevel()), plan.name, leader->GetExactDist2d(creature));
            if (leader->isMoving())
                BotDriver::Rest(leader);
            return true;
        }
        // Still on the stretch after the wait: the tank pulls it to the group, but not while others stand within 12 yd of
        // it (they come with it: a Necrofiend pulled on its round through the hall before Scholomance's Viewing Room
        // brought eight more, 2026-11-20); then the group waits up to a minute for it to come out of them.
        uint32 crowd = 0;
        for (Creature* other : around)
            if (other != creature && !other->IsInCombat() && other->GetExactDist(creature) < 12.0f)
                ++crowd;
        if (crowd && getMSTimeDiff(run.patrolSince, now) < MINUTE * IN_MILLISECONDS)
        {
            if (leader->isMoving())
                BotDriver::Rest(leader);
            rt.why = Acore::StringFormat("In {}: waiting for {} on its round to leave the others", plan.name, creature->GetName());
            return true;
        }
        if (crowd)
            break; // (Then it is one of what stands on the way: the careful pull below judges its pack.)
        float const at[3] = { leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ() };
        if (Escape::BeginPull(leader, rt, creature, at))
        {
            LOG_INFO(ActivityLog, "activity: {} pulls {} (level {}) on its round to its group in {}: it did not pass in {} s", bot.name,
                creature->GetName(), uint32(creature->GetLevel()), plan.name, PatrolWaitMs / IN_MILLISECONDS);
            run.patrol.Clear();
            return true;
        }
        break;
    }
    if (!patrolSeen)
        run.patrol.Clear();
    // The creature a camp was chosen for, while the group goes back to that camp: the same pull. (From the camp the
    // way ahead looks different, and another creature or none would be "on the way".)
    // (Only while it is still among what stands around the tank: Lower Blackrock Spire on j5, 2026-11-26, a Smolderthorn
    // Witch Doctor the camp had been chosen for stood 86 yd away and a floor below a minute later; the walk-in's hold lets
    // go of a creature beyond 60 yd, this kept it, and the tank "walked into" it every second for a minute and a half,
    // its walk getting no nearer.)
    // (Nor one that was left out since: no pull from the camp and a way there far longer than the straight line. The kept
    // camp brought it back each tick past that rule, and the owner's Palatwo in Blackrock Depths' Upper City "could not pull"
    // a Doomforge Craftsman and "did not walk into" it every 2-4 s, its way 1065 yd for 38 yd straight, round 242.)
    auto refusedForWay = [&](Creature const* creature)
    {
        auto refused = run.wayRefused.find(creature->GetGUID());
        return refused != run.wayRefused.end() && now < refused->second.first;
    };
    if (!run.campFor.IsEmpty() && getMSTimeDiff(run.campSince, now) < 2 * MINUTE * IN_MILLISECONDS)
    {
        Creature* kept = ObjectAccessor::GetCreature(*leader, run.campFor);
        if (kept && kept->IsAlive() && !kept->IsInCombat() && std::find(around.begin(), around.end(), kept) != around.end() &&
            !(kept->GetGUID() == rt.pullFailedTarget && now < rt.pullFailedUntil) && !refusedForWay(kept))
            blocker = kept;
        else
            run.campFor.Clear();
    }
    for (Creature* creature : around)
    {
        if (blocker && blocker->GetGUID() == run.campFor)
            break;
        if (creature->IsInCombat() || offStretchZ(creature) > 10.0f)
            continue;
        if (creature->GetGUID() == rt.pullFailedTarget && now < rt.pullFailedUntil)
            continue;
        if (refusedForWay(creature))
            continue;
        // (One that stands beside the boss the group walks to belongs to its room: the room is cleared, or left for
        // the boss fight, when the group is there. A taunt on one 8 yd beside Theka brought her.)
        if (creature->GetExactDist2d(toX, toY) <= BossRoomYards)
            continue;
        float const t = std::clamp(((creature->GetPositionX() - ax) * abx + (creature->GetPositionY() - ay) * aby) / ab2, 0.0f, 1.0f);
        float const px = ax + abx * t - creature->GetPositionX();
        float const py = ay + aby * t - creature->GetPositionY();
        float const engage = engageOf(creature);
        if (px * px + py * py > engage * engage || !leader->IsWithinLOSInMap(creature))
            continue;
        if (!blocker || leader->GetDistance(creature) < leader->GetDistance(blocker))
            blocker = creature;
    }
    if (!blocker)
        return false;

    // Its pack, and what kind of pull it is (E3: to a camp from 4 creatures at the group's level, from 6 when the group
    // is five levels above them, and always for a pack with casters; after a wipe or the tank's death in this run every
    // pull, the owner's DP1 and DP5).
    std::vector<Creature*> pack;
    uint8 highest = 0;
    bool caster = false;
    for (Creature* creature : around)
        if (!creature->IsInCombat() && creature->IsWithinDistInMap(blocker, PackYards))
        {
            pack.push_back(creature);
            highest = std::max<uint8>(highest, uint8(std::min<uint32>(creature->GetLevel() + (creature->isElite() ? 3 : 0), 255)));
            caster = caster || creature->GetCreatureTemplate()->unit_class == CLASS_MAGE;
        }
    // A formation whose members assist each other comes whole, however far apart they stand: Blackwing Lair's
    // formations of 14 (a Death Talon Overseer, technicians, warlocks, spellbinders) came at a raid of 40 that walked
    // into "an Overseer with 2 creatures", and wiped it (2026-11-22).
    if (CreatureGroup const* formation = blocker->GetFormation())
    {
        bool assist = false;
        for (auto const& [member, info] : formation->GetMembers())
            assist = assist || info.HasGroupFlag(uint16(GroupAIFlags::GROUP_AI_FLAG_ASSIST_MASK));
        if (assist)
            for (auto const& [member, info] : formation->GetMembers())
                if (member && member->IsAlive() && !member->IsInCombat() && std::find(pack.begin(), pack.end(), member) == pack.end())
                {
                    pack.push_back(member);
                    highest = std::max<uint8>(highest, uint8(std::min<uint32>(member->GetLevel() + (member->isElite() ? 3 : 0), 255)));
                    caster = caster || member->GetCreatureTemplate()->unit_class == CLASS_MAGE;
                }
    }
    // ... and what comes by the calls for help: every creature in a fight calls those within
    // CreatureFamilyAssistanceRadius of it that would assist it, at once and again every CreatureFamilyAssistancePeriod
    // (3 s) from wherever it is, so the members of a dense hall that stand within that reach of one another come one
    // after the other (a chain). The owner's Bloodyo (Lower Blackrock Spire, 2026-11-26) pulled a Firebrand Grunt as "6
    // creatures" (those within 10 yd of it) and 14 came over 35 s, the last from 27 yd; the tank died.
    std::size_t const nearPack = pack.size();
    {
        float const assistYards = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS);
        for (std::size_t i = 0; i < pack.size() && pack.size() < 40 && assistYards > 0.0f; ++i)
            for (Creature* creature : around)
            {
                if (!creature->IsAlive() || creature->IsInCombat() || creature->IsDungeonBoss() ||
                    creature->GetExactDist(pack[i]) > assistYards + 2.0f || std::find(pack.begin(), pack.end(), creature) != pack.end() ||
                    !Escape::WouldAssist(creature, pack[i], leader))
                    continue;
                pack.push_back(creature);
                highest = std::max<uint8>(highest, uint8(std::min<uint32>(creature->GetLevel() + (creature->isElite() ? 3 : 0), 255)));
                caster = caster || creature->GetCreatureTemplate()->unit_class == CLASS_MAGE;
            }
    }
    // (An elite counts three levels higher, as on the walks: a level-56 group walked into a formation of five
    // Anvilrage elites of level 50-52 in Blackrock Depths as "five levels above them", twelve came and it wiped,
    // 2026-11-19.)
    bool const above = lowest->GetLevel() >= highest + 5;
    uint32 const bigAt = uint32(std::max<int32>(2, sBotSettings.Get(leader, Setting::DungeonsBigPullAt))) + (above ? 2u : 0u);
    bool const afterTrouble = run.wipes > 0 || run.tankDeaths > 0;
    bool const camp = pack.size() >= bigAt || caster || afterTrouble;
    std::string const chained = pack.size() > nearPack ? Acore::StringFormat(", {} of them by calls for help", pack.size() - nearPack) : "";
    std::string const why = (afterTrouble ? (run.wipes ? std::string("the group was wiped out in this run") : std::string("the tank died in this run")) :
        caster ? std::string("casters among them") : Acore::StringFormat("{} creatures", pack.size())) +
        (afterTrouble || caster ? Acore::StringFormat("; {} creatures", pack.size()) : std::string()) + chained;

    // Big pulls (the mode "big pulls"; not after a wipe or the tank's death in this run, DP1 and DP5: then one pack at a
    // time, carefully): the cluster is what stands within gatherYards of the creature, in its sight and on its level,
    // the nearest to it first, up to gatherCount creatures; the tank gathers it (StepDungeonGather).
    if (rules.mode == ModeBigPulls && !afterTrouble)
    {
        std::vector<Creature*> cluster;
        for (Creature* creature : around)
            if (creature->IsAlive() && !creature->IsInCombat() && !creature->IsDungeonBoss() && creature->GetExactDist2d(toX, toY) > BossRoomYards &&
                std::fabs(creature->GetPositionZ() - blocker->GetPositionZ()) <= 8.0f &&
                creature->GetExactDist2d(blocker) <= float(rules.gatherYards) && blocker->IsWithinLOSInMap(creature) &&
                std::none_of(plan.bosses.begin(), plan.bosses.end(), [&](DungeonBoss const& boss) { return boss.entry == creature->GetEntry(); }))
                cluster.push_back(creature);
        std::sort(cluster.begin(), cluster.end(), [&](Creature* a, Creature* b) { return a->GetExactDist(blocker) < b->GetExactDist(blocker); });
        if (cluster.size() > rules.gatherCount)
            cluster.resize(rules.gatherCount);
        if (cluster.size() > pack.size())
        {
            if (!DungeonGate(guild, bot, leader, rt, plan, group, guest, Acore::StringFormat("the big pull of {}", blocker->GetName()),
                    1000 + run.pulls))
                return true;
            BotRuntime::DungeonRun::Gather& gather = run.gather;
            gather = BotRuntime::DungeonRun::Gather();
            gather.active = true;
            gather.since = now;
            gather.campX = leader->GetPositionX();
            gather.campY = leader->GetPositionY();
            gather.campZ = leader->GetPositionZ();
            for (Creature* creature : cluster)
                gather.targets.push_back(creature->GetGUID());
            ++run.pulls;
            ++run.metrics.bigPulls;
            LOG_INFO(ActivityLog, "activity: {} gathers a big pull in {}: {} creatures within {} yd of {} (level {}), until {} are on it",
                bot.name, plan.name, cluster.size(), rules.gatherYards, blocker->GetName(), uint32(blocker->GetLevel()), rules.gatherCount);
            rt.why = Acore::StringFormat("In {}: gathering a big pull", plan.name);
            return true;
        }
        // (Nothing beside its own pack within reach: a pull of one pack, as in the mode "rooms".)
    }

    if (!camp)
    {
        // A small pack: the tank walks in, the group comes up behind it; first the owner's release for a normal pull.
        if (!DungeonGate(guild, bot, leader, rt, plan, group, guest, Acore::StringFormat("the pack of {}", blocker->GetName()),
            3000 + run.wayKey, false))
            return true;
        if (GoForWalkIn(leader, rt, blocker))
        {
            run.wayBlocker = blocker->GetGUID();
            run.wayBlockerAt = now;
            // (A creature that is in a fight already is no walk-in to hold, so the decision comes again each second until
            // the tank fights it: one line and one count for it. Lower Blackrock Spire on j5, 2026-11-26: six lines in ten
            // seconds for one Scarshield Legionnaire, "18446744073709551615 creatures with it" when its pack was empty.)
            if (run.walkInNamed != blocker->GetGUID() || getMSTimeDiff(run.walkInNamedAt, now) >= 20 * IN_MILLISECONDS)
            {
                run.walkInNamed = blocker->GetGUID();
                run.walkInNamedAt = now;
                ++run.wayKey;
                ++run.metrics.walkIns;
                std::size_t const with = pack.empty() ? 0 : pack.size() - 1;
                LOG_INFO(ActivityLog, "activity: {} walks into {} (level {}) on its way in {}: {} creature{} with it, the reach of its "
                    "lowest member {} is {:.0f} yd", bot.name, blocker->GetName(), uint32(blocker->GetLevel()), plan.name, with,
                    with == 1 ? "" : "s", lowest->GetName(), engageOf(blocker));
            }
        }
        return true;
    }

    // A big pull: the camp first, on the cleared way behind the tank, and the group there (the gate: the whole group at
    // the tank, health and mana up), then the creature nearest to the group pulled at the farthest reach to it. (The
    // group gathered where the tank stood and the camp lay 25 yd behind it: the pulled creature turned to the healer on
    // its way, who stood between the camp and the hall, and the fight was 15 yd from the hall: the owner's Bloodyo died in
    // Lower Blackrock Spire, 2026-11-26, "called off (something else attacked it)" and 14 came.)
    float at[3] = { leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ() };
    if (run.campFor == blocker->GetGUID() && getMSTimeDiff(run.campSince, now) < 2 * MINUTE * IN_MILLISECONDS)
    {
        at[0] = run.campX;
        at[1] = run.campY;
        at[2] = run.campZ;
    }
    else
    {
        float left = CampBackYards;
        BotRuntime::DungeonRun::TrailPoint previous { at[0], at[1], at[2] };
        for (BotRuntime::DungeonRun::TrailPoint const& point : run.trail)
        {
            float const step = std::sqrt((point.x - previous.x) * (point.x - previous.x) + (point.y - previous.y) * (point.y - previous.y) +
                (point.z - previous.z) * (point.z - previous.z));
            if (step >= left && step > 0.01f)
            {
                float const f = left / step;
                at[0] = previous.x + (point.x - previous.x) * f;
                at[1] = previous.y + (point.y - previous.y) * f;
                at[2] = previous.z + (point.z - previous.z) * f;
                left = 0.0f;
                break;
            }
            left -= step;
            previous = point;
        }
        if (left > 0.0f && !run.trail.empty())
        {
            at[0] = run.trail.back().x;
            at[1] = run.trail.back().y;
            at[2] = run.trail.back().z;
        }
        // Farther back while creatures that are not pulled would come at the camp (IdleSees): in a dense hall every
        // creature in a fight calls those within 10 yd of it, and the hall comes along, at most 60 yd back. (The
        // members of the pack that came by a chain of calls count too: a camp within their call's reach and a few
        // steps brings the rest of the hall.)
        std::vector<Creature*> idle;
        for (Creature* creature : HostilesAround(leader, leader, 100.0f))
            if (!creature->IsInCombat() && !creature->IsCritter() && !creature->IsCivilian() && creature != blocker &&
                std::find(pack.begin(), pack.end(), creature) == pack.end())
                idle.push_back(creature);
        float const chainReach = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS) + 12.0f;
        auto busy = [&](float x, float y, float z)
        {
            for (Creature* creature : idle)
                if (IdleSees(creature, leader, x, y, z, 8.0f))
                    return true;
            for (std::size_t i = nearPack; i < pack.size(); ++i)
                if (pack[i]->GetExactDist(x, y, z) <= chainReach)
                    return true;
            return false;
        };
        if (busy(at[0], at[1], at[2]))
        {
            float back = 0.0f;
            BotRuntime::DungeonRun::TrailPoint last { leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ() };
            for (BotRuntime::DungeonRun::TrailPoint const& point : run.trail)
            {
                back += std::sqrt((point.x - last.x) * (point.x - last.x) + (point.y - last.y) * (point.y - last.y));
                last = point;
                if (back > 60.0f)
                    break;
                if (back > CampBackYards && !busy(point.x, point.y, point.z))
                {
                    at[0] = point.x;
                    at[1] = point.y;
                    at[2] = point.z;
                    break;
                }
            }
        }
        run.campFor = blocker->GetGUID();
        run.campX = at[0];
        run.campY = at[1];
        run.campZ = at[2];
        run.campSince = now;
    }
    // The group to the camp: the tank walks there and its members follow it; the gate gathers them there.
    if (leader->GetExactDist(at[0], at[1], at[2]) > 6.0f && !leader->IsInCombat())
    {
        if (!WalkDungeonWay(leader, at[0], at[1], at[2], false))
            DungeonGoTo(leader, leader->GetMapId(), at[0], at[1], at[2]);
        rt.why = Acore::StringFormat("In {}: back to the camp for the pull of {}", plan.name, blocker->GetName());
        return true;
    }
    if (!DungeonGate(guild, bot, leader, rt, plan, group, guest, Acore::StringFormat("the pull of {}", blocker->GetName()), 1000 + run.pulls))
        return true;
    if (!Escape::BeginPull(leader, rt, blocker, at))
    {
        // No pull from here (it sees the tank already, there is no way to the camp): the tank walks in. The camp is
        // given up with it: the creature is no camp's any more (round 242).
        rt.pullFailedTarget = blocker->GetGUID();
        rt.pullFailedUntil = now + 20 * IN_MILLISECONDS;
        run.campFor.Clear();
        LOG_INFO(ActivityLog, "activity: {} cannot pull {} (level {}, {:.0f} yd away, {}) to a camp in {}: no pull from here; it walks in",
            bot.name, blocker->GetName(), uint32(blocker->GetLevel()), leader->GetDistance(blocker), why, plan.name);
        if (GoForWalkIn(leader, rt, blocker))
        {
            run.wayBlocker = blocker->GetGUID();
            run.wayBlockerAt = now;
            ++run.wayKey;
            ++run.metrics.walkIns;
        }
        return true;
    }
    ++run.pulls;
    ++run.metrics.campPulls;
    LOG_INFO(ActivityLog, "activity: {} pulls {} (level {}) carefully to its group in {}: {} (the camp {:.0f} yd from it on the way back; "
        "the highest level {}, an elite counted three higher)", bot.name, blocker->GetName(), uint32(blocker->GetLevel()), plan.name, why,
        blocker->GetExactDist(at[0], at[1], at[2]), uint32(highest));
    rt.why = Acore::StringFormat("In {}: pulling {} to the group", plan.name, blocker->GetName());
    return true;
}

bool OrderService::GoForWalkIn(Player* leader, BotRuntime& rt, Creature* target)
{
    // mod-playerbots' AI sets off only for a target in its sight: given one around a corner it holds it and stands. The owner's
    // tank stood 24 to 28 yd from a Bloodaxe Worg Pup in Halycon's den in Lower Blackrock Spire with the pup as its target,
    // four walk-ins of 20 s in a row, and its group with it ([quote left out], [quote left out], 2026-11-25); the Cathedral's group had walked into the same monk every 20 s for 14 minutes (round 6). So the
    // tank walks the navmesh's way to it itself until it is near and in its sight, and its AI fights from there. One in its
    // sight is left to the AI, which walks to it, unless the tank still stands 3 s after the decision.
    BotRuntime::DungeonRun& run = rt.dungeon;
    // Not a creature whose way is far longer than the straight line (behind a wall, up a ramp, in the next room): the
    // walk there passes what stands between, and a walk-in of three is a fight of six. Lower Blackrock Spire, 2026-11-26:
    // the owner's death knight tank walked into a Smolderthorn Berserker 43 yd away "with 2 creatures", its way 100 yd round
    // a corner, met Bloodaxe Evokers on it and died with six on it. Such a creature is met when the group's way gets
    // there; until then it is no walk-in (30 s).
    if (!leader->IsInCombat() && !run.walkInWalking && !leader->IsWithinDistInMap(target, 15.0f))
    {
        float length = 0.0f;
        float const straight = leader->GetExactDist(target);
        if (sDungeonNav.FindWay(leader->GetMapId(), leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ(),
                target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(), nullptr, &length, leader->GetMap()) &&
            length > std::max(40.0f, straight * 1.5f + 10.0f))
        {
            uint32 const now = getMSTime();
            constexpr uint32 RefusedMs = 2 * MINUTE * IN_MILLISECONDS;
            constexpr uint32 NoteAgainMs = 10 * MINUTE * IN_MILLISECONDS;
            rt.pullFailedTarget = target->GetGUID();
            rt.pullFailedUntil = now + RefusedMs;
            if (run.wayBlocker == target->GetGUID())
                run.wayBlocker.Clear();
            if (run.campFor == target->GetGUID())
                run.campFor.Clear();
            if (run.wayRefused.size() > 64)
                for (auto it = run.wayRefused.begin(); it != run.wayRefused.end();)
                    it = getMSTimeDiff(it->second.second, now) >= NoteAgainMs ? run.wayRefused.erase(it) : std::next(it);
            auto [entry, added] = run.wayRefused.try_emplace(target->GetGUID(), now + RefusedMs, now);
            bool const note = added || getMSTimeDiff(entry->second.second, now) >= NoteAgainMs;
            entry->second.first = now + RefusedMs;
            if (note)
            {
                entry->second.second = now;
                LOG_INFO(ActivityLog, "activity: {} does not walk into {} (level {}): its way there is {:.0f} yd for {:.0f} yd straight; "
                    "the group meets it on its way", leader->GetName(), target->GetName(), uint32(target->GetLevel()), length, straight);
            }
            return false;
        }
    }
    bool const inSight = leader->IsWithinLOSInMap(target);
    bool const closeBy = inSight && leader->IsWithinDistInMap(target, 8.0f);
    bool const decided = run.wayBlocker == target->GetGUID();
    bool const aiStands = inSight && decided && !leader->isMoving() && getMSTimeDiff(run.wayBlockerAt, getMSTime()) >= 3 * IN_MILLISECONDS;
    if (!leader->IsInCombat() && !closeBy && (!inSight || aiStands || run.walkInWalking) &&
        WalkDungeonWay(leader, target->GetPositionX(), target->GetPositionY(), target->GetPositionZ()))
    {
        if (!run.walkInWalking)
            LOG_INFO(ActivityLog, "activity: {} walks to {} (level {}, {:.0f} yd away{}) in {} to fight it: {}", leader->GetName(),
                target->GetName(), uint32(target->GetLevel()), leader->GetDistance(target), inSight ? "" : ", out of its sight",
                leader->GetMap()->GetMapName(), inSight ? "its AI stood with it as its target" :
                "its AI does not set off for a creature it does not see");
        run.walkInWalking = true;
        return true;
    }
    // (The pause its own walk set is lifted at once: the fight begins as the creature comes at it.)
    if (run.walkInWalking)
    {
        run.walkInWalking = false;
        BotDriver::PauseAI(leader, 0);
    }
    return BotDriver::Attack(leader, target, false);
}

bool OrderService::DungeonGate(AdventureGuild& guild, GuildBot const& bot, Player* leader, BotRuntime& rt, DungeonPlan const& plan,
    std::vector<Player*> const& group, Player* guest, std::string const& what, uint32 key, bool big, bool counted)
{
    // the owner's thresholds for bosses and big pulls (E2): health 90 %, a healer's mana 80 %, a damage dealer's 60 %; the
    // whole group at the tank (12 yd, the Doc's gather distance), the character its owner plays by hand included, but
    // for that one only its place counts (E6).
    // (A normal pull, package P3: the owner's release for normal pulls, 70 / 50 / 30, within the spread limit.)
    // (A raid of 40 stands in a wider crowd: its last rows are 20 yd from the tank when the first stand at its feet.)
    float const GateYards = (big ? 12.0f : 15.0f) + (plan.raid ? 10.0f : 0.0f);
    // (The big form by the bot settings of the category Dungeon runs, package PW: the leader's own values.)
    uint8 const GateHealth = big ? uint8(std::clamp<int32>(sBotSettings.Get(leader, Setting::DungeonsGateHealth), 0, 100)) : 70;
    uint8 const GateHealerMana = big ? uint8(std::clamp<int32>(sBotSettings.Get(leader, Setting::DungeonsGateHealerMana), 0, 100)) : 50;
    uint8 const GateDamageMana = big ? uint8(std::clamp<int32>(sBotSettings.Get(leader, Setting::DungeonsGateDamageMana), 0, 100)) : 30;
    constexpr uint32 AskMs = 6 * IN_MILLISECONDS;          // a member's rest is asked for this long at a time
    constexpr uint32 CallAfterMs = 3 * IN_MILLISECONDS;    // a member that does not come is walked to the tank ...
    constexpr uint32 PutAfterMs = MINUTE * IN_MILLISECONDS; // ... and then put beside it (the Doc: a minute)
    constexpr uint32 LogEveryMs = 20 * IN_MILLISECONDS;
    BotRuntime::DungeonRun& run = rt.dungeon;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    if (run.gateKey != key)
    {
        run.gateKey = key;
        run.gateSince = now;
        run.gateNextLog = 0;
        run.gateMissing.clear();
        run.gateAway.clear();
        run.gateTold = false;
    }

    std::vector<Player*> characters = group;
    if (guest && guest->GetMapId() == plan.mapId && std::find(characters.begin(), characters.end(), guest) == characters.end())
        characters.push_back(guest);
    std::string missing;
    auto add = [&missing](std::string const& text) { missing += (missing.empty() ? "" : ", ") + text; };
    for (Player* member : characters)
    {
        bool const byHand = member == guest || !sGuildBotHolder.IsAIPlayed(member->GetGUID());
        if (!member->IsAlive())
        {
            add(member->GetName() + " is dead");
            continue;
        }
        if (!SameInstance(member, leader))
        {
            add(member->GetName() + " is not in the group's instance");
            continue;
        }
        if (InRealFight(member))
        {
            add(member->GetName() + " fights");
            continue;
        }
        float const distance = member == leader ? 0.0f : member->GetDistance(leader);
        if (distance > GateYards)
        {
            add(Acore::StringFormat("{} is {:.0f} yd from the tank", member->GetName(), distance));
            uint32& away = run.gateAway[member->GetGUID()];
            if (!away)
                away = now;
            if (byHand)
            {
                if (!run.gateTold)
                {
                    run.gateTold = true;
                    TellGuest(member, Acore::StringFormat("Your group waits for you at its tank before {}.", what));
                }
            }
            else if (getMSTimeDiff(away, now) >= PutAfterMs && !member->IsBeingTeleported())
            {
                // (A ledge, a wall the path finder sends it around: the gate does not wait for good.)
                Position spot = leader->GetPosition();
                leader->MovePositionToFirstCollision(spot, 3.0f, float(M_PI) + frand(-0.6f, 0.6f));
                LOG_INFO(ActivityLog, "activity: {} did not come to its tank {} before {} in {} in {} s ({:.0f} yd away): it is put "
                    "beside it", member->GetName(), leader->GetName(), what, plan.name, PutAfterMs / IN_MILLISECONDS, distance);
                member->NearTeleportTo(spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ(), leader->GetOrientation());
                if (BotRuntime* other = FindRuntime(member->GetGUID()))
                    other->hasLastPosition = false;
                ++run.metrics.fetched;
                away = now;
            }
            else if (getMSTimeDiff(away, now) >= CallAfterMs && !member->isMoving())
            {
                // (Its own AI does not bring it: mod-playerbots' follow of a squad member keeps its distance, and a member
                // stood 22-38 yd from its waiting tank in Zul'Farrak until it was put beside it. A point 3 yd behind the
                // tank, its AI held for the walk.)
                Position spot = leader->GetPosition();
                leader->MovePositionToFirstCollision(spot, 3.0f, float(M_PI) + frand(-0.6f, 0.6f));
                BotDriver::PauseAI(member, std::min<uint32>(6000, uint32(distance / 7.0f * 1000.0f) + 500));
                member->GetMotionMaster()->MovePoint(0, spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ(), FORCED_MOVEMENT_RUN);
            }
            continue;
        }
        run.gateAway.erase(member->GetGUID());
        if (byHand)
            continue;
        BotRuntime* other = member == leader ? &rt : FindRuntime(member->GetGUID());
        GuildBot const* memberBot = guild.FindMember(member->GetGUID());
        bool const healer = memberBot && sRoleService.EffectiveRole(*memberBot) == DungeonRole::Healer;
        uint8 const health = uint8(std::clamp(member->GetHealthPct(), 0.0f, 100.0f));
        bool const manaUser = member->getPowerType() == POWER_MANA && member->GetMaxPower(POWER_MANA) > 0;
        uint8 const mana = manaUser ? uint8(uint64(member->GetPower(POWER_MANA)) * 100 / member->GetMaxPower(POWER_MANA)) : 100;
        uint8 const wantMana = healer ? GateHealerMana : GateDamageMana;
        bool shortOf = false;
        if (health < GateHealth)
        {
            add(Acore::StringFormat("{} health {}%", member->GetName(), uint32(health)));
            shortOf = true;
        }
        bool askMana = false;
        if (manaUser && mana < wantMana)
        {
            // A damage dealer with nothing to drink anywhere does not hold the group (E2); it is named once per run.
            if (!healer && !HasDrinkAnywhere(member))
            {
                if (run.gateDryNamed.insert(member->GetGUID()).second)
                    LOG_INFO(ActivityLog, "activity: {} has nothing to drink (bags, guild storage, a mage of its group): its mana ({}%) does "
                        "not hold its group in {}", member->GetName(), uint32(mana), plan.name);
            }
            else
            {
                add(Acore::StringFormat("{} mana {}%", member->GetName(), uint32(mana)));
                shortOf = true;
                askMana = true;
            }
        }
        if (shortOf && other)
        {
            other->gateHealth = health < GateHealth ? GateHealth : 0;
            other->gateMana = askMana ? wantMana : 0;
            other->gateUntil = now + AskMs;
        }
    }

    if (missing.empty())
    {
        if (counted)
        {
            if (getMSTimeDiff(run.gateSince, now) >= 2 * IN_MILLISECONDS)
                LOG_INFO(ActivityLog, "activity: {}'s group is ready for {} in {} after {} s at its tank", bot.name, what, plan.name,
                    getMSTimeDiff(run.gateSince, now) / IN_MILLISECONDS);
            ++run.metrics.gates;
            run.metrics.gateMs += getMSTimeDiff(run.gateSince, now);
        }
        run.gateKey = 0;
        return true;
    }
    // (The wait at the gate is no "no nearer to the boss": the clock stands still.)
    if (run.bossStuckSince)
        run.bossStuckSince = now;
    if (missing != run.gateMissing && now >= run.gateNextLog)
    {
        run.gateMissing = missing;
        run.gateNextLog = now + LogEveryMs;
        LOG_INFO(ActivityLog, "activity: {}'s group gathers at its tank before {} in {}: {}", bot.name, what, plan.name, missing);
    }
    // The tank stands where it is (out of the boss's reach, before the pack) while the group comes and rests.
    if (leader->isMoving() && !leader->IsInCombat())
        BotDriver::Rest(leader);
    PauseStuck(rt);
    run.bossStuckSince = now;
    rt.strandSince = 0;
    rt.why = Acore::StringFormat("In {}: gathering before {} ({})", plan.name, what, missing);
    return false;
}

void OrderService::StepDungeonMetricsTick(Player* leader, BotRuntime& rt)
{
    // Four times a second (from OrderService::Update): a fight of a group far above its creatures lasts a second or two,
    // and the leader's step once a second saw none of the probe group's fights in the Deadmines (round 119), nor
    // one of three bosses of a group farming the Cathedral.
    BotRuntime::DungeonRun& run = rt.dungeon;
    if (!run.id || run.phase != PhaseInside)
        return;
    DungeonPlan const* plan = sDungeons.GetPlan(run.id);
    AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfMember(leader->GetGUID());
    GuildBot const* bot = guild ? guild->FindMember(leader->GetGUID()) : nullptr;
    if (!plan || !bot)
        return;
    std::vector<Player*> group;
    for (ObjectGuid const& guid : run.groupGuids)
        if (Player* member = ObjectAccessor::FindConnectedPlayer(guid); member && member->IsInWorld())
            group.push_back(member);
    StepDungeonMetrics(*guild, *bot, leader, rt, *plan, group, DungeonGuest(leader->GetGUID()));
}

void OrderService::StepDungeonMetrics(AdventureGuild& guild, GuildBot const& bot, Player* leader, BotRuntime& rt, DungeonPlan const& plan,
    std::vector<Player*> const& group, Player* guest)
{
    // The thresholds of a boss and a big pull (the owner's E2: health 90 %, a healer's mana 80 %, a damage dealer's 60 %)
    // and the gather distance at the tank (the Doc's acceptance table: 15 yd).
    constexpr float ReadyYards = 15.0f;
    constexpr float ReadyHealthPct = 90.0f;
    constexpr float ReadyHealerManaPct = 80.0f;
    constexpr float ReadyDamageManaPct = 60.0f;
    constexpr uint32 FightGapMs = 3 * IN_MILLISECONDS;   // nobody in combat this long: the fight is over
    constexpr uint32 LateMs = 5 * IN_MILLISECONDS;       // a member this much after the tank, or after the release, came late
    constexpr float JumpYards = 15.0f;                   // a step this long in a quarter of a second is a teleport or a hop

    BotRuntime::DungeonRun::Metrics& m = rt.dungeon.metrics;
    uint32 const now = std::max<uint32>(getMSTime(), 1);

    // The way the leader walked.
    if (leader->IsAlive() && leader->GetMapId() == plan.mapId && !leader->IsBeingTeleported())
    {
        if (m.hasLast)
            if (float const step = leader->GetExactDist(m.lastX, m.lastY, m.lastZ); step < JumpYards)
                m.walked += step;
        m.lastX = leader->GetPositionX();
        m.lastY = leader->GetPositionY();
        m.lastZ = leader->GetPositionZ();
        m.hasLast = true;
    }
    else
        m.hasLast = false;

    // The group's fights: who came into each when. (The character its owner plays by hand counts as a member.)
    std::vector<Player*> characters = group;
    if (guest && guest->GetMapId() == plan.mapId)
        characters.push_back(guest);
    bool fighting = false;
    for (Player* character : characters)
    {
        if (character->GetMapId() != plan.mapId || !character->IsAlive() || !character->IsInCombat())
            continue;
        fighting = true;
        if (!m.inFight)
        {
            m.inFight = true;
            m.fightStart = now;
            m.enteredAt.clear();
            m.ownAttack.clear();
            m.placeJudged = false;
            m.releasedAt = 0;
            m.startYards.clear();
            for (Player* other : characters)
                if (other->IsAlive() && other->GetMapId() == plan.mapId)
                    m.startYards[other->GetGUID()] = other == leader ? 0.0f :
                        other->GetDistance(leader) + (other->IsWithinLOSInMap(leader) ? 0.0f : 1000.0f);
        }
        if (m.enteredAt.emplace(character->GetGUID(), now).second && character != leader && character->getAttackers().empty() &&
            character->GetVictim())
            m.ownAttack.insert(character->GetGUID());
    }
    if (fighting)
    {
        m.quietSince = 0;
        if (!m.placeJudged && getMSTimeDiff(m.fightStart, now) >= 5 * IN_MILLISECONDS)
        {
            m.placeJudged = true;
            for (Player* character : characters)
                if (character != leader && character->IsAlive() && character->GetMapId() == plan.mapId)
                {
                    ++m.placedIn;
                    m.placedFar += character->IsWithinDistInMap(leader, ReadyYards) ? 0 : 1;
                    // The healer by its reach: since P6a it does no damage and is in a fight only once it heals, so
                    // "in a fight 5 s after the tank" says little about it.
                    GuildBot const* memberBot = guild.FindMember(character->GetGUID());
                    if (memberBot && sRoleService.EffectiveRole(*memberBot) == DungeonRole::Healer)
                    {
                        ++m.healerReach;
                        m.healerOut += character->IsWithinDistInMap(leader, 40.0f) && character->IsWithinLOSInMap(leader) ? 0 : 1;
                    }
                }
        }
    }
    else if (m.inFight)
    {
        if (!m.quietSince)
            m.quietSince = now;
        else if (getMSTimeDiff(m.quietSince, now) >= FightGapMs)
        {
            m.inFight = false;
            m.quietSince = 0;
            ++m.fights;
            // (The run's tank is its leader: the first tank of the group leads it.)
            auto const tankItr = m.enteredAt.find(leader->GetGUID());
            uint32 first = m.fightStart;
            for (auto const& [guid, at] : m.enteredAt)
                first = std::min(first, at);
            if (tankItr == m.enteredAt.end() || tankItr->second > first)
            {
                ++m.memberFirst;
                for (auto const& [guid, at] : m.enteredAt)
                    if (guid != leader->GetGUID() && (tankItr == m.enteredAt.end() || at < tankItr->second) && m.ownAttack.count(guid))
                    {
                        ++m.memberPulled;
                        break;
                    }
            }
            // (Debug: who came late into this fight, and how far it stood from the tank when the fight began.)
            if (GetSettings().debug && tankItr != m.enteredAt.end())
            {
                std::string late;
                for (Player* character : characters)
                {
                    if (character == leader)
                        continue;
                    auto const at = m.enteredAt.find(character->GetGUID());
                    uint32 const freeFrom = std::max(tankItr->second, m.releasedAt);
                    if (at != m.enteredAt.end() && at->second < freeFrom + LateMs)
                        continue;
                    auto const start = m.startYards.find(character->GetGUID());
                    float const yards = start == m.startYards.end() ? -1.0f : start->second;
                    late += Acore::StringFormat("{}{} {}{}", late.empty() ? "" : ", ", character->GetName(),
                        at == m.enteredAt.end() ? std::string("not in it") :
                        Acore::StringFormat("+{} s after the tank, +{} s after the release", (at->second - tankItr->second) / IN_MILLISECONDS,
                            at->second > freeFrom ? (at->second - freeFrom) / IN_MILLISECONDS : 0u),
                        yards < 0.0f ? std::string() : Acore::StringFormat(" ({:.0f} yd from the tank at its start{})",
                            yards >= 1000.0f ? yards - 1000.0f : yards, yards >= 1000.0f ? ", out of its sight" : ""));
                }
                // (And who was in it before the tank: what it was doing is in the fight lines of that second.)
                std::string before;
                for (Player* character : characters)
                {
                    auto const at = m.enteredAt.find(character->GetGUID());
                    if (character == leader || at == m.enteredAt.end() || at->second >= tankItr->second)
                        continue;
                    auto const start = m.startYards.find(character->GetGUID());
                    float const yards = start == m.startYards.end() ? -1.0f : start->second;
                    before += Acore::StringFormat("{}{} {} s before the tank ({}){}", before.empty() ? "" : ", ", character->GetName(),
                        (tankItr->second - at->second + 500) / IN_MILLISECONDS,
                        m.ownAttack.count(character->GetGUID()) ? "its own attack" : "attacked", yards < 0.0f ? std::string() :
                        Acore::StringFormat(" ({:.0f} yd from it at the start{})", yards >= 1000.0f ? yards - 1000.0f : yards,
                            yards >= 1000.0f ? ", out of its sight" : ""));
                }
                if (!late.empty() || !before.empty())
                    LOG_INFO(ActivityLog, "activity: dungeon fight {} of {}'s group, {} s: {}{}{}", uint32(m.fights), bot.name,
                        getMSTimeDiff(m.fightStart, now) / IN_MILLISECONDS, before.empty() ? "" : "before the tank: " + before,
                        !before.empty() && !late.empty() ? "; " : "", late.empty() ? "" : "late: " + late);
            }
            // Late two ways: 5 s or more after the tank (the plan's measure, kept to compare with its starting numbers),
            // and 5 s or more after the group was free to fight (the owner's DP3 holds the others back while the tank takes
            // the creatures, up to its lead time; the round-5 debug lines showed nearly every member "late" by the first
            // measure 0 to 12 yd from the tank at the start and in the fight 5 or 6 s after it, the hold and a cast).
            uint32 const freeAt = tankItr == m.enteredAt.end() ? 0 : std::max(tankItr->second, m.releasedAt);
            if (tankItr != m.enteredAt.end())
                for (auto const& [guid, at] : m.enteredAt)
                {
                    if (guid == leader->GetGUID())
                        continue;
                    bool const late = at >= tankItr->second + LateMs;
                    bool const lateFree = at >= freeAt + LateMs;
                    Player* member = nullptr;
                    for (Player* character : characters)
                        if (character->GetGUID() == guid)
                            member = character;
                    GuildBot const* memberBot = guild.FindMember(guid);
                    if (memberBot && sRoleService.EffectiveRole(*memberBot) == DungeonRole::Healer)
                    {
                        ++m.healerIn;
                        m.healerLate += late ? 1 : 0;
                    }
                    else if (member && BotDriver::FightsInMelee(member))
                    {
                        ++m.meleeIn;
                        m.meleeLate += late ? 1 : 0;
                        m.meleeFreeLate += lateFree ? 1 : 0;
                    }
                    else
                    {
                        ++m.othersIn;
                        m.othersLate += late ? 1 : 0;
                        m.othersFreeLate += lateFree ? 1 : 0;
                    }
                }
            m.enteredAt.clear();
        }
    }

    // A boss pulled: the whole group at the tank, its health and mana up? (For the character its owner plays by hand
    // only its place counts, the owner's E6.)
    for (DungeonBoss const& boss : plan.bosses)
    {
        if (m.bossesJudged.count(boss.entry))
            continue;
        Creature* creature = leader->FindNearestCreature(boss.entry, 80.0f, true);
        if (!creature || !creature->IsInCombat())
            continue;
        m.bossesJudged.insert(boss.entry);
        ++m.bossPulls;
        std::string missing;
        auto add = [&missing](std::string const& what) { missing += (missing.empty() ? "" : ", ") + what; };
        // (A boss the tank pulls to its group: the group is measured where it waits for it, the pull's fight place;
        // the tank stepped forward for its opener, and the healer that waited 3 yd from the place was "17 yd away".)
        bool const pulledToGroup = rt.pull.active && rt.pull.forGroup && rt.pull.target == creature->GetGUID();
        for (Player* character : characters)
        {
            if (!character->IsAlive())
            {
                add(character->GetName() + " dead");
                continue;
            }
            float const away = character->GetMapId() != plan.mapId ? 999.0f : pulledToGroup ?
                character->GetExactDist(rt.pull.fightX, rt.pull.fightY, rt.pull.fightZ) : character->GetDistance(leader);
            if (character != leader && away > ReadyYards)
            {
                add(Acore::StringFormat("{} {:.0f} yd away", character->GetName(), away));
                continue;
            }
            if (character == guest)
                continue;
            if (character->GetHealthPct() < ReadyHealthPct)
                add(Acore::StringFormat("{} health {:.0f}%", character->GetName(), character->GetHealthPct()));
            GuildBot const* memberBot = guild.FindMember(character->GetGUID());
            DungeonRole const role = memberBot ? sRoleService.EffectiveRole(*memberBot) : DungeonRole::None;
            if (role != DungeonRole::Tank && character->getPowerType() == POWER_MANA && character->GetMaxPower(POWER_MANA))
            {
                float const mana = character->GetPowerPct(POWER_MANA);
                if (mana < (role == DungeonRole::Healer ? ReadyHealerManaPct : ReadyDamageManaPct))
                    add(Acore::StringFormat("{} mana {:.0f}%", character->GetName(), mana));
            }
        }
        if (missing.empty())
            ++m.bossReady;
        LOG_INFO(ActivityLog, "activity: {}'s group pulls {} in {}: {}", bot.name, boss.name, plan.name, missing.empty() ?
            Acore::StringFormat("ready (all {} within {:.0f} yd of the tank, health and mana up)", characters.size(), ReadyYards) :
            "not ready: " + missing);
    }
}

std::string OrderService::DescribeDungeonMetrics(BotRuntime::DungeonRun::Metrics const& m)
{
    return Acore::StringFormat("metrics: {} fights, a member in a fight before the tank in {} ({}%); 5 s or more after the tank: melee {} of {} "
        "({}%), healer {} of {} ({}%), others {} of {} ({}%); 15 yd or more from the tank 5 s into a fight: {} of {} ({}%); bosses pulled "
        "with the whole group ready at the tank: {} of {}; waits for a "
        "member over 60 yd behind: {}; went on without a resting member: {}; walked {:.0f} yd; gathered at the tank before a boss or a big "
        "pull {} time{} ({} s); the tank stood for its group {} time{}, {} member{} put beside it; {} taunt{}, {} area threat{}, {} run{} to "
        "the tank; the healer healed through in {} fight{}; bosses that fell before they were judged: {}; the group rested together {} "
        "time{}, the healer walked {} time{} to see the tank; packs on the way walked into {}, pulled to a camp {}, big pulls {} (at most {} creatures); the healer out of heal "
        "reach of the tank (40 yd, line of sight) 5 s into a fight: {} of {} ({}%); 5 s or more after the group was free to fight (the tank had "
        "the creatures, or its lead time was over): melee {} of {} ({}%), others {} of {} ({}%); a member in a fight before the tank by its "
        "own attack (nothing was attacking it): {} ({}%)", uint32(m.fights),
        uint32(m.memberFirst),
        Percent(m.memberFirst, m.fights), uint32(m.meleeLate), uint32(m.meleeIn), Percent(m.meleeLate, m.meleeIn), uint32(m.healerLate),
        uint32(m.healerIn), Percent(m.healerLate, m.healerIn), uint32(m.othersLate), uint32(m.othersIn), Percent(m.othersLate, m.othersIn),
        uint32(m.placedFar), uint32(m.placedIn), Percent(m.placedFar, m.placedIn),
        uint32(m.bossReady), uint32(m.bossPulls), uint32(m.catchUps), uint32(m.wentOnResting), m.walked, uint32(m.gates),
        m.gates == 1 ? "" : "s", m.gateMs / IN_MILLISECONDS, uint32(m.spreadHalts), m.spreadHalts == 1 ? "" : "s", uint32(m.fetched),
        m.fetched == 1 ? "" : "s", uint32(m.taunts), m.taunts == 1 ? "" : "s", uint32(m.areaThreats), m.areaThreats == 1 ? "" : "s",
        uint32(m.toTank), m.toTank == 1 ? "" : "s", uint32(m.pumps), m.pumps == 1 ? "" : "s", uint32(m.bossUnseen),
        uint32(m.groupRests), m.groupRests == 1 ? "" : "s", uint32(m.healerMoves), m.healerMoves == 1 ? "" : "s", uint32(m.walkIns), uint32(m.campPulls),
        uint32(m.bigPulls), uint32(m.bigPullMost), uint32(m.healerOut), uint32(m.healerReach), Percent(m.healerOut, m.healerReach), uint32(m.meleeFreeLate), uint32(m.meleeIn),
        Percent(m.meleeFreeLate, m.meleeIn), uint32(m.othersFreeLate), uint32(m.othersIn), Percent(m.othersFreeLate, m.othersIn),
        uint32(m.memberPulled), Percent(m.memberPulled, m.fights));
}

void OrderService::StepDungeonSkull(AdventureGuild& guild, BotRuntime::DungeonRun& run, std::vector<Player*> const& group, Player* tank,
    std::vector<Creature*> const& fight)
{
    // One target for all (package P5): the skull marks it, and mod-playerbots' damage dealers and tank take the skull
    // first. A creature on the healer (the owner's DP6: the group's first concern), else the tank's own target when it is a
    // creature of this fight.
    Creature* mark = nullptr;
    Player* healer = nullptr;
    for (Player* member : group)
        if (GuildBot const* memberBot = guild.FindMember(member->GetGUID()); memberBot && !healer && member->IsAlive() &&
            sRoleService.EffectiveRole(*memberBot) == DungeonRole::Healer)
            healer = member;
    // The Twin Emperors (Temple of Ahn'Qiraj): Vek'nilash takes no magic damage, Vek'lor no physical damage, and what one
    // of them takes the other takes too (Twin Empathy, by the share of its health). The skull marks Vek'nilash, the cross
    // Vek'lor, and who deals magic damage takes the cross (a mage, warlock or priest, a druid or shaman that fights from
    // afar); the others the skull. Back to the skull for all when they are dead or out of the fight.
    {
        constexpr uint32 VeknilashEntry = 15275, VeklorEntry = 15276;
        Creature* nilash = tank->FindNearestCreature(VeknilashEntry, 250.0f, true);
        Creature* lor = tank->FindNearestCreature(VeklorEntry, 250.0f, true);
        Group* party = tank->GetGroup();
        if (party && nilash && lor && (nilash->IsInCombat() || lor->IsInCombat()))
        {
            if (party->GetTargetIcon(SkullIcon) != nilash->GetGUID())
                party->SetTargetIcon(SkullIcon, tank->GetGUID(), nilash->GetGUID());
            if (party->GetTargetIcon(CrossIcon) != lor->GetGUID())
                party->SetTargetIcon(CrossIcon, tank->GetGUID(), lor->GetGUID());
            run.skull = nilash->GetGUID();
            for (Player* member : group)
            {
                if (!member->IsAlive() || !sGuildBotHolder.IsAIPlayed(member->GetGUID()))
                    continue;
                uint8 const playerClass = member->getClass();
                bool const magic = playerClass == CLASS_MAGE || playerClass == CLASS_WARLOCK || playerClass == CLASS_PRIEST ||
                    ((playerClass == CLASS_DRUID || playerClass == CLASS_SHAMAN) && !BotDriver::FightsInMelee(member));
                BotDriver::SetRti(member, magic ? "cross" : "skull");
            }
            if (!run.twinMarks)
                LOG_INFO(ActivityLog, "activity: the raid fights the Twin Emperors: the skull on {}, the cross on {} (who deals magic "
                    "damage takes the cross)", nilash->GetName(), lor->GetName());
            run.twinMarks = true;
            return;
        }
        if (run.twinMarks)
        {
            run.twinMarks = false;
            for (Player* member : group)
                if (sGuildBotHolder.IsAIPlayed(member->GetGUID()))
                    BotDriver::SetRti(member, "skull");
            if (party)
                party->SetTargetIcon(CrossIcon, tank->GetGUID(), ObjectGuid::Empty);
        }
    }
    // Viscidus's globs first, the one nearest to him: shattered, he comes back with 5 % of his health for each glob that
    // reaches him, and dies at once when he is shattered at 5 % or less (boss_viscidus). They attack nobody, so they are
    // no creatures of the fight until somebody hits them: the raid of 40 killed 5 of 15 and he came back at 50 %,
    // 2026-11-23.
    {
        constexpr uint32 GlobEntry = 15667, ViscidusEntry = 15299;
        std::list<Creature*> globs;
        tank->GetCreatureListWithEntryInGrid(globs, GlobEntry, 100.0f);
        Creature const* viscidus = tank->FindNearestCreature(ViscidusEntry, 150.0f, true);
        for (Creature* glob : globs)
            if (glob->IsAlive() && (!mark || (viscidus ? glob->GetExactDist2d(viscidus) < mark->GetExactDist2d(viscidus) :
                tank->GetExactDist2d(glob) < tank->GetExactDist2d(mark))))
                mark = glob;
    }
    if (healer && !mark)
        for (Creature* creature : fight)
            if (creature->IsAlive() && creature->GetVictim() == healer && (!mark || creature->GetHealth() < mark->GetHealth()))
                mark = creature;
    // Adds a raid kills first, the one with the least health.
    if (!mark)
        for (Creature* creature : fight)
            if (creature->IsAlive() && KilledFirst(creature) && (!mark || creature->GetHealth() < mark->GetHealth()))
                mark = creature;
    if (!mark)
        if (Unit* victim = tank->GetVictim(); victim && victim->IsAlive())
            for (Creature* creature : fight)
                if (creature == victim)
                    mark = creature;
    if (!mark || mark->GetGUID() == run.skull)
        return;
    if (Group* party = tank->GetGroup())
    {
        party->SetTargetIcon(SkullIcon, tank->GetGUID(), mark->GetGUID());
        run.skull = mark->GetGUID();
    }
}

void OrderService::StepDungeonFight(AdventureGuild& guild, GuildBot& bot, Player* player, BotRuntime& rt)
{
    BotRuntime::DungeonRun& run = rt.dungeon;
    if (!run.id || run.phase != PhaseInside)
        return;
    DungeonFightRules const rules = GetFightRules(bot.guid);
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    std::vector<Player*> const group = DungeonGroup(guild, bot.guid);
    // The others into the leader's instance, every tick (also while the leader fights, when the run's own step does
    // not come).
    if (DungeonPlan const* plan = sDungeons.GetPlan(run.id))
    {
        Player* guest = DungeonGuest(bot.guid);
        if (guest && (!GetSettings().dungeonsTakeOwnerAlong || !GetSettings().dungeonsTeleport ||
            !sGuildBotHolder.HasPlaceholder(guest->GetGUID())))
            guest = nullptr;
        StepDungeonEnter(bot, player, run, *plan, group, guest);
        if (run.tribute)
            StepTributeFight(run, group, bot.name, plan->name);
        StepDungeonFlames(run, group, plan->name);
        StepWayTriggers(run, group, plan->name);
        // (Creatures that burst at their death are in dungeons too: the Unstable Corpses of Scholomance.)
        if (!plan->raid)
            StepRaidBursts(run, group, plan->name);
        StepDungeonPulsers(run, group, plan->name);
        if (plan->raid)
        {
            // The Temple of Ahn'Qiraj's poison (Lord Kri, Viscidus, Huhuran, the Eye's beam): its shamans set Cleansing
            // Totem and Nature Resistance Totem (the owner, 2026-11-24: the Alliance has shamans too).
            if (plan->mapId == 531)
                for (Player* member : group)
                    if (member->IsAlive() && member->GetMapId() == plan->mapId && sGuildBotHolder.IsAIPlayed(member->GetGUID()) &&
                        BotDriver::SetTotems(member, "cleansing", "nature resistance") && !run.burstLogged.count(8170))
                    {
                        run.burstLogged.insert(8170);
                        LOG_INFO(ActivityLog, "activity: the shamans of the raid in {} set Cleansing Totem and Nature Resistance Totem "
                            "against its poison", plan->name);
                    }
            StepRaidBursts(run, group, plan->name);
            StepRaidTwins(run, group, plan->name);
            StepRaidViscidus(run, group, plan->name);
            StepRaidCthun(run, group, plan->name);
            StepRaidTankSwap(run, group, plan->name);
            StepRaidOssirian(run, group, plan->name);
            StepRaidHakkar(run, group, plan->name);
            StepRaidRazorgore(run, group, *plan, player);
        }
    }
    // The tank: the first character of the group that tanks (the leader comes first).
    Player* tank = nullptr;
    for (Player* member : group)
        if (GuildBot const* memberBot = guild.FindMember(member->GetGUID()))
            if (!tank && member->IsAlive() && sRoleService.EffectiveRole(*memberBot) == DungeonRole::Tank)
                tank = member;
    BotRuntime const* tankRuntime = tank ? FindRuntime(tank->GetGUID()) : nullptr;
    if (run.gather.active)
        if (DungeonPlan const* plan = sDungeons.GetPlan(run.id))
            StepDungeonGather(bot, player, rt, *plan, rules);
    bool const pulling = (tankRuntime && tankRuntime->pull.active) || run.gather.active;
    bool fighting = false;
    for (Player* member : group)
        fighting = fighting || (member->IsAlive() && member->IsInCombat());
    // An event that brings a boss is going on (StepDungeonEvent): what attacks its NPC is everybody's target, every
    // second (the run's own step does not come while its leader fights, rests or waits for its group).
    if (run.eventSince && !run.eventBossSeen)
        if (DungeonPlan const* plan = sDungeons.GetPlan(run.id); plan && run.boss < RunBossCount(run, *plan) && RunBoss(run, *plan, run.boss).talkEntry)
            if (Creature* npc = player->FindNearestCreature(RunBoss(run, *plan, run.boss).talkEntry, 200.0f, true))
            {
                run.eventNpcSeen = now;
                Unit* attacker = nullptr;
                for (Unit* unit : npc->getAttackers())
                    if (unit && unit->IsAlive() && unit->GetTypeId() == TYPEID_UNIT && (!attacker || unit->GetHealth() < attacker->GetHealth()))
                        attacker = unit;
                if (attacker)
                    for (Player* member : group)
                    {
                        Unit const* victim = member->GetVictim();
                        if (member->IsAlive() && (!victim || !victim->IsAlive()) && member->IsWithinDistInMap(attacker, 60.0f))
                            BotDriver::Attack(member, attacker);
                    }
                // An NPC that walks off does not wait: a leader that has fallen behind (a fight, a place players jump
                // or swim at and the path finder has no way through) is put beside it with its group, like the hop
                // to a boss without a way on foot. (The Disciple of Naralex was lost 2 minutes into its walk.)
                if (RunBoss(run, *plan, run.boss).follow && !attacker && player->IsAlive() && !player->IsInCombat() &&
                    !player->IsWithinDistInMap(npc, 35.0f) && now >= run.eventNextHop && GetSettings().dungeonsHopWhenNoWay)
                {
                    run.eventNextHop = now + 8 * IN_MILLISECONDS;
                    if (!run.eventHops++)
                        LOG_INFO(ActivityLog, "activity: {}'s group fell {:.0f} yd behind {} in {}: it is put beside it (and again whenever "
                            "it falls behind on this walk)", bot.name, player->GetDistance(npc), npc->GetName(), plan->name);
                    TakeGuestAlong(bot, player, npc->GetPositionX(), npc->GetPositionY(), npc->GetPositionZ(),
                        Acore::StringFormat("it fell behind {}", npc->GetName()));
                    for (Player* member : group)
                        if (member->IsAlive() && !member->IsInCombat())
                        {
                            member->NearTeleportTo(npc->GetPositionX(), npc->GetPositionY(), npc->GetPositionZ(), npc->GetOrientation());
                            if (BotRuntime* other = FindRuntime(member->GetGUID()))
                                other->hasLastPosition = false;
                        }
                }
            }
    if (!fighting && !pulling)
    {
        run.fightSince = 0;
        run.heldThisFight = false;
        run.rescueLogged.clear();
        run.toTankAt.clear();
        if (!run.skull.IsEmpty())
        {
            if (Group* party = player->GetGroup())
                party->SetTargetIcon(SkullIcon, player->GetGUID(), ObjectGuid::Empty);
            run.skull.Clear();
        }
        return;
    }
    if (!run.fightSince)
        run.fightSince = now;
    std::vector<Creature*> const fight = CreaturesOfTheFight(group);

    // A fight in which nothing happens: no creature is on anybody of the group and nobody casts or has a living target
    // within 40 yd, for 45 s, while the boss the group is after is in no fight. The game counts the group as fighting
    // for as long as a creature holds a combat reference to it, and a group stood "in combat" so for 25 minutes before
    // Mograine's room (roles instance, 14:45) while its leader waited for a member that "fights". The fight is ended
    // for everybody of the group. (The planner's own rule for this is off in dungeons, for a boss that talks: here the
    // boss is in no fight.)
    {
        bool happening = pulling;
        for (Creature* creature : fight)
            if (Unit const* victim = creature->GetVictim())
                for (Player* member : group)
                    happening = happening || victim == member || victim == member->GetPet();
        for (Player* member : group)
        {
            Unit const* target = member->GetVictim();
            happening = happening || (member->IsAlive() && member->IsNonMeleeSpellCast(false)) ||
                (target && target->IsAlive() && member->IsWithinDistInMap(target, 40.0f));
        }
        if (!happening)
            if (DungeonPlan const* plan = sDungeons.GetPlan(run.id); plan && run.boss < RunBossCount(run, *plan))
                if (Creature const* boss = player->FindNearestCreature(RunBoss(run, *plan, run.boss).entry, 150.0f, true))
                    happening = boss->IsInCombat();
        constexpr uint32 StaleFightMs = 45 * IN_MILLISECONDS;
        if (happening)
            run.staleSince = 0;
        else if (!run.staleSince)
            run.staleSince = now;
        else if (getMSTimeDiff(run.staleSince, now) >= StaleFightMs)
        {
            std::string holders;
            for (Player* member : group)
                for (auto const& [guid, ref] : member->GetCombatManager().GetPvECombatRefs())
                    if (Unit* other = ref->GetOther(member); other && holders.size() < 300)
                    {
                        Creature* holder = other->ToCreature();
                        holders += Acore::StringFormat("{}{} with {} ({:.0f} yd{}{})", holders.empty() ? "" : ", ", member->GetName(), other->GetName(),
                            member->GetExactDist(other), other->IsAlive() ? "" : ", dead", holder && holder->IsInEvadeMode() ? ", evading" :
                            (holder && holder->CanNotReachTarget() ? ", cannot reach its target" : ""));
                    }
            LOG_INFO(ActivityLog, "activity: the group of {} has been in a fight for {} s in which nothing has happened for {} s ({}): "
                "the module ends the fight", bot.name, getMSTimeDiff(run.fightSince, now) / IN_MILLISECONDS,
                getMSTimeDiff(run.staleSince, now) / IN_MILLISECONDS, holders.empty() ? "no creature holds it" : holders);
            for (Player* member : group)
            {
                if (Pet* pet = member->GetPet())
                    pet->CombatStop(true);
                member->CombatStop(true);
            }
            run.staleSince = 0;
            return;
        }
    }

    // While the tank pulls and takes the creatures, the others hold back (the owner, 2026-11-13): until every creature
    // of the fight attacks the tank, at most TankLeadSeconds; the whole time of a careful pull. A character that
    // is attacked itself fights. The healer heals when somebody is in danger.
    bool held = false;
    if (tank && rules.tankLeadSeconds)
    {
        bool hold = pulling;
        if (!hold && getMSTimeDiff(run.fightSince, now) < rules.tankLeadSeconds * IN_MILLISECONDS && tank->GetHealthPct() > 35.0f)
        {
            bool onTank = !fight.empty();
            for (Creature* creature : fight)
                onTank = onTank && creature->GetVictim() == tank;
            hold = !onTank;
        }
        if (hold)
        {
            bool emergency = false;
            for (Player* member : group)
                emergency = emergency || (member->IsAlive() && member->GetHealthPct() < float(rules.emergencyHealth));
            // The healer holds back only until a creature of the fight is on somebody of the group (the owner, 2026-11-14:
            // [quote left out]): a careful pull of a room creature
            // held it for the whole pull, also while the creature hit the tank.
            bool contact = false;
            for (Creature* creature : fight)
                if (Unit const* victim = creature->GetVictim())
                    for (Player* member : group)
                        contact = contact || victim == member;
            for (Player* member : group)
            {
                GuildBot const* memberBot = guild.FindMember(member->GetGUID());
                if (member == tank || !memberBot || !member->IsAlive())
                    continue;
                if ((emergency || contact) && sRoleService.EffectiveRole(*memberBot) == DungeonRole::Healer)
                    continue;
                // Attacked itself: it defends itself.
                bool attacked = false;
                for (Creature* creature : fight)
                    attacked = attacked || creature->GetVictim() == member;
                if (attacked)
                    continue;
                if (member->GetVictim())
                    member->AttackStop();
                BotDriver::PauseAI(member, 1500);
            }
            if (!run.heldThisFight)
            {
                run.heldThisFight = true;
                if (!run.holds++)
                    LOG_INFO(ActivityLog, "activity: the group of {} holds back while its tank {} {} (at most {} s per fight; the "
                        "healer heals below {}% health until a creature is on somebody)", bot.name, tank->GetName(),
                        pulling ? "pulls" : "takes the creatures",
                        rules.tankLeadSeconds, rules.emergencyHealth);
            }
        }
        held = hold;
    }
    if (!held && run.metrics.inFight && !run.metrics.releasedAt)
        run.metrics.releasedAt = now;

    StepDungeonRescue(guild, bot, player, run, group, tank, pulling, held, now);
    StepDungeonSkull(guild, run, group, tank ? tank : player, fight);

    // A creature that runs away (the owner, 2026-11-13): everybody in reach turns to it, so that it brings nobody back.
    // It is followed only while that pulls nothing: no creature outside the fight within its reach of the runner.
    if (!rules.focusFleeing || pulling)
        return;
    constexpr uint32 FleePatienceMs = 20 * IN_MILLISECONDS;
    for (Creature* runner : fight)
    {
        if (!runner->HasUnitState(UNIT_STATE_FLEEING))
            continue;
        // (A creature that keeps running, or that got stuck somewhere while it ran: after a while the rule lets go
        // of it and mod-playerbots deals with it as it always did.)
        auto const seen = run.fleeSeen.find(runner->GetGUID());
        if (seen != run.fleeSeen.end() && getMSTimeDiff(seen->second, now) > FleePatienceMs)
            continue;
        if (seen == run.fleeSeen.end())
            run.fleeSeen[runner->GetGUID()] = now;
        Creature* inTheWay = nullptr;
        for (Creature* other : HostilesAround(runner, player, 30.0f))
            if (other != runner && !other->IsInCombat() && other->IsWithinDistInMap(runner, other->GetAggroRange(player) + 6.0f))
                inTheWay = other;
        // How far it has run from the fight: a chase down a corridor passes creatures the check above does not see
        // (a Stockade group that followed its runners past the cells was wiped out), so nobody follows far.
        constexpr float ChaseYards = 18.0f;
        Player const* anchor = tank ? tank : player;
        bool const ranFar = !runner->IsWithinDistInMap(anchor, ChaseYards);
        uint32 others = 0;
        for (Creature* creature : fight)
            others += creature != runner && !creature->HasUnitState(UNIT_STATE_FLEEING) ? 1 : 0;
        uint32 turned = 0;
        for (Player* member : group)
        {
            GuildBot const* memberBot = guild.FindMember(member->GetGUID());
            if (!memberBot || !member->IsAlive() ||
                sRoleService.EffectiveRole(*memberBot) == DungeonRole::Healer)
                continue;
            // The tank stays with the creatures it holds; it turns to the runner only when nothing else is left.
            if (member == tank && others)
                continue;
            bool const melee = BotDriver::FightsInMelee(member);
            bool const mayFollow = !inTheWay && !ranFar;
            if (!melee || mayFollow)
            {
                // (A ranged character from where it stands; a melee character goes after it.)
                if (member->GetVictim() != runner && (melee || (member->IsWithinDistInMap(runner, 36.0f) && member->IsWithinLOSInMap(runner))) &&
                    BotDriver::Attack(member, runner))
                    ++turned;
                continue;
            }
            // It would pull more: a melee character lets the runner go and takes another creature of the fight.
            if (member->GetVictim() != runner)
                continue;
            Creature* instead = nullptr;
            for (Creature* creature : fight)
                if (creature != runner && !creature->HasUnitState(UNIT_STATE_FLEEING) && member->IsWithinDistInMap(creature, 20.0f))
                    instead = creature;
            if (!instead || !BotDriver::Attack(member, instead))
            {
                member->AttackStop();
                BotDriver::PauseAI(member, 1500);
            }
        }
        if (run.lastFleeing != runner->GetGUID())
        {
            run.lastFleeing = runner->GetGUID();
            ++run.fleeing;
            LOG_INFO(ActivityLog, "activity: the group of {} turns to {} that runs away ({} character{} turned to it; {})", bot.name,
                runner->GetName(), turned, turned == 1 ? "" : "s", inTheWay ?
                Acore::StringFormat("melee characters let it go: {} stands where it runs to", inTheWay->GetName()) :
                ranFar ? std::string("melee characters let it go: it has run too far from the fight") :
                std::string("it is followed: nothing else stands where it runs to"));
        }
    }
}

void OrderService::LoadDungeonsFinished()
{
    if (_dungeonsFinishedLoaded)
        return;
    _dungeonsFinishedLoaded = true;
    if (QueryResult result = CharacterDatabase.Query("SELECT owner_account, dungeon_id, runs_done FROM ga_guild_dungeon"))
        do
        {
            Field* fields = result->Fetch();
            _dungeonsFinished[fields[0].Get<uint32>()][fields[1].Get<uint32>()] = fields[2].Get<uint32>();
        } while (result->NextRow());
}

std::vector<uint32> OrderService::DungeonOrder(AdventureGuild const& guild) const
{
    std::vector<DungeonInfo const*> list;
    for (DungeonInfo const& info : sReadiness.GetDungeons())
    {
        DungeonPlan const* plan = sDungeons.GetPlan(info.id);
        if (info.raid || !plan || !plan->supported || (info.team < 2 && info.team != uint8(guild.team)))
            continue;
        list.push_back(&info);
    }
    // (At the same lowest level the dungeon of the guild's own continent comes first: the Deadmines before the
    // Wailing Caverns for the Alliance, the other way round for the Horde.)
    uint32 const homeMap = guild.team == TEAM_ALLIANCE ? 0 : 1;
    auto away = [homeMap](DungeonInfo const* info)
    {
        DungeonPlan const* plan = sDungeons.GetPlan(info->id);
        return plan && plan->outMap != homeMap ? 1 : 0;
    };
    std::sort(list.begin(), list.end(), [&away](DungeonInfo const* a, DungeonInfo const* b)
    {
        if (a->minLevel != b->minLevel)
            return a->minLevel < b->minLevel;
        if (away(a) != away(b))
            return away(a) < away(b);
        if (a->targetLevel != b->targetLevel)
            return a->targetLevel < b->targetLevel;
        return a->id < b->id;
    });
    std::vector<uint32> order;
    for (DungeonInfo const* info : list)
        order.push_back(info->id);
    return order;
}

uint32 OrderService::GetDungeonsFinished(AdventureGuild const& guild, uint32 dungeonId)
{
    LoadDungeonsFinished();
    auto owner = _dungeonsFinished.find(guild.ownerAccount);
    if (owner == _dungeonsFinished.end())
        return 0;
    auto itr = owner->second.find(dungeonId);
    return itr != owner->second.end() ? itr->second : 0;
}

uint32 OrderService::GetDungeonProgressVersion(AdventureGuild const& guild)
{
    LoadDungeonsFinished();
    uint32 version = 0;
    if (auto owner = _dungeonsFinished.find(guild.ownerAccount); owner != _dungeonsFinished.end())
        for (auto const& [dungeon, runs] : owner->second)
            version += runs;
    return version;
}

void OrderService::SetDungeonsFinished(AdventureGuild const& guild, uint32 dungeonId, uint32 runs)
{
    LoadDungeonsFinished();
    if (runs)
    {
        _dungeonsFinished[guild.ownerAccount][dungeonId] = runs;
        uint32 const stamp = uint32(GameTime::GetGameTime().count());
        CharacterDatabase.Execute("REPLACE INTO ga_guild_dungeon (owner_account, dungeon_id, runs_done, first_at, last_at) "
            "VALUES ({}, {}, {}, {}, {})", guild.ownerAccount, dungeonId, runs, stamp, stamp);
        return;
    }
    _dungeonsFinished[guild.ownerAccount].erase(dungeonId);
    CharacterDatabase.Execute("DELETE FROM ga_guild_dungeon WHERE owner_account = {} AND dungeon_id = {}", guild.ownerAccount, dungeonId);
}

bool OrderService::IsDungeonUnlocked(AdventureGuild const& guild, uint32 dungeonId, std::string* need)
{
    if (!GetSettings().dungeonsUnlockInOrder)
        return true;
    std::vector<uint32> const order = DungeonOrder(guild);
    auto itr = std::find(order.begin(), order.end(), dungeonId);
    // (A dungeon outside the order cannot be run at all: that is said elsewhere.)
    if (itr == order.end() || itr == order.begin() || GetDungeonsFinished(guild, dungeonId))
        return true;
    uint32 const before = *(itr - 1);
    if (GetDungeonsFinished(guild, before))
        return true;
    if (need)
        if (DungeonPlan const* plan = sDungeons.GetPlan(before))
            *need = plan->name;
    return false;
}

void OrderService::LoadDungeonGroups()
{
    if (_dungeonGroupsLoaded)
        return;
    _dungeonGroupsLoaded = true;
    if (QueryResult result = CharacterDatabase.Query("SELECT leader_guid, owner_account, dungeon_id, prev_order, prev_zone FROM ga_dungeon_group"))
        do
        {
            Field* fields = result->Fetch();
            DungeonGroupState& state = _dungeonGroups[fields[0].Get<uint32>()];
            state.owner = fields[1].Get<uint32>();
            state.dungeon = fields[2].Get<uint32>();
            state.prevOrder = fields[3].Get<std::string>();
            state.prevZone = fields[4].Get<uint32>();
        } while (result->NextRow());
    // The preparation step's choices of a group (a table of its own: stage C came after the groups).
    if (QueryResult result = CharacterDatabase.Query("SELECT leader_guid, items, fight FROM ga_dungeon_group_options"))
        do
        {
            Field* fields = result->Fetch();
            auto group = _dungeonGroups.find(fields[0].Get<uint32>());
            if (group == _dungeonGroups.end())
                continue;
            DungeonGroupState& state = group->second;
            std::string const itemText = fields[1].Get<std::string>();
            std::string const ruleText = fields[2].Get<std::string>();
            // items: "" or "-a,b" every kind but those; "a,b" only those (the ticks of an older addon).
            state.options.listed = !itemText.empty() && itemText[0] != '-';
            for (std::string_view token : Acore::Tokenize(std::string_view(itemText).substr(state.options.listed || itemText.empty() ? 0 : 1), ',', false))
                if (Optional<uint32> entry = Acore::StringTo<uint32>(token))
                    (state.options.listed ? state.options.items : state.options.excluded).push_back(*entry);
            // fight: tank lead seconds, emergency health, careful pulls, pack, focus fleeing[, mode, gather yards, gather
            // count] ("" = the server's values).
            std::vector<std::string_view> const rules = Acore::Tokenize(ruleText, ',', false);
            if (rules.size() >= 5 && rules.size() <= 8)
            {
                if (rules.size() >= 6)
                    state.options.rules.mode = uint8(std::min<uint32>(Acore::StringTo<uint32>(rules[5]).value_or(0), ModeTribute));
                if (rules.size() >= 8)
                {
                    state.options.rules.gatherYards = std::clamp<uint32>(Acore::StringTo<uint32>(rules[6]).value_or(25), 10, 40);
                    state.options.rules.gatherCount = std::clamp<uint32>(Acore::StringTo<uint32>(rules[7]).value_or(6), 2, 15);
                }
                state.options.hasRules = true;
                state.options.rules.tankLeadSeconds = Acore::StringTo<uint32>(rules[0]).value_or(4);
                state.options.rules.emergencyHealth = Acore::StringTo<uint32>(rules[1]).value_or(50);
                state.options.rules.carefulPulls = Acore::StringTo<uint32>(rules[2]).value_or(1) != 0;
                state.options.rules.carefulPullPack = Acore::StringTo<uint32>(rules[3]).value_or(3);
                state.options.rules.focusFleeing = Acore::StringTo<uint32>(rules[4]).value_or(1) != 0;
            }
        } while (result->NextRow());
    if (QueryResult result = CharacterDatabase.Query("SELECT leader_guid, member_guid, prev_leader_guid, in_group FROM ga_dungeon_group_member"))
        do
        {
            Field* fields = result->Fetch();
            auto itr = _dungeonGroups.find(fields[0].Get<uint32>());
            if (itr == _dungeonGroups.end())
                continue;
            DungeonGroupState::Link link;
            link.member = fields[1].Get<uint32>();
            link.prevLeader = fields[2].Get<uint32>();
            link.inGroup = fields[3].Get<uint8>() != 0;
            link.lentOrder = fields[3].Get<uint8>() == 2;
            itr->second.links.push_back(link);
        } while (result->NextRow());
}

OrderService::DungeonFightRules OrderService::DefaultFightRules()
{
    Settings const& settings = GetSettings();
    DungeonFightRules rules;
    rules.tankLeadSeconds = settings.dungeonsTankLeadSeconds;
    rules.emergencyHealth = settings.dungeonsEmergencyHealth;
    rules.carefulPulls = settings.dungeonsCarefulPulls;
    rules.carefulPullPack = settings.dungeonsCarefulPullPack;
    rules.focusFleeing = settings.dungeonsFocusFleeing;
    return rules;
}

void OrderService::StepDungeonGather(GuildBot const& bot, Player* leader, BotRuntime& rt, DungeonPlan const& plan,
    DungeonFightRules const& rules)
{
    // A big pull of the run window's mode "big pulls" (the owner, 2026-11-19: [quote left out]): the tank goes from creature to creature of the
    // cluster StepDungeonWay chose, the nearest first, until gatherCount creatures are on it, and brings them to its group.
    // Its AI is held meanwhile (it would stand and fight the first one); the others wait where they stand, the healer
    // heals it (StepDungeonConductor, StepDungeonFight).
    BotRuntime::DungeonRun::Gather& gather = rt.dungeon.gather;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    constexpr uint32 HoldMs = 1500;
    constexpr uint32 TargetMs = 8 * IN_MILLISECONDS;   // the most it goes for one creature
    constexpr uint32 GatherMs = 30 * IN_MILLISECONDS;  // the most a gathering takes
    constexpr uint32 BackMs = 15 * IN_MILLISECONDS;    // the most the way back takes
    constexpr float LowHealth = 50.0f;                 // below it the tank goes back at once
    if (!leader->IsAlive())
    {
        gather.active = false;
        BotDriver::PauseAI(leader, 0);
        return;
    }
    uint32 on = 0;
    for (Unit* unit : leader->getAttackers())
        if (unit && unit->IsAlive())
            ++on;
    gather.most = uint8(std::max<uint32>(gather.most, std::min<uint32>(on, 255)));
    auto moveTo = [&](float x, float y, float z)
    {
        BotDriver::PauseAI(leader, HoldMs);
        if (now >= gather.nextMove || !leader->isMoving())
        {
            leader->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_RUN);
            gather.nextMove = now + IN_MILLISECONDS;
        }
    };
    if (!gather.back)
    {
        std::string why;
        if (on >= rules.gatherCount)
            why = Acore::StringFormat("{} creatures are on it", on);
        else if (leader->GetHealthPct() < LowHealth)
            why = Acore::StringFormat("its health is down to {:.0f}%", leader->GetHealthPct());
        else if (getMSTimeDiff(gather.since, now) >= GatherMs)
            why = "the time is up";
        Creature* next = nullptr;
        if (why.empty())
        {
            Creature* current = gather.target.IsEmpty() ? nullptr : ObjectAccessor::GetCreature(*leader, gather.target);
            if (current && current->IsAlive() && !current->IsInCombat() && getMSTimeDiff(gather.targetSince, now) < TargetMs)
                next = current;
            else
            {
                // (Taken, dead, or not taken in its time: the next of the cluster.)
                if (!gather.target.IsEmpty())
                    gather.targets.erase(std::remove(gather.targets.begin(), gather.targets.end(), gather.target), gather.targets.end());
                gather.target.Clear();
                for (ObjectGuid const& guid : gather.targets)
                    if (Creature* creature = ObjectAccessor::GetCreature(*leader, guid); creature && creature->IsAlive() &&
                        !creature->IsInCombat() && (!next || leader->GetExactDist(creature) < leader->GetExactDist(next)))
                        next = creature;
                if (next)
                {
                    gather.target = next->GetGUID();
                    gather.targetSince = now;
                    gather.nextMove = 0;
                }
                else
                    why = "the cluster is gathered";
            }
        }
        if (why.empty())
        {
            moveTo(next->GetPositionX(), next->GetPositionY(), next->GetPositionZ());
            rt.why = Acore::StringFormat("In {}: gathering a big pull ({} on it)", plan.name, on);
            return;
        }
        gather.back = true;
        gather.backSince = now;
        gather.nextMove = 0;
        LOG_INFO(ActivityLog, "activity: {} brings its big pull back to its group in {}: {} ({} creature{} on it)", bot.name, plan.name, why,
            on, on == 1 ? "" : "s");
    }
    if (leader->GetExactDist(gather.campX, gather.campY, gather.campZ) > 4.0f && getMSTimeDiff(gather.backSince, now) < BackMs)
    {
        moveTo(gather.campX, gather.campY, gather.campZ);
        rt.why = Acore::StringFormat("In {}: bringing its big pull to the group ({} on it)", plan.name, on);
        return;
    }
    // At the group: the tank's AI takes the creatures, the nearest first.
    gather.active = false;
    BotDriver::PauseAI(leader, 0);
    rt.dungeon.metrics.bigPullMost = std::max<uint16>(rt.dungeon.metrics.bigPullMost, gather.most);
    Unit* first = nullptr;
    for (Unit* unit : leader->getAttackers())
        if (unit && unit->IsAlive() && (!first || leader->GetExactDist(unit) < leader->GetExactDist(first)))
            first = unit;
    if (first)
        BotDriver::Attack(leader, first, false);
    LOG_INFO(ActivityLog, "activity: {} is back with its group in {} after {} s: {} creature{} on it at most", bot.name, plan.name,
        getMSTimeDiff(gather.since, now) / IN_MILLISECONDS, uint32(gather.most), gather.most == 1 ? "" : "s");
}

OrderService::DungeonFightRules OrderService::GetFightRules(ObjectGuid leader)
{
    LoadDungeonGroups();
    auto itr = _dungeonGroups.find(leader.GetCounter());
    return itr != _dungeonGroups.end() && itr->second.options.hasRules ? itr->second.options.rules : DefaultFightRules();
}

std::vector<uint32> OrderService::GetRunItems(ObjectGuid leader)
{
    LoadDungeonGroups();
    auto itr = _dungeonGroups.find(leader.GetCounter());
    // (Not the Frost Oil of a tribute run's trap: the group put it on a weapon at the start of the first test run, and
    // Slip'kik had to be fought, 2026-11-21.)
    constexpr uint32 TrapFrostOil = 3829;
    bool const tribute = itr != _dungeonGroups.end() && itr->second.options.rules.mode == ModeTribute;
    if (itr != _dungeonGroups.end() && itr->second.options.listed)
    {
        std::vector<uint32> items = itr->second.options.items;
        if (tribute)
            items.erase(std::remove(items.begin(), items.end(), TrapFrostOil), items.end());
        return items;
    }
    // Every kind the guild storage holds for a run, but what the leader took out in the preparation step. A run
    // that was not prepared in the run window: every kind too, or none (GuildAdventure.Dungeons.SuppliesByDefault).
    AdventureGuild const* guild = sGuildAdventureMgr.FindGuildOfMember(leader);
    if (!guild || (itr == _dungeonGroups.end() && !GetSettings().dungeonsSuppliesByDefault))
        return {};
    // (Kinds that lie wholly within a reserve of the storage count only while a run may use the reserves.)
    std::optional<StorageService::OwnerOrder> intoReserve;
    if (sBotSettings.GetForGuild(guild->ownerAccount, Setting::StorageReserveForDungeons))
        intoReserve.emplace();
    std::vector<uint32> items;
    for (SupplyItem const& supply : DungeonSupplies::List(guild->ownerAccount))
        if (itr == _dungeonGroups.end() || std::find(itr->second.options.excluded.begin(), itr->second.options.excluded.end(), supply.entry) ==
            itr->second.options.excluded.end() && !(tribute && supply.entry == TrapFrostOil))
            items.push_back(supply.entry);
    return items;
}

bool OrderService::StartDungeonGroup(AdventureGuild& guild, uint32 dungeonId, std::vector<ObjectGuid> const& chosen, ObjectGuid lead,
    uint32 runs, std::string& error, bool checkUnlock, DungeonRunOptions const* options)
{
    LoadDungeonGroups();
    DungeonPlan const* plan = sDungeons.GetPlan(dungeonId);
    DungeonInfo const* info = sReadiness.Find(dungeonId);
    if (!plan || !info)
        error = "that dungeon is not known";
    else if (!plan->supported)
        error = Acore::StringFormat("{} cannot be run yet: {}", plan->name, plan->why);
    else if (info->team < 2 && info->team != uint8(guild.team))
        error = Acore::StringFormat("{} lies in the other side's capital", plan->name);
    else if (chosen.empty() || chosen.size() > std::max<std::size_t>(info->size, 1))
        error = Acore::StringFormat("a group for {} has 1 to {} bots", plan->name, uint32(std::max<uint8>(info->size, 1)));
    else if (std::string need; checkUnlock && !IsDungeonUnlocked(guild, dungeonId, &need))
        error = Acore::StringFormat("{} is not open yet: the guild finishes {} first", plan->name, need);
    else if (options && options->rules.mode == ModeTribute && dungeonId != TributeDungeonId)
        error = "a tribute run is for Dire Maul North only";
    // A tribute run needs the trap's and the suit's materials in the guild storage (the owner's choice, 2026-11-21: else it
    // does not start, and what is missing becomes craft orders and shopping list entries).
    else if (std::string missing; options && options->rules.mode == ModeTribute && !(missing = CheckTributeStart(guild, chosen)).empty())
        error = Acore::StringFormat("the tribute run needs from the guild storage: {}", missing);
    if (!error.empty())
        return false;

    // Who: bots of the guild the AI plays now, each once, none on a run already, each ready enough; and, if the
    // owner wants to come along, the guild leader's own character, whoever plays it (the owner, 2026-11-13: [quote left out]).
    std::vector<GuildBot*> bots;
    std::string notReady;
    for (ObjectGuid guid : chosen)
    {
        GuildBot* bot = guild.FindMember(guid);
        bool const own = bot && guid == guild.leader;
        Player* player = bot ? ObjectAccessor::FindPlayer(guid) : nullptr;
        bool const byHand = player && player->GetSession() && !player->GetSession()->IsHeadless() && !sGuildBotHolder.IsAIPlayed(guid) &&
            !sGuildBotHolder.IsWatched(guid);
        if (!bot)
            error = "only characters of the guild can be in a dungeon group";
        else if (std::find(bots.begin(), bots.end(), bot) != bots.end())
            error = Acore::StringFormat("{} is in the group twice", bot->name);
        else if (!player || !player->IsInWorld())
            error = Acore::StringFormat("{} is not in the world", bot->name);
        else if (!own && (!BotDriver::GetAI(player) || !sGuildBotHolder.IsAIPlayed(guid)))
            error = Acore::StringFormat("you play {} yourself right now: a group starts with bots the AI plays", bot->name);
        else if (own && byHand && !GuildAdventure::IdleAI::Available())
            error = Acore::StringFormat("{} cannot join a group of bots while you play it yourself (see the server log at startup)", bot->name);
        else if (IsOnDungeonRun(guild, *bot))
            error = Acore::StringFormat("{} is on a dungeon run already", bot->name);
        // (The game's own door: the level the core asks for to let a character onto the dungeon's map.)
        else if (DungeonProgressionRequirements const* access = sObjectMgr->GetAccessRequirement(plan->mapId, Difficulty(0));
            access && player->GetLevel() < access->levelMin)
            error = Acore::StringFormat("{} is level {}: the game lets nobody below level {} into {}", bot->name, uint32(player->GetLevel()),
                uint32(access->levelMin), plan->name);
        if (!error.empty())
            return false;
        uint32 const percent = Readiness::Judge(player, *info).percent;
        if (percent < GetSettings().dungeonsMinReadiness)
            notReady += Acore::StringFormat("{}{} {}%", notReady.empty() ? "" : ", ", bot->name, percent);
        bots.push_back(bot);
    }
    if (!notReady.empty())
    {
        error = Acore::StringFormat("not ready for {} (at least {}%): {}", plan->name, GetSettings().dungeonsMinReadiness, notReady);
        return false;
    }

    // Who leads: the one the owner named, else the first tank, else the first. Always a bot: the guild leader's own
    // character comes along, it does not lead the run.
    GuildBot* leader = nullptr;
    for (GuildBot* bot : bots)
        if (bot->guid == lead && bot->guid != guild.leader)
            leader = bot;
    for (GuildBot* bot : bots)
        if (!leader && bot->guid != guild.leader && sRoleService.EffectiveRole(*bot) == DungeonRole::Tank)
            leader = bot;
    for (GuildBot* bot : bots)
        if (!leader && bot->guid != guild.leader)
            leader = bot;
    if (!leader)
    {
        error = "a dungeon group needs a bot that leads it";
        return false;
    }

    // What they are now: the squads of the chosen bots, and those of their followers that stay outside.
    DungeonGroupState state;
    state.owner = guild.ownerAccount;
    state.dungeon = dungeonId;
    state.prevOrder = ToToken(leader->order.type);
    state.prevZone = leader->order.zoneId;
    auto isChosen = [&](ObjectGuid guid)
    {
        return std::any_of(bots.begin(), bots.end(), [&](GuildBot const* bot) { return bot->guid == guid; });
    };
    for (GuildBot* bot : bots)
    {
        DungeonGroupState::Link link;
        link.member = bot->guid.GetCounter();
        link.prevLeader = sSquadService.GetLeader(bot->guid).GetCounter();
        link.inGroup = true;
        // (The guild leader's own character without an order holds its position; for the run it gets one, so that
        // the AI plays it as a member of the group whenever its owner does not.)
        link.lentOrder = bot->guid == guild.leader && !guild.leaderPlanned;
        state.links.push_back(link);
        for (ObjectGuid follower : sSquadService.GetMembers(bot->guid))
            if (!isChosen(follower))
            {
                DungeonGroupState::Link outside;
                outside.member = follower.GetCounter();
                outside.prevLeader = bot->guid.GetCounter();
                state.links.push_back(outside);
            }
    }
    auto restore = [&]()
    {
        std::string ignored;
        for (DungeonGroupState::Link const& link : state.links)
            if (link.inGroup)
                sSquadService.Set(guild, ObjectGuid::Create<HighGuid::Player>(link.member), ObjectGuid::Empty, ignored);
        for (DungeonGroupState::Link const& link : state.links)
            if (link.prevLeader)
                sSquadService.Set(guild, ObjectGuid::Create<HighGuid::Player>(link.member),
                    ObjectGuid::Create<HighGuid::Player>(link.prevLeader), ignored);
    };
    // Out of their squads, into the leader's (whatever their levels: a dungeon group has no level gap rule).
    struct AnyLevel
    {
        AnyLevel() { sSquadService.AllowAnyLevel(true); }
        ~AnyLevel() { sSquadService.AllowAnyLevel(false); }
    } anyLevel;
    std::string ignored;
    for (DungeonGroupState::Link const& link : state.links)
        if (link.prevLeader)
            sSquadService.Set(guild, ObjectGuid::Create<HighGuid::Player>(link.member), ObjectGuid::Empty, ignored);
    for (GuildBot* bot : bots)
        if (bot != leader && !sSquadService.Set(guild, bot->guid, leader->guid, error))
        {
            restore();
            error = Acore::StringFormat("{} cannot join the group of {}: {}", bot->name, leader->name, error);
            return false;
        }

    // The preparation step's choices: only kinds of items a run uses, and rules within their ranges.
    std::string itemList, ruleList;
    if (options)
    {
        state.options.listed = options->listed;
        std::vector<uint32>& kept = state.options.listed ? state.options.items : state.options.excluded;
        for (uint32 entry : state.options.listed ? options->items : options->excluded)
            if (DungeonSupplies::Classify(sObjectMgr->GetItemTemplate(entry)) != SupplyKind::None && kept.size() < 120 &&
                std::find(kept.begin(), kept.end(), entry) == kept.end())
            {
                kept.push_back(entry);
                itemList += (itemList.empty() ? "" : ",") + std::to_string(entry);
            }
        if (!state.options.listed)
            itemList = "-" + itemList;
        if (options->hasRules)
        {
            state.options.hasRules = true;
            state.options.rules.tankLeadSeconds = std::min<uint32>(options->rules.tankLeadSeconds, 30);
            state.options.rules.emergencyHealth = std::min<uint32>(options->rules.emergencyHealth, 100);
            state.options.rules.carefulPulls = options->rules.carefulPulls;
            state.options.rules.carefulPullPack = std::clamp<uint32>(options->rules.carefulPullPack, 2, 10);
            state.options.rules.focusFleeing = options->rules.focusFleeing;
            state.options.rules.mode = std::min<uint8>(options->rules.mode, ModeTribute);
            state.options.rules.gatherYards = std::clamp<uint32>(options->rules.gatherYards, 10, 40);
            state.options.rules.gatherCount = std::clamp<uint32>(options->rules.gatherCount, 2, 15);
            ruleList = Acore::StringFormat("{},{},{},{},{},{},{},{}", state.options.rules.tankLeadSeconds, state.options.rules.emergencyHealth,
                state.options.rules.carefulPulls ? 1 : 0, state.options.rules.carefulPullPack, state.options.rules.focusFleeing ? 1 : 0,
                uint32(state.options.rules.mode), state.options.rules.gatherYards, state.options.rules.gatherCount);
        }
    }
    _dungeonGroups[leader->guid.GetCounter()] = state;
    CharacterDatabase.Execute("REPLACE INTO ga_dungeon_group (leader_guid, owner_account, dungeon_id, prev_order, prev_zone, started_at) "
        "VALUES ({}, {}, {}, '{}', {}, {})", leader->guid.GetCounter(), state.owner, dungeonId, state.prevOrder, state.prevZone,
        uint32(GameTime::GetGameTime().count()));
    CharacterDatabase.Execute("DELETE FROM ga_dungeon_group_options WHERE leader_guid = {}", leader->guid.GetCounter());
    if (!itemList.empty() || !ruleList.empty())
        CharacterDatabase.Execute("INSERT INTO ga_dungeon_group_options (leader_guid, items, fight) VALUES ({}, '{}', '{}')",
            leader->guid.GetCounter(), itemList, ruleList);
    CharacterDatabase.Execute("DELETE FROM ga_dungeon_group_member WHERE leader_guid = {}", leader->guid.GetCounter());
    // (in_group 2: the guild leader's own character, which got its order for the run.)
    for (DungeonGroupState::Link const& link : state.links)
        CharacterDatabase.Execute("INSERT INTO ga_dungeon_group_member (leader_guid, member_guid, prev_leader_guid, in_group) "
            "VALUES ({}, {}, {}, {})", leader->guid.GetCounter(), link.member, link.prevLeader, link.lentOrder ? 2 : link.inGroup ? 1 : 0);

    std::string names;
    for (GuildBot* bot : bots)
        if (bot != leader)
            names += ", " + bot->name + (bot->guid == guild.leader ? " (the guild leader's own character)" : "");
    // The guild leader's own character: an order for the run when it had none, and, while its owner plays it by hand,
    // the placeholder AI before it joins the party of bots (GuildBotHolder::StartPlaceholder).
    for (GuildBot* bot : bots)
        if (bot->guid == guild.leader)
        {
            if (!guild.leaderPlanned)
            {
                BotOrder order;
                order.type = OrderType::Idle;
                SetLeaderOrder(guild, order);
            }
            if (Player* own = ObjectAccessor::FindPlayer(bot->guid); own && own->GetSession() && !own->GetSession()->IsHeadless() &&
                !sGuildBotHolder.IsAIPlayed(bot->guid) && !sGuildBotHolder.IsWatched(bot->guid))
            {
                sGuildBotHolder.StartPlaceholder(own);
                TellGuest(own, Acore::StringFormat("You are in the dungeon group of {} for {}. The group runs by itself; play along, "
                    "or let your character play (Watch).", leader->name, plan->name));
            }
        }
    LOG_INFO(ActivityLog, "activity: the guild leader sends a group into {}: {} (leads){}; {}{}{}", plan->name, leader->name, names,
        runs == DungeonRunsEndless ? std::string("run after run until it is stopped") :
        Acore::StringFormat("{} run{}", std::max<uint32>(runs, 1), runs > 1 ? "s" : ""),
        state.options.listed ? (state.options.items.empty() ? std::string("; it uses nothing out of the guild storage") :
            Acore::StringFormat("; it may use {} kind{} of items of the guild storage", state.options.items.size(),
                state.options.items.size() == 1 ? "" : "s")) :
        !options && !GetSettings().dungeonsSuppliesByDefault ? std::string() :
        Acore::StringFormat("; each character uses what fits it out of the guild storage{}", state.options.excluded.empty() ? std::string() :
            Acore::StringFormat(" ({} kind{} taken out by the leader)", state.options.excluded.size(), state.options.excluded.size() == 1 ? "" : "s")),
        !state.options.hasRules ? std::string() : Acore::StringFormat("; fight rules: wait for the tank {} s, emergency heals below {}%, "
            "careful pulls {}, fleeing creatures {}{}", state.options.rules.tankLeadSeconds, state.options.rules.emergencyHealth,
            state.options.rules.carefulPulls ? Acore::StringFormat("from {} creatures", state.options.rules.carefulPullPack) : std::string("off"),
            state.options.rules.focusFleeing ? "focused" : "left to each",
            state.options.rules.mode == ModeTribute ? "; mode: a tribute run (the named ogres stay alive, King Gordok dies)" :
            state.options.rules.mode == ModeRooms ? "; mode: every room on the way is cleared, one pack at a time" :
            state.options.rules.mode == ModeBigPulls ? Acore::StringFormat("; mode: every room on the way is cleared in big pulls (what stands "
                "within {} yd, up to {} creatures)", state.options.rules.gatherYards, state.options.rules.gatherCount) :
            std::string("; mode: the bosses, only what would attack the group on its way")));

    leader->order.type = OrderType::Dungeon;
    leader->order.zoneId = dungeonId;
    leader->order.setAt = uint32(GameTime::GetGameTime().count());
    sGuildAdventureMgr.SaveOrder(*leader);
    // (A raid is run once per order: its lockout holds for the week, a second run would find every boss dead.)
    SetDungeonRuns(leader->guid, plan->raid ? 1 : runs);
    OnOrderChanged(guild, *leader);
    return true;
}

OrderService::EventStep OrderService::StepDungeonEvent(GuildBot& bot, Player* player, BotRuntime& rt, DungeonPlan const& plan,
    DungeonBoss const& boss, std::vector<Player*> const& group, bool groupBusy)
{
    BotRuntime::DungeonRun& run = rt.dungeon;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    constexpr float TalkYards = 4.0f;
    constexpr uint32 TalkEveryMs = 6 * IN_MILLISECONDS;
    constexpr uint8 MaxTalks = 5;
    constexpr uint32 NpcGoneMs = 45 * IN_MILLISECONDS;
    Creature* creature = player->FindNearestCreature(boss.entry, 200.0f, true);
    if (boss.summonObject)
        return StepObjectEvent(bot, player, rt, plan, boss, group, groupBusy, creature);
    // (Its body after the talk: the boss came and fell while the group fought and this step did not run. Nefarian
    // fell, Lord Victor Nefarius with him, and the run gave the event up for its NPC's death, 2026-11-22.)
    if (!creature && run.eventSince && !run.eventBossSeen)
        if (Creature const* body = player->FindNearestCreature(boss.entry, 250.0f, false); body && !body->IsAlive())
            run.eventBossSeen = true;
    Creature* npc = player->FindNearestCreature(boss.talkEntry, 200.0f, true);
    CreatureTemplate const* npcProto = sObjectMgr->GetCreatureTemplate(boss.talkEntry);
    std::string const npcName = npc ? npc->GetName() : npcProto ? npcProto->Name : std::string("its NPC");
    auto walkTo = [&](WorldObject const* target)
    {
        if (BotDriver::GetRpgState(player) != RpgState::GoCamp || getMSTimeDiff(rt.task.lastAssert, now) >= AssertMs)
        {
            DungeonGoTo(player, plan.mapId, target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());
            rt.task.lastAssert = now;
        }
    };

    if (!run.eventSince)
    {
        // (A boss that is its own NPC is there before the talk, friendly: Vaelastrasz the Corrupt.)
        if (creature && (boss.entry != boss.talkEntry || !creature->IsFriendlyTo(player)))
        {
            // The boss is there already (the event ran before the group came).
            run.eventSince = now;
            run.eventBossSeen = true;
        }
        else
        {
            // The way the NPC will walk, once, before the talk (it walks ahead of the group, and what stands on its
            // way kills it: the Disciple of Naralex died five minutes into its walk). Every third point and the last.
            if (!boss.route.empty() && run.eventRouteAt < boss.route.size() && (run.eventRouteBegun || (npc && player->IsWithinDistInMap(npc, 30.0f))))
            {
                if (!run.eventRouteBegun)
                {
                    run.eventRouteBegun = true;
                    LOG_INFO(ActivityLog, "activity: {}'s group walks the way of {} in {} first ({} points): what stands on it would "
                        "kill it on its walk", bot.name, npcName, plan.name, boss.route.size());
                }
                PauseStuck(rt);
                run.bossStuckSince = now;
                DungeonBoss::Point const& point = boss.route[run.eventRouteAt];
                rt.why = Acore::StringFormat("In {}: clears the way of {} ({} of {})", plan.name, npcName, run.eventRouteAt + 1,
                    boss.route.size());
                if (groupBusy)
                    return EventStep::Busy;
                bool const there = Dist2dTo(player, point.x, point.y) <= 8.0f && std::fabs(player->GetPositionZ() - point.z) <= 8.0f;
                if (!there && ++run.eventRouteSteps > 40 && GetSettings().dungeonsHopWhenNoWay)
                {
                    // No way on foot to this point (a place players jump or swim at): the group is put there.
                    TakeGuestAlong(bot, player, point.x, point.y, point.z, "it found no way on foot");
                    for (Player* member : group)
                        if (member->IsAlive() && !member->IsInCombat())
                        {
                            member->NearTeleportTo(point.x, point.y, point.z, member->GetOrientation());
                            if (BotRuntime* other = FindRuntime(member->GetGUID()))
                                other->hasLastPosition = false;
                        }
                }
                if (there || run.eventRouteSteps > 40)
                {
                    run.eventRouteSteps = 0;
                    run.eventRouteAt = run.eventRouteAt + 1 == boss.route.size() ? uint32(boss.route.size()) :
                        std::min<uint32>(run.eventRouteAt + 3, uint32(boss.route.size()) - 1);
                    if (run.eventRouteAt == boss.route.size())
                        LOG_INFO(ActivityLog, "activity: {}'s group has walked the way of {} in {}: back to it", bot.name, npcName, plan.name);
                    return EventStep::Busy;
                }
                if (BotDriver::GetRpgState(player) != RpgState::GoCamp || getMSTimeDiff(rt.task.lastAssert, now) >= AssertMs)
                {
                    DungeonGoTo(player, plan.mapId, point.x, point.y, point.z);
                    rt.task.lastAssert = now;
                }
                return EventStep::Busy;
            }
            // To the NPC first (the caller's walk to the stop's place), then right up to it.
            if (!npc || !player->IsWithinDistInMap(npc, 30.0f))
            {
                if (!npc && Dist2dTo(player, boss.x, boss.y) <= 15.0f && std::fabs(player->GetPositionZ() - boss.z) <= 15.0f)
                {
                    LOG_INFO(ActivityLog, "activity: {}'s group cannot start the event of {} in {}: {} is not at its place", bot.name, boss.name,
                        plan.name, npcName);
                    return EventStep::GivenUp;
                }
                return EventStep::Approach;
            }
            PauseStuck(rt);
            bool together = !groupBusy;
            for (Player* member : group)
                together = together && member->IsWithinDistInMap(player, 30.0f) && member->GetHealthPct() >= 60.0f;
            if (!together)
            {
                rt.why = Acore::StringFormat("In {}: waits for its group before {} starts the event of {}", plan.name, npcName, boss.name);
                return EventStep::Busy;
            }
            if (!player->IsWithinDistInMap(npc, TalkYards))
            {
                rt.why = Acore::StringFormat("In {}: walks up to {}", plan.name, npcName);
                walkTo(npc);
                return EventStep::Busy;
            }
            if (now < run.eventNextTalk)
                return EventStep::Busy;
            // The talk: the client's gossip hello, then the option that starts the event.
            WorldSession* session = player->GetSession();
            player->PlayerTalkClass->ClearMenus();
            WorldPacket hello(CMSG_GOSSIP_HELLO, 8);
            hello << npc->GetGUID();
            session->HandleGossipHelloOpcode(hello);
            GossipMenu const& menu = player->PlayerTalkClass->GetGossipMenu();
            if (menu.GetSenderGUID() != npc->GetGUID() || !menu.GetItem(boss.talkOption))
            {
                player->PlayerTalkClass->SendCloseGossip();
                run.eventNextTalk = now + TalkEveryMs;
                if (++run.eventTalks >= MaxTalks)
                {
                    LOG_INFO(ActivityLog, "activity: {}'s group cannot start the event of {} in {}: {} does not offer it (what it asks "
                        "for is not done, or the event is over)", bot.name, boss.name, plan.name, npcName);
                    return EventStep::GivenUp;
                }
                rt.why = Acore::StringFormat("In {}: {} does not offer its event yet", plan.name, npcName);
                return EventStep::Busy;
            }
            WorldPacket select(CMSG_GOSSIP_SELECT_OPTION, 16);
            select << npc->GetGUID() << menu.GetMenuId() << uint32(boss.talkOption);
            session->HandleGossipSelectOptionOpcode(select);
            // (An option that opens the next menu: Majordomo Executus asks three times before he summons Ragnaros.)
            for (uint8 step = 1; step < boss.talkSteps; ++step)
            {
                GossipMenu const& next = player->PlayerTalkClass->GetGossipMenu();
                if (next.GetSenderGUID() != npc->GetGUID() || !next.GetItem(boss.talkOption))
                    break;
                WorldPacket again(CMSG_GOSSIP_SELECT_OPTION, 16);
                again << npc->GetGUID() << next.GetMenuId() << uint32(boss.talkOption);
                session->HandleGossipSelectOptionOpcode(again);
            }
            player->PlayerTalkClass->SendCloseGossip();
            run.eventSince = now;
            run.eventNpcSeen = now;
            LOG_INFO(ActivityLog, "activity: {} starts the event of {} in {}: it talks to {} ({})", bot.name, boss.name, plan.name, npcName,
                boss.follow ? "the group stays with it on its way" : "the group stays with it");
            return EventStep::Busy;
        }
    }

    PauseStuck(rt);
    run.bossStuckSince = now;
    if (creature)
    {
        // The boss is there: up to it, and the attack (the group fights what its leader fights).
        run.eventBossSeen = true;
        rt.why = Acore::StringFormat("In {}: fighting {}", plan.name, boss.name);
        Unit const* victim = player->GetVictim();
        if (!player->IsInCombat() || !victim || !victim->IsAlive())
        {
            if (!player->IsWithinDistInMap(creature, 8.0f) || !player->IsWithinLOSInMap(creature))
                walkTo(creature);
            else
                BotDriver::Attack(player, creature);
        }
        return EventStep::Busy;
    }
    if (run.eventBossSeen)
    {
        // It was there and it is not alive any more: the rest of the fight, its loot, then on.
        if (!run.eventDoneSince)
            run.eventDoneSince = now;
        // The NPC's event goes on after its boss: the group stays beside it until it is over, and loots after it.
        // (Blastmaster Emi Shortfuse blows up the cave Grubbis came out of 12 s after him: the whole group died at his
        // corpse there, Gnomeregan at level 34, 2026-11-19.)
        constexpr uint32 AfterEventMs = 20 * IN_MILLISECONDS;
        if (npc && npc->IsAlive() && getMSTimeDiff(run.eventDoneSince, now) < AfterEventMs)
        {
            rt.why = Acore::StringFormat("In {}: with {} until its event is over", plan.name, npcName);
            if (!player->IsInCombat() && !player->IsWithinDistInMap(npc, 6.0f))
                walkTo(npc);
            return EventStep::Busy;
        }
        Creature* corpse = nullptr;
        bool rolling = false;
        if ((player->IsInCombat() || LootPending(player, corpse, rolling)) &&
            getMSTimeDiff(run.eventDoneSince, now) < 3 * LootWaitMs + (npc ? AfterEventMs : 0))
        {
            rt.why = Acore::StringFormat("In {}: the loot of {}", plan.name, boss.name);
            if (corpse && !player->IsInCombat() && !player->IsWithinDistInMap(corpse, 4.0f))
                walkTo(corpse);
            return EventStep::Busy;
        }
        return EventStep::Done;
    }
    if (getMSTimeDiff(run.eventSince, now) > std::max<uint32>(boss.timeout, 60) * IN_MILLISECONDS)
    {
        LOG_INFO(ActivityLog, "activity: {}'s group gives the event of {} in {} up after {} s: {} did not come", bot.name, boss.name, plan.name,
            getMSTimeDiff(run.eventSince, now) / IN_MILLISECONDS, boss.name);
        return EventStep::GivenUp;
    }
    // The boss has not come yet: the group stays with the NPC and fights what attacks it.
    rt.why = Acore::StringFormat("In {}: the event of {} ({} comes at its end)", plan.name, npcName, boss.name);
    if (npc)
    {
        run.eventNpcSeen = now;
        if (!player->IsInCombat())
            for (Unit* attacker : npc->getAttackers())
                if (attacker && attacker->IsAlive() && attacker->GetTypeId() == TYPEID_UNIT)
                {
                    BotDriver::Attack(player, attacker);
                    return EventStep::Busy;
                }
        if (boss.follow && !groupBusy && !player->IsInCombat() && !player->IsWithinDistInMap(npc, 8.0f))
            walkTo(npc);
    }
    else if (getMSTimeDiff(run.eventNpcSeen, now) > NpcGoneMs)
    {
        Creature const* body = player->FindNearestCreature(boss.talkEntry, 200.0f, false);
        LOG_INFO(ActivityLog, "activity: {}'s group gives the event of {} in {} up: {} {}", bot.name, boss.name, plan.name, npcName,
            body ? Acore::StringFormat("is dead ({:.0f} yd from the group)", player->GetDistance(body)) : "is gone (not within 200 yd)");
        return EventStep::GivenUp;
    }
    return EventStep::Busy;
}

OrderService::EventStep OrderService::StepObjectEvent(GuildBot& bot, Player* player, BotRuntime& rt, DungeonPlan const& plan,
    DungeonBoss const& boss, std::vector<Player*> const& group, bool groupBusy, Creature* creature)
{
    // the owner, 2026-11-14: [quote left out]. The gong (its SmartAI): each use brings a group of creatures and
    // makes it unusable until they are dead; the first two uses bring spiders, the third Tuten'kash. The group stands
    // at the object, fights what comes, and uses it again whenever it can, until the boss is there.
    BotRuntime::DungeonRun& run = rt.dungeon;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    constexpr float UseYards = 4.0f;
    constexpr float StayYards = 10.0f;
    constexpr uint32 UseEveryMs = 8 * IN_MILLISECONDS;
    constexpr uint8 MaxUses = 6;
    GameObject* object = player->FindNearestGameObject(boss.summonObject, 200.0f);
    GameObjectTemplate const* objectProto = sObjectMgr->GetGameObjectTemplate(boss.summonObject);
    std::string const objectName = object ? object->GetGOInfo()->name : objectProto ? objectProto->name : std::string("its object");
    auto walkTo = [&](WorldObject const* target)
    {
        if (BotDriver::GetRpgState(player) != RpgState::GoCamp || getMSTimeDiff(rt.task.lastAssert, now) >= AssertMs)
        {
            DungeonGoTo(player, plan.mapId, target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());
            rt.task.lastAssert = now;
        }
    };
    auto usable = [&](GameObject const* go)
    {
        return go && go->isSpawned() && !go->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE) && !go->HasGameObjectFlag(GO_FLAG_IN_USE) &&
            go->GetGoState() == GO_STATE_READY;
    };
    auto use = [&](GameObject* go)
    {
        player->AttackStop();
        player->StopMoving();
        go->Use(player);
        ++run.eventTalks;
        run.eventNextTalk = now + UseEveryMs;
        LOG_INFO(ActivityLog, "activity: {} uses {} in {} ({}. time): {} comes when what it brings is dead", bot.name, objectName,
            plan.name, uint32(run.eventTalks), boss.name);
    };

    if (!run.eventSince)
    {
        if (creature)
        {
            run.eventSince = now;
            run.eventBossSeen = true;
        }
        else
        {
            if (!object || !player->IsWithinDistInMap(object, 30.0f))
            {
                if (!object && Dist2dTo(player, boss.x, boss.y) <= 15.0f && std::fabs(player->GetPositionZ() - boss.z) <= 15.0f)
                {
                    LOG_INFO(ActivityLog, "activity: {}'s group cannot start the event of {} in {}: {} is not there", bot.name, boss.name,
                        plan.name, objectName);
                    return EventStep::GivenUp;
                }
                return EventStep::Approach;
            }
            PauseStuck(rt);
            bool together = !groupBusy;
            for (Player* member : group)
                together = together && member->IsWithinDistInMap(player, 30.0f) && member->GetHealthPct() >= 60.0f;
            if (!together)
            {
                rt.why = Acore::StringFormat("In {}: waits for its group before it uses {}", plan.name, objectName);
                return EventStep::Busy;
            }
            // In reach as for the objects that wake a boss: the core's interact distance, or, when the walk gets no nearer
            // for 20 s, 12 yd on its level. (The group of Razorfen Downs stood 8 yd from the gong for 40 minutes, silently.)
            bool const inReach = player->IsWithinDistInMap(object, UseYards) || object->IsAtInteractDistance(player) ||
                (run.objectSince && getMSTimeDiff(run.objectSince, now) > 20 * IN_MILLISECONDS && player->GetExactDist(object) <= 12.0f &&
                    std::fabs(player->GetPositionZ() - object->GetPositionZ()) <= 5.0f &&
                    (player->GetExactDist(object) <= 8.0f || player->IsWithinLOSInMap(object)));
            if (!inReach)
            {
                // (Its clock runs while the walk gets no 3 yd nearer: the owner's raid walked on to the Gong of Bethekk, 31
                // yd from it, and was put beside it because the minute had run from where it first came within 30 yd,
                // round 268.)
                float const toObject = player->GetExactDist(object);
                if (!run.objectSince || toObject < run.objectBest - 3.0f)
                {
                    run.objectSince = now;
                    run.objectBest = toObject;
                }
                // A minute without getting to it (on a platform the path finder does not lead onto: the Gong of Bethekk
                // in Zul'Gurub, 13 yd away and 11 yd below it, 2026-11-22): the group is put beside it, once, as beside
                // what wakes a boss; but only where the navmesh has no way on foot there (the owner's movement ruling: the
                // bots walk where a player walks).
                if (getMSTimeDiff(run.objectSince, now) > MINUTE * IN_MILLISECONDS && run.objectHopFor != boss.summonObject &&
                    GetSettings().dungeonsHopWhenNoWay && !player->IsInCombat() &&
                    NoWayOnFoot(player, object->GetPositionX(), object->GetPositionY(), object->GetPositionZ()))
                {
                    run.objectHopFor = boss.summonObject;
                    float x = 0.0f, y = 0.0f, z = 0.0f;
                    if (FindHopSpot(player, object->GetPositionX(), object->GetPositionY(), object->GetPositionZ(), x, y, z, 2.0f, 6.0f))
                    {
                        LOG_INFO(ActivityLog, "activity: {}'s group gets no nearer to {} in {} in a minute ({:.0f} yd away, {:.0f} yd up or down): "
                            "it is put beside it", bot.name, objectName, plan.name, player->GetExactDist(object),
                            std::fabs(player->GetPositionZ() - object->GetPositionZ()));
                        TakeGuestAlong(bot, player, x, y, z, "no way on foot to what brings the boss");
                        for (Player* member : group)
                        {
                            if (!member->IsAlive() || member->GetMapId() != plan.mapId)
                                continue;
                            member->NearTeleportTo(x, y, z, member->GetOrientation());
                            if (BotRuntime* other = FindRuntime(member->GetGUID()))
                                other->hasLastPosition = false;
                        }
                        return EventStep::Busy;
                    }
                }
                if (getMSTimeDiff(run.objectSince, now) > 2 * MINUTE * IN_MILLISECONDS)
                {
                    LOG_INFO(ActivityLog, "activity: {}'s group cannot start the event of {} in {}: it does not get to {} in two minutes "
                        "({:.0f} yd away, {:.0f} yd up or down)", bot.name, boss.name, plan.name, objectName, player->GetExactDist(object),
                        std::fabs(player->GetPositionZ() - object->GetPositionZ()));
                    run.objectSince = 0;
                    return EventStep::GivenUp;
                }
                rt.why = Acore::StringFormat("In {}: walks up to {}", plan.name, objectName);
                walkTo(object);
                return EventStep::Busy;
            }
            run.objectSince = 0;
            if (!usable(object))
            {
                // (Used before: the event ran, and nothing it brings came: it is over.)
                LOG_INFO(ActivityLog, "activity: {}'s group cannot start the event of {} in {}: {} cannot be used", bot.name, boss.name,
                    plan.name, objectName);
                return EventStep::GivenUp;
            }
            run.eventSince = now;
            run.eventNpcSeen = now;
            run.eventTalks = 0;
            use(object);
            return EventStep::Busy;
        }
    }

    PauseStuck(rt);
    run.bossStuckSince = now;
    if (creature)
    {
        run.eventBossSeen = true;
        rt.why = Acore::StringFormat("In {}: fighting {}", plan.name, boss.name);
        Unit const* victim = player->GetVictim();
        if (!player->IsInCombat() || !victim || !victim->IsAlive())
        {
            if (!player->IsWithinDistInMap(creature, 8.0f) || !player->IsWithinLOSInMap(creature))
                walkTo(creature);
            else
                BotDriver::Attack(player, creature);
        }
        return EventStep::Busy;
    }
    // (Seen and not found alive any more: done only with its corpse there. High Priestess Arlokk vanishes for seconds in
    // her fight, and the group had counted her done four seconds after the gong, 2026-11-22.)
    if (run.eventBossSeen && !player->FindNearestCreature(boss.entry, 250.0f, false) &&
        getMSTimeDiff(run.eventSince, now) <= std::max<uint32>(boss.timeout, 60) * IN_MILLISECONDS)
    {
        rt.why = Acore::StringFormat("In {}: waits for {} to show again", plan.name, boss.name);
        if (object && !player->IsInCombat() && !player->IsWithinDistInMap(object, StayYards))
            walkTo(object);
        return EventStep::Busy;
    }
    if (run.eventBossSeen)
    {
        if (!run.eventDoneSince)
            run.eventDoneSince = now;
        Creature* corpse = nullptr;
        bool rolling = false;
        if ((player->IsInCombat() || LootPending(player, corpse, rolling)) && getMSTimeDiff(run.eventDoneSince, now) < 3 * LootWaitMs)
        {
            rt.why = Acore::StringFormat("In {}: the loot of {}", plan.name, boss.name);
            if (corpse && !player->IsInCombat() && !player->IsWithinDistInMap(corpse, 4.0f))
                walkTo(corpse);
            return EventStep::Busy;
        }
        return EventStep::Done;
    }
    if (getMSTimeDiff(run.eventSince, now) > std::max<uint32>(boss.timeout, 60) * IN_MILLISECONDS)
    {
        LOG_INFO(ActivityLog, "activity: {}'s group gives the event of {} in {} up after {} s ({} uses of {}): {} did not come", bot.name,
            boss.name, plan.name, getMSTimeDiff(run.eventSince, now) / IN_MILLISECONDS, uint32(run.eventTalks), objectName, boss.name);
        return EventStep::GivenUp;
    }
    // What it brings comes to the object: the group stays there and fights it; when the object can be used again
    // (what came is dead), the next use.
    rt.why = Acore::StringFormat("In {}: the event of {} ({} comes at its end)", plan.name, objectName, boss.name);
    if (groupBusy || player->IsInCombat())
        return EventStep::Busy;
    if (object && !player->IsWithinDistInMap(object, StayYards))
    {
        walkTo(object);
        return EventStep::Busy;
    }
    if (object && usable(object) && now >= run.eventNextTalk && (object->IsAtInteractDistance(player) ||
        (player->GetExactDist(object) <= StayYards && std::fabs(player->GetPositionZ() - object->GetPositionZ()) <= 5.0f)))
    {
        // The next use only with the whole group ready at the tank, as before a boss: each use brings a wave, the last
        // one the boss (the gong of Razorfen Downs was struck again eight seconds after a wave, and Tuten'kash came to
        // a group whose healer had 25 % of its mana).
        if (AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfMember(bot.guid);
            guild && !DungeonGate(*guild, bot, player, rt, plan, group, nullptr, Acore::StringFormat("the next use of {}", objectName),
                80000 + uint32(run.boss) * 16 + run.eventTalks))
            return EventStep::Busy;
        if (run.eventTalks >= MaxUses)
        {
            LOG_INFO(ActivityLog, "activity: {}'s group gives the event of {} in {} up: {} used {} times and {} did not come", bot.name,
                boss.name, plan.name, objectName, uint32(run.eventTalks), boss.name);
            return EventStep::GivenUp;
        }
        use(object);
    }
    else if (object && usable(object) && now >= run.eventNextTalk)
        walkTo(object);
    return EventStep::Busy;
}

bool OrderService::PutTankInFront(AdventureGuild& guild, GuildBot& bot, Player* player, std::vector<Player*> const& group)
{
    LoadDungeonGroups();
    if (_dungeonGroups.count(bot.guid.GetCounter()) || sRoleService.EffectiveRole(bot) == DungeonRole::Tank)
        return false;
    GuildBot* tank = nullptr;
    std::vector<ObjectGuid> chosen{ bot.guid };
    for (Player* member : group)
    {
        if (member == player)
            continue;
        chosen.push_back(member->GetGUID());
        GuildBot* memberBot = guild.FindBot(member->GetGUID());
        if (!tank && memberBot && sRoleService.EffectiveRole(*memberBot) == DungeonRole::Tank)
            tank = memberBot;
    }
    if (!tank)
        return false;
    uint32 const dungeonId = bot.order.zoneId;
    uint32 const runs = GetDungeonRuns(bot.guid);
    BotOrder const before = bot.order;
    // The squad's leader is on the autopilot again (what a dungeon order ends with); the group's order is the tank's.
    bot.order.type = OrderType::Auto;
    bot.order.zoneId = 0;
    std::string error;
    if (!StartDungeonGroup(guild, dungeonId, chosen, tank->guid, runs, error, false))
    {
        bot.order = before;
        if (!_tankFrontNoted.count(bot.guid.GetCounter()))
        {
            _tankFrontNoted.insert(bot.guid.GetCounter());
            LOG_INFO(ActivityLog, "activity: {} leads its squad into the dungeon itself although {} tanks: {}", bot.name, tank->name, error);
        }
        return false;
    }
    _tankFrontNoted.erase(bot.guid.GetCounter());
    sGuildAdventureMgr.SaveOrder(bot);
    LOG_INFO(ActivityLog, "activity: the tank {} goes in front: it leads the dungeon run of {}'s squad (the squad is what it was "
        "before when the run is over; {} is on the autopilot then)", tank->name, bot.name, bot.name);
    if (BotRuntime* rt = FindRuntime(bot.guid))
        rt->dungeon = BotRuntime::DungeonRun();
    OnOrderChanged(guild, bot);
    return true;
}

bool OrderService::DisbandDungeonGroup(AdventureGuild& guild, GuildBot& leader, bool restoreOrder)
{
    LoadDungeonGroups();
    auto itr = _dungeonGroups.find(leader.guid.GetCounter());
    if (itr == _dungeonGroups.end())
        return false;
    DungeonGroupState const state = itr->second;
    _dungeonGroups.erase(itr);
    CharacterDatabase.Execute("DELETE FROM ga_dungeon_group WHERE leader_guid = {}", leader.guid.GetCounter());
    CharacterDatabase.Execute("DELETE FROM ga_dungeon_group_options WHERE leader_guid = {}", leader.guid.GetCounter());
    CharacterDatabase.Execute("DELETE FROM ga_dungeon_group_member WHERE leader_guid = {}", leader.guid.GetCounter());

    // (What they were before comes back whatever their levels are by now.)
    struct AnyLevel
    {
        AnyLevel() { sSquadService.AllowAnyLevel(true); }
        ~AnyLevel() { sSquadService.AllowAnyLevel(false); }
    } anyLevel;
    std::string error;
    // The guild leader's own character that got its order for the run: watch mode ends with that order (the owner
    // has the controls again), and a leader the AI holds leaves the party of bots here (it is not planned afterwards).
    for (DungeonGroupState::Link const& link : state.links)
        if (link.lentOrder && guild.leader.GetCounter() == link.member)
        {
            if (sGuildBotHolder.IsWatched(guild.leader))
                sControlSwapService.EndWatchOf(guild.leader);
            if (Player* own = sGuildBotHolder.GetAIDrivenPlayer(guild.leader); own && own->GetGroup())
                BotDriver::LeaveNormalGroup(own);
        }
    // ... and with an order of its own too: out of the party of the group (the owner's Retadin, watched and on the
    // autopilot, stayed in that party after his Uldaman run, with a bot that leads a squad and its member; nothing
    // takes a watched character out of a party).
    if (Player* own = ObjectAccessor::FindConnectedPlayer(guild.leader); own && own->GetGroup())
        if (Player* runLeader = ObjectAccessor::FindConnectedPlayer(leader.guid); runLeader && own != runLeader &&
            runLeader->GetGroup() == own->GetGroup())
            for (DungeonGroupState::Link const& link : state.links)
                if (link.member == guild.leader.GetCounter())
                {
                    BotDriver::LeaveNormalGroup(own);
                    break;
                }
    for (DungeonGroupState::Link const& link : state.links)
        if (link.inGroup && link.member != leader.guid.GetCounter())
            sSquadService.Set(guild, ObjectGuid::Create<HighGuid::Player>(link.member), ObjectGuid::Empty, error);
    for (DungeonGroupState::Link const& link : state.links)
        if (link.lentOrder && guild.leader.GetCounter() == link.member && guild.leaderPlanned)
            SetLeaderOrder(guild, std::nullopt);
    uint32 back = 0;
    for (DungeonGroupState::Link const& link : state.links)
    {
        if (!link.prevLeader)
            continue;
        error.clear();
        if (sSquadService.Set(guild, ObjectGuid::Create<HighGuid::Player>(link.member), ObjectGuid::Create<HighGuid::Player>(link.prevLeader), error))
            ++back;
        else
            LOG_INFO(ActivityLog, "activity: a squad from before the dungeon group of {} does not come back (bot {} behind {}): {}",
                leader.name, link.member, link.prevLeader, error);
    }
    if (restoreOrder)
    {
        std::optional<OrderType> const type = OrderTypeFromToken(state.prevOrder);
        leader.order.type = type && *type != OrderType::Dungeon ? *type : OrderType::Auto;
        leader.order.zoneId = leader.order.type == OrderType::Auto ? 0 : state.prevZone;
    }
    LOG_INFO(ActivityLog, "activity: the dungeon group of {} is over: its bots are what they were before{}{}", leader.name,
        back ? Acore::StringFormat(" ({} squad link{} back)", back, back == 1 ? "" : "s") : std::string(),
        restoreOrder ? Acore::StringFormat("; {} has the order {} again", leader.name, ToToken(leader.order.type)) : std::string());
    return true;
}

bool OrderService::StopDungeonRun(AdventureGuild& guild, ObjectGuid leaderGuid, bool now, std::string& error)
{
    GuildBot* bot = guild.FindBot(leaderGuid);
    if (!bot || bot->order.type != OrderType::Dungeon)
    {
        error = "that bot leads no dungeon run";
        return false;
    }
    if (!now)
    {
        // The run that is going is the last.
        SetDungeonRuns(leaderGuid, 1);
        LOG_INFO(ActivityLog, "activity: the guild leader ends {}'s dungeon order after the run that is going", bot->name);
        return true;
    }
    Player* player = ObjectAccessor::FindPlayer(leaderGuid);
    BotRuntime* rt = FindRuntime(leaderGuid);
    if (player && player->IsInWorld() && rt)
    {
        EndDungeonRun(guild, *bot, player, *rt, "stopped by the guild leader", false);
        return true;
    }
    // Its leader is not in the world: the order goes, the group with it.
    SetDungeonRuns(leaderGuid, 1);
    if (!DisbandDungeonGroup(guild, *bot, true))
    {
        bot->order.type = OrderType::Auto;
        bot->order.zoneId = 0;
    }
    sGuildAdventureMgr.SaveOrder(*bot);
    OnOrderChanged(guild, *bot);
    return true;
}

bool OrderService::SkipDungeonBosses(AdventureGuild& guild, ObjectGuid leaderGuid, uint32 number, std::string& result)
{
    // (the owner, 2026-11-18: a fix is tested at the place it is for, not with a whole run from the entrance each time.)
    GuildBot* bot = guild.FindBot(leaderGuid);
    Player* player = ObjectAccessor::FindPlayer(leaderGuid);
    BotRuntime* rt = FindRuntime(leaderGuid);
    if (!bot || bot->order.type != OrderType::Dungeon || !player || !player->IsInWorld() || !rt || !rt->dungeon.id ||
        rt->dungeon.phase != PhaseInside)
    {
        result = "that bot leads no dungeon run that is inside its dungeon";
        return false;
    }
    BotRuntime::DungeonRun& run = rt->dungeon;
    DungeonPlan const* plan = sDungeons.GetPlan(run.id);
    if (!plan || player->GetMapId() != plan->mapId)
    {
        result = "its leader is not inside its dungeon";
        return false;
    }
    std::size_t const count = RunBossCount(run, *plan);
    if (number < 1 || number > count)
    {
        result = Acore::StringFormat("the run has {} bosses", count);
        return false;
    }
    std::string killed;
    while (run.boss + 1u < number)
    {
        DungeonBoss const& boss = RunBoss(run, *plan, run.boss);
        uint32 dead = 0;
        std::vector<Creature*> alive;
        for (auto const& [spawnId, creature] : player->GetMap()->GetCreatureBySpawnIdStore())
            if (creature && creature->GetEntry() == boss.entry && creature->IsAlive())
                alive.push_back(creature);
        for (Creature* creature : alive)
        {
            Unit::Kill(player, creature);
            ++dead;
        }
        killed += Acore::StringFormat("{}{}{}", killed.empty() ? "" : ", ", boss.name, dead ? "" : " (not there)");
        ++run.boss;
        ++run.bossesDown;
    }
    run.bossStuckSince = 0;
    run.hopped = false;
    run.objectAt = run.objectNext = run.objectSince = run.objectsUsed = 0;
    run.lootSince = 0;
    player->CombatStop(true);
    LOG_INFO(ActivityLog, "activity: test aid: {}'s run in {} goes on at {} (boss {} of {}); killed for it: {}", bot->name, plan->name,
        RunBoss(run, *plan, run.boss).name, uint32(run.boss + 1), count, killed.empty() ? "nothing" : killed);
    result = Acore::StringFormat("goes on at {} (boss {} of {}); killed for it: {}", RunBoss(run, *plan, run.boss).name,
        uint32(run.boss + 1), count, killed.empty() ? "nothing" : killed);
    return true;
}

std::vector<OrderService::DungeonRunStatus> OrderService::GetDungeonRunStatus(AdventureGuild& guild)
{
    LoadDungeonRuns();
    std::vector<DungeonRunStatus> result;
    for (auto const& [guid, bot] : guild.bots)
    {
        if (bot.order.type != OrderType::Dungeon)
            continue;
        DungeonPlan const* plan = sDungeons.GetPlan(bot.order.zoneId);
        DungeonRunStatus status;
        status.leader = guid;
        status.dungeon = bot.order.zoneId;
        status.name = plan ? plan->name : "?";
        status.bosses = plan ? uint8(plan->bosses.size()) : 0;
        status.runsLeft = GetDungeonRuns(guid);
        status.runsDone = GetDungeonRunsDone(guid);
        status.members = sSquadService.GetMembers(guid);
        if (BotRuntime const* rt = FindRuntime(guid))
        {
            BotRuntime::DungeonRun const& run = rt->dungeon;
            status.phase = run.id ? (run.phase == PhaseInside ? 2 : 1) : 0;
            status.bossesDown = run.bossesDown;
            status.wipes = run.wipes;
            if (plan && run.phase == PhaseInside && run.boss < RunBossCount(run, *plan))
                status.nextBoss = RunBoss(run, *plan, run.boss).name;
            status.minutes = run.startedAt ? getMSTimeDiff(run.startedAt, getMSTime()) / (MINUTE * IN_MILLISECONDS) : 0;
            status.why = rt->why;
        }
        result.push_back(std::move(status));
    }
    return result;
}

void OrderService::DungeonUpkeep(AdventureGuild& guild, GuildBot const& leader, std::vector<Player*> const& group)
{
    std::string done;
    for (Player* member : group)
    {
        GuildBot const* bot = guild.FindMember(member->GetGUID());
        if (!bot || !member->IsAlive())
            continue;
        std::string what;
        // Repairs where the group stands, for what a vendor would charge (the character's own money).
        if (GetSettings().dungeonsFieldRepair)
        {
            float worst = 100.0f;
            for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
                if (Item const* item = member->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                    if (uint32 const most = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY))
                        worst = std::min(worst, 100.0f * float(item->GetUInt32Value(ITEM_FIELD_DURABILITY)) / float(most));
            if (worst < RepairBelow)
            {
                // (The core's function returns the cost only for a repair out of the guild bank: the money says it.)
                uint32 const before = member->GetMoney();
                member->DurabilityRepairAll(true, 1.0f, false);
                uint32 const paid = before - std::min(before, member->GetMoney());
                what = paid ? Acore::StringFormat("repairs for {}c", paid) : std::string("no money for repairs");
            }
        }
        // Room in the bags: the loot filter's way (junk by the junk rule is sold, the rest into the guild storage).
        if (member->GetFreeInventorySpace() < BagSlotsWanted)
            if (uint32 const freed = MakeBagSpace(*bot, member))
                what += Acore::StringFormat("{}{} bag slots freed", what.empty() ? "" : ", ", freed);
        if (!what.empty())
            done += Acore::StringFormat("{}{} ({})", done.empty() ? "" : "; ", bot->name, what);
    }
    if (!done.empty())
        LOG_INFO(ActivityLog, "activity: {}'s group between two runs: {}", leader.name, done);
}

void OrderService::OpenDoorsInTheWay(GuildBot const& bot, Player* player, BotRuntime& rt, std::string const& dungeon, bool tribute)
{
    uint32 const now = getMSTime();
    // (Also in a fight: the group is often in one when it comes to a door, and an open door harms nobody.)
    if (!GetSettings().dungeonsOpenDoors || now < rt.dungeon.nextDoorLook)
        return;
    rt.dungeon.nextDoorLook = now + DoorLookMs;
    // A door that is closed. (A bot walks through it, but nobody behind it can be healed or shot.)
    std::list<GameObject*> objects;
    ClosedDoorCheck check(player, DoorYards);
    Acore::GameObjectListSearcher<ClosedDoorCheck> searcher(player, objects, check);
    Cell::VisitObjects(player, searcher, DoorYards);
    for (GameObject* object : objects)
    {
        // (A tribute run opens the Gordok Courtyard and Inner Doors itself, with their key: OrderTribute.cpp.)
        constexpr uint32 GordokCourtyardDoor = 177219, GordokInnerDoor = 177217;
        if (tribute && (object->GetEntry() == GordokCourtyardDoor || object->GetEntry() == GordokInnerDoor))
            continue;
        object->UseDoorOrButton(0, false, player);
        LOG_INFO(ActivityLog, "activity: {}'s group opens {} in its way in {} (at {:.0f}, {:.0f}, {:.0f})", bot.name,
            object->GetGOInfo() ? object->GetGOInfo()->name : std::string("a door"), dungeon, object->GetPositionX(),
            object->GetPositionY(), object->GetPositionZ());
    }
}

bool OrderService::LeaveDungeonWithoutRun(AdventureGuild& guild, GuildBot& bot, Player* player, BotRuntime& rt)
{
    if (!player->GetMap()->IsDungeon())
        return false;
    // Its own run, or its squad leader's.
    if (bot.order.type == OrderType::Dungeon)
        return false;
    ObjectGuid const leaderGuid = sSquadService.GetLeader(bot.guid);
    if (!leaderGuid.IsEmpty())
        if (GuildBot const* leader = guild.FindBot(leaderGuid))
            if (leader->order.type == OrderType::Dungeon)
                return false;

    DungeonPlan const* plan = nullptr;
    for (DungeonPlan const& candidate : sDungeons.GetPlans())
        if (candidate.mapId == player->GetMapId() && candidate.outMap != candidate.mapId && (candidate.outX != 0.0f || candidate.outY != 0.0f))
            plan = &candidate;
    if (!plan)
        return false;
    // (A fight is fought out or left first; a dead bot is brought back at the entrance.)
    if (player->IsAlive() && player->IsInCombat())
        return false;

    // Where a run gathers: the road map's node nearest to the entrance, else the entrance.
    float x = plan->outX, y = plan->outY, z = plan->outZ;
    RoutePoint node;
    std::string nodeName;
    if (sRoadMap.FindNearestNode(plan->outMap, plan->outX, plan->outY, 0.0f, 800.0f, node, nodeName))
    {
        x = node.x;
        y = node.y;
        z = node.z;
    }
    LOG_INFO(ActivityLog, "activity: {} is inside {} without a dungeon run: back to its entrance", bot.name, plan->name);
    if (!player->IsAlive())
        Revive(player, nullptr);
    rt.dungeon = BotRuntime::DungeonRun();
    ClearTask(rt, 5 * IN_MILLISECONDS, "left the dungeon");
    BotDriver::Rest(player);
    player->TeleportTo(plan->outMap, x, y, z, player->GetOrientation());
    rt.hasLastPosition = false;
    rt.why = Acore::StringFormat("Leaving {}", plan->name);
    return true;
}

std::vector<Player*> OrderService::DungeonGroup(AdventureGuild& guild, ObjectGuid leader) const
{
    std::vector<Player*> group;
    auto add = [&](ObjectGuid guid)
    {
        Player* player = ObjectAccessor::FindPlayer(guid);
        if (player && player->IsInWorld() && BotDriver::GetAI(player))
            group.push_back(player);
    };
    add(leader);
    for (ObjectGuid member : sSquadService.GetMembers(leader))
        add(member);
    (void)guild;
    return group;
}

bool OrderService::IsDungeonGuest(ObjectGuid guid) const
{
    ObjectGuid const lead = sSquadService.GetLeader(guid);
    if (lead.IsEmpty())
        return false;
    AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfMember(guid);
    GuildBot const* leaderBot = guild ? guild->FindBot(lead) : nullptr;
    return leaderBot && leaderBot->order.type == OrderType::Dungeon;
}

Player* OrderService::DungeonGuest(ObjectGuid leader) const
{
    for (ObjectGuid member : sSquadService.GetMembers(leader))
    {
        Player* player = ObjectAccessor::FindPlayer(member);
        if (!player || !player->IsInWorld() || player->IsDuringRemoveFromWorld())
            continue;
        WorldSession* session = player->GetSession();
        if (session && !session->IsHeadless() && !sGuildBotHolder.IsAIPlayed(member) && !sGuildBotHolder.IsWatched(member))
            return player;
    }
    return nullptr;
}

namespace
{
    // The character its owner plays by hand leaves the dungeon after its group: a moment later (what the server tells
    // the client now, the answer to its own "stop" among it, would reach it during the loading screen, where the
    // addon gets no messages), and not out of a fight: it is looked at again every second and a half until the fight
    // is over, and a character that died in it is brought back and taken out. (The first build left a character
    // that was in a fight at the end of the run inside, alone: the probe's leader stood in a fight when its run was
    // stopped, died there and came out as a spirit at the graveyard; the core also puts a character out of an
    // instance a minute after its group is gone.)
    // The names of the group's characters for the client of the character its owner plays by hand. The client asks
    // for the name of a party member it does not know (CMSG_NAME_QUERY) the moment the party is formed, the run
    // teleports the character in the same moment, and the core drops what a client sends while its character is
    // between two maps: the client then waits for that answer for the rest of its session, and the bot that leads
    // the party is "Unknown" in its party frame. So the answers are sent unasked, before the teleport and once more
    // inside.
    std::unordered_map<ObjectGuid, uint32> guestNamedForRun;

    void SendGroupNames(Player* guest, std::vector<Player*> const& group)
    {
        if (!guest || !guest->GetSession())
            return;
        for (Player* member : group)
            if (member && member != guest)
                guest->GetSession()->SendNameQueryOpcode(member->GetGUID());
    }

    void LeaveAfterGroup(Player* guest, uint32 from, uint32 map, float x, float y, float z, uint32 tries)
    {
        guest->m_Events.AddEventAtOffset([guest, from, map, x, y, z, tries]()
        {
            if (!guest->IsInWorld() || guest->GetMapId() != from)
                return;
            if (guest->IsBeingTeleported() || (guest->IsAlive() && guest->IsInCombat()))
            {
                if (tries)
                    LeaveAfterGroup(guest, from, map, x, y, z, tries - 1);
                return;
            }
            if (!guest->IsAlive())
            {
                guest->ResurrectPlayer(0.5f);
                guest->SpawnCorpseBones();
            }
            guest->TeleportTo(map, x, y, z, guest->GetOrientation());
        }, Milliseconds(1500));
    }
}

void OrderService::TakeGuestAlong(GuildBot const& leader, Player* leaderPlayer, float x, float y, float z, std::string const& why)
{
    Player* guest = DungeonGuest(leader.guid);
    if (!guest || !GetSettings().dungeonsTakeOwnerAlong || !guest->IsAlive() || guest->IsInCombat() || guest->IsBeingTeleported() ||
        guest->GetMapId() != leaderPlayer->GetMapId() || !guest->IsWithinDistInMap(leaderPlayer, 60.0f))
        return;
    TellGuest(guest, Acore::StringFormat("Your group is put ahead ({}): you go with it.", why));
    guest->NearTeleportTo(x, y, z, guest->GetOrientation());
}

bool OrderService::StepDungeonGuest(GuildBot& bot, Player* player, BotRuntime& rt, DungeonPlan const& plan, Player* guest,
    std::vector<Player*> const& group)
{
    constexpr float BehindYards = 45.0f;
    constexpr uint32 OfferEveryMs = 30 * IN_MILLISECONDS;
    BotRuntime::DungeonRun& run = rt.dungeon;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    // Inside: the names of its party once more (SendGroupNames), once per run.
    if (guest->GetMapId() == plan.mapId && !guest->IsBeingTeleported())
        if (uint32& named = guestNamedForRun[guest->GetGUID()]; named != std::max<uint32>(run.startedAt, 1))
        {
            named = std::max<uint32>(run.startedAt, 1);
            SendGroupNames(guest, group);
        }
    // Not inside (on its way in, or it left): the run goes on. In a fight of its own the leader does not wait.
    if (guest->GetMapId() != plan.mapId || guest->IsBeingTeleported() || !player->IsAlive() || player->IsInCombat())
    {
        run.guestWaitSince = 0;
        return false;
    }
    bool fighting = false;
    for (Player* member : group)
        fighting = fighting || (member->IsAlive() && member->IsInCombat());

    // Dead: once the group's fight is over it offers to bring the character back (the request a resurrection spell
    // sends; the owner accepts it or walks back as a spirit).
    if (!guest->IsAlive() && !fighting && now >= run.guestResAt && !guest->HasPlayerFlag(PLAYER_FLAGS_GHOST))
    {
        run.guestResAt = now + OfferEveryMs;
        OfferResurrection(player, guest);
    }

    Unit* const foe = guest->IsAlive() && guest->IsInCombat() ? guest->GetVictim() : nullptr;
    float const distance = player->GetDistance(guest);
    std::string reason;
    if (!guest->IsAlive())
        reason = "is dead";
    else if (guest->IsInCombat())
        reason = "is in a fight";
    else if (guest->IsSitState())
        reason = "rests";
    else if (distance > BehindYards)
        reason = Acore::StringFormat("is {:.0f} yd behind", distance);
    if (reason.empty())
    {
        run.guestWaitSince = 0;
        run.guestLeftBehind = false;
        return false;
    }
    // (It went on without the character once: it does so until the character is with it again.)
    if (run.guestLeftBehind)
        return false;
    if (!run.guestWaitSince)
    {
        run.guestWaitSince = now;
        BotDriver::Rest(player);
    }
    else if (getMSTimeDiff(run.guestWaitSince, now) > GetSettings().squadsMaxWaitSeconds * IN_MILLISECONDS)
    {
        run.guestWaitSince = 0;
        run.guestLeftBehind = true;
        LOG_INFO(ActivityLog, "activity: {}'s group has waited {} s for {} ({}) in {}: it goes on", bot.name,
            GetSettings().squadsMaxWaitSeconds, guest->GetName(), reason, plan.name);
        TellGuest(guest, Acore::StringFormat("Your group has waited {} s for you and goes on. It waits again once you are with it.",
            GetSettings().squadsMaxWaitSeconds));
        return false;
    }
    PauseStuck(rt);
    rt.strandSince = 0;
    rt.why = Acore::StringFormat("In {}: waiting for {} ({})", plan.name, guest->GetName(), reason);
    // What the character fights is the group's fight too.
    if (foe && foe->IsAlive() && player->IsWithinDistInMap(foe, 60.0f))
        BotDriver::Attack(player, foe);
    return true;
}

void OrderService::EndDungeonRun(AdventureGuild& guild, GuildBot& bot, Player* player, BotRuntime& rt, std::string const& how, bool done)
{
    DungeonPlan const* plan = sDungeons.GetPlan(rt.dungeon.id ? rt.dungeon.id : bot.order.zoneId);
    std::string const name = plan ? plan->name : "the dungeon";
    uint32 const minutes = rt.dungeon.startedAt ? getMSTimeDiff(rt.dungeon.startedAt, getMSTime()) / (MINUTE * IN_MILLISECONDS) : 0;
    LOG_INFO(ActivityLog, "activity: {} ends its dungeon run in {} after {} min: {} ({} of {} bosses, {} wipe{}{}; {})", bot.name, name, minutes,
        how, uint32(rt.dungeon.bossesDown), plan ? uint32(plan->bosses.size()) : 0u, uint32(rt.dungeon.wipes),
        rt.dungeon.wipes == 1 ? "" : "s", rt.dungeon.holds || rt.dungeon.pulls || rt.dungeon.fleeing || rt.dungeon.rescues ?
        Acore::StringFormat("; the group held back for its tank in {} fight{}, {} careful pull{}, {} fleeing creature{} focused, went to "
            "help a member under attack {} time{}", uint32(rt.dungeon.holds), rt.dungeon.holds == 1 ? "" : "s", uint32(rt.dungeon.pulls),
            rt.dungeon.pulls == 1 ? "" : "s", uint32(rt.dungeon.fleeing), rt.dungeon.fleeing == 1 ? "" : "s", uint32(rt.dungeon.rescues),
            rt.dungeon.rescues == 1 ? "" : "s") : std::string(), DescribeDungeonMetrics(rt.dungeon.metrics));
    RecordEvent(guild, "DUNGEON", bot.guid, done ?
        Acore::StringFormat("{}'s group finished {} ({} bosses, {} min).", bot.name, name, uint32(rt.dungeon.bossesDown), minutes) :
        Acore::StringFormat("{}'s run in {} is over: {} ({} bosses down).", bot.name, name, how, uint32(rt.dungeon.bossesDown)),
        player->GetLevel(), player->GetZoneId());

    // The end boss fell: the guild has finished the dungeon (once more), which opens the next one.
    if (done && plan && rt.dungeon.endBossDown)
    {
        LoadDungeonsFinished();
        uint32& count = _dungeonsFinished[guild.ownerAccount][plan->id];
        ++count;
        uint32 const stamp = uint32(GameTime::GetGameTime().count());
        CharacterDatabase.Execute("INSERT INTO ga_guild_dungeon (owner_account, dungeon_id, runs_done, first_at, last_at) "
            "VALUES ({}, {}, 1, {}, {}) ON DUPLICATE KEY UPDATE runs_done = runs_done + 1, last_at = {}", guild.ownerAccount, plan->id,
            stamp, stamp, stamp);
        if (count == 1)
        {
            std::string next;
            std::vector<uint32> const order = DungeonOrder(guild);
            auto itr = std::find(order.begin(), order.end(), plan->id);
            if (itr != order.end() && itr + 1 != order.end())
                if (DungeonPlan const* following = sDungeons.GetPlan(*(itr + 1)))
                    next = following->name;
            LOG_INFO(ActivityLog, "activity: the guild of {} has finished {} for the first time{}", bot.name, plan->name,
                next.empty() || !GetSettings().dungeonsUnlockInOrder ? std::string() : ": " + next + " is open now");
            if (!next.empty() && GetSettings().dungeonsUnlockInOrder)
                RecordEvent(guild, "DUNGEON", bot.guid, Acore::StringFormat("{} is finished: {} is open now.", plan->name, next),
                    player->GetLevel(), player->GetZoneId());
        }
    }

    // Out again: everyone who is inside, alive, to the entrance in the open world.
    if (plan)
        for (Player* member : DungeonGroup(guild, bot.guid))
        {
            if (member->GetMapId() != plan->mapId)
            {
                if (!member->IsAlive())
                    Revive(member, nullptr);
                continue;
            }
            Revive(member, nullptr);
            // (With the teleport on: each to its hearthstone place, the owner's flow of the run window; else out at the
            // gathering place before the entrance.)
            if (GetSettings().dungeonsTeleport)
                member->TeleportTo(member->m_homebindMapId, member->m_homebindX, member->m_homebindY, member->m_homebindZ,
                    member->GetOrientation());
            else
                member->TeleportTo(plan->outMap, rt.dungeon.approachX, rt.dungeon.approachY, rt.dungeon.approachZ, member->GetOrientation());
            if (BotRuntime* other = FindRuntime(member->GetGUID()))
                other->hasLastPosition = false;
        }
    // The character the owner plays by hand goes out with the group, out of a fight once the fight is over (alive or
    // not: LeaveAfterGroup); with TakeOwnerAlong off it stays where it is.
    if (plan)
        if (Player* guest = DungeonGuest(bot.guid); guest && guest->GetMapId() == plan->mapId)
        {
            if (!GetSettings().dungeonsTakeOwnerAlong)
                TellGuest(guest, Acore::StringFormat("Your group has left {}.", name));
            else
            {
                bool const fighting = guest->IsAlive() && guest->IsInCombat();
                if (!fighting)
                    Revive(guest, nullptr);
                TellGuest(guest, Acore::StringFormat("The run in {} is over ({}): you leave with your group{}.", name, how,
                    fighting ? " as soon as your fight is over" : ""));
                bool const home = GetSettings().dungeonsTeleport;
                // (Two minutes of looks: a fight that goes on longer is the owner's own business in there.)
                LeaveAfterGroup(guest, plan->mapId, home ? guest->m_homebindMapId : plan->outMap,
                    home ? guest->m_homebindX : rt.dungeon.approachX, home ? guest->m_homebindY : rt.dungeon.approachY,
                    home ? guest->m_homebindZ : rt.dungeon.approachZ, 80);
            }
        }
    // Another run of the same order? Only after a run that got somewhere (a boss down, no second wipe).
    LoadDungeonRuns();
    bool again = false;
    if (auto runs = _dungeonRuns.find(bot.guid.GetCounter()); runs != _dungeonRuns.end())
    {
        if (done && plan && !plan->raid && runs->second.left > 1 && rt.dungeon.wipes < WipesFor(*plan))
        {
            bool const endless = runs->second.left == DungeonRunsEndless;
            if (!endless)
                --runs->second.left;
            if (runs->second.done < 250)
                ++runs->second.done;
            CharacterDatabase.Execute("UPDATE ga_bot_dungeon_runs SET runs_left = {}, runs_done = {} WHERE bot_guid = {}",
                uint32(runs->second.left), uint32(runs->second.done), bot.guid.GetCounter());
            again = true;
            if (endless)
                LOG_INFO(ActivityLog, "activity: {}'s order goes on with another run of {} ({} done; until it is stopped)", bot.name, name,
                    uint32(runs->second.done));
            else
                LOG_INFO(ActivityLog, "activity: {}'s order has {} more run{} of {}", bot.name, uint32(runs->second.left),
                    runs->second.left == 1 ? "" : "s", name);
        }
        else
        {
            if (runs->second.left == DungeonRunsEndless)
                LOG_INFO(ActivityLog, "activity: {}'s order of runs of {} ends after {} run{}: {}", bot.name, name,
                    uint32(runs->second.done), runs->second.done == 1 ? "" : "s", how);
            else if (runs->second.left > 1)
                LOG_INFO(ActivityLog, "activity: {}'s order drops its {} further run{} of {}: {}", bot.name,
                    uint32(runs->second.left) - 1, runs->second.left == 2 ? "" : "s", name, how);
            CharacterDatabase.Execute("DELETE FROM ga_bot_dungeon_runs WHERE bot_guid = {}", bot.guid.GetCounter());
            _dungeonRuns.erase(runs);
        }
    }
    // Tanks and healers by their dungeon role: the levelling talents and the levelling set again (a member that
    // cannot change now, in the middle of the way out, does it at its next quiet moment: RoleService::Step). Not
    // between two runs of one order.
    if (!again)
        for (Player* member : DungeonGroup(guild, bot.guid))
            if (GuildBot const* memberBot = guild.FindBot(member->GetGUID()))
                sRoleService.LeaveDungeonMode(*memberBot, member);
    // The next run finds the dungeon fresh.
    Player::ResetInstances(player->GetGUID(), INSTANCE_RESET_ALL, false);
    // The squad's party loots free for all again (KeepSquadGroup set group loot for the run).
    if (Group* group = player->GetGroup(); group && group->GetLeaderGUID() == player->GetGUID() &&
        group->GetLootMethod() != FREE_FOR_ALL && !group->isLFGGroup() && !group->isBGGroup())
    {
        group->SetLootMethod(FREE_FOR_ALL);
        group->SendUpdate();
    }

    rt.dungeon = BotRuntime::DungeonRun();
    ClearTask(rt, 5 * IN_MILLISECONDS, "dungeon run over");
    BotDriver::Rest(player);
    if (again)
    {
        // The order stays: the next step starts the next run, from the gathering place the group stands at.
        rt.dungeon.repeat = true;
        return;
    }
    // The order is done: a group of the run window gets back what its bots were, else the autopilot again.
    if (!DisbandDungeonGroup(guild, bot, true))
    {
        bot.order.type = OrderType::Auto;
        bot.order.zoneId = 0;
    }
    sGuildAdventureMgr.SaveOrder(bot);
    OnOrderChanged(guild, bot);
}

bool OrderService::StepDungeonDead(AdventureGuild& guild, GuildBot& bot, Player* player, BotRuntime& rt)
{
    // The run it belongs to: its own (it leads) or its squad leader's.
    ObjectGuid const leaderGuid = bot.order.type == OrderType::Dungeon ? bot.guid : sSquadService.GetLeader(bot.guid);
    GuildBot* leaderBot = leaderGuid.IsEmpty() ? nullptr : guild.FindBot(leaderGuid);
    BotRuntime* lead = leaderBot && leaderBot->order.type == OrderType::Dungeon ? FindRuntime(leaderGuid) : nullptr;
    if (!lead)
        return false;
    DungeonPlan const* plan = sDungeons.GetPlan(lead->dungeon.id ? lead->dungeon.id : leaderBot->order.zoneId);
    if (!plan)
        return false;
    // (After a restart of the server the run's state is new, and the core has put whoever was dead inside at a
    // graveyard outside: a Lower Blackrock Spire leader stood there as a ghost for minutes while its group waited
    // inside. While its group is in the dungeon its dead are the run's.)
    // (Nor before the run counts as inside: a group wiped out at its first pull, the tank among them, lay dead for
    // over an hour. A character that lies dead in the dungeon is the run's whatever its phase.)
    if (lead->dungeon.phase != PhaseInside && player->GetMapId() != plan->mapId)
    {
        bool groupInside = false;
        for (Player* member : DungeonGroup(guild, leaderGuid))
            groupInside = groupInside || (member->IsAlive() && member->GetMapId() == plan->mapId);
        if (!groupInside)
            return false;
    }

    uint32 const now = std::max<uint32>(getMSTime(), 1);
    if (!rt.dungeon.deadSince)
    {
        rt.dungeon.deadSince = now;
        // The tank died: what it walked into was too much for the way the group fought it (careful pulls from here on).
        if (sRoleService.EffectiveRole(bot) == DungeonRole::Tank && !lead->dungeon.tankDeaths++)
            LOG_INFO(ActivityLog, "activity: the tank {} of {}'s group died in {}: it pulls carefully for the rest of this run",
                bot.name, leaderBot->name, plan->name);
    }
    rt.why = Acore::StringFormat("Dead in {}: its group brings it back", plan->name);
    // (Its spirit stays with its body: mod-playerbots would release it to the graveyard outside, and the way back in
    // is a change of map during which its group takes its leader for gone, plays its own orders, dies and walks to
    // an inn: an Uldaman group fell apart that way after its tank's sixth death.)
    if (player->GetMapId() == plan->mapId && !player->HasPlayerFlag(PLAYER_FLAGS_GHOST))
        BotDriver::PauseAI(player, 3000);

    // Dead in another instance of the map than its leader: no wipe of the group, StepDungeonEnter brings it back.
    if (Player* leaderPlayer = ObjectAccessor::FindPlayer(leaderGuid); leaderPlayer && leaderPlayer != player &&
        leaderPlayer->GetMapId() == plan->mapId && player->GetMapId() == plan->mapId && !SameInstance(player, leaderPlayer))
        return true;
    std::vector<Player*> const group = DungeonGroup(guild, leaderGuid);
    Player* const guest = DungeonGuest(leaderGuid);
    Player* alive = nullptr;
    bool fighting = false;
    // A fight that is one: something is on the character, or it has a living target in reach. (The game's "in combat"
    // also holds for a creature that keeps a combat reference and does nothing: a mage and a rogue of a Deadmines group
    // stood so beside three corpses, and nobody was brought back.)
    auto inFight = [](Player const* member)
    {
        Unit const* target = member->GetVictim();
        return !member->getAttackers().empty() || (target && target->IsAlive() && member->IsWithinDistInMap(target, 40.0f));
    };
    for (Player* member : group)
    {
        // (Not one in another instance of the map: StepDungeonEnter brings either of them to the group.)
        if (!member->IsAlive() || member->GetMapId() != plan->mapId ||
            (player->GetMapId() == plan->mapId && member->GetInstanceId() != player->GetInstanceId()))
            continue;
        alive = alive ? alive : member;
        fighting = fighting || inFight(member);
    }
    // (The character the owner plays by hand: while it stands, the group is not wiped out and comes back beside it.)
    if (guest && guest->IsAlive() && guest->GetMapId() == plan->mapId && !guest->IsBeingTeleported())
    {
        alive = alive ? alive : guest;
        fighting = fighting || inFight(guest);
    }
    // (Nor while the run's boss fights: the Twin Emperors drop their threat at each teleport and stand two seconds
    // without a victim, and the dead of the raid were brought back into the fight at half their health and died
    // again, 2026-11-24.)
    if (alive && !fighting && lead->dungeon.boss < RunBossCount(lead->dungeon, *plan))
        if (Creature const* boss = alive->FindNearestCreature(RunBoss(lead->dungeon, *plan, lead->dungeon.boss).entry, 200.0f, true))
            fighting = boss->IsInCombat();
    // (Nor while creatures still fight about its living or its body: in the owner's Molten Core raid, 2026-11-27, the two
    // healers stood apart with nothing on them while the Flame Imps that had killed the others were still in their
    // fight; the dead were brought back among them, died again, and no wipe was ever counted. For 90 s after its death,
    // so that a creature that keeps a combat reference and does nothing does not hold it for good.)
    constexpr uint32 FightAboutMs = 90 * IN_MILLISECONDS;
    if (alive && !fighting && getMSTimeDiff(rt.dungeon.deadSince, now) < FightAboutMs)
    {
        auto fightNear = [alive](WorldObject const* center, float range)
        {
            for (Creature* creature : HostilesAround(center, alive, range))
                if (creature->IsInCombat() && !creature->IsInEvadeMode())
                    return true;
            return false;
        };
        fighting = fightNear(player, 30.0f);
        for (Player* member : group)
            if (!fighting && member->IsAlive() && member->GetMapId() == plan->mapId && member->GetInstanceId() == alive->GetInstanceId())
                fighting = fightNear(member, 40.0f);
    }
    if (alive)
    {
        lead->dungeon.wipeSince = 0;
        // Not into the creatures that killed it (the observer, 2026-11-14: in Uldaman two characters were brought back
        // every 11 to 15 s at Grimlok and died again within seconds, twelve deaths in 87 s). A death soon after the
        // last revival makes the next one wait longer; creatures at the body bring it up beside the one that stands,
        // and only when nothing hostile is near that one either.
        constexpr uint32 QuickDeathMs = 30 * IN_MILLISECONDS;
        constexpr float BodyYards = 25.0f;
        if (rt.dungeon.revivedAt && rt.dungeon.deadSince && getMSTimeDiff(rt.dungeon.revivedAt, rt.dungeon.deadSince) < QuickDeathMs)
        {
            rt.dungeon.revivedAt = 0;
            rt.dungeon.quickDeaths = std::min<uint8>(rt.dungeon.quickDeaths + 1, 5);
        }
        else if (rt.dungeon.revivedAt && rt.dungeon.deadSince)
        {
            rt.dungeon.revivedAt = 0;
            rt.dungeon.quickDeaths = 0;
        }
        uint32 const wait = ReviveAfterMs * (1 + 2 * rt.dungeon.quickDeaths);
        // (At most three minutes dead: then it comes back beside the one that stands, whatever stands about; a group
        // whose living stood near creatures that had not seen them waited for good.)
        constexpr uint32 DeadMaxMs = 3 * MINUTE * IN_MILLISECONDS;
        bool const overdue = getMSTimeDiff(rt.dungeon.deadSince, now) >= DeadMaxMs;
        if (!fighting && getMSTimeDiff(rt.dungeon.deadSince, now) >= wait)
        {
            // (Nor into a hostile ground effect that still lies on the body: it comes back beside the one that stands.)
            bool const inEffect = HostileGroundEffectAt(player, player);
            bool const inTrap = NearHazard(lead->dungeon, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());
            bool const atBody = overdue || inEffect || inTrap || Escape::HostilesAround(player, BodyYards, true) > 0;
            if (atBody && !overdue && (Escape::HostilesAround(alive, BodyYards) > 0 ||
                    NearHazard(lead->dungeon, alive->GetPositionX(), alive->GetPositionY(), alive->GetPositionZ())))
            {
                rt.why = Acore::StringFormat("Dead in {}: its group brings it back once nothing hostile is near", plan->name);
                return true;
            }
            LOG_INFO(ActivityLog, "activity: {} is brought back by its group in {}{}", bot.name, plan->name, overdue ?
                Acore::StringFormat(" (beside {}: dead for {} s)", alive->GetName(), getMSTimeDiff(rt.dungeon.deadSince, now) / IN_MILLISECONDS) :
                inEffect ? Acore::StringFormat(" (beside {}: a hostile ground effect lies on its body)", alive->GetName()) :
                inTrap ? Acore::StringFormat(" (beside {}: its body lies in a trap)", alive->GetName()) :
                atBody ? Acore::StringFormat(" (beside {}: creatures stand at its body)", alive->GetName()) : std::string());
            Revive(player, alive, atBody);
            rt.dungeon.deadSince = 0;
            rt.dungeon.revivedAt = now;
            rt.hasLastPosition = false;
        }
        return true;
    }

    // Nobody is left standing: a wipe.
    if (!lead->dungeon.wipeSince)
        lead->dungeon.wipeSince = now;
    else if (getMSTimeDiff(lead->dungeon.wipeSince, now) >= WipeAfterMs)
    {
        lead->dungeon.wipeSince = 0;
        ++lead->dungeon.wipes;
        Player* leaderPlayer = ObjectAccessor::FindPlayer(leaderGuid);
        if (lead->dungeon.wipes >= WipesFor(*plan) && leaderPlayer)
        {
            EndDungeonRun(guild, *leaderBot, leaderPlayer, *lead, plan->raid ? Acore::StringFormat("the raid was wiped out {} times",
                uint32(WipesFor(*plan))) : std::string("the group was wiped out twice"), false);
            return true;
        }
        LOG_INFO(ActivityLog, "activity: {}'s group was wiped out in {} (wipe {} of {}): it starts again from the entrance",
            leaderBot->name, plan->name, uint32(lead->dungeon.wipes), uint32(WipesFor(*plan)));
        // No mark left from the fight: the damage dealers attack the creature their icon marks as soon as they see it again.
        if (leaderPlayer)
            if (Group* party = leaderPlayer->GetGroup())
                for (uint8 icon : { SkullIcon, CrossIcon })
                    party->SetTargetIcon(icon, leaderPlayer->GetGUID(), ObjectGuid::Empty);
        lead->dungeon.skull.Clear();
        lead->dungeon.twinPulledAt = lead->dungeon.twinPlacesSince = 0;
        lead->dungeon.cthunSlot.clear();
        lead->dungeon.cthunPuller.Clear();
        lead->dungeon.cthunFightSince = 0;
        lead->dungeon.cthunPut.clear();
        lead->dungeon.twinPlacedAt = lead->dungeon.twinSwapAt = 0;
        lead->dungeon.twinOpened = false;
        // A raid leaves a boss that wiped it twice for this run and goes on to the next (a guild raid does the same with a
        // boss beyond it): the run's wipes are for every boss, not two tries at one. (Garr wiped the first Molten Core raid
        // twice, and the run would have ended there before Lucifron and Magmadar.)
        if (plan->raid && lead->dungeon.boss < RunBossCount(lead->dungeon, *plan))
        {
            uint8 const at = uint8(lead->dungeon.boss);
            if (++lead->dungeon.bossWipes[at] >= 2)
            {
                LOG_INFO(ActivityLog, "activity: {}'s raid leaves {} in {} for this run: it wiped the raid twice", leaderBot->name,
                    RunBoss(lead->dungeon, *plan, at).name, plan->name);
                ++lead->dungeon.boss;
                lead->dungeon.bossStuckSince = 0;
                lead->dungeon.hopped = false;
                lead->dungeon.summonWaitSince = 0;
                lead->dungeon.summonSeen = false;
                lead->dungeon.eventSince = lead->dungeon.eventDoneSince = lead->dungeon.eventNextTalk = lead->dungeon.eventNpcSeen = 0;
                lead->dungeon.eventBossSeen = false;
                lead->dungeon.eventTalks = 0;
            }
        }
        for (Player* member : group)
        {
            if (!member->IsAlive())
            {
                member->ResurrectPlayer(0.5f);
                member->SpawnCorpseBones();
            }
            member->TeleportTo(plan->mapId, plan->inX, plan->inY, plan->inZ, plan->inO);
            if (BotRuntime* other = FindRuntime(member->GetGUID()))
            {
                other->dungeon.deadSince = 0;
                other->hasLastPosition = false;
            }
        }
        // (A raid's gear repaired where it stands, as between two runs: its wipes leave it at half its durability.)
        if (plan->raid)
            DungeonUpkeep(guild, *leaderBot, group);
        // (What the group uses out of the guild storage went with its deaths: once more for everybody.)
        lead->dungeon.suppliesPending.clear();
        lead->dungeon.suppliesGiven.clear();
        for (Player* member : group)
            lead->dungeon.suppliesPending.push_back(member->GetGUID());
        // (Its body lies with the others': the character the owner plays starts again with the group.)
        if (guest && !guest->IsAlive() && guest->GetMapId() == plan->mapId && !guest->IsBeingTeleported() &&
            GetSettings().dungeonsTakeOwnerAlong)
        {
            guest->ResurrectPlayer(0.5f);
            guest->SpawnCorpseBones();
            TellGuest(guest, "Your group was wiped out: it starts again from the entrance, and you with it.");
            guest->TeleportTo(plan->mapId, plan->inX, plan->inY, plan->inZ, plan->inO);
        }
        lead->dungeon.bossStuckSince = 0;
    }
    return true;
}

void OrderService::RunDungeon(AdventureGuild& guild, GuildBot& bot, Player* player, BotRuntime& rt)
{
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    rt.targetZone = 0;
    DungeonPlan const* plan = sDungeons.GetPlan(bot.order.zoneId);
    DungeonInfo const* info = sReadiness.Find(bot.order.zoneId);
    BotRuntime::DungeonRun& run = rt.dungeon;
    std::string error;
    if (!plan || !info || !CheckDungeonOrder(guild, bot, bot.order.zoneId, error))
    {
        EndDungeonRun(guild, bot, player, rt, error.empty() ? "the dungeon is not known" : error, false);
        return;
    }
    std::vector<Player*> const group = DungeonGroup(guild, bot.guid);
    run.groupGuids.clear();
    for (Player* member : group)
        run.groupGuids.push_back(member->GetGUID());
    // The character the owner plays by hand in this group (docs/orders.md "The guild leader in a dungeon run"): no
    // part of 'group', which the run moves and steers. Behind its placeholder AI it joins the group's party.
    Player* const guest = DungeonGuest(bot.guid);
    // No character played by hand (any more): no wait for it. (The owner's client watched the leader two seconds after
    // it came back from a restart in the Wailing Caverns; the wait begun in those seconds was never ended, the "no nearer
    // to the boss" clock stood still, and the group stood eight minutes below Verdan's ledge without its hop.)
    if (!guest)
    {
        run.guestWaitSince = 0;
        run.guestLeftBehind = false;
    }
    if (guest)
    {
        if (!sGuildBotHolder.HasPlaceholder(guest->GetGUID()))
            sGuildBotHolder.StartPlaceholder(guest);
        if (sGuildBotHolder.HasPlaceholder(guest->GetGUID()) && !guest->IsBeingTeleported() &&
            (!player->GetGroup() || guest->GetGroup() != player->GetGroup()))
            BotDriver::JoinGroupOf(guest, player);
    }
    bool const takeGuest = guest && GetSettings().dungeonsTakeOwnerAlong && sGuildBotHolder.HasPlaceholder(guest->GetGUID());
    if (run.id == plan->id && player->GetMapId() == plan->mapId)
        NoteHazards(bot, *plan, run, group, guest, now);
    // (A raid's party is a raid group; so is the party of a group bigger than five: Blackrock Spire's map takes 15, and
    // the owner's Upper Blackrock Spire group of 15 had five in its party, the others outside it and out of its instance,
    // 2026-11-26.)
    bool const raidParty = plan->raid || group.size() > MAXGROUPSIZE;
    if (raidParty)
        KeepRaidGroup(guild, player, group, false);

    // The tank goes in front (the owner's runs start in the window, which makes the first tank the leader; a dungeon
    // order given to a squad whose leader does not tank gets the same): the squad becomes a group of the run window
    // that its first tank leads, and is what it was before when the order is over.
    if (run.id != plan->id && !run.repeat && PutTankInFront(guild, bot, player, group))
        return;

    if (run.id != plan->id)
    {
        bool const repeat = run.repeat;
        run = BotRuntime::DungeonRun();
        run.repeat = repeat;
        run.id = plan->id;
        run.startedAt = now;
        run.phase = PhaseApproach;
        // Where the group gathers before it goes in: the road map's node nearest to the entrance (the entrance itself
        // lies on the dungeon's own ground: tunnels full of elites before the Deadmines' portal), else the entrance.
        run.approachX = plan->outX;
        run.approachY = plan->outY;
        run.approachZ = plan->outZ;
        RoutePoint node;
        std::string nodeName;
        if (!GetSettings().dungeonsTeleport && sRoadMap.FindNearestNode(plan->outMap, plan->outX, plan->outY, 0.0f, 800.0f, node, nodeName))
        {
            run.approachX = node.x;
            run.approachY = node.y;
            run.approachZ = node.z;
        }
        std::string names;
        bool const ready = CheckDungeonReady(guild, bot, plan->id, error, &names);
        // The roles of the group, by its talent templates (a group without a tank or a healer is not refused: the
        // owner sees it in the addon before he gives the order, and here).
        // (The dungeon role the owner gave a bot, else the role of the talents it levels with.)
        uint32 tanks = 0, healers = 0;
        for (Player* member : group)
            if (GuildBot const* memberBot = guild.FindMember(member->GetGUID()))
            {
                DungeonRole const role = sRoleService.EffectiveRole(*memberBot);
                tanks += role == DungeonRole::Tank ? 1 : 0;
                healers += role == DungeonRole::Healer ? 1 : 0;
            }
        std::string const roles = Acore::StringFormat("{} tank{}, {} healer{}, {} damage", tanks, tanks == 1 ? "" : "s", healers,
            healers == 1 ? "" : "s", uint32(group.size()) - std::min<uint32>(uint32(group.size()), tanks + healers));
        LOG_INFO(ActivityLog, "activity: {} starts a dungeon run: {} with {} character{} (readiness: {}; roles: {})", bot.name, plan->name,
            group.size(), group.size() == 1 ? "" : "s", names, roles);
        if (!ready)
        {
            EndDungeonRun(guild, bot, player, rt, error, false);
            return;
        }
        RecordEvent(guild, "DUNGEON", bot.guid, Acore::StringFormat("{} leads {} character{} into {}.", bot.name, group.size(),
            group.size() == 1 ? "" : "s", plan->name), player->GetLevel(), player->GetZoneId());
        // A run that begins with its group already inside (the server was restarted during a run, and the characters
        // entered the world where they stood) starts at the entrance when they stand among creatures: on 2026-11-14 a
        // group entered the world in Mograine's hall of the Cathedral and was wiped out 40 s later.
        if (player->GetMapId() == plan->mapId)
        {
            uint32 among = 0;
            std::string where;
            for (Player* member : group)
                if (member->IsAlive() && member->GetMapId() == plan->mapId)
                    if (uint32 const seen = Escape::HostilesAround(member, 40.0f, true); seen > among)
                    {
                        among = seen;
                        where = member->GetName();
                    }
            if (among)
            {
                LOG_INFO(ActivityLog, "activity: {}'s group stands inside {} as its run begins ({} hostile creatures within 40 yd of {}): it "
                    "starts at the entrance", bot.name, plan->name, among, where);
                for (Player* member : group)
                {
                    if (!member->IsAlive() || member->GetMapId() != plan->mapId || member->IsBeingTeleported())
                        continue;
                    member->CombatStop(true);
                    member->TeleportTo(plan->mapId, plan->inX, plan->inY, plan->inZ, plan->inO);
                    if (BotRuntime* other = member == player ? &rt : FindRuntime(member->GetGUID()))
                        other->hasLastPosition = false;
                }
                run.phase = PhaseInside;
                return;
            }
            // Members far from their leader (they entered the world where each stood when the server went down) are put
            // beside it: they walked to it alone, through creatures nobody of the group had fought, and three of the owner's
            // group died in Blackrock Depths' Manufactory on their way, 350 yd from their leader, 2026-11-23.
            constexpr float GatherYards = 60.0f;
            std::string gathered;
            for (Player* member : group)
            {
                if (member == player || !member->IsAlive() || member->GetMapId() != plan->mapId || member->IsBeingTeleported() ||
                    !SameInstance(member, player) || member->GetExactDist(player) <= GatherYards)
                    continue;
                gathered += (gathered.empty() ? "" : ", ") + Acore::StringFormat("{} ({:.0f} yd)", member->GetName(), member->GetExactDist(player));
                member->CombatStop(true);
                member->NearTeleportTo(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), player->GetOrientation());
                if (BotRuntime* other = FindRuntime(member->GetGUID()))
                    other->hasLastPosition = false;
            }
            if (!gathered.empty())
                LOG_INFO(ActivityLog, "activity: {}'s group stands apart inside {} as its run begins: {} put beside its leader", bot.name,
                    plan->name, gathered);
        }
    }
    if (getMSTimeDiff(run.startedAt, now) > RunMsFor(*plan))
    {
        EndDungeonRun(guild, bot, player, rt, "it took too long", false);
        return;
    }

    // ---- outside: to the entrance, then in
    if (player->GetMapId() != plan->mapId)
    {
        run.phase = PhaseApproach;
        bool const teleport = GetSettings().dungeonsTeleport;
        // (With the teleport on nobody walks: the group is taken into the dungeon from wherever its characters are.)
        if (!teleport)
        {
            // On another land mass: by ship, zeppelin or portal first, as any travel goes.
            if (rt.task.kind == TaskKind::Voyage)
            {
                StepTask(guild, bot, player, rt);
                return;
            }
            Region const here = GetRegion(player);
            Region const there = GetRegionAt(plan->outMap, run.approachX, run.approachY, run.approachZ);
            if (here != there)
            {
                TravelLink const* link = sWorldIndex.NextTravelLink(here, there, player->GetTeamId(), player->GetMapId(),
                    player->GetPositionX(), player->GetPositionY(), plan->outMap, run.approachX, run.approachY);
                if (!link)
                {
                    EndDungeonRun(guild, bot, player, rt, "no ship, zeppelin or portal for us leads to its entrance from here", false);
                    return;
                }
                StartVoyage(player, rt, *link);
                StepTask(guild, bot, player, rt);
                return;
            }
            float const distance = Dist2dTo(player, run.approachX, run.approachY);
            if (distance > EntranceReached)
            {
                if (rt.task.kind != TaskKind::Travel || !rt.task.dungeonWalk)
                {
                    BotTask task;
                    task.kind = TaskKind::Travel;
                    task.dungeonWalk = true;
                    task.hasSpot = true;
                    task.spot.mapId = plan->outMap;
                    task.spot.x = run.approachX;
                    task.spot.y = run.approachY;
                    task.spot.z = run.approachZ;
                    task.targetName = plan->name;
                    StartTask(player, rt, task);
                }
                rt.why = Acore::StringFormat("On the way to {} ({})", plan->name, RoughDistance(distance));
                // Far away: the travel step, which books flights; near: a walk along the road map.
                if (distance > EntranceTravelDistance)
                {
                    if (!StepTask(guild, bot, player, rt) && rt.task.kind == TaskKind::None && ++run.travelFailures >= 3)
                        EndDungeonRun(guild, bot, player, rt, "no way to its entrance was found", false);
                    return;
                }
                if (UpdateStuck(player, rt, run.approachX, run.approachY))
                {
                    ClearTask(rt, 5 * IN_MILLISECONDS, "entrance not reached");
                    if (++run.travelFailures >= 3)
                        EndDungeonRun(guild, bot, player, rt, "its entrance was not reached", false);
                    return;
                }
                WalkTo(player, rt, plan->outMap, run.approachX, run.approachY, run.approachZ);
                return;
            }
        }
        // At the entrance: together, alive and out of combat, then in. (With the teleport: wherever they are, alive,
        // out of combat, not on a flight, and nobody inside any more.)
        if (rt.task.kind != TaskKind::None)
            ClearTask(rt, 0, teleport ? "getting ready for the dungeon" : "at the entrance");
        // (With the teleport the owner's own character goes in with the group, like the bots: he started the run
        // with it in the group. Without it he walks in by himself.)
        std::vector<Player*> movers = group;
        if (takeGuest && teleport)
            movers.push_back(guest);
        for (Player* member : movers)
        {
            bool const wait = teleport ?
                (!member->IsAlive() || member->IsInCombat() || member->IsInFlight() || member->IsBeingTeleported() ||
                    member->GetMapId() == plan->mapId) :
                (member != player && (!member->IsAlive() || member->GetMapId() != player->GetMapId() ||
                    !member->IsWithinDistInMap(player, GroupDistance) || member->IsInCombat()));
            if (!wait)
                continue;
            rt.why = Acore::StringFormat("{} {}: waiting for {}{}", teleport ? "Getting ready for" : "At the entrance of", plan->name,
                member->GetName(), !teleport ? "" : !member->IsAlive() ? " (dead)" : member->IsInCombat() ? " (in a fight)" :
                member->IsInFlight() ? " (on a flight)" : "");
            run.readySince = 0;
            if (member == guest && !run.guestToldStart && member->GetMapId() != plan->mapId)
            {
                run.guestToldStart = true;
                TellGuest(guest, Acore::StringFormat("Your group is ready for {} and waits for you ({}).", plan->name,
                    !member->IsAlive() ? "you are dead" : member->IsInCombat() ? "you are in a fight" : "you are on your way"));
            }
            // (Nobody walks to the group here: a member that does not get free in time ends the start.)
            if (teleport)
            {
                if (!run.gatherSince)
                    run.gatherSince = now;
                else if (getMSTimeDiff(run.gatherSince, now) > 2 * ReadyWaitMs)
                    EndDungeonRun(guild, bot, player, rt, Acore::StringFormat("{} did not get free for it ({})", member->GetName(),
                        !member->IsAlive() ? "dead" : member->IsInCombat() ? "in a fight" : "on its way"), false);
            }
            return;
        }
        run.gatherSince = 0;
        // Between two runs of one order: repairs and room in the bags, once.
        if (run.repeat)
        {
            run.repeat = false;
            DungeonUpkeep(guild, bot, group);
        }
        // A fresh dungeon: nobody of the group is bound to an old instance of it. (The reset at the end of a run
        // does not reach the instance the group has just left: its characters are still on that map while their
        // teleports out are on the way, and the core resets no instance with a player in it. A group that went in
        // again at once found its old instance, every boss dead, and "finished" it in seconds.)
        // (A raid cannot be reset: its lockout holds for the week. The group goes into its saved instance, and what fell
        // there this week stays dead and is counted when the group comes to its place.)
        if (!run.fresh && plan->raid)
        {
            run.fresh = true;
            // (Repairs and room in the bags before a raid, as between two runs of a dungeon.)
            DungeonUpkeep(guild, bot, group);
            if (sInstanceSaveMgr->PlayerGetInstanceSave(player->GetGUID(), plan->mapId, player->GetDifficulty(true)))
                LOG_INFO(ActivityLog, "activity: {}'s raid goes into its saved instance of {} (the raid's lockout holds for the week: what "
                    "fell there stays dead)", bot.name, plan->name);
        }
        if (!run.fresh)
        {
            bool bound = false;
            for (Player* member : movers)
                if (sInstanceSaveMgr->PlayerGetInstanceSave(member->GetGUID(), plan->mapId, DUNGEON_DIFFICULTY_NORMAL))
                {
                    bound = true;
                    Player::ResetInstances(member->GetGUID(), INSTANCE_RESET_ALL, false);
                }
            if (bound)
            {
                if (!run.resetSince)
                    run.resetSince = now;
                if (getMSTimeDiff(run.resetSince, now) > ResetWaitMs)
                {
                    EndDungeonRun(guild, bot, player, rt, "its old instance could not be reset", false);
                    return;
                }
                rt.why = Acore::StringFormat("At the entrance of {}: the dungeon is reset", plan->name);
                run.readySince = 0;
                return;
            }
            if (run.resetSince)
                LOG_INFO(ActivityLog, "activity: {} reset {} for its run (the group's old instance is gone)", bot.name, plan->name);
            run.fresh = true;
            run.resetSince = 0;
        }
        // The core lets a character into a few different dungeon instances per hour (AccountInstancesPerHour, 5):
        // a group that farms a short dungeon waits here until its oldest visit is an hour ago.
        for (Player* member : movers)
            if (!member->CheckInstanceCount(0))
            {
                if (!run.limitNoted)
                {
                    run.limitNoted = true;
                    LOG_INFO(ActivityLog, "activity: {}'s group waits at the entrance of {}: {} has been in as many dungeons "
                        "within the hour as the server allows (AccountInstancesPerHour)", bot.name, plan->name, member->GetName());
                }
                rt.why = Acore::StringFormat("At the entrance of {}: waiting, {} was in too many dungeons this hour", plan->name,
                    member->GetName());
                // (The run has not begun: the wait does not count as its time.)
                run.startedAt = now;
                run.readySince = 0;
                return;
            }
        // Everybody recovered (they rest by themselves), or the group has waited long enough.
        if (!run.readySince)
            run.readySince = now;
        if (getMSTimeDiff(run.readySince, now) < ReadyWaitMs)
            for (Player* member : movers)
            {
                // (Its mana only when it has something to drink, or it is the healer: a hunter with nothing to drink
                // held the start for three minutes, "At the entrance of Deadmines: Hunterbm recovers".)
                GuildBot const* memberBot = guild.FindMember(member->GetGUID());
                bool const healer = memberBot && sRoleService.EffectiveRole(*memberBot) == DungeonRole::Healer;
                bool const waitsForMana = healer || CarriesDrink(member) || StorageSupply::HasDrink(member);
                if (member->GetHealthPct() < ReadyHealth || (waitsForMana && member->getPowerType() == POWER_MANA &&
                    member->GetMaxPower(POWER_MANA) && member->GetPowerPct(POWER_MANA) < ReadyMana))
                {
                    rt.why = Acore::StringFormat("At the entrance of {}: {} recovers", plan->name, member->GetName());
                    return;
                }
                // (No wait for a drink out of the guild storage any more: a character takes one piece there at the
                // moment it drinks, StorageSupply::TakeOne; the old wait for a stack held every start for three minutes.)
            }
        // Tanks and healers by their dungeon role: the talents of the role and the dungeon set, here at the entrance.
        for (Player* member : group)
            if (GuildBot const* memberBot = guild.FindBot(member->GetGUID()))
                if (!sRoleService.EnterDungeonMode(guild, *memberBot, member))
                {
                    rt.why = Acore::StringFormat("At the entrance of {}: {} puts on its dungeon set", plan->name, member->GetName());
                    return;
                }
        // What the group uses out of the guild storage (elixirs, scrolls, stones, scopes): everybody once, inside.
        run.suppliesPending.clear();
        run.suppliesGiven.clear();
        run.lateGathered.clear();
        for (Player* member : group)
            run.suppliesPending.push_back(member->GetGUID());
        // (A mana user that goes in with nothing to drink is the slowest way through a dungeon: the log says so.)
        std::string thirsty;
        for (Player* member : group)
            if (member->getPowerType() == POWER_MANA && !CarriesDrink(member) && !StorageSupply::HasDrink(member))
                thirsty += (thirsty.empty() ? "" : ", ") + member->GetName();
        LOG_INFO(ActivityLog, "activity: {} goes into {} with {} character{}{}", bot.name, plan->name, group.size(),
            group.size() == 1 ? "" : "s", thirsty.empty() ? std::string() : " (nothing to drink: " + thirsty + ")");
        if (takeGuest && teleport)
        {
            TellGuest(guest, Acore::StringFormat("Your group goes into {} now.", plan->name));
            SendGroupNames(guest, group);
        }
        // A raid: the core lets nobody onto its map who is not in a raid group, so the leader's party is one first, with
        // the whole group in it (two minutes at most for those who are still on their way into it). The same for a group
        // bigger than a party: the core takes a character into the instance of its party's leader.
        if (raidParty)
        {
            uint32 inRaid = 0;
            Group const* party = player->GetGroup();
            for (Player* member : group)
                inRaid += party && member->GetGroup() == party ? 1 : 0;
            bool const raidReady = KeepRaidGroup(guild, player, group, true);
            if (!raidReady || (inRaid < group.size() && (!run.raidWaitSince || getMSTimeDiff(run.raidWaitSince, now) < 2 * MINUTE * IN_MILLISECONDS)))
            {
                if (!run.raidWaitSince)
                    run.raidWaitSince = now;
                if (!raidReady && getMSTimeDiff(run.raidWaitSince, now) > ReadyWaitMs)
                {
                    EndDungeonRun(guild, bot, player, rt, "its raid group could not be formed", false);
                    return;
                }
                rt.why = Acore::StringFormat("Getting ready for {}: the raid group forms ({} of {} in it)", plan->name, inRaid, group.size());
                return;
            }
            run.raidWaitSince = 0;
        }
        // The leader goes in first, the others once it is inside (StepDungeonEnter): the core gives a character the
        // instance of its party leader only when that leader is bound to one, and a character it cannot place so a new
        // instance of its own. All went in in the same second, and an Uldaman group stood in three instances (the owner
        // came to his healer's place as the tank and saw neither it, nor its foes, nor the dead mage).
        run.enterPending.clear();
        run.nextEnterLook = 0;
        for (Player* member : movers)
        {
            if (member != player)
            {
                run.enterPending.push_back(member->GetGUID());
                continue;
            }
            member->TeleportTo(plan->mapId, plan->inX, plan->inY, plan->inZ, plan->inO);
            if (BotRuntime* other = FindRuntime(member->GetGUID()))
                other->hasLastPosition = false;
        }
        run.phase = PhaseInside;
        run.bossStuckSince = 0;
        return;
    }

    // ---- inside
    run.phase = PhaseInside;
    run.wipeSince = 0;
    // A member that came into the world after its run had begun (the server was restarted during the run, and the
    // character entered the world a moment after its group had been gathered or taken to the entrance) is put beside
    // its leader, as the members are when a run begins with its group apart. The owner's leader entered the world 2 s
    // after its group had been taken to the entrance of Dire Maul West, stood alone 243 yd from it, and walked to it
    // through creatures nobody had fought, round 260. (Only within a minute and a half of its login: a character the
    // owner played by hand and then lets the AI play is no character that entered the world where it stood.)
    if (!player->IsBeingTeleported())
        for (Player* member : group)
        {
            if (member == player || !member->IsAlive() || member->GetMapId() != plan->mapId || member->IsBeingTeleported() ||
                !SameInstance(member, player) || member->GetExactDist(player) <= 60.0f ||
                GameTime::GetGameTime().count() - member->m_logintime > 90 ||
                std::find(run.lateGathered.begin(), run.lateGathered.end(), member->GetGUID()) != run.lateGathered.end())
                continue;
            run.lateGathered.push_back(member->GetGUID());
            LOG_INFO(ActivityLog, "activity: {} came into the world inside {} after its group's run had begun, {:.0f} yd from its "
                "leader {}: it is put beside it", member->GetName(), plan->name, member->GetExactDist(player), bot.name);
            Position spot = player->GetPosition();
            player->MovePositionToFirstCollision(spot, 3.0f, float(M_PI) + frand(-0.6f, 0.6f));
            member->CombatStop(true);
            member->NearTeleportTo(spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ(), player->GetOrientation());
            if (BotRuntime* other = FindRuntime(member->GetGUID()))
                other->hasLastPosition = false;
        }
    // (Not while an area trigger's event runs: its gates are the event's. Upper Blackrock Spire, 2026-11-20: the
    // group opened the stadium's wave door, walked into the pen behind it and fought its creatures with every wave.)
    if (!(run.triggerAt && run.triggerFor == run.boss + 1 && getMSTimeDiff(run.triggerAt, now) < 15 * MINUTE * IN_MILLISECONDS))
        OpenDoorsInTheWay(bot, player, rt, plan->name, run.tribute != 0);
    // The clock of "no nearer to the boss" only runs while the group could walk: not while somebody fights, rests,
    // is dead or the leader waits for its members (a group at the dungeon's level fights its way for minutes; the
    // first at-level run passed Gilnid over while it was clearing the room before him).
    bool groupBusy = false;
    {
        bool busy = !rt.squad.waitFor.empty() || run.guestWaitSince != 0;
        // (A fight counts when its creature is near and on the member's level: Blackrock Depths' group stood four
        // minutes "in a fight" with creatures on the floor below, which it could not get to, and was not put on to
        // Warder Stilgiss meanwhile, 2026-11-23.)
        auto fightsNear = [](Player* member)
        {
            auto reachable = [member](Unit const* foe)
            {
                return foe && foe->IsAlive() && member->GetExactDist2d(foe) < 40.0f && std::fabs(foe->GetPositionZ() - member->GetPositionZ()) < 8.0f;
            };
            if (reachable(member->GetVictim()))
                return true;
            for (Unit* attacker : member->getAttackers())
                if (reachable(attacker))
                    return true;
            return false;
        };
        for (Player* member : group)
        {
            BotRuntime const* other = FindRuntime(member->GetGUID());
            busy = busy || !member->IsAlive() || (member->IsInCombat() && fightsNear(member)) || (other && other->rest.active);
        }
        groupBusy = busy;
        // (A long gap since the last step is time this step did not run at all: the leader waited for its squad,
        // rested, fought or was dead. The second run of the first double order passed Gilnid over because every
        // two-minute wait for a resting member counted as five seconds of waiting and the rest as walking.)
        uint32 const gap = run.lastStep ? getMSTimeDiff(run.lastStep, now) : 0;
        // (Never past now: a clock set to now later in a step, after a step that returned before this point, was pushed
        // into the future, and getMSTimeDiff of a time after now wraps round: Zul'Farrak's group "did not get to" Theka
        // 2 min into its run, and to the next two bosses within seconds, round 6.)
        if (run.bossStuckSince && (busy || gap > StepGapMs))
            run.bossStuckSince = std::min(run.bossStuckSince + gap, now);
        // (The same for the way to an object to use: the owner's Uldaman group fought and waited on its way to the Altar of
        // Archaedas, ran out of the 90 s and went back to use the first altar again before it came.)
        if (run.objectSince && (busy || gap > StepGapMs))
            run.objectSince = std::min(run.objectSince + gap, now);
        run.lastStep = now;
        // (Where the leader stands while it could walk: the hop comes sooner when it stands still.)
        if (busy || !run.stillSince || player->GetExactDist2d(run.stillX, run.stillY) > StillYards)
        {
            run.stillSince = now;
            run.stillX = player->GetPositionX();
            run.stillY = player->GetPositionY();
        }
    }
    // A big pull: the fight step drives the tank (StepDungeonGather).
    if (run.gather.active)
    {
        PauseStuck(rt);
        return;
    }
    // The character the owner plays by hand: the group stays with it.
    if (guest && StepDungeonGuest(bot, player, rt, *plan, guest, group))
        return;
    // The spread limit (package P2): the tank stands while a member is farther than 15 yd from it. (Not while an
    // event's NPC walks: the group goes with the NPC, and the event has its own rules for one that falls behind.)
    if (!run.eventSince && StepDungeonSpread(bot, player, rt, *plan, group))
        return;
    // The group rests together when somebody is below the trigger (package P6b).
    if (!run.eventSince && StepDungeonGroupRest(guild, bot, player, rt, *plan, group))
        return;
    // The buff plan (package P8).
    StepDungeonBuffPlan(guild, bot, player, rt, *plan, group, guest);
    // What the group uses out of the guild storage: each character once at the start of a run and once after a wipe
    // (the owner, 2026-11-14: [quote left out]), at its first
    // quiet moment inside, and only what fits it best by the stat weights of its role (DungeonSupplies::Use).
    // A member that is in the group now and had no round yet gets it: the guild leader's character, played by hand
    // or not yet in the world when the run began, is a member once the AI plays it. (The owner watched his leader through
    // a Dire Maul West run: its four bots used their elixirs and scrolls inside, Retadin nothing, round 257.)
    for (Player* member : group)
        if (std::find(run.suppliesGiven.begin(), run.suppliesGiven.end(), member->GetGUID()) == run.suppliesGiven.end() &&
            std::find(run.suppliesPending.begin(), run.suppliesPending.end(), member->GetGUID()) == run.suppliesPending.end())
            run.suppliesPending.push_back(member->GetGUID());
    if (!run.suppliesPending.empty())
    {
        std::vector<uint32> const items = GetRunItems(bot.guid);
        std::vector<ObjectGuid> later;
        for (ObjectGuid const& pending : run.suppliesPending)
        {
            Player* member = nullptr;
            for (Player* candidate : group)
                if (candidate->GetGUID() == pending)
                    member = candidate;
            if (items.empty())
                continue;
            // (Not in the group for a moment: since the leader goes in first, its members see it "not in the world"
            // during its teleport and leave its squad for a few seconds. Dropped then, they used nothing at all, only
            // the leaders did, in the owner's Uldaman and Cathedral runs.)
            if (!member || member->GetMapId() != plan->mapId || !member->IsAlive() || member->IsInCombat() || member->IsMounted() ||
                member->IsBeingTeleported())
            {
                later.push_back(pending);
                continue;
            }
            GuildBot const* memberBot = guild.FindMember(pending);
            DungeonSupplies::Use(member, items, "for its dungeon run", memberBot ? int8(sRoleService.EffectiveRole(*memberBot)) : int8(0));
            run.suppliesGiven.push_back(pending);
        }
        run.suppliesPending = std::move(later);
    }
    // The next boss that is still there.
    // The order of the way (package P4), once the group is inside.
    // Dire Maul North's tribute run (OrderTribute.cpp): its own bosses, and its steps before King Gordok.
    // (Once the whole group is inside, at most 30 s after the leader: its members leave its squad for a few seconds
    // while it is teleported in, and the rogue with Lockpicking 300 of the fourth test was not seen, 2026-11-21.)
    if (!run.tribute && plan->id == TributeDungeonId && GetFightRules(bot.guid).mode == ModeTribute)
    {
        if (!run.tributeSince)
            run.tributeSince = now;
        if (group.size() >= sSquadService.GetMembers(bot.guid).size() + 1 || getMSTimeDiff(run.tributeSince, now) > 30 * IN_MILLISECONDS)
            BeginTribute(guild, bot, *plan, run, group);
    }
    if (run.order.empty() && plan->bosses.size() > 1)
        run.order = BossOrder(player, *plan);
    if (run.tribute && StepTribute(guild, bot, player, rt, *plan, group, guest))
        return;
    while (run.boss < RunBossCount(run, *plan))
    {
        DungeonBoss const& boss = RunBoss(run, *plan, run.boss);
        float const distance = Dist2dTo(player, boss.x, boss.y);
        Creature* creature = player->FindNearestCreature(boss.entry, 150.0f, true);
        // A boss that walks or flies a round (the dragons of the Sunken Temple) is not at its place: its spawn says
        // where it is now. (The first Temple run counted Dreamscythe and Weaver as done without having seen them.)
        float goX = boss.x, goY = boss.y, goZ = boss.z;
        if (!creature && boss.spawnId && !boss.IsEvent())
        {
            auto const bounds = player->GetMap()->GetCreatureBySpawnIdStore().equal_range(boss.spawnId);
            for (auto itr = bounds.first; itr != bounds.second; ++itr)
                if (itr->second && itr->second->IsAlive() && itr->second->IsInWorld())
                    creature = itr->second;
            if (creature)
            {
                goX = creature->GetPositionX();
                goY = creature->GetPositionY();
                goZ = creature->GetPositionZ();
            }
        }
        // The Shade of Eranikus stays unselectable when Jammal'an the Prophet fell before: the instance script frees him
        // at Jammal'an's death only, and not when his grid was not loaded then or the server restarted since (it sets
        // the flag again whenever he is created). The owner's group stood at him in four runs of the Sunken Temple, "does not
        // get to Shade of Eranikus (0 yd away)", 2026-11-22. Jammal'an is dead when the map holds a respawn time for him.
        if (creature && creature->GetEntry() == 5709 && creature->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE))
            for (DungeonBoss const& other : plan->bosses)
                if (other.entry == 5710 && other.spawnId && player->GetMap()->GetCreatureRespawnTime(other.spawnId))
                {
                    creature->RemoveUnitFlag(UNIT_FLAG_NOT_SELECTABLE);
                    LOG_INFO(ActivityLog, "activity: {}'s group frees the Shade of Eranikus in {}: Jammal'an the Prophet is dead, but the "
                        "instance script left him unselectable", bot.name, plan->name);
                    break;
                }
        // A boss that an event brings: the stop at the NPC that starts it, or at the object that brings it.
        if (boss.IsEvent())
        {
            EventStep const step = StepDungeonEvent(bot, player, rt, *plan, boss, group, groupBusy);
            if (step == EventStep::Busy)
                return;
            if (step != EventStep::Approach)
            {
                if (step == EventStep::Done)
                {
                    ++run.bossesDown;
                    // (A raid's end boss that its event brings, Ragnaros: it ends the raid.)
                    if (boss.last)
                        run.endBossDown = true;
                    if (!rt.dungeon.metrics.bossesJudged.count(boss.entry))
                        ++rt.dungeon.metrics.bossUnseen;
                    LOG_INFO(ActivityLog, "activity: {}'s group is done with {} in {} ({} of {})", bot.name, boss.name, plan->name,
                        uint32(run.boss + 1), RunBossCount(run, *plan));
                }
                if (run.eventHops > 1)
                    LOG_INFO(ActivityLog, "activity: {}'s group was put beside the NPC of {}'s event {} times in {}", bot.name, boss.name,
                        run.eventHops, plan->name);
                run.eventSince = run.eventDoneSince = run.eventNextTalk = run.eventNpcSeen = run.eventNextHop = run.eventHops = 0;
                run.eventRouteAt = run.eventRouteSteps = 0;
                run.eventRouteBegun = false;
                run.eventBossSeen = false;
                run.eventTalks = 0;
                ++run.boss;
                run.bossStuckSince = 0;
                run.hopped = false;
                continue;
            }
        }
        // What holds the boss back first (DungeonBoss::clearFirst): the creatures of those kinds within their reach of the
        // places, the nearest to the group first, each pulled as on the way. When none is left the boss comes (Timmy the
        // Cruel 10 s later) or its way is open; at most 20 minutes, a creature it gets no 5 yd nearer to in a minute is
        // left.
        if (!boss.clearFirst.empty() && (!creature || !creature->IsInCombat()))
        {
            constexpr uint32 ClearMaxMs = 20 * MINUTE * IN_MILLISECONDS;
            constexpr uint32 NoNearerMs = MINUTE * IN_MILLISECONDS;
            constexpr uint32 ComeMs = 30 * IN_MILLISECONDS;
            if (run.clearFor != run.boss + 1)
            {
                run.clearFor = uint8(run.boss + 1);
                run.clearSince = now;
                run.clearDoneAt = 0;
                run.clearTarget.Clear();
                run.clearSkip.clear();
            }
            Creature* next = nullptr;
            uint32 live = 0, fighting = 0;
            std::string kinds;
            if (getMSTimeDiff(run.clearSince, now) < ClearMaxMs)
                for (auto const& [spawnId, other] : player->GetMap()->GetCreatureBySpawnIdStore())
                {
                    if (!other || !other->IsAlive() || !other->IsInWorld())
                        continue;
                    bool held = false;
                    for (DungeonBoss::Clear const& item : boss.clearFirst)
                        held = held || (std::find(item.entries.begin(), item.entries.end(), other->GetEntry()) != item.entries.end() &&
                            other->GetExactDist(item.x, item.y, item.z) <= item.yards);
                    if (!held)
                        continue;
                    ++live;
                    if (kinds.size() < 120 && kinds.find(other->GetName()) == std::string::npos)
                        kinds += (kinds.empty() ? "" : ", ") + other->GetName();
                    if (other->IsInCombat())
                    {
                        ++fighting;
                        continue;
                    }
                    if (run.clearSkip.count(other->GetGUID()))
                        continue;
                    if (!next || player->GetExactDist(other) < player->GetExactDist(next))
                        next = other;
                }
            if (next || fighting)
            {
                if (run.clearLogged != run.boss + 1)
                {
                    run.clearLogged = uint8(run.boss + 1);
                    LOG_INFO(ActivityLog, "activity: {}'s group clears what holds {} back in {} first: {} creature{} ({})", bot.name,
                        boss.name, plan->name, live, live == 1 ? "" : "s", kinds);
                }
                rt.why = Acore::StringFormat("In {}: clearing what holds {} back ({} left)", plan->name, boss.name, live);
                PauseStuck(rt);
                run.bossStuckSince = now;
                if (!next || player->IsInCombat() || rt.pull.active)
                    return;
                if (run.clearTarget != next->GetGUID())
                {
                    run.clearTarget = next->GetGUID();
                    run.clearTargetSince = now;
                    run.clearBest = player->GetExactDist(next);
                }
                float const away = player->GetExactDist(next);
                if (away < run.clearBest - 5.0f)
                {
                    run.clearBest = away;
                    run.clearTargetSince = now;
                }
                else if (getMSTimeDiff(run.clearTargetSince, now) >= NoNearerMs)
                {
                    // No way on foot to it (a level below that players drop down to: the two lower crystal generators of
                    // Dire Maul West, 2026-11-21): the group is put near it, once per creature, as before a boss (DG6).
                    if (GetSettings().dungeonsHopWhenNoWay && run.clearHopTarget != next->GetGUID() &&
                        NoWayOnFoot(player, next->GetPositionX(), next->GetPositionY(), next->GetPositionZ()))
                    {
                        run.clearHopTarget = next->GetGUID();
                        float x = 0.0f, y = 0.0f, z = 0.0f;
                        if (FindHopSpot(player, next->GetPositionX(), next->GetPositionY(), next->GetPositionZ(), x, y, z))
                        {
                            LOG_INFO(ActivityLog, "activity: {}'s group finds no way on foot to {} (level {}, {:.0f} yd away) in {} while "
                                "it clears what holds {} back: it is put near it", bot.name, next->GetName(), uint32(next->GetLevel()),
                                away, plan->name, boss.name);
                            TakeGuestAlong(bot, player, x, y, z, Acore::StringFormat("no way on foot to {}", next->GetName()));
                            for (Player* member : group)
                            {
                                if (!member->IsAlive() || member->GetMapId() != plan->mapId)
                                    continue;
                                member->NearTeleportTo(x, y, z, member->GetOrientation());
                                if (BotRuntime* other = FindRuntime(member->GetGUID()))
                                    other->hasLastPosition = false;
                            }
                            run.clearTargetSince = now;
                            run.clearBest = 1000.0f;
                            return;
                        }
                    }
                    run.clearSkip.insert(next->GetGUID());
                    LOG_INFO(ActivityLog, "activity: {}'s group does not get to {} (level {}, {:.0f} yd away) in {} in a minute: it is left",
                        bot.name, next->GetName(), uint32(next->GetLevel()), away, plan->name);
                    return;
                }
                if (away > 30.0f || !player->IsWithinLOSInMap(next))
                {
                    // No 5 yd nearer for 20 s by the long walk: the core's path, walked by a point movement (Dire Maul
                    // West, 2026-11-21: the tank stood with the walk to an Arcane Aberration 167 yd away set, a whole path
                    // there, and left one creature after the other).
                    if (getMSTimeDiff(run.clearTargetSince, now) >= 20 * IN_MILLISECONDS)
                    {
                        if (getMSTimeDiff(rt.task.lastAssert, now) >= 2 * IN_MILLISECONDS || !player->isMoving())
                        {
                            if (BotDriver::GetRpgState(player) == RpgState::GoCamp)
                                BotDriver::Rest(player);
                            player->GetMotionMaster()->MovePoint(0, next->GetPositionX(), next->GetPositionY(), next->GetPositionZ());
                            rt.task.lastAssert = now;
                        }
                        return;
                    }
                    if (BotDriver::GetRpgState(player) != RpgState::GoCamp || getMSTimeDiff(rt.task.lastAssert, now) >= AssertMs)
                    {
                        DungeonGoTo(player, plan->mapId, next->GetPositionX(), next->GetPositionY(), next->GetPositionZ());
                        rt.task.lastAssert = now;
                    }
                    return;
                }
                // In reach: the owner's release for a normal pull first, then the pull (as on the way).
                if (!DungeonGate(guild, bot, player, rt, *plan, group, guest, Acore::StringFormat("the pull of {}", next->GetName()),
                        60000 + run.clearCount, false))
                    return;
                ++run.clearCount;
                float const at[3] = { player->GetPositionX(), player->GetPositionY(), player->GetPositionZ() };
                if (!GetFightRules(bot.guid).carefulPulls || !Escape::BeginPull(player, rt, next, at))
                    BotDriver::Attack(player, next, false);
                return;
            }
            // All of them are dead (or left): the boss comes, or its way opens.
            if (!run.clearDoneAt)
            {
                run.clearDoneAt = now;
                LOG_INFO(ActivityLog, "activity: {}'s group has cleared what held {} back in {}{}", bot.name, boss.name, plan->name,
                    run.clearSkip.empty() ? "" : Acore::StringFormat(" ({} left where it did not get to them)", run.clearSkip.size()));
            }
            if ((!creature || !player->IsValidAttackTarget(creature)) && getMSTimeDiff(run.clearDoneAt, now) < ComeMs)
            {
                rt.why = Acore::StringFormat("In {}: waiting for {} to come", plan->name, boss.name);
                PauseStuck(rt);
                run.bossStuckSince = now;
                return;
            }
        }
        // A boss whose event an area trigger starts (DungeonBoss::areaTrigger): while it cannot be fought, the leader
        // walks into the trigger and reports it as a client does, once; then the group waits for the boss where it
        // stands (the waves of the event come to it), at most the event's time.
        if (boss.areaTrigger && creature && creature->IsAlive() && !creature->IsInCombat() && Dist2dTo(player, boss.x, boss.y) <= 150.0f &&
            !player->IsInCombat() && (!player->IsValidAttackTarget(creature) || creature->HasUnitFlag(UnitFlags(UNIT_FLAG_NON_ATTACKABLE |
                UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_IMMUNE_TO_PC))))
        {
            uint32 const waitMs = (boss.timeout ? boss.timeout : 900) * IN_MILLISECONDS;
            if (run.triggerFor != run.boss + 1)
            {
                AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(boss.areaTrigger);
                if (!trigger)
                    run.triggerFor = uint8(run.boss + 1);
                else if (!player->IsInAreaTriggerRadius(trigger))
                {
                    if (run.triggerWalkFor != run.boss + 1)
                    {
                        run.triggerWalkFor = uint8(run.boss + 1);
                        run.triggerSince = now;
                    }
                    // (Only the time it walks: the stadium's group fought dragonspawn and died on the way, and gave the
                    // trigger up after two minutes of fights, 2026-11-20.)
                    else if (run.triggerSeen && getMSTimeDiff(run.triggerSeen, now) > 3 * IN_MILLISECONDS)
                        run.triggerSince = std::min(run.triggerSince + getMSTimeDiff(run.triggerSeen, now), now);
                    run.triggerSeen = now;
                    if (getMSTimeDiff(run.triggerSince, now) < 3 * MINUTE * IN_MILLISECONDS)
                    {
                        if (BotDriver::GetRpgState(player) != RpgState::GoCamp || getMSTimeDiff(rt.task.lastAssert, now) >= AssertMs)
                        {
                            DungeonGoTo(player, plan->mapId, trigger->x, trigger->y, trigger->z);
                            rt.task.lastAssert = now;
                        }
                        PauseStuck(rt);
                        run.bossStuckSince = now;
                        rt.why = Acore::StringFormat("In {}: on the way to where {}'s event begins", plan->name, boss.name);
                        return;
                    }
                    LOG_INFO(ActivityLog, "activity: {} does not get into area trigger {} for {} in {} in 3 min of walking", bot.name,
                        boss.areaTrigger, boss.name, plan->name);
                    run.triggerFor = uint8(run.boss + 1);
                }
                else
                {
                    // The whole group ready first, as before a boss: the waves of the event do not wait for a rest.
                    if (!DungeonGate(guild, bot, player, rt, *plan, group, guest, Acore::StringFormat("the event of {}", boss.name),
                            80000 + run.boss, true))
                        return;
                    WorldPacket packet(CMSG_AREATRIGGER, 4);
                    packet << uint32(boss.areaTrigger);
                    player->GetSession()->HandleAreaTriggerOpcode(packet);
                    run.triggerFor = uint8(run.boss + 1);
                    run.triggerAt = now;
                    LOG_INFO(ActivityLog, "activity: {} starts the event of {} in {}: it walks into area trigger {}", bot.name, boss.name,
                        plan->name, boss.areaTrigger);
                    return;
                }
            }
            else if (run.triggerAt && getMSTimeDiff(run.triggerAt, now) < waitMs)
            {
                PauseStuck(rt);
                run.bossStuckSince = now;
                // The group holds the place where the event began (the stadium's middle): a fight's chase took the tank
                // towards the wave door, and the next wave met it there with the creatures of the pen behind it.
                AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(boss.areaTrigger);
                if (trigger && player->GetExactDist2d(trigger->x, trigger->y) > 12.0f)
                {
                    if (BotDriver::GetRpgState(player) != RpgState::GoCamp || getMSTimeDiff(rt.task.lastAssert, now) >= AssertMs)
                    {
                        DungeonGoTo(player, plan->mapId, trigger->x, trigger->y, trigger->z);
                        rt.task.lastAssert = now;
                    }
                    rt.why = Acore::StringFormat("In {}: back to where {}'s event began", plan->name, boss.name);
                    return;
                }
                if (player->isMoving())
                    BotDriver::Rest(player);
                rt.why = Acore::StringFormat("In {}: waiting for {} to come", plan->name, boss.name);
                return;
            }
        }
        // A boss that sleeps or stands behind a door until something is used (DungeonBoss::useObjects): while it lives
        // and cannot be fought, the leader uses those objects one after the other, as far as it can get to them.
        // (It sleeps while nothing is in combat with it: a leader that "attacks" the stoned Archaedas is in a fight
        // that never begins.)
        bool const asleep = creature && (creature->HasUnitState(UNIT_STATE_STUNNED) || !player->IsValidAttackTarget(creature) ||
            creature->HasUnitFlag(UnitFlags(UNIT_FLAG_STUNNED | UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_IMMUNE_TO_PC)) ||
            // (Out of sight counts only until every object was used once: Atal'alarion woke in the pit after the sixth
            // statue, out of the balcony's sight, and the group started again at the first, which poisons its user.)
            (!player->IsWithinLOSInMap(creature) && (boss.useObjects.empty() || run.objectAt < boss.useObjects.size())));
        if (!boss.useObjects.empty() && asleep && Dist2dTo(player, boss.x, boss.y) <= 150.0f &&
            (!player->IsInCombat() || !player->GetVictim() || player->GetVictim() == creature) &&
            run.objectsUsed < 4 * boss.useObjects.size())
        {
            if (player->GetVictim() == creature)
            {
                player->AttackStop();
                player->CombatStop();
            }
            uint32 const entry = boss.useObjects[run.objectAt % boss.useObjects.size()];
            GameObject* object = player->FindNearestGameObject(entry, 100.0f);
            // Farther than the look reaches: where it is spawned (the Altar of Archaedas stands in the vault that the
            // Altar of the Keepers opens, more than 100 yd from it: the owner's group used the first altar four times,
            // never found the second from the corridor, and gave Archaedas up).
            float spawnX = 0.0f, spawnY = 0.0f, spawnZ = 0.0f;
            bool spawnKnown = false;
            if (!object)
                for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
                    if (data.mapid == plan->mapId && data.id == entry &&
                        (!spawnKnown || std::hypot(data.posX - boss.x, data.posY - boss.y) < std::hypot(spawnX - boss.x, spawnY - boss.y)))
                    {
                        spawnX = data.posX;
                        spawnY = data.posY;
                        spawnZ = data.posZ;
                        spawnKnown = true;
                    }
            // No 5 yd on (in three dimensions) in a minute by the wall clock (the 90 s of the walk stand still while the group
            // is busy, and the Sunken Temple's group stood 20 min on its way to the second statue; in sight of the first
            // one it stood 40 yd below it in the pit): the group is put beside it, once per object.
            auto const hopBeside = [&](float ox, float oy, float oz, bool force = false) -> bool
            {
                if (!run.objectStillSince || player->GetExactDist(run.objectStillX, run.objectStillY, run.objectStillZ) > 5.0f)
                {
                    run.objectStillSince = now;
                    run.objectStillX = player->GetPositionX();
                    run.objectStillY = player->GetPositionY();
                    run.objectStillZ = player->GetPositionZ();
                    return false;
                }
                if ((!force && getMSTimeDiff(run.objectStillSince, now) <= MINUTE * IN_MILLISECONDS) || run.objectHopFor == entry ||
                    !GetSettings().dungeonsHopWhenNoWay || player->IsInCombat())
                    return false;
                run.objectHopFor = entry;
                run.objectStillSince = 0;
                float x = 0.0f, y = 0.0f, z = 0.0f;
                if (!FindHopSpot(player, ox, oy, oz, x, y, z, 2.0f, 6.0f))
                    return false;
                LOG_INFO(ActivityLog, "activity: {}'s group gets no nearer to object {} in {} in a minute ({:.0f} yd away, {:.0f} yd up or "
                    "down): it is put beside it", bot.name, entry, plan->name, player->GetExactDist(ox, oy, oz), std::fabs(player->GetPositionZ() - oz));
                TakeGuestAlong(bot, player, x, y, z, "no way on foot to what wakes the boss");
                for (Player* member : group)
                {
                    if (!member->IsAlive() || member->GetMapId() != plan->mapId)
                        continue;
                    member->NearTeleportTo(x, y, z, member->GetOrientation());
                    if (BotRuntime* other = FindRuntime(member->GetGUID()))
                        other->hasLastPosition = false;
                }
                return true;
            };
            if (now >= run.objectNext)
            {
                if (!object && spawnKnown && Dist2dTo(player, spawnX, spawnY) > 20.0f)
                {
                    // On the way to it.
                    if (!run.objectSince)
                        run.objectSince = now;
                    if (getMSTimeDiff(run.objectSince, now) > 90 * IN_MILLISECONDS)
                    {
                        // (The same one again: they work in their order only. The Sunken Temple's group passed the
                        // second statue over this way, and the third to sixth did nothing.)
                        LOG_INFO(ActivityLog, "activity: {} does not get to object {} ({} of {}) in {} in 90 s, {:.0f} yd away: it tries "
                            "again", bot.name, entry, run.objectAt % boss.useObjects.size() + 1, boss.useObjects.size(), plan->name,
                            Dist2dTo(player, spawnX, spawnY));
                        run.objectSince = 0;
                        ++run.objectsUsed;
                        return;
                    }
                    if (hopBeside(spawnX, spawnY, spawnZ))
                        return;
                    PauseStuck(rt);
                    run.bossStuckSince = now;
                    if (GameObjectTemplate const* info = sObjectMgr->GetGameObjectTemplate(entry))
                        rt.why = Acore::StringFormat("In {}: on the way to {} (for {})", plan->name, info->name, boss.name);
                    if (BotDriver::GetRpgState(player) != RpgState::GoCamp || getMSTimeDiff(rt.task.lastAssert, now) >= AssertMs)
                    {
                        DungeonGoTo(player, plan->mapId, spawnX, spawnY, spawnZ);
                        rt.task.lastAssert = now;
                    }
                    return;
                }
                if (object && object->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE))
                {
                    // Used already (a statue of the Sunken Temple cannot be chosen once it did its part, also when the
                    // group came back after a wipe or a restart): the next one, without a try counted; the boss's own
                    // clock still runs. (Used again, out of its order, a statue poisons its user.)
                    ++run.objectAt;
                    run.objectSince = 0;
                    run.objectNext = now + 2 * IN_MILLISECONDS;
                    return;
                }
                if (!object)
                {
                    // Not there (not spawned, or behind what the one before it opens): the next one.
                    ++run.objectAt;
                    ++run.objectsUsed;
                    run.objectNext = now + 2 * IN_MILLISECONDS;
                    PauseStuck(rt);
                    return;
                }
                // In reach: the core's interact distance (the object's model and 5 yd), or, when the walk gets it no nearer
                // for 20 s, 12 yd on its level. (The Sunken Temple's group was put 8 yd beside the first statue on the
                // balcony, never came within 4 yd of it, and gave Atal'alarion up after 24 silent tries of a minute.)
                else if (!player->IsWithinDistInMap(object, 4.0f) && !object->IsAtInteractDistance(player) &&
                    !(run.objectSince && getMSTimeDiff(run.objectSince, now) > 20 * IN_MILLISECONDS &&
                        player->GetExactDist(object) <= 12.0f && std::fabs(player->GetPositionZ() - object->GetPositionZ()) <= 5.0f &&
                        (player->GetExactDist(object) <= 8.0f || player->IsWithinLOSInMap(object))))
                {
                    if (!run.objectSince)
                        run.objectSince = now;
                    if (hopBeside(object->GetPositionX(), object->GetPositionY(), object->GetPositionZ()))
                        return;
                    // (A minute on the way to one on another floor: the group is put beside it, also when it walked about
                    // meanwhile. Blackwing Lair's raid walked 68 yd below Chromaggus's lever for four minutes, 2026-11-22.)
                    if (getMSTimeDiff(run.objectSince, now) > 60 * IN_MILLISECONDS &&
                        std::fabs(player->GetPositionZ() - object->GetPositionZ()) > 8.0f &&
                        hopBeside(object->GetPositionX(), object->GetPositionY(), object->GetPositionZ(), true))
                        return;
                    if (getMSTimeDiff(run.objectSince, now) > 60 * IN_MILLISECONDS)
                    {
                        // No way to it for now: the same one again (see above).
                        LOG_INFO(ActivityLog, "activity: {} does not get to {} ({} of {}) in {} in 60 s, {:.0f} yd away ({:.0f} yd up or "
                            "down): it tries again", bot.name, object->GetGOInfo()->name, run.objectAt % boss.useObjects.size() + 1,
                            boss.useObjects.size(), plan->name, player->GetExactDist(object),
                            std::fabs(player->GetPositionZ() - object->GetPositionZ()));
                        run.objectSince = 0;
                        ++run.objectsUsed;
                        return;
                    }
                    else
                    {
                        PauseStuck(rt);
                        run.bossStuckSince = now;
                        rt.why = Acore::StringFormat("In {}: on the way to {} (for {})", plan->name, object->GetGOInfo()->name, boss.name);
                        if (BotDriver::GetRpgState(player) != RpgState::GoCamp || getMSTimeDiff(rt.task.lastAssert, now) >= AssertMs)
                        {
                            DungeonGoTo(player, plan->mapId, object->GetPositionX(), object->GetPositionY(), object->GetPositionZ());
                            rt.task.lastAssert = now;
                        }
                        return;
                    }
                }
                else
                {
                    LOG_INFO(ActivityLog, "activity: {} uses {} ({}, {} of {}) in {}: {} sleeps or stands behind a door until it is used",
                        bot.name, object->GetGOInfo()->name, entry, run.objectAt % boss.useObjects.size() + 1, boss.useObjects.size(),
                        plan->name, boss.name);
                    player->AttackStop();
                    player->StopMoving();
                    object->Use(player);
                    // (And the report a client sends after its use: Chromaggus's lever acts on it alone, and four uses did
                    // nothing, 2026-11-22.)
                    if (object->IsInWorld())
                        if (GameObjectAI* ai = object->AI())
                            ai->GossipHello(player, true);
                    run.objectSince = 0;
                    ++run.objectAt;
                    ++run.objectsUsed;
                    run.objectNext = now + 12 * IN_MILLISECONDS;
                    PauseStuck(rt);
                    run.bossStuckSince = now;
                    return;
                }
            }
            else
            {
                // (It channels at the object: the ritual of an altar takes some seconds.)
                PauseStuck(rt);
                run.bossStuckSince = now;
                rt.why = Acore::StringFormat("In {}: waits for what it used to work (for {})", plan->name, boss.name);
                return;
            }
        }
        // (Not before what was used last has had its time: the Sunken Temple's sixth statue woke Atal'alarion one second
        // after its use, and the group had given him up in that second.)
        if (!boss.useObjects.empty() && asleep && run.objectsUsed >= 4 * boss.useObjects.size() && now >= run.objectNext &&
            Dist2dTo(player, boss.x, boss.y) <= 60.0f)
        {
            LOG_INFO(ActivityLog, "activity: {}'s group cannot wake {} in {}: what there is to use did not do it; it goes on", bot.name,
                boss.name, plan->name);
            ++run.boss;
            run.bossStuckSince = 0;
            run.hopped = false;
            run.objectAt = run.objectNext = run.objectSince = run.objectsUsed = 0;
            continue;
        }
        // A boss that is a friend of the group's side (Baelog and his brothers in Uldaman are the Alliance's friends; not a
        // neutral one: Witch Doctor Zum'rah turns on whoever comes near):
        // left out as soon as the group sees it (the first Uldaman run stood before them for eight minutes).
        if (!boss.IsEvent() && creature && !creature->IsInCombat() && player->IsWithinDistInMap(creature, BossReached) &&
            creature->IsFriendlyTo(player) && !player->IsValidAttackTarget(creature))
        {
            LOG_INFO(ActivityLog, "activity: {}'s group leaves {} in {} alone: it is no enemy of the group's side", bot.name, boss.name,
                plan->name);
            run.summonWaitSince = 0;
            run.summonSeen = false;
            ++run.required;
            ++run.boss;
            run.bossStuckSince = 0;
            run.hopped = false;
            continue;
        }
        // A boss the instance brings by itself (Majordomo Executus): at its place it may not have come yet. The group waits
        // for it there (its timeout), then counts it as done (it fell in this instance before) or passes it over.
        if (boss.summoned && creature)
            run.summonSeen = true;
        // (One that came and fell while the leader fought, when this step did not run: its corpse counts. Ouro died in
        // the fight and the raid waited ten minutes for him at his mound, 2026-11-23.)
        if (boss.summoned && !creature && !run.summonSeen)
            if (Creature* fallen = player->FindNearestCreature(boss.entry, 150.0f, false); fallen && !fallen->IsAlive())
                run.summonSeen = true;
        if (boss.summoned && !creature && !run.summonSeen && !player->IsInCombat() && distance <= BossReached &&
            std::fabs(player->GetPositionZ() - boss.z) <= 25.0f)
        {
            if (!run.summonWaitSince)
            {
                run.summonWaitSince = now;
                LOG_INFO(ActivityLog, "activity: {}'s group waits for {} at its place in {} (the instance brings it when the bosses before "
                    "it are dead)", bot.name, boss.name, plan->name);
            }
            if (getMSTimeDiff(run.summonWaitSince, now) < std::max<uint32>(boss.timeout, 30) * IN_MILLISECONDS)
            {
                PauseStuck(rt);
                run.bossStuckSince = now;
                rt.why = Acore::StringFormat("In {}: waiting for {} to come", plan->name, boss.name);
                return;
            }
            LOG_INFO(ActivityLog, "activity: {}'s group gave {} in {} {} s: it did not come (it fell in this instance before, or a boss "
                "before it lives)", bot.name, boss.name, plan->name, std::max<uint32>(boss.timeout, 30));
        }
        // A boss that fell in this instance away from its place (pulled to the group) or before a restart of the server: the
        // map holds a respawn time for its spawn, and the group does not walk to its place to see it is gone. (The owner's
        // group killed Highlord Omokk 90 yd from his place in Lower Blackrock Spire five seconds before its run began
        // anew after a restart; the leader set out for his place every 17 s and its walk there had no way, 2026-11-25.)
        bool const fellHere = !creature && !boss.IsEvent() && boss.spawnId && player->GetMap()->GetCreatureRespawnTime(boss.spawnId);
        // (While the group is after the boss's loot the leader may stand farther from its place: at a corpse.)
        if (!boss.IsEvent() && !creature && (fellHere || (distance <= (run.lootSince ? BossReached + LootLookYards : BossReached) &&
            std::fabs(player->GetPositionZ() - boss.z) <= 25.0f)))
        {
            // At its place and it is not alive: done (killed now, or before). The group stays for the loot first: the
            // rolls, and what nobody rolled for (the leader walks to the corpse, the auto loot reaches a few yards).
            // (The rest of the fight first: the boss falls before its helpers do, and the first build counted it done
            // in that second and walked on from its loot.)
            Creature* corpse = nullptr;
            bool rolling = false;
            bool const fighting = player->IsInCombat();
            if (LootPending(player, corpse, rolling) || fighting)
            {
                if (!run.lootSince)
                    run.lootSince = now;
                bool ownerPlays = guest && guest->GetMapId() == plan->mapId;
                for (Player* member : group)
                    ownerPlays = ownerPlays || !sGuildBotHolder.IsAIPlayed(member->GetGUID());
                if (getMSTimeDiff(run.lootSince, now) < (ownerPlays || fighting ? LootWaitOwnerMs : LootWaitMs))
                {
                    PauseStuck(rt);
                    if (fighting)
                    {
                        rt.why = Acore::StringFormat("In {}: the rest of the fight at {}", plan->name, boss.name);
                        return;
                    }
                    rt.why = Acore::StringFormat("In {}: the loot of {}{}", plan->name, boss.name, rolling ? " (the group rolls)" : "");
                    if (corpse && !player->IsWithinDistInMap(corpse, 4.0f) &&
                        (BotDriver::GetRpgState(player) != RpgState::GoCamp || getMSTimeDiff(rt.task.lastAssert, now) >= AssertMs))
                    {
                        DungeonGoTo(player, plan->mapId, corpse->GetPositionX(), corpse->GetPositionY(), corpse->GetPositionZ());
                        rt.task.lastAssert = now;
                    }
                    return;
                }
                if (rolling || corpse)
                    LOG_INFO(ActivityLog, "activity: {}'s group leaves loot behind at {} in {}: {} after {} s", bot.name, boss.name, plan->name,
                        rolling ? "a roll is still open" : Acore::StringFormat("something is left in {}", corpse->GetName()),
                        getMSTimeDiff(run.lootSince, now) / IN_MILLISECONDS);
            }
            else if (run.lootSince)
                LOG_INFO(ActivityLog, "activity: {}'s group is through with the fight and the loot at {} in {} after {} s", bot.name,
                    boss.name, plan->name, getMSTimeDiff(run.lootSince, now) / IN_MILLISECONDS);
            run.lootSince = 0;
            run.summonWaitSince = 0;
            run.summonSeen = false;
            ++run.bossesDown;
            // The boss that ends the dungeon (where the dungeon finder names none: every boss of the list).
            // (A boss that an event brings does not count for it: an event that does not start must not keep the
            // guild's next dungeon closed.)
            bool anyLast = false;
            uint32 required = 0;
            for (DungeonBoss const& other : plan->bosses)
            {
                anyLast = anyLast || other.last;
                required += other.IsEvent() ? 0 : 1;
            }
            ++run.required;
            if (boss.last || (!anyLast && run.required == required))
                run.endBossDown = true;
            if (!rt.dungeon.metrics.bossesJudged.count(boss.entry))
                ++rt.dungeon.metrics.bossUnseen;
            LOG_INFO(ActivityLog, "activity: {}'s group is done with {} in {} ({} of {})", bot.name, boss.name, plan->name,
                uint32(run.boss + 1), RunBossCount(run, *plan));
            ++run.boss;
            run.bossStuckSince = 0;
            run.hopped = false;
            run.objectAt = run.objectNext = run.objectSince = run.objectsUsed = 0;
            continue;
        }
        // No nearer for a long time (a door, a way the path finder does not have): on to the next one. Nearer along the
        // navmesh's way when there is one (DungeonNav): Shadowfang Keep's way from the courtyard to Odo the Blindwatcher
        // leads away from him through the stables before it climbs to him.
        // (A boss pulled to a place or come to from one: the group walks to that place, and the way and the hop below
        // are the place's: Jeklik's platform has no way up, and the raid was put up to her, 2026-11-27.)
        PullPlace const* walkPlace = nullptr;
        if (!creature || !creature->IsInCombat())
        {
            walkPlace = ApproachPlaceOf(boss.entry);
            if (!walkPlace)
                walkPlace = PullPlaceOf(boss.entry);
        }
        float const walkX = walkPlace ? walkPlace->x : boss.x;
        float const walkY = walkPlace ? walkPlace->y : boss.y;
        float const walkZ = walkPlace ? walkPlace->z : boss.z;
        float wayLeft = distance;
        if (float length = 0.0f; sDungeonNav.FindWay(player->GetMapId(), player->GetPositionX(), player->GetPositionY(),
                player->GetPositionZ(), boss.x, boss.y, boss.z, nullptr, &length, player->GetMap()))
            wayLeft = length;
        float walkLeft = walkPlace ? Dist2dTo(player, walkX, walkY) : wayLeft;
        if (float length = 0.0f; walkPlace && sDungeonNav.FindWay(player->GetMapId(), player->GetPositionX(), player->GetPositionY(),
                player->GetPositionZ(), walkX, walkY, walkZ, nullptr, &length, player->GetMap()))
            walkLeft = length;
        if (!run.bossStuckSince || walkLeft < run.bossBest - BossProgress)
        {
            run.bossStuckSince = now;
            run.bossBest = walkLeft;
        }
        // (Far from it, or near it on another level: Lord Serpentis stands on a ledge 29 yd above the way; or near it
        // with no way for twice the time: the group of Razorfen Kraul stood 24 yd from Roogug for eight minutes, its
        // path ran through the rock.)
        else if (GetSettings().dungeonsHopWhenNoWay && !run.hopped && !groupBusy &&
            (distance > 60.0f || std::fabs(player->GetPositionZ() - boss.z) > 12.0f ||
                getMSTimeDiff(run.bossStuckSince, now) >= 2 * HopAfterMs) &&
            (getMSTimeDiff(run.bossStuckSince, now) >= HopAfterMs ||
                (getMSTimeDiff(run.bossStuckSince, now) >= HopStillMs && run.stillSince && getMSTimeDiff(run.stillSince, now) >= HopStillMs)) &&
            NoWayOnFoot(player, walkX, walkY, walkZ))
        {
            // The path finder has no way there (players swim or jump: the pools of Blackfathom Deeps): the group is
            // put on walkable ground before the boss, once per boss. (Through a closed door on the way: on foot to it.)
            if (StepThroughDoor(bot, player, rt, group, *plan, boss.x, boss.y, boss.z, boss.name))
                return;
            run.hopped = true;
            float x = 0.0f, y = 0.0f, z = 0.0f;
            // (A boss pulled to a place: the group is put there, not beside the boss: the Ruins' raid put 25 yd before
            // Ossirian had him on it at once, 2026-11-22.)
            PullPlace const* place = PullPlaceOf(boss.entry);
            if (!place)
                place = ApproachPlaceOf(boss.entry);
            if (place ? FindHopSpot(player, place->x, place->y, place->z, x, y, z, 4.0f, 10.0f) :
                FindHopSpot(player, boss.x, boss.y, boss.z, x, y, z))
            {
                LOG_INFO(ActivityLog, "activity: {}'s group finds no way on foot to {} in {} ({:.0f} yd away; a way players swim or jump): "
                    "it is put {:.0f} yd before it", bot.name, boss.name, plan->name, distance,
                    std::sqrt((x - boss.x) * (x - boss.x) + (y - boss.y) * (y - boss.y)));
                TakeGuestAlong(bot, player, x, y, z, Acore::StringFormat("no way on foot to {}", boss.name));
                for (Player* member : group)
                {
                    if (!member->IsAlive() || member->GetMapId() != plan->mapId)
                        continue;
                    member->NearTeleportTo(x, y, z, member->GetOrientation());
                    if (BotRuntime* other = FindRuntime(member->GetGUID()))
                        other->hasLastPosition = false;
                }
                ClearTask(rt, 2 * IN_MILLISECONDS, "put near the boss");
                run.bossStuckSince = 0;
                return;
            }
        }
        else if (getMSTimeDiff(run.bossStuckSince, now) >= BossStuckMs)
        {
            // Once more before the boss that ends the dungeon (package P4: the order of the way may come to a boss before
            // the way to it is open).
            bool again = false;
            if (!run.order.empty() && run.retried.insert(run.order[run.boss]).second)
            {
                std::size_t at = run.boss + 1;
                while (at < run.order.size() && !plan->bosses[run.order[at]].last && !plan->bosses[run.order[at]].IsEvent())
                    ++at;
                uint8 const index = run.order[run.boss];
                run.order.insert(run.order.begin() + at, index);
                again = true;
            }
            LOG_INFO(ActivityLog, "activity: {}'s group does not get to {} in {} ({:.0f} yd away): it goes on to the next boss{}", bot.name,
                boss.name, plan->name, distance, again ? " and tries it once more later" : "");
            ++run.boss;
            run.bossStuckSince = 0;
            run.hopped = false;
            continue;
        }
        // (The Twin Emperors: Vek'lor's guards in the hall first, before the raid goes down into their room.)
        if (plan->raid && boss.entry == 15275 && StepTwinGuards(bot, player, rt))
            return;
        // (The Twin Emperors' halves take their places from there: the leader is at its own then, StepTwinsPull.)
        bool const atApproach = walkPlace && (player->GetExactDist2d(walkPlace->x, walkPlace->y) < 10.0f ||
                (boss.entry == 15275 && (run.twinPlacesSince || run.twinPulledAt || run.twinGuards)));
        if (!atApproach && (rt.task.kind != TaskKind::Travel || !rt.task.dungeonWalk || rt.task.spot.entry != boss.entry))
        {
            BotTask task;
            task.kind = TaskKind::Travel;
            task.dungeonWalk = true;
            task.hasSpot = true;
            task.spot.mapId = plan->mapId;
            task.spot.entry = boss.entry;
            task.spot.x = boss.x;
            task.spot.y = boss.y;
            task.spot.z = boss.z;
            // (To the hand place of a boss that has one, not up to the boss: the leader that walked to the Twin Emperors
            // pulled both before its group was there, 2026-11-23.)
            // (And to the place a boss is pulled to: Jeklik's, below her platform.)
            if (walkPlace)
            {
                task.spot.x = walkPlace->x;
                task.spot.y = walkPlace->y;
                task.spot.z = walkPlace->z;
            }
            task.targetName = boss.name;
            StartTask(player, rt, task);
        }
        PauseStuck(rt);
        // C'Thun: the raid holds on the ramp and a hunter pulls the Eye from there (StepCthunPull), before the room and
        // the gate below: those walked the leader up into the chamber, where the Eye took the raid on its way in
        // (five tries, 2026-11-24).
        if (plan->raid && atApproach && (boss.entry == 15727 || boss.entry == 15589))
            if (Creature* eye = player->FindNearestCreature(15589, 150.0f, true))
                if (StepCthunPull(rt, group, eye, plan->name))
                    return;
        // The room of the boss (below) is cleared before the group comes near it, from a place out of the boss's
        // reach: the group turns to it as soon as it is within the room and that distance of the boss.
        // (A boss whose script calls a bigger room: Mograine's engage sends every monk, abbot, champion, centurion,
        // wizard and chaplain within 80 yd at the group, the whole cathedral; the owner's group cleared 30 yd and had 15
        // creatures on it.)
        constexpr uint32 ScarletCommanderMograine = 3976;
        float const RoomYards = boss.entry == ScarletCommanderMograine ? 80.0f : 30.0f;
        // (Only a call that reaches far beyond the 30 yd: Odo's 40 yd held 17 creatures that stand within his own reach
        // and could not be pulled out of it, and the group cleared his courtyard for more than ten minutes; with 30 yd
        // Shadowfang Keep went 8 of 8.)
        float const CallYards = BossCallForHelpYards(boss.entry) >= 60.0f ? std::min(BossCallForHelpYards(boss.entry), 100.0f) : 0.0f;
        // (The Shade of Eranikus: its aggro sets the Sunken Temple's instance data that puts every dragonkin of the
        // temple's loaded grids in a fight with the group; the group that came to it after Hazzas and Morphaz had 21 on it
        // and was wiped out, dclear 2026-11-18. Every dragonkin within 250 yd is of its room.)
        constexpr uint32 ShadeOfEranikus = 5709;
        float const WideYards = boss.entry == ShadeOfEranikus ? 250.0f : 0.0f;
        uint32 const WideType = boss.entry == ShadeOfEranikus ? uint32(CREATURE_TYPE_DRAGONKIN) : 0u;
        // The room's fights are fought out of the boss's reach: 45 yd at least (its aggro range plus 15 yd, if more). A
        // creature in a fight calls those of its side within 10 yd every few seconds, one that flees runs to a helper
        // within 30 yd, and the group that fought the cathedral from beside Mograine (roles instance, 13:38) and the
        // one that came within 40 yd of him after Fairbanks (14:29, a monk fled to him) had him and the cathedral on it.
        // (A boss that is no enemy until it is attacked has no reach: Scholomance's Vectus stands neutral among the
        // students of his room, and with 45 yd of him kept no place was left to fight them from, 2026-11-20.)
        float const keep = !creature ? 45.0f : creature->IsHostileTo(player) ? std::max(45.0f, creature->GetAggroRange(player) + 15.0f) :
            30.0f;
        // Near by the straight line only: the way there is far longer (the Wailing Caverns, 2026-11-24: Verdan the
        // Everliving stands 60 yd from the way below his plateau, which is 1950 yd off through the caves and down a drop;
        // the leader turned to his room from below every few seconds and walked back to it, and gave him up after eight
        // minutes). Until the way left is short the group walks on: no room, no gate, no pull.
        bool const farByWay = wayLeft > 1.5f * distance + 40.0f;
        bool const nearBoss = creature && !farByWay && player->IsWithinDistInMap(creature, BossReached);
        bool const clearingRoom = run.roomLogged == run.boss + 1 && !run.roomDone && !farByWay;
        bool const roomAhead = creature && !farByWay && run.roomLogged != run.boss + 1 && creature->IsAlive() && !creature->IsInCombat() &&
            player->IsWithinDistInMap(creature, std::max(RoomYards, CallYards) + keep + 20.0f);
        // The gate before the boss (package P1): once open it holds until the boss is pulled, unless the group fights
        // something else first (a patrol): then it gathers again.
        if (run.gateOpenFor == run.boss + 1 && creature && !creature->IsInCombat())
            for (Player* member : group)
                if (member->IsAlive() && InRealFight(member))
                    run.gateOpenFor = 0;
        bool const gating = run.gateKey == run.boss + 1;
        // A raid boss pulled to a place (a hand list): Ossirian to his first crystal, 126 yd from where he stands. His
        // Strength (+300 % damage) is on until a crystal's weakness reaches him, and the walk of his tank to the crystal
        // with him at its back killed it in 4 s twice (2026-11-22). The raid gathers at the crystal, the tank walks up to
        // him alone and taunts him from 25 yd, and StepRaidOssirian walks it back to the crystal with him behind it. (The
        // careful pull found no place to open from on his terrace.)
        if (run.longPullFor == run.boss + 1 && creature && !creature->IsInCombat())
        {
            if (getMSTimeDiff(run.longPullSince, now) > 45 * IN_MILLISECONDS || !player->IsAlive())
            {
                run.longPullFor = 0;
                LOG_INFO(ActivityLog, "activity: {} gives up its pull of {} in {}: it did not get within reach of it in 45 s", bot.name,
                    boss.name, plan->name);
            }
            else if (player->IsInCombat() && player->GetVictim() != creature)
            {
                // Something else came at it on the way: the pull waits until that fight is over and the group is ready
                // again (the gate).
                run.longPullFor = 0;
                run.bossPullTried = 0;
                run.gateOpenFor = 0;
                LOG_INFO(ActivityLog, "activity: {} stops its pull of {} in {}: a fight on its way there; it pulls again from its "
                    "group's place", bot.name, boss.name, plan->name);
            }
            else
            {
                rt.why = Acore::StringFormat("In {}: pulling {} to its group", plan->name, boss.name);
                BotDriver::PauseAI(player, 1500);
                // (On foot: its taunt failed in the saddle, cast result 64, and the mount took it into his reach.)
                if (player->IsMounted())
                    player->RemoveAurasByType(SPELL_AURA_MOUNTED);
                float const yards = player->GetExactDist(creature);
                if (yards <= 29.0f)
                {
                    uint32 taunt = 0;
                    switch (player->getClass())
                    {
                        case CLASS_WARRIOR: taunt = 355; break;
                        case CLASS_PALADIN: taunt = 62124; break;
                        case CLASS_DRUID: taunt = 6795; break;
                        case CLASS_DEATH_KNIGHT: taunt = 56222; break;
                        default: break;
                    }
                    if (taunt && player->HasSpell(taunt) && !player->HasSpellCooldown(taunt))
                    {
                        player->StopMoving();
                        SpellCastResult const result = player->CastSpell(creature, taunt, false);
                        if (result == SPELL_CAST_OK)
                        {
                            run.longPullFor = 0;
                            // (What protects it on the way back with the boss at its back: Shield Wall, Divine Protection,
                            // Barkskin, Icebound Fortitude.)
                            for (uint32 guard : { 871u, 498u, 22812u, 48792u })
                                if (player->HasSpell(guard) && !player->HasSpellCooldown(guard))
                                {
                                    player->CastSpell(player, guard, true);
                                    break;
                                }
                            LOG_INFO(ActivityLog, "activity: {} pulls {} in {} from {:.0f} yd and takes it to the place its group waits at",
                                bot.name, boss.name, plan->name, yards);
                            return;
                        }
                        if (run.longPullFail != uint32(result) + 1)
                        {
                            run.longPullFail = uint32(result) + 1;
                            LOG_INFO(ActivityLog, "activity: {} cannot taunt {} from {:.0f} yd (cast result {}; its aggro range {:.0f} yd)",
                                bot.name, boss.name, yards, uint32(result), creature->GetAggroRange(player));
                        }
                    }
                    if (yards <= 6.0f)
                    {
                        run.longPullFor = 0;
                        BotDriver::Attack(player, creature, false);
                        return;
                    }
                }
                if (now >= run.longPullStepAt)
                {
                    // (To the place it pulls from when it has one: Jeklik's platform has no way up, the path towards
                    // her ends under its edge, out of her sight.)
                    PullPlace const* from = PullPlaceOf(boss.entry);
                    if (from && from->fromX != 0.0f)
                        player->GetMotionMaster()->MovePoint(0, from->fromX, from->fromY, from->fromZ, FORCED_MOVEMENT_RUN);
                    else
                        player->GetMotionMaster()->MovePoint(0, creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ(),
                            FORCED_MOVEMENT_RUN);
                    run.longPullStepAt = now + 1000;
                }
                return;
            }
        }
        if (creature && !creature->IsInCombat() && !player->IsInCombat() && run.bossPullTried != run.boss + 1)
        {
            PullPlace const* place = PullPlaceOf(boss.entry);
            // (From nearer than 150 yd to it: on the way there the group walks the way to it as to an approach place.)
            if (place && player->GetExactDist2d(place->x, place->y) > 150.0f)
                place = nullptr;
            if (place)
            {
                // A few yards from the place on the boss's side, on the floor.
                float const toBoss = std::atan2(creature->GetPositionY() - place->y, creature->GetPositionX() - place->x);
                float px = place->x + std::cos(toBoss) * 6.0f, py = place->y + std::sin(toBoss) * 6.0f, pz = place->z;
                player->UpdateAllowedPositionZ(px, py, pz);
                if (player->GetExactDist2d(px, py) > 12.0f) // (NewRpg calls 10 yd there)
                {
                    rt.why = Acore::StringFormat("In {}: to the place it pulls {} to", plan->name, boss.name);
                    if (run.pullPlaceLogged != run.boss + 1)
                    {
                        run.pullPlaceLogged = run.boss + 1;
                        LOG_INFO(ActivityLog, "activity: {}'s group goes to the place it pulls {} to in {} ({:.0f} yd from it)", bot.name,
                            boss.name, plan->name, creature->GetExactDist2d(px, py));
                    }
                    if (BotDriver::GetRpgState(player) != RpgState::GoCamp || getMSTimeDiff(rt.task.lastAssert, now) >= AssertMs)
                    {
                        DungeonGoTo(player, plan->mapId, px, py, pz);
                        rt.task.lastAssert = now;
                    }
                    return;
                }
                if (player->isMoving())
                    BotDriver::Rest(player);
                // What stands within 70 yd of the place is fought first: an Anubisath Guardian 57 yd from Ossirian's first
                // crystal came at the raid while its tank walked to him, the tank fought it on the way and came back to
                // the raid at 4 % health with Ossirian behind it (2026-11-22).
                // (Not what stands on another level with no way on foot to it: the bats on Jeklik's platform, 12 yd
                // above the floor her raid waits on. Looked at once a minute per creature.)
                {
                    // (The world thread's: the planner.)
                    static std::unordered_map<ObjectGuid, uint32> outOfReach;
                    auto unreachable = [&](Creature* other)
                    {
                        if (std::fabs(other->GetPositionZ() - player->GetPositionZ()) <= 8.0f)
                            return false;
                        auto found = outOfReach.find(other->GetGUID());
                        if (found != outOfReach.end() && getMSTimeDiff(found->second, now) < MINUTE * IN_MILLISECONDS)
                            return true;
                        if (!NoWayOnFoot(player, other->GetPositionX(), other->GetPositionY(), other->GetPositionZ()))
                            return false;
                        outOfReach[other->GetGUID()] = now;
                        return true;
                    };
                    Creature* nearest = nullptr;
                    for (Creature* other : HostilesAround(player, player, 70.0f))
                        if (other != creature && !other->IsDungeonBoss() && (!nearest || player->GetExactDist(other) < player->GetExactDist(nearest)) &&
                            !unreachable(other))
                            nearest = other;
                    if (nearest)
                    {
                        rt.why = Acore::StringFormat("In {}: clearing the place it pulls {} to", plan->name, boss.name);
                        if (run.pullPlaceLogged != run.boss + 101)
                        {
                            run.pullPlaceLogged = run.boss + 101;
                            LOG_INFO(ActivityLog, "activity: {}'s group fights {} (level {}, {:.0f} yd away) first: it stands within 70 yd of "
                                "the place it pulls {} to in {}", bot.name, nearest->GetName(), nearest->GetLevel(), player->GetExactDist(nearest),
                                boss.name, plan->name);
                        }
                        BotDriver::Attack(player, nearest, false);
                        return;
                    }
                }
                if (!DungeonGate(guild, bot, player, rt, *plan, group, guest, "the pull of " + boss.name, run.boss + 1))
                    return;
                run.gateOpenFor = run.boss + 1;
                run.bossPullTried = run.boss + 1;
                run.longPullFor = run.boss + 1;
                run.longPullSince = now;
                run.longPullStepAt = 0;
                LOG_INFO(ActivityLog, "activity: {} goes to pull {} in {} alone: its group waits at the place it takes it to, {:.0f} yd "
                    "from it", bot.name, boss.name, plan->name, player->GetExactDist(creature));
                return;
            }
        }
        if (creature && (nearBoss || clearingRoom || roomAhead || (gating && !farByWay)))
        {
            rt.why = Acore::StringFormat("In {}: fighting {}", plan->name, boss.name);
            // On another level than the boss with no way to it (Dire Maul West, 2026-11-20: the way to Immol'thar ended
            // on the top of his force field, 18 yd above him; the tank pulled from there and its group dropped down to
            // him one by one and died): the group is put on the boss's floor out of its reach first, once per boss.
            // (Not for a boss that cannot be fought yet: Rend Blackhand waits on a balcony over the stadium, and the
            // group was put up there to him before his event.)
            if (!creature->IsInCombat() && !player->IsInCombat() && run.levelHopFor != run.boss + 1 && GetSettings().dungeonsHopWhenNoWay &&
                player->IsValidAttackTarget(creature) && !creature->HasUnitFlag(UnitFlags(UNIT_FLAG_NON_ATTACKABLE |
                    UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_IMMUNE_TO_PC)) &&
                std::fabs(player->GetPositionZ() - creature->GetPositionZ()) >= 8.0f && player->GetExactDist2d(creature) < keep + 20.0f &&
                NoWayOnFoot(player, creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ()))
            {
                run.levelHopFor = run.boss + 1;
                float x = 0.0f, y = 0.0f, z = 0.0f;
                PullPlace const* approach = ApproachPlaceOf(boss.entry);
                if (approach ? FindHopSpot(player, approach->x, approach->y, approach->z, x, y, z, 4.0f, 10.0f) :
                    FindHopSpot(player, creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ(), x, y, z,
                        std::max(20.0f, keep - 15.0f), keep, true))
                {
                    LOG_INFO(ActivityLog, "activity: {}'s group stands {:.0f} yd {} {} in {} with no way on foot to it: it is put on its floor, "
                        "{:.0f} yd from it", bot.name, std::fabs(player->GetPositionZ() - creature->GetPositionZ()),
                        player->GetPositionZ() > creature->GetPositionZ() ? "above" : "below", boss.name, plan->name,
                        std::sqrt((x - creature->GetPositionX()) * (x - creature->GetPositionX()) +
                            (y - creature->GetPositionY()) * (y - creature->GetPositionY())));
                    TakeGuestAlong(bot, player, x, y, z, Acore::StringFormat("no way on foot to {}", boss.name));
                    for (Player* member : group)
                    {
                        if (!member->IsAlive() || member->GetMapId() != plan->mapId)
                            continue;
                        member->NearTeleportTo(x, y, z, member->GetAngle(creature));
                        if (BotRuntime* other = FindRuntime(member->GetGUID()))
                            other->hasLastPosition = false;
                    }
                    ClearTask(rt, 2 * IN_MILLISECONDS, "put on the boss's floor");
                    run.bossStuckSince = 0;
                    return;
                }
            }
            // (In combat with something that is not there any more, a creature that ran off: the group stood beside
            // Hamhock for minutes and nobody attacked him.)
            Unit const* victim = player->GetVictim();
            bool const idle = player->IsInCombat() && (!victim || !victim->IsAlive() || !player->IsWithinDistInMap(victim, 30.0f));
            if (!player->IsInCombat() || idle)
            {
                // The room first (the owner, 2026-11-14: [quote left out]): a creature within 30 yd of the boss that is in no fight yet is fought
                // before the boss, from the place out of its reach, the one nearest to that place first.
                std::list<Creature*> room;
                std::vector<uint32> bossEntries;
                for (DungeonBoss const& other : plan->bosses)
                    bossEntries.push_back(other.entry);
                std::vector<std::pair<uint32, float>> const joiners = BossScriptJoiners(boss.entry);
                float joinYards = 0.0f;
                for (auto const& [entry, yards] : joiners)
                    joinYards = std::max(joinYards, yards);
                RoomCreatureCheck roomCheck(player, creature, RoomYards, bossEntries, CallYards, WideYards, WideType, &joiners);
                Acore::CreatureListSearcher<RoomCreatureCheck> roomSearcher(creature, room, roomCheck);
                Cell::VisitObjects(creature, roomSearcher, std::max({ RoomYards, CallYards, WideYards, joinYards }));
                // (At most ten minutes per boss, twenty for a room as big as Mograine's (27 creatures took the group
                // about half a minute each), thirty for the Viewing Room's 32 students: a creature on a ledge or behind
                // a wall would hold the group for good.)
                uint32 const RoomMaxMs = (joinYards > 30.0f ? 30 : std::max({ RoomYards, CallYards, WideYards }) > 30.0f ? 20 : 10) *
                    MINUTE * IN_MILLISECONDS;
                bool const starting = run.roomLogged != run.boss + 1;
                // A boss the group has no way on foot to (players swim or jump): it is put on the boss's level first, out of
                // its reach, and clears the room from there. (The Sunken Temple's group cleared Jammal'an's room of 90 yd
                // from the floor above, where none of it could be reached, and the hop before the boss waited for that.)
                // (Its own once per boss: when the place it is put at has no way on to the boss either, the hop before
                // the boss still comes. Blackrock Depths, 2026-11-20: the group was put 50 yd before Lord Incendius on a
                // floor with no way down to his anvil, and stood there for two hours.)
                if (starting && !room.empty() && GetSettings().dungeonsHopWhenNoWay && run.roomHopFor != run.boss + 1 && !player->IsInCombat() &&
                    (player->GetExactDist2d(boss.x, boss.y) > 60.0f || std::fabs(player->GetPositionZ() - boss.z) > 12.0f) &&
                    NoWayOnFoot(player, walkX, walkY, walkZ))
                {
                    if (StepThroughDoor(bot, player, rt, group, *plan, boss.x, boss.y, boss.z, boss.name))
                        return;
                    float x = 0.0f, y = 0.0f, z = 0.0f;
                    PullPlace const* approach = ApproachPlaceOf(boss.entry);
                    if (!approach)
                        approach = PullPlaceOf(boss.entry);
                    if (approach ? FindHopSpot(player, approach->x, approach->y, approach->z, x, y, z, 4.0f, 10.0f) :
                        FindHopSpot(player, boss.x, boss.y, boss.z, x, y, z, keep + 5.0f, keep + 15.0f, true))
                    {
                        run.roomHopFor = uint8(run.boss + 1);
                        LOG_INFO(ActivityLog, "activity: {}'s group finds no way on foot to {} in {} ({:.0f} yd away; a way players swim "
                            "or jump): it is put {:.0f} yd before it, out of its reach, to clear its room from there", bot.name, boss.name,
                            plan->name, player->GetExactDist2d(boss.x, boss.y), std::hypot(x - boss.x, y - boss.y));
                        TakeGuestAlong(bot, player, x, y, z, Acore::StringFormat("no way on foot to {}", boss.name));
                        for (Player* member : group)
                        {
                            if (!member->IsAlive() || member->GetMapId() != plan->mapId)
                                continue;
                            member->NearTeleportTo(x, y, z, member->GetOrientation());
                            if (BotRuntime* other = FindRuntime(member->GetGUID()))
                                other->hasLastPosition = false;
                        }
                        ClearTask(rt, 2 * IN_MILLISECONDS, "put near the boss");
                        run.bossStuckSince = 0;
                        return;
                    }
                }
                // The clear begins from a place the group reached on a cleared way, within 45 yd of the room's nearest
                // creature: until then it walks on towards the boss in stretches, with what stands on them pulled first.
                // (Scholomance, 2026-11-20: Vectus's clear began at the entrance 67 yd from him, with the hall of 43
                // creatures between, and every walk to a student went through the hall.)
                if (starting && !room.empty() && !player->IsInCombat())
                {
                    float nearest = 1000.0f;
                    Creature* nearestOne = nullptr;
                    for (Creature* other : room)
                        if (player->GetExactDist2d(other) < nearest)
                        {
                            nearest = player->GetExactDist2d(other);
                            nearestOne = other;
                        }
                    if (nearest > 45.0f)
                    {
                        // Towards the boss, or, when the group stands at it already, towards the room's nearest creature:
                        // the Shade of Eranikus's room is every dragonkin within 250 yd, the nearest 102 yd from him, and a
                        // group put beside him stood there "on the way to the room" for eight minutes, 2026-11-22.
                        float toX = boss.x, toY = boss.y, toZ = boss.z;
                        if (nearestOne && player->GetExactDist2d(boss.x, boss.y) < 25.0f)
                        {
                            toX = nearestOne->GetPositionX();
                            toY = nearestOne->GetPositionY();
                            toZ = nearestOne->GetPositionZ();
                        }
                        float wayX = toX, wayY = toY, wayZ = toZ;
                        if (!run.eventSince && StepDungeonWay(guild, bot, player, rt, *plan, group, guest, toX, toY, toZ, wayX, wayY, wayZ))
                            return;
                        if (BotDriver::GetRpgState(player) != RpgState::GoCamp || getMSTimeDiff(rt.task.lastAssert, now) >= AssertMs ||
                            std::fabs(wayX - run.wayX) + std::fabs(wayY - run.wayY) > 6.0f)
                        {
                            DungeonGoTo(player, plan->mapId, wayX, wayY, wayZ);
                            rt.task.lastAssert = now;
                            run.wayX = wayX;
                            run.wayY = wayY;
                        }
                        rt.why = Acore::StringFormat("In {}: on the way to the room of {}", plan->name, boss.name);
                        return;
                    }
                }
                if (starting)
                {
                    run.roomSince = now;
                    run.roomDone = false;
                    run.roomSkip.clear();
                    // Where the room is fought: where the leader stands when that is out of the boss's reach, else
                    // walkable ground on the side the group comes from.
                    run.roomX = player->GetPositionX();
                    run.roomY = player->GetPositionY();
                    run.roomZ = player->GetPositionZ();
                    float x = 0.0f, y = 0.0f, z = 0.0f;
                    PullPlace const* approach = ApproachPlaceOf(boss.entry);
                    if (approach ? FindHopSpot(player, approach->x, approach->y, approach->z, x, y, z, 2.0f, 6.0f) :
                        player->GetExactDist2d(creature) < keep + 3.0f &&
                        FindHopSpot(player, creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ(), x, y, z, keep + 5.0f,
                            keep + 12.0f))
                    {
                        run.roomX = x;
                        run.roomY = y;
                        run.roomZ = z;
                    }
                }
                for (auto skip = run.roomSkip.begin(); skip != run.roomSkip.end();)
                    skip = now >= skip->second ? run.roomSkip.erase(skip) : std::next(skip);
                std::list<Creature*> const wholeRoom = room;  // (the place checks count the creatures left for a minute too)
                room.remove_if([&](Creature* other) { return run.roomSkip.count(other->GetGUID()) != 0; });
                if (!run.roomDone && !room.empty() && !creature->IsInCombat() && getMSTimeDiff(run.roomSince, now) < RoomMaxMs)
                {
                    constexpr float RoomWalkInYards = 20.0f;
                    auto fromPlace = [&](Creature const* other) { return other->GetExactDist(run.roomX, run.roomY, run.roomZ); };
                    // The one it chose stays its next until it is dead or left (package P4: the room is worked through one
                    // after the other, not each second the creature that happens to be nearest).
                    Creature* first = nullptr;
                    for (Creature* other : room)
                        if (other->GetGUID() == run.roomTarget)
                            first = other;
                    if (!first)
                        for (Creature* other : room)
                            if (!first || fromPlace(other) < fromPlace(first))
                                first = other;
                    run.roomTarget = first->GetGUID();
                    if (starting)
                    {
                        run.roomLogged = run.boss + 1;
                        LOG_INFO(ActivityLog, "activity: {}'s group clears the room of {} in {} first: {} creature{} stand{} within {:.0f} yd "
                            "of it (the nearest: {} level {}); it fights them at ({:.0f}, {:.0f}), {:.0f} yd from it", bot.name, boss.name,
                            plan->name, room.size(), room.size() == 1 ? "" : "s", room.size() == 1 ? "s" : "", std::max(RoomYards, CallYards), first->GetName(),
                            uint32(first->GetLevel()), run.roomX, run.roomY, creature->GetExactDist(run.roomX, run.roomY, run.roomZ));
                    }
                    rt.why = Acore::StringFormat("In {}: clearing the room of {}", plan->name, boss.name);
                    run.bossStuckSince = now;
                    auto goTo = [&](float x, float y, float z)
                    {
                        if (BotDriver::GetRpgState(player) != RpgState::GoCamp || getMSTimeDiff(rt.task.lastAssert, now) >= AssertMs)
                        {
                            DungeonGoTo(player, plan->mapId, x, y, z);
                            rt.task.lastAssert = now;
                        }
                    };
                    // The leader stands where no way on foot leads to the place the room is fought from: the group is put
                    // there, once per boss. (Shadowfang Keep, 2026-11-24: the test group stood on a wall top 7 yd above Odo
                    // the Blindwatcher, where the path finder has a patch of ground that leads nowhere, for the room's ten
                    // minutes and the boss's eight, every walk refused.)
                    if (GetSettings().dungeonsHopWhenNoWay && run.roomPlaceHopFor != run.boss + 1 && !groupBusy && !player->IsInCombat() &&
                        run.stillSince && getMSTimeDiff(run.stillSince, now) >= HopStillMs &&
                        player->GetExactDist(run.roomX, run.roomY, run.roomZ) > 8.0f && NoWayOnFoot(player, run.roomX, run.roomY, run.roomZ))
                    {
                        run.roomPlaceHopFor = uint8(run.boss + 1);
                        LOG_INFO(ActivityLog, "activity: {}'s group stands where no way on foot leads to the place it clears the room of {} from "
                            "in {} ({:.0f} yd away, {:.0f} yd up or down): it is put there", bot.name, boss.name, plan->name,
                            player->GetExactDist(run.roomX, run.roomY, run.roomZ), std::fabs(player->GetPositionZ() - run.roomZ));
                        TakeGuestAlong(bot, player, run.roomX, run.roomY, run.roomZ, Acore::StringFormat("no way on foot to the room of {}", boss.name));
                        for (Player* member : group)
                        {
                            if (!member->IsAlive() || member->GetMapId() != plan->mapId)
                                continue;
                            member->NearTeleportTo(run.roomX, run.roomY, run.roomZ, member->GetAngle(creature));
                            if (BotRuntime* other = FindRuntime(member->GetGUID()))
                                other->hasLastPosition = false;
                        }
                        ClearTask(rt, 2 * IN_MILLISECONDS, "put at the room's place");
                        run.stillSince = 0;
                        return;
                    }
                    bool const joinerFirst = std::any_of(joiners.begin(), joiners.end(),
                        [&](auto const& joiner) { return joiner.first == first->GetEntry(); });
                    // A neutral creature the boss's script brings, 20 yd or more from the dungeon's bosses: the group goes
                    // to it and fights it, and the few neutral ones beside it its fight turns, where they stand. (The owner,
                    // 2026-11-21, on the Viewing Room's students: [quote left out].) One nearer to a boss is pulled to the place below, so
                    // that what the fight splashes does not reach the boss.
                    if (joinerFirst && !first->IsHostileTo(player) && !first->IsInCombat())
                    {
                        float bossYards = first->GetExactDist2d(creature);
                        for (DungeonBoss const& other : plan->bosses)
                            if (other.entry != boss.entry)
                                if (Creature* found = player->FindNearestCreature(other.entry, 120.0f, true))
                                    bossYards = std::min(bossYards, first->GetExactDist2d(found));
                        if (bossYards >= 20.0f)
                        {
                            if (player->IsWithinDistInMap(first, 8.0f) && player->IsWithinLOSInMap(first))
                            {
                                BotDriver::Attack(player, first);
                                return;
                            }
                            if (run.roomInPlace != first->GetGUID())
                            {
                                run.roomInPlace = first->GetGUID();
                                LOG_INFO(ActivityLog, "activity: {}'s group fights {} (level {}) where it stands in the room of {} in {}: neutral "
                                    "until its fight, {:.0f} yd from the nearest boss", bot.name, first->GetName(), uint32(first->GetLevel()),
                                    boss.name, plan->name, bossYards);
                            }
                            float wayX = first->GetPositionX(), wayY = first->GetPositionY(), wayZ = first->GetPositionZ();
                            if (player->GetExactDist2d(first) > 30.0f && !run.eventSince &&
                                StepDungeonWay(guild, bot, player, rt, *plan, group, guest, wayX, wayY, wayZ, wayX, wayY, wayZ))
                                return;
                            goTo(wayX, wayY, wayZ);
                            return;
                        }
                    }
                    // One beside the group (it came, or the group went to it): fought where it is.
                    if (player->IsWithinDistInMap(first, 10.0f) && player->IsWithinLOSInMap(first))
                    {
                        if (run.walkInWalking)
                        {
                            run.walkInWalking = false;
                            BotDriver::PauseAI(player, 0);
                        }
                        BotDriver::Attack(player, first);
                        return;
                    }
                    // One the whole group walks into (below): the tank's target until it fights; one it did not get into a
                    // fight with in 20 s is left for a minute (the Cathedral's group walked into the same monk every 20 s for
                    // 14 minutes, round 6).
                    // (In a raid the ranged open on it too: it comes to them where no way on foot leads to it. Hakkar's
                    // room in Zul'Gurub stands on his platform, which the path finder does not lead onto; the raid walked
                    // into its creatures for ten minutes without a fight and fought Hakkar with four of them, 2026-11-22.)
                    auto const rangedOpen = [&](Creature* target)
                    {
                        if (!plan->raid)
                            return;
                        for (Player* member : group)
                        {
                            uint8 const cls = member->getClass();
                            if ((cls == CLASS_HUNTER || cls == CLASS_MAGE || cls == CLASS_WARLOCK) && member->IsAlive() &&
                                sGuildBotHolder.IsAIPlayed(member->GetGUID()) && member->GetDistance(target) < 40.0f &&
                                member->GetVictim() != target)
                                BotDriver::Attack(member, target, false);
                        }
                    };
                    if (run.wayBlocker == first->GetGUID())
                    {
                        if (getMSTimeDiff(run.wayBlockerAt, now) < 20 * IN_MILLISECONDS)
                        {
                            GoForWalkIn(player, rt, first);
                            rangedOpen(first);
                            return;
                        }
                        if (run.walkInWalking)
                        {
                            run.walkInWalking = false;
                            BotDriver::PauseAI(player, 0);
                        }
                        run.wayBlocker.Clear();
                        run.roomSkip[first->GetGUID()] = now + MINUTE * IN_MILLISECONDS;
                        LOG_INFO(ActivityLog, "activity: {}'s group did not get into a fight with {} (level {}) in the room of {} in {} in 20 s "
                            "({:.0f} yd from the tank): left for a minute", bot.name, first->GetName(), uint32(first->GetLevel()), boss.name,
                            plan->name, player->GetDistance(first));
                        return;
                    }
                    // A creature far from the room's place: the place moves up to it, on the group's side, as far from the
                    // boss as the room's first place (round 6 moved the Cathedral's place into the chapel, 25 yd from Mograine)
                    // and out of the reach of what is left for the boss fight; else it is left for a minute.
                    // (The pull walked the tank 50 yd ahead of its group past three creatures left beside Velratha, and
                    // they came on it out of the group's sight, Zul'Farrak round 5.)
                    constexpr float PullFromYards = 35.0f;
                    // (A creature the boss's script brings, neutral until then, out to 75 yd: the tank walks past its
                    // fellows, which see nobody, and pulls it back to the place.)
                    if (fromPlace(first) > (joinerFirst ? 75.0f : PullFromYards))
                    {
                        float x = 0.0f, y = 0.0f, z = 0.0f;
                        // (Away from the dungeon's other bosses as well: the place in Vectus's room lay 12 yd from Marduk
                        // Blackpool, neutral like him; the fights there drew Marduk in, and his pull brought all 32
                        // students, Scholomance 2026-11-20.)
                        std::vector<Creature*> otherBosses;
                        for (DungeonBoss const& other : plan->bosses)
                            if (other.entry != boss.entry)
                                if (Creature* found = player->FindNearestCreature(other.entry, 120.0f, true))
                                    if (!found->IsInCombat())
                                        otherBosses.push_back(found);
                        auto tryPlace = [&](float bossYards, float nearYards, float farYards)
                        {
                            if (!FindHopSpot(player, first->GetPositionX(), first->GetPositionY(), first->GetPositionZ(), x, y, z, nearYards,
                                    farYards) || creature->GetExactDist2d(x, y) < bossYards)
                                return false;
                            // (A place the group walks to and back from on the navmesh: the place moved up to a Twilight
                            // Shadowmage in Twilight Lord Kelris's room lay on the raised floor 5 yd above the walkway the
                            // group came along; the tank got up there in a fight, a warlock stood below it for over five
                            // minutes with no way up, and the run waited for it, Blackfathom Deeps, 2026-11-25.)
                            if (!sDungeonNav.FindWay(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
                                    x, y, z, nullptr, nullptr, player->GetMap()) ||
                                !sDungeonNav.FindWay(player->GetMapId(), x, y, z, player->GetPositionX(), player->GetPositionY(),
                                    player->GetPositionZ(), nullptr, nullptr, player->GetMap()))
                                return false;
                            for (Creature const* other : otherBosses)
                                if (other->GetExactDist2d(x, y) < (other->IsHostileTo(player) ? std::max(45.0f, other->GetAggroRange(player) + 15.0f) : 30.0f))
                                    return false;
                            // (Not among the room's other creatures either: a creature in a fight calls those within 10 yd
                            // of it every 3 s, and a neutral one beside the place joins that way. The place moved up to a
                            // student lay in the middle of the Viewing Room's students, and the group died there.)
                            for (Creature const* other : wholeRoom)
                                if (other != first && (other->GetExactDist2d(x, y) < 16.0f ||
                                        (other->IsHostileTo(player) && IdleSees(other, player, x, y, z, 8.0f))))
                                    return false;
                            for (auto const& [guid, until] : run.roomSkip)
                                if (Creature* left = ObjectAccessor::GetCreature(*player, guid))
                                    if (left->IsAlive() && left->IsHostileTo(player) && left->GetExactDist2d(x, y) <= left->GetAggroRange(player) + 8.0f)
                                        return false;
                            return true;
                        };
                        // (Else out of the boss's sight only: the throne room of Blackrock Depths lies all within 45 yd of
                        // the Emperor, no place was found for any of its 40 creatures, and all came with him, 2026-11-20.)
                        bool const usable = tryPlace(keep, 20.0f, 28.0f) ||
                            tryPlace(std::max(25.0f, creature->GetAggroRange(player) + 10.0f), 20.0f, 32.0f);
                        if (!usable)
                        {
                            run.roomSkip[first->GetGUID()] = now + MINUTE * IN_MILLISECONDS;
                            LOG_INFO(ActivityLog, "activity: {}'s group finds no place to fight {} (level {}) from in the room of {} in {}: "
                                "{:.0f} yd from its place, and nearer it is in the reach of the boss or of what is left for the boss fight; "
                                "left for a minute", bot.name, first->GetName(), uint32(first->GetLevel()), boss.name, plan->name, fromPlace(first));
                            return;
                        }
                        LOG_INFO(ActivityLog, "activity: {}'s group moves its place in the room of {} in {} up to {} (level {}): to ({:.0f}, "
                            "{:.0f}), {:.0f} yd from it", bot.name, boss.name, plan->name, first->GetName(), uint32(first->GetLevel()), x, y,
                            first->GetExactDist2d(x, y));
                        run.roomX = x;
                        run.roomY = y;
                        run.roomZ = z;
                    }
                    // The group to its place first. (Within 12 yd: the long walk stops about 10 yd short of its point,
                    // and a leader that waited to come within 6 yd stood there for six minutes.)
                    if (player->GetExactDist2d(run.roomX, run.roomY) > 12.0f)
                    {
                        // (A far place along the way with its look ahead, as to a boss: Scholomance's group walked
                        // straight through the hall before the Viewing Room to the place it had moved to beyond it, and
                        // 19 came at it, 2026-11-20.)
                        float wayX = run.roomX, wayY = run.roomY, wayZ = run.roomZ;
                        if (player->GetExactDist2d(run.roomX, run.roomY) > 30.0f && !run.eventSince &&
                            StepDungeonWay(guild, bot, player, rt, *plan, group, guest, run.roomX, run.roomY, run.roomZ, wayX, wayY, wayZ))
                            return;
                        goTo(wayX, wayY, wayZ);
                        return;
                    }
                    // Not one that stands within the boss's own reach: the pull walks the tank into its sight, and the
                    // boss comes with it (Mograine and the whole cathedral came with a monk 6 yd from him in a test run of
                    // the gate, 2026-11-14). It stays for the boss fight.
                    // (On the ground: Theka stands raised above her room in Zul'Farrak, and a creature 8 yd beside her
                    // was 20 yd from her in three dimensions.)
                    // (A neutral boss sees nobody: only what a fight beside it would splash onto it counts, 8 yd.)
                    if (first->GetExactDist2d(creature) <= (creature->IsHostileTo(player) ? creature->GetAggroRange(player) + 10.0f : 8.0f))
                    {
                        run.roomSkip[first->GetGUID()] = now + RoomMaxMs;
                        LOG_INFO(ActivityLog, "activity: {}'s group leaves {} (level {}) beside {} in {} for the boss fight: it stands {:.0f} yd "
                            "from it, and its pull would bring the boss", bot.name, first->GetName(), uint32(first->GetLevel()), boss.name,
                            plan->name, first->GetExactDist2d(creature));
                        return;
                    }
                    // From there: pulled to it (the leader opens on it from a distance, and the group fights it at
                    // its place), ...
                    float const at[3] = { run.roomX, run.roomY, run.roomZ };
                    if (Escape::BeginPull(player, rt, first, at))
                    {
                        // Without a ranged opener the pull walks the tank into the creature's sight, and in a boss's room
                        // that brings what stands around it (the Cathedral's chapel came with one monk, and Mograine with
                        // it, on the live server and on the dev instance, 2026-11-14): only for one that stands alone.
                        // Instead the whole group walks into it, at the tank, health and mana up (the gate's form for big
                        // pulls), and fights it and what comes with it where it stands. (The cancelled pull's walk is stopped:
                        // the tank walked on into the sight of a Deadmines overseer and eleven creatures came, 2026-11-14.)
                        if (!rt.pull.ranged)
                        {
                            uint32 beside = 0;
                            for (Creature* other : HostilesAround(first, player, RoomWalkInYards))
                                beside += other != first && !other->IsInCombat() ? 1 : 0;
                            if (beside)
                            {
                                Escape::CancelPull(player, rt, "called off (no ranged opener, and others stand beside it in a boss's room)");
                                player->StopMoving();
                                player->GetMotionMaster()->Clear();
                                if (!DungeonGate(guild, bot, player, rt, *plan, group, guest, Acore::StringFormat("the pack of {}", first->GetName()),
                                    4000 + run.wayKey))
                                    return;
                                run.wayBlocker = first->GetGUID();
                                run.wayBlockerAt = now;
                                ++run.wayKey;
                                ++run.metrics.walkIns;
                                LOG_INFO(ActivityLog, "activity: {}'s group walks into {} (level {}) in the room of {} in {} together: no ranged "
                                    "opener reaches it, and {} creature{} stand{} within {:.0f} yd of it", bot.name, first->GetName(),
                                    uint32(first->GetLevel()), boss.name, plan->name, beside, beside == 1 ? "" : "s", beside == 1 ? "s" : "",
                                    RoomWalkInYards);
                                GoForWalkIn(player, rt, first);
                                rangedOpen(first);
                                return;
                            }
                        }
                        ++run.pulls;
                        rt.why = Acore::StringFormat("In {}: pulling {} out of the room of {}", plan->name, first->GetName(), boss.name);
                        return;
                    }
                    // ... or, when it stands out of the boss's reach or near the place (it sees the leader there
                    // already), the group goes to it; one beside the boss that cannot be pulled out is left for a minute.
                    if (first->GetExactDist(creature) >= keep || fromPlace(first) <= 20.0f)
                    {
                        goTo(first->GetPositionX(), first->GetPositionY(), first->GetPositionZ());
                        return;
                    }
                    run.roomSkip[first->GetGUID()] = now + MINUTE * IN_MILLISECONDS;
                    LOG_INFO(ActivityLog, "activity: {}'s group cannot pull {} (level {}) out of the room of {} in {}: it stands {:.0f} yd "
                        "from it; left for a minute", bot.name, first->GetName(), uint32(first->GetLevel()), boss.name, plan->name,
                        first->GetExactDist(creature));
                    return;
                }
                // (Nothing in the room when the group comes: nothing to clear for this boss.)
                if (starting && !run.roomDone)
                {
                    run.roomLogged = run.boss + 1;
                    run.roomDone = true;
                }
                else if (!starting && !run.roomDone)
                {
                    // A room the boss's own script turns on the group, still full when the time is up: its pull would
                    // bring all of it, a sure wipe (Scholomance: Vectus with 32 students, again and again, 2026-11-20).
                    // The boss is passed over then, and the group goes on.
                    uint32 joinersLeft = 0;
                    for (Creature const* other : wholeRoom)
                        if (std::any_of(joiners.begin(), joiners.end(), [&](auto const& joiner) { return joiner.first == other->GetEntry(); }))
                            ++joinersLeft;
                    if (joinersLeft >= 6 && !creature->IsInCombat())
                    {
                        run.roomDone = true;
                        LOG_INFO(ActivityLog, "activity: {}'s group could not clear the room of {} in {} in {} s ({} creatures its pull "
                            "would bring are left): it passes it over and goes on to the next boss", bot.name, boss.name, plan->name,
                            getMSTimeDiff(run.roomSince, now) / IN_MILLISECONDS, joinersLeft);
                        ++run.boss;
                        run.bossStuckSince = 0;
                        run.hopped = false;
                        continue;
                    }
                    run.roomDone = true;
                    LOG_INFO(ActivityLog, "activity: {}'s group is through with the room of {} in {} after {} s{}", bot.name, boss.name,
                        plan->name, getMSTimeDiff(run.roomSince, now) / IN_MILLISECONDS,
                        room.empty() ? "" : Acore::StringFormat(" ({} creature{} left: the time is up, or the boss is in a fight)", room.size(),
                            room.size() == 1 ? "" : "s"));
                }
                // The gate before the boss (package P1; the owner: the whole group at the tank, health and mana up), where the
                // group stands now: after the room, out of the boss's reach.
                if (run.gateOpenFor != run.boss + 1 && !creature->IsInCombat())
                {
                    if (!DungeonGate(guild, bot, player, rt, *plan, group, guest, boss.name, run.boss + 1))
                        return;
                    run.gateOpenFor = run.boss + 1;
                }
                // The Twin Emperors: on two sides (StepTwinsPull, StepRaidTwins).
                if (plan->raid && creature->GetEntry() == 15275 && StepTwinsPull(guild, bot, player, rt, group, creature))
                    return;
                // C'Thun's Eye: a hunter pulls it while the raid waits out of its sight (StepCthunPull).
                // (The run's boss is C'Thun's body, which stands where the Eye is; the Eye is what engages.)
                if (plan->raid && (creature->GetEntry() == 15727 || creature->GetEntry() == 15589))
                    if (Creature* eye = creature->GetEntry() == 15589 ? creature : creature->FindNearestCreature(15589, 50.0f, true))
                        if (StepCthunPull(rt, group, eye, plan->name))
                            return;
                // Up to it first (a wall of its cabin between them, and the attack never began), then the attack.
                // (Farther off: the walk to it below.)
                // Razorgore in his first phase: the eggs (StepRaidRazorgore); the raid fights his troops and waits.
                if (plan->raid && creature->GetEntry() == 12435 && RazorgoreEggsLeft(creature))
                {
                    rt.why = Acore::StringFormat("In {}: Razorgore the Untamed breaks his eggs under the Orb of Domination", plan->name);
                    run.bossStuckSince = 0;
                    return;
                }
                if (nearBoss)
                {
                    // The last yards with the whole group: the tank does not walk on while somebody is more than 12 yd
                    // behind it (round 2: Rhahk'Zor pulled with the mage 16 yd back, after a gate the group had passed).
                    if (!creature->IsInCombat() && !player->IsInCombat() && !player->IsWithinDistInMap(creature, 8.0f) &&
                        !DungeonGate(guild, bot, player, rt, *plan, group, guest, "the last yards to " + boss.name, 70000 + run.boss, true, false))
                    {
                        if (player->isMoving())
                            BotDriver::Rest(player);
                        return;
                    }
                    // The boss to the group, once per boss: the tank opens on it from where the whole group stands ready
                    // and holds there until it comes (a careful pull). (Of 25 bosses pulled not ready in the runs of
                    // 2026-11-19 ten had members 15-21 yd behind the tank that walked up to the boss, and others joined
                    // a fight of creatures beside the group while it gathered: Mr. Smite one second into its gather.)
                    if (!creature->IsInCombat() && !player->IsInCombat() && !rt.pull.active && run.bossPullTried != run.boss + 1 &&
                        player->GetExactDist(creature) > 10.0f)
                    {
                        run.bossPullTried = run.boss + 1;
                        float const at[3] = { player->GetPositionX(), player->GetPositionY(), player->GetPositionZ() };
                        if (Escape::BeginPull(player, rt, creature, at))
                        {
                            LOG_INFO(ActivityLog, "activity: {} pulls {} to its group in {}: the whole group is ready at it, {:.0f} yd "
                                "from the boss", bot.name, boss.name, plan->name, player->GetExactDist(creature));
                            return;
                        }
                    }
                    if (rt.pull.active)
                        return;
                    if (!player->IsWithinDistInMap(creature, 8.0f) || !player->IsWithinLOSInMap(creature))
                    {
                        if (BotDriver::GetRpgState(player) != RpgState::GoCamp || getMSTimeDiff(rt.task.lastAssert, now) >= AssertMs)
                        {
                            DungeonGoTo(player, plan->mapId, creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ());
                            rt.task.lastAssert = now;
                        }
                    }
                    else
                        BotDriver::Attack(player, creature);
                }
            }
            if (nearBoss || (player->IsInCombat() && !idle))
                return;
        }
        rt.why = Acore::StringFormat("In {}: on the way to {} ({})", plan->name, boss.name, RoughDistance(distance));
        // The way in stretches, and what stands on it first (package P3; not while an event's NPC walks).
        float wayX = goX, wayY = goY, wayZ = goZ;
        if (!run.eventSince && StepDungeonWay(guild, bot, player, rt, *plan, group, guest, goX, goY, goZ, wayX, wayY, wayZ))
            return;
        if (BotDriver::GetRpgState(player) != RpgState::GoCamp || getMSTimeDiff(rt.task.lastAssert, now) >= 2 * AssertMs ||
            std::fabs(wayX - run.wayX) + std::fabs(wayY - run.wayY) > 6.0f)
        {
            DungeonGoTo(player, plan->mapId, wayX, wayY, wayZ);
            rt.task.lastAssert = now;
            run.wayX = wayX;
            run.wayY = wayY;
        }
        return;
    }
    // A tribute run: Mizzle, the tribute, the guards' buffs first.
    if (run.tribute && run.bossesDown && StepTributeEnd(guild, bot, player, rt, *plan, group, guest))
        return;
    // Through: the group stays for its loot, then it leaves (the owner: 30 seconds after the end boss).
    if (GetSettings().dungeonsTeleport && run.bossesDown && GetSettings().dungeonsLingerSeconds)
    {
        if (!run.endSince)
        {
            run.endSince = now;
            LOG_INFO(ActivityLog, "activity: {}'s group is through with {}: it leaves in {} s", bot.name, plan->name,
                GetSettings().dungeonsLingerSeconds);
            if (guest && guest->GetMapId() == plan->mapId)
                TellGuest(guest, Acore::StringFormat("Your group is through with {}: it leaves in {} seconds{}.", plan->name,
                    GetSettings().dungeonsLingerSeconds, GetSettings().dungeonsTakeOwnerAlong ? ", and you with it" : ""));
        }
        uint32 const stayed = getMSTimeDiff(run.endSince, now) / IN_MILLISECONDS;
        if (stayed < GetSettings().dungeonsLingerSeconds)
        {
            rt.why = Acore::StringFormat("Through with {}: leaving in {} s", plan->name, GetSettings().dungeonsLingerSeconds - stayed);
            return;
        }
    }
    EndDungeonRun(guild, bot, player, rt, run.bossesDown ? "every boss it could get to is down" : "it got to no boss",
        run.bossesDown > 0);
}
