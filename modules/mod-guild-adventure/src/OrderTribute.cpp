/*
 * mod-guild-adventure: Dire Maul North's tribute run (part of OrderService).
 *
 * The owner, 2026-11-21: [quote left out] - the guards and the other
 * named ogres stay alive, only King Gordok is killed, and the more of them live, the better the Gordok Tribute that
 * Mizzle the Crafty sets up afterwards. Everything the game needs for it is scripted in AzerothCore's world database
 * (SmartAI): the Broken Trap's quest freezes Guard Slip'kik when he walks into the repaired trap, Knot Thimblejack's
 * quest makes the Gordok Ogre Suit, Captain Kromcrush walks off when somebody in the suit talks to him, Cho'Rush the
 * Observer turns friendly at King Gordok's death, Mizzle crowns the group and summons the tribute, whose loot mode the
 * instance script sets by how many of the six died, and the guards give their buffs to the new king.
 *
 * So the group does the steps a player group does (docs/orders.md "The Dire Maul tribute run"):
 *   the Gordok Courtyard Key out of Fengus's Chest while Fengus is away -> the courtyard door with it -> the Broken Trap
 *   repaired with Frost Oil and a Thorium Widget of the guild storage -> the Ogre Tannin out of its basket -> the suit
 *   at Knot with the guild storage's runecloth, leather and thread -> Slip'kik frozen -> the inner door (a rogue's
 *   lockpicking, an engineer's Large Seaforium Charge, a blacksmith's Truesilver Skeleton Key, else Mol'dar's key) ->
 *   Kromcrush fooled -> King Gordok, with Cho'Rush never targeted -> Mizzle -> the tribute -> the guards' buffs.
 * A step that cannot be done costs one named ogre: it is fought after all, and the tribute is smaller.
 */

#include "OrderService.h"
#include "BotDriver.h"
#include "CraftOrders.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "Dungeons.h"
#include "GameObject.h"
#include "GameObjectAI.h"
#include "GuildAdventureMgr.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "QuestInteraction.h"
#include "Spell.h"
#include "SpellMgr.h"
#include "Storage/ShoppingList.h"
#include "Storage/StorageService.h"
#include "StringFormat.h"
#include "WaypointMgr.h"
#include <algorithm>
#include <cmath>

using namespace GuildAdventure;

namespace
{
    constexpr char const* ActivityLog = "module.ga.activity";
    constexpr uint32 AssertMs = 5 * IN_MILLISECONDS;

    enum TributeStep : uint8
    {
        StepNone = 0,
        StepKey,        // the Gordok Courtyard Key out of Fengus's Chest
        StepCourtyard,  // the Gordok Courtyard Door
        StepTrap,       // the Broken Trap repaired
        StepTannin,     // the Ogre Tannin out of its basket
        StepSuit,       // the Gordok Ogre Suit from Knot
        StepInner,      // Slip'kik frozen, the Gordok Inner Door
        StepKromcrush,  // Captain Kromcrush fooled
        StepThrone,     // King Gordok (the boss loop)
        StepMizzle,     // crowned, the tribute summoned
        StepChest,      // the tribute looted
        StepBuffs,      // the guards' buffs
        StepDone
    };

    enum TributeDoor : uint8
    {
        DoorMoldar = 0,     // Guard Mol'dar is killed for the Gordok Inner Door Key
        DoorLockpick,       // a rogue with Lockpicking 300
        DoorCharge,         // an engineer with a Large Seaforium Charge of the guild storage
        DoorSkeletonKey     // a blacksmith with a Truesilver Skeleton Key of the guild storage
    };

    constexpr uint32 NpcMoldar = 14326, NpcKreeg = 14322, NpcFengus = 14321, NpcSlipkik = 14323, NpcKromcrush = 14325,
        NpcChorush = 14324, NpcGordok = 11501, NpcKnot = 14338, NpcMizzle = 14353;
    constexpr uint32 GoFengusChest = 179516, GoCourtyardDoor = 177219, GoInnerDoor = 177217, GoBrokenTrap = 179485,
        GoFixedTrap = 179512, GoTanninBasket = 179499, GoTribute = 179564;
    constexpr uint32 ItemCourtyardKey = 18266, ItemInnerKey = 18268, ItemFrostOil = 3829, ItemThoriumWidget = 15994,
        ItemRunecloth = 14048, ItemRuggedLeather = 8170, ItemRuneThread = 14341, ItemTannin = 18240, ItemSuit = 18258,
        ItemCharge = 4398, ItemSkeletonKey = 15871;
    constexpr uint32 QuestTrap = 1193, QuestSuit = 5518;
    constexpr float BuffYards = 60.0f;  // a guard's buff is fetched only this near the throne
    constexpr uint32 SpellSuit = 22736, SpellIceLock = 22856, SpellKing = 22799;
    // The gossip menus whose option the SmartAI scripts act on (gossip_menu_option, smart_scripts event 62).
    constexpr uint32 MenuKromcrush = 5738, MenuMizzleKing = 5708, MenuMizzleTribute = 5740;

    struct Material
    {
        uint32 item;
        uint32 count;
    };
    constexpr Material TrapMaterials[] = { { ItemFrostOil, 1 }, { ItemThoriumWidget, 1 } };
    constexpr Material SuitMaterials[] = { { ItemRunecloth, 4 }, { ItemRuggedLeather, 8 }, { ItemRuneThread, 2 } };

    // The six the tribute counts (the instance script: TYPE_NORTH_WING_BOSSES at each death).
    constexpr uint32 Spared[] = { NpcMoldar, NpcKreeg, NpcFengus, NpcSlipkik, NpcKromcrush, NpcChorush };

    // The guards that give a buff to the new king, in the order of the way out (Slip'kik, Fengus, Mol'dar), and the
    // gossip menu and buff of each.
    struct Guard
    {
        uint32 entry;
        uint32 menu;
        uint32 buff;
    };
    constexpr Guard BuffGuards[] = { { NpcSlipkik, 5733, 22820 }, { NpcFengus, 5734, 22817 }, { NpcMoldar, 5735, 22818 } };

    // A patrol is waited for while it comes this near to the leader (its reach at the server's aggro rate and a margin).
    constexpr float PatrolKeepYards = 10.0f;
    constexpr uint32 PatrolWaitMs = 90 * IN_MILLISECONDS;
    constexpr uint32 FreezeWaitMs = 6 * MINUTE * IN_MILLISECONDS;

    std::string ItemName(uint32 entry)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        return proto ? proto->Name1 : Acore::StringFormat("item {}", entry);
    }

    std::string CreatureName(uint32 entry)
    {
        CreatureTemplate const* proto = sObjectMgr->GetCreatureTemplate(entry);
        return proto ? proto->Name : Acore::StringFormat("creature {}", entry);
    }

    // A creature of the instance by its entry, wherever it is (the guards stand hundreds of yards apart).
    Creature* FindSpawned(Player* player, uint32 entry, bool alive = true)
    {
        for (auto const& [spawnId, creature] : player->GetMap()->GetCreatureBySpawnIdStore())
            if (creature && creature->GetEntry() == entry && creature->IsInWorld() && (!alive || creature->IsAlive()))
                return creature;
        return nullptr;
    }

    // How near a guard on its round comes to a place: the nearest point of its waypoint path (or where it stands).
    float RoundDistance(Creature const* creature, float x, float y, float z)
    {
        float best = creature->GetExactDist2d(x, y);
        if (uint32 const pathId = creature->GetWaypointPath())
            if (WaypointPath const* path = sWaypointMgr->GetPath(pathId); path && path->Nodes.size() > 1)
                for (std::size_t i = 0; i < path->Nodes.size(); ++i)
                {
                    WaypointNode const& a = path->Nodes[i];
                    WaypointNode const& b = path->Nodes[(i + 1) % path->Nodes.size()];
                    if (std::fabs(a.Z - z) > 12.0f && std::fabs(b.Z - z) > 12.0f)
                        continue;
                    float const abx = b.X - a.X, aby = b.Y - a.Y;
                    float const ab2 = std::max(0.01f, abx * abx + aby * aby);
                    float const t = std::clamp(((x - a.X) * abx + (y - a.Y) * aby) / ab2, 0.0f, 1.0f);
                    best = std::min(best, std::hypot(a.X + abx * t - x, a.Y + aby * t - y));
                }
        return best;
    }

    // A game object of the instance by its entry, wherever it is (FindNearestGameObject sees only the loaded grids around
    // the character: after a wipe the basket 500 yd away was "not there", and Kromcrush was fought for it, 2026-11-21).
    GameObject* FindSpawnedObject(Player* player, uint32 entry)
    {
        for (auto const& [spawnId, object] : player->GetMap()->GetGameObjectBySpawnIdStore())
            if (object && object->GetEntry() == entry && object->IsInWorld())
                return object;
        return nullptr;
    }

    Player* Holding(std::vector<Player*> const& group, uint32 item)
    {
        for (Player* member : group)
            if (member->HasItemCount(item, 1))
                return member;
        return nullptr;
    }

    bool DoorOpen(GameObject const* door)
    {
        return door && door->GetGoState() != GO_STATE_READY;
    }

    // What `player` lacks of `materials` comes out of the guild storage into its bags. Empty: all there; else what is
    // missing.
    std::string TakeMaterials(Player* player, uint32 owner, Material const* materials, std::size_t count)
    {
        std::vector<std::string> missing;
        for (std::size_t i = 0; i < count; ++i)
        {
            uint32 const have = player->GetItemCount(materials[i].item);
            if (have >= materials[i].count)
                continue;
            std::string error;
            uint32 const taken = sStorageService.Withdraw(player, materials[i].item, 0, materials[i].count - have, error);
            if (have + taken < materials[i].count)
                missing.push_back(Acore::StringFormat("{} x {}", materials[i].count - have - taken, ItemName(materials[i].item)));
        }
        (void)owner;
        std::string text;
        for (std::string const& one : missing)
            text += (text.empty() ? "" : ", ") + one;
        return text;
    }
}

bool OrderService::IsTributeSpared(BotRuntime::DungeonRun const& run, Creature const* creature)
{
    if (!run.tribute || !creature || run.tribute >= StepDone)
        return false;
    uint32 const entry = creature->GetEntry();
    return std::find(std::begin(Spared), std::end(Spared), entry) != std::end(Spared) && !run.tributeKill.count(entry);
}

std::string OrderService::CheckTributeStart(AdventureGuild& guild, std::vector<ObjectGuid> const& members)
{
    // What the storage lacks: a craft order for what the guild's crafters make, the shopping list for what vendors sell
    // (the developers' default while the owner slept, 2026-11-21: the run does not start then, and the window says what is missing).
    // What the group's characters carry counts too (a run that ended early leaves the materials in its leader's bags).
    std::vector<std::string> missing;
    auto need = [&](uint32 item, uint32 count)
    {
        uint32 have = sStorageService.GetAmountOfEntry(guild.ownerAccount, item);
        for (ObjectGuid const& guid : members)
            if (Player* member = ObjectAccessor::FindPlayer(guid))
                have += member->GetItemCount(item);
        if (have >= count)
            return;
        std::string order;
        std::string error;
        if (!sShoppingList.RefuseReason(sObjectMgr->GetItemTemplate(item), guild.team))
        {
            uint32 min = count;
            for (ShoppingEntry const& entry : sShoppingList.Get(guild.ownerAccount))
                if (entry.entry == item)
                    min = std::max(min, entry.min);
            if (sShoppingList.Set(guild.ownerAccount, guild.team, ShoppingEntry { item, min, true }, error))
                order = "on the shopping list";
        }
        else if (!sCraftOrders.HasOpen(guild.ownerAccount, item))
        {
            std::string code, text;
            if (sCraftOrders.Add(guild, item, count - have, code, text))
                order = "a craft order";
        }
        else
            order = "a craft order is open";
        missing.push_back(Acore::StringFormat("{} x {} (the guild storage has {}{}{})", count - have, ItemName(item), have,
            order.empty() ? "" : "; ", order));
    };
    for (Material const& material : TrapMaterials)
        need(material.item, material.count);
    for (Material const& material : SuitMaterials)
        need(material.item, material.count);
    std::string text;
    for (std::string const& one : missing)
        text += (text.empty() ? "" : ", ") + one;
    return text;
}

void OrderService::BeginTribute(AdventureGuild& guild, GuildBot const& bot, DungeonPlan const& plan, BotRuntime::DungeonRun& run,
    std::vector<Player*> const& group)
{
    run.tribute = StepKey;
    run.tributeSince = getMSTime();
    run.tributeKill.clear();
    run.tributeTries = 0;
    run.tributeWaitSince = 0;
    run.tributeTrapAt = 0;
    run.tributeBuffAt = 0;
    run.tributeOpener.Clear();
    run.tributeMaker.Clear();
    // The ogre suit: Knot gives it once per character (quest 5518 is not repeatable). The leader makes it when it never
    // had it, else another member that never had it (a bot, not the owner's own character); a suit somebody of the group
    // carries from before is used as it is.
    if (!Holding(group, ItemSuit))
    {
        auto never = [](Player* member) { return member->IsAlive() && member->GetQuestStatus(QuestSuit) == QUEST_STATUS_NONE; };
        for (Player* member : group)
            if (member->GetGUID() == bot.guid && never(member))
                run.tributeMaker = member->GetGUID();
        for (Player* member : group)
            if (run.tributeMaker.IsEmpty() && never(member) && member->GetGUID() != guild.leader)
                run.tributeMaker = member->GetGUID();
    }
    // The inner door (the owner's list): a rogue with Lockpicking 300, an engineer with a Large Seaforium Charge, a
    // blacksmith with a Truesilver Skeleton Key, else Guard Mol'dar's key.
    run.tributeDoor = DoorMoldar;
    for (Player* member : group)
        if (run.tributeDoor == DoorMoldar && member->getClass() == CLASS_ROGUE && member->GetSkillValue(SKILL_LOCKPICKING) >= 300)
        {
            run.tributeDoor = DoorLockpick;
            run.tributeOpener = member->GetGUID();
        }
    if (run.tributeDoor == DoorMoldar && sStorageService.GetAmountOfEntry(guild.ownerAccount, ItemCharge))
        for (Player* member : group)
            if (run.tributeDoor == DoorMoldar && member->GetSkillValue(SKILL_ENGINEERING) >= 200)
            {
                run.tributeDoor = DoorCharge;
                run.tributeOpener = member->GetGUID();
            }
    if (run.tributeDoor == DoorMoldar && sStorageService.GetAmountOfEntry(guild.ownerAccount, ItemSkeletonKey))
        for (Player* member : group)
            if (run.tributeDoor == DoorMoldar && member->GetSkillValue(SKILL_BLACKSMITHING) >= 200)
            {
                run.tributeDoor = DoorSkeletonKey;
                run.tributeOpener = member->GetGUID();
            }
    // The bosses of the run: King Gordok, and Mol'dar first when his key is the way through the inner door.
    run.order.clear();
    for (std::size_t i = 0; i < plan.bosses.size(); ++i)
        if (plan.bosses[i].entry == NpcMoldar && run.tributeDoor == DoorMoldar)
            run.order.push_back(uint8(i));
    for (std::size_t i = 0; i < plan.bosses.size(); ++i)
        if (plan.bosses[i].entry == NpcGordok)
            run.order.push_back(uint8(i));
    if (run.tributeDoor == DoorMoldar)
        run.tributeKill.insert(NpcMoldar);
    // The trap's and the suit's materials into the leader's bags at once, so that nothing else of the guild uses them
    // meanwhile (the run's own consumables put the Frost Oil on a weapon in the first test run, 2026-11-21).
    Player* leader = nullptr;
    for (Player* member : group)
        if (member->GetGUID() == bot.guid)
            leader = member;
    if (leader)
    {
        std::string const trapMissing = TakeMaterials(leader, guild.ownerAccount, TrapMaterials, std::size(TrapMaterials));
        Player* maker = ObjectAccessor::FindPlayer(run.tributeMaker);
        std::string const suitMissing = maker ? TakeMaterials(maker, guild.ownerAccount, SuitMaterials, std::size(SuitMaterials)) :
            std::string();
        if (!trapMissing.empty() || !suitMissing.empty())
            LOG_INFO(ActivityLog, "activity: {}'s group lacks for its tribute run in {}: {}{}{}", bot.name, plan.name, trapMissing,
                !trapMissing.empty() && !suitMissing.empty() ? ", " : "", suitMissing);
    }
    Player const* opener = ObjectAccessor::FindPlayer(run.tributeOpener);
    LOG_INFO(ActivityLog, "activity: {}'s group plays a tribute run in {}: the named ogres stay alive, King Gordok dies; the inner "
        "door {}", bot.name, plan.name,
        run.tributeDoor == DoorLockpick ? Acore::StringFormat("by {}'s lockpicking", opener ? opener->GetName() : std::string("a rogue")) :
        run.tributeDoor == DoorCharge ? Acore::StringFormat("by a Large Seaforium Charge of the guild storage ({})", opener ? opener->GetName() : std::string("an engineer")) :
        run.tributeDoor == DoorSkeletonKey ? Acore::StringFormat("by a Truesilver Skeleton Key of the guild storage ({})", opener ? opener->GetName() : std::string("a blacksmith")) :
        std::string("by Guard Mol'dar's key: nobody of the group picks the lock, and the guild storage has no charge or skeleton key for "
            "who could use it, so Mol'dar is killed"));
    if (Player const* maker = ObjectAccessor::FindPlayer(run.tributeMaker))
        LOG_INFO(ActivityLog, "activity: {}'s tribute run in {}: {} makes the Gordok Ogre Suit (it never had Knot's quest)", bot.name,
            plan.name, maker->GetName());
    else if (Player const* holder = Holding(group, ItemSuit))
        LOG_INFO(ActivityLog, "activity: {}'s tribute run in {}: {} carries a Gordok Ogre Suit from before", bot.name, plan.name,
            holder->GetName());
    else
        LOG_INFO(ActivityLog, "activity: {}'s tribute run in {}: nobody of the group can make the Gordok Ogre Suit (each had Knot's quest "
            "before, it is once per character): Captain Kromcrush is fought", bot.name, plan.name);
}

void OrderService::StepTributeFight(BotRuntime::DungeonRun& run, std::vector<Player*> const& group, std::string const& leaderName,
    std::string const& dungeon)
{
    if (!run.tribute || run.tribute >= StepDone)
        return;
    // A named ogre that came at the group before the throne room (a patrol walked into it): fought after all. Cho'Rush
    // never: he is held while King Gordok dies (the owner: "must be off-tanked while the group ensures King Gordok is killed
    // first").
    for (Player* member : group)
        for (Unit* attacker : member->getAttackers())
        {
            Creature* creature = attacker ? attacker->ToCreature() : nullptr;
            if (!creature || !IsTributeSpared(run, creature) || creature->GetEntry() == NpcChorush)
                continue;
            run.tributeKill.insert(creature->GetEntry());
            LOG_INFO(ActivityLog, "activity: {}'s group fights {} in {} after all: it came at {}; the tribute is smaller by one",
                leaderName, creature->GetName(), dungeon, member->GetName());
        }
    // Nobody keeps a spared ogre as its target: another foe of the fight, else none.
    for (Player* member : group)
    {
        Creature* victim = member->GetVictim() ? member->GetVictim()->ToCreature() : nullptr;
        Pet* pet = member->GetPet();
        Creature* petVictim = pet && pet->GetVictim() ? pet->GetVictim()->ToCreature() : nullptr;
        if (!IsTributeSpared(run, victim) && !IsTributeSpared(run, petVictim))
            continue;
        Unit* other = nullptr;
        for (Player* someone : group)
            for (Unit* attacker : someone->getAttackers())
                if (attacker && attacker->IsAlive() && !IsTributeSpared(run, attacker->ToCreature()) && member->IsValidAttackTarget(attacker))
                    if (!other || member->GetDistance(attacker) < member->GetDistance(other))
                        other = attacker;
        if (IsTributeSpared(run, victim))
        {
            if (other)
                BotDriver::Attack(member, other);
            else
                member->AttackStop();
        }
        if (pet && IsTributeSpared(run, petVictim))
        {
            pet->AttackStop();
            if (other && pet->AI())
                pet->AI()->AttackStart(other);
        }
    }
}

bool OrderService::StepTribute(AdventureGuild& guild, GuildBot& bot, Player* leader, BotRuntime& rt, DungeonPlan const& plan,
    std::vector<Player*> const& group, Player* guest)
{
    BotRuntime::DungeonRun& run = rt.dungeon;
    if (!run.tribute || run.tribute >= StepThrone || run.boss >= RunBossCount(run, plan) || RunBoss(run, plan, run.boss).entry != NpcGordok)
        return false;
    if (leader->IsInCombat() || !leader->IsAlive())
        return true;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    auto advance = [&](uint8 step)
    {
        run.tribute = step;
        run.tributeSince = now;
        run.tributeTries = 0;
        run.tributeWaitSince = 0;
    };
    // A named ogre that cannot be got past (its step failed): fought after all, before King Gordok.
    auto fightAfterAll = [&](uint32 entry, std::string const& why)
    {
        if (run.tributeKill.insert(entry).second)
        {
            LOG_INFO(ActivityLog, "activity: {}'s group fights {} in {} after all: {}; the tribute is smaller by one", bot.name,
                CreatureName(entry), plan.name, why);
            for (std::size_t i = 0; i < plan.bosses.size(); ++i)
                if (plan.bosses[i].entry == entry)
                    run.order.insert(run.order.begin() + run.boss, uint8(i));
        }
    };
    auto walkTo = [&](float x, float y, float z, std::string const& why)
    {
        rt.why = Acore::StringFormat("In {}: tribute run: {}", plan.name, why);
        PauseStuck(rt);
        run.bossStuckSince = now;
        // The last yards by a point movement: the long walk stops about 10 yd short of its point (the first full test
        // stood 25 minutes 9 yd from Fengus's Chest, 2026-11-21).
        if (leader->GetExactDist(x, y, z) < 15.0f)
        {
            if (getMSTimeDiff(rt.task.lastAssert, now) >= 2 * IN_MILLISECONDS || !leader->isMoving())
            {
                if (BotDriver::GetRpgState(leader) == RpgState::GoCamp)
                    BotDriver::Rest(leader);
                leader->GetMotionMaster()->MovePoint(0, x, y, z);
                rt.task.lastAssert = now;
            }
            return;
        }
        // A leader that has not got on for half a minute out of a fight walks the core's path itself for a while (the
        // eighth test run stood 20 minutes 105 yd from the Ogre Tannin Basket with a whole path to it, 2026-11-21).
        if (leader->IsInCombat() || leader->GetExactDist2d(run.tributeStillX, run.tributeStillY) > 3.0f)
        {
            run.tributeStillX = leader->GetPositionX();
            run.tributeStillY = leader->GetPositionY();
            run.tributeStillSince = now;
        }
        else if (getMSTimeDiff(run.tributeStillSince, now) >= 30 * IN_MILLISECONDS && now >= run.tributeDirectUntil)
        {
            run.tributeDirectUntil = now + 25 * IN_MILLISECONDS;
            run.tributeStillSince = now;
            LOG_INFO(ActivityLog, "activity: {} has not got on for 30 s in {} on its tribute run ({}): it walks the core's path there itself "
                "for a while", bot.name, plan.name, why);
        }
        if (now < run.tributeDirectUntil)
        {
            if (getMSTimeDiff(rt.task.lastAssert, now) >= 2 * IN_MILLISECONDS || !leader->isMoving())
            {
                if (BotDriver::GetRpgState(leader) == RpgState::GoCamp)
                    BotDriver::Rest(leader);
                leader->GetMotionMaster()->MovePoint(0, x, y, z);
                rt.task.lastAssert = now;
            }
            return;
        }
        float wayX = x, wayY = y, wayZ = z;
        if (StepDungeonWay(guild, bot, leader, rt, plan, group, guest, x, y, z, wayX, wayY, wayZ))
            return;
        if (BotDriver::GetRpgState(leader) != RpgState::GoCamp || getMSTimeDiff(rt.task.lastAssert, now) >= AssertMs ||
            std::fabs(wayX - run.wayX) + std::fabs(wayY - run.wayY) > 6.0f)
        {
            BotDriver::GoTo(leader, plan.mapId, wayX, wayY, wayZ, false);
            rt.task.lastAssert = now;
            run.wayX = wayX;
            run.wayY = wayY;
        }
    };
    auto stand = [&](std::string const& why)
    {
        rt.why = Acore::StringFormat("In {}: tribute run: {}", plan.name, why);
        PauseStuck(rt);
        run.bossStuckSince = now;
        if (leader->isMoving())
            BotDriver::Rest(leader);
    };
    // A member walks to a point of the tribute run by itself (the basket, Knot): its AI is held meanwhile.
    auto bring = [&](Player* member, float x, float y, float z)
    {
        BotDriver::PauseAI(member, 3 * IN_MILLISECONDS);
        if (member->GetExactDist(x, y, z) > 1.5f)
            member->GetMotionMaster()->MovePoint(0, x, y, z);
    };
    // While the group waits for a guard on its round: not on that round. Back along the tank's way to the first place
    // its round keeps 18 yd from, at most 120 yd. (The first full test waited 29 yd from Fengus's chest on his round,
    // and he walked into the group, 2026-11-21.)
    auto waitOffRound = [&](Creature const* guard, std::string const& why)
    {
        constexpr float ClearYards = 18.0f;
        if (RoundDistance(guard, leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ()) >= ClearYards)
        {
            stand(why);
            return;
        }
        float back = 0.0f;
        BotRuntime::DungeonRun::TrailPoint previous { leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ() };
        for (BotRuntime::DungeonRun::TrailPoint const& point : run.trail)
        {
            back += std::hypot(point.x - previous.x, point.y - previous.y);
            previous = point;
            if (back > 120.0f)
                break;
            if (RoundDistance(guard, point.x, point.y, point.z) >= ClearYards)
            {
                rt.why = Acore::StringFormat("In {}: tribute run: {}", plan.name, why);
                PauseStuck(rt);
                run.bossStuckSince = now;
                if (getMSTimeDiff(rt.task.lastAssert, now) >= 2 * IN_MILLISECONDS || !leader->isMoving())
                {
                    if (BotDriver::GetRpgState(leader) == RpgState::GoCamp)
                        BotDriver::Rest(leader);
                    leader->GetMotionMaster()->MovePoint(0, point.x, point.y, point.z);
                    rt.task.lastAssert = now;
                }
                return;
            }
        }
        stand(why);
    };
    // A guard on its round near the leader (Fengus, Slip'kik): the group waits until it has walked on.
    auto patrolNear = [&](uint32 entry, float x, float y, float keep) -> Creature*
    {
        Creature* guard = FindSpawned(leader, entry);
        if (!guard || guard->IsInCombat() || !IsTributeSpared(run, guard))
            return nullptr;
        return guard->GetExactDist2d(x, y) < keep ? guard : nullptr;
    };
    // A walk of a step: not while Fengus or Slip'kik walks his round across it (each came at the group in the second
    // full test: Fengus on the way from his chest to the courtyard door, Slip'kik at the trap, 2026-11-21). The group
    // waits off the round, three minutes at most per step, then walks.
    auto goTo = [&](float x, float y, float z, std::string const& why)
    {
        float const ax = leader->GetPositionX(), ay = leader->GetPositionY();
        float const abx = x - ax, aby = y - ay;
        float const ab2 = std::max(0.01f, abx * abx + aby * aby);
        auto toWay = [&](float px, float py)
        {
            float const t = std::clamp(((px - ax) * abx + (py - ay) * aby) / ab2, 0.0f, 1.0f);
            return std::hypot(ax + abx * t - px, ay + aby * t - py);
        };
        for (uint32 entry : { NpcFengus, NpcSlipkik })
        {
            Creature* guard = FindSpawned(leader, entry);
            if (!guard || guard->IsInCombat() || !IsTributeSpared(run, guard) || guard->HasAura(SpellIceLock) ||
                guard->GetExactDist2d(leader) > 90.0f || std::fabs(guard->GetPositionZ() - leader->GetPositionZ()) > 15.0f)
                continue;
            float nearYards = toWay(guard->GetPositionX(), guard->GetPositionY());
            if (!guard->movespline->Finalized())
            {
                G3D::Vector3 const end = guard->movespline->FinalDestination();
                nearYards = std::min(nearYards, toWay(end.x, end.y));
            }
            if (nearYards >= 25.0f)
                continue;
            if (!run.tributeWaitSince)
            {
                run.tributeWaitSince = now;
                LOG_INFO(ActivityLog, "activity: {}'s group waits for {} to walk past its way in {} ({:.0f} yd from it)", bot.name,
                    guard->GetName(), plan.name, nearYards);
            }
            if (getMSTimeDiff(run.tributeWaitSince, now) < 3 * MINUTE * IN_MILLISECONDS)
            {
                waitOffRound(guard, Acore::StringFormat("waiting for {} to walk past", guard->GetName()));
                return;
            }
        }
        walkTo(x, y, z, why);
    };

    switch (run.tribute)
    {
        case StepKey:
        {
            GameObject* door = FindSpawnedObject(leader, GoCourtyardDoor);
            if (Holding(group, ItemCourtyardKey) || DoorOpen(door))
            {
                advance(StepCourtyard);
                return true;
            }
            GameObject* chest = FindSpawnedObject(leader, GoFengusChest);
            if (!chest || !chest->isSpawned() || run.tributeTries >= 3)
            {
                // (The chest was looted, or gave no key three times: the door rule opens the courtyard door then, as in a
                // normal run.)
                LOG_INFO(ActivityLog, "activity: {}'s group gets no Gordok Courtyard Key out of Fengus's Chest in {} ({}): the door is "
                    "opened as in a normal run", bot.name, plan.name, !chest || !chest->isSpawned() ? "it is not there" : "three tries");
                advance(StepTrap);
                if (door)
                    door->UseDoorOrButton(0, false, leader);
                return true;
            }
            // Not while Fengus walks near his chest: 30 yd, waited out from 45 yd.
            if (Creature* fengus = patrolNear(NpcFengus, chest->GetPositionX(), chest->GetPositionY(), 30.0f))
            {
                if (leader->GetExactDist2d(chest) < 45.0f)
                {
                    if (!run.tributeWaitSince)
                    {
                        run.tributeWaitSince = now;
                        LOG_INFO(ActivityLog, "activity: {}'s group waits for {} to walk away from his chest in {} ({:.0f} yd from it)",
                            bot.name, fengus->GetName(), plan.name, fengus->GetExactDist2d(chest));
                    }
                    if (getMSTimeDiff(run.tributeWaitSince, now) < 3 * MINUTE * IN_MILLISECONDS)
                    {
                        waitOffRound(fengus, "waiting for Guard Fengus to walk away from his chest");
                        return true;
                    }
                }
            }
            else
                run.tributeWaitSince = 0;
            if (!chest->IsAtInteractDistance(leader))
            {
                goTo(chest->GetPositionX(), chest->GetPositionY(), chest->GetPositionZ(), "to Fengus's Chest for the courtyard key");
                return true;
            }
            leader->StopMoving();
            std::string const how = QuestInteraction::UseGameObject(leader, chest);
            uint32 const taken = QuestInteraction::TakeObjectLoot(leader, chest, ItemCourtyardKey);
            ++run.tributeTries;
            LOG_INFO(ActivityLog, "activity: {} opens Fengus's Chest in {} for the Gordok Courtyard Key: {} ({} taken, {})", bot.name,
                plan.name, leader->HasItemCount(ItemCourtyardKey, 1) ? "it has the key" : "no key", taken, how);
            return true;
        }
        case StepCourtyard:
        {
            GameObject* door = FindSpawnedObject(leader, GoCourtyardDoor);
            if (!door || DoorOpen(door))
            {
                advance(StepTrap);
                return true;
            }
            if (leader->GetExactDist(door) > 8.0f)
            {
                goTo(door->GetPositionX(), door->GetPositionY(), door->GetPositionZ(), "to the courtyard door");
                return true;
            }
            Player* holder = Holding(group, ItemCourtyardKey);
            door->UseDoorOrButton(0, false, leader);
            LOG_INFO(ActivityLog, "activity: {}'s group opens the Gordok Courtyard Door in {} with {}", bot.name, plan.name,
                holder ? Acore::StringFormat("{}'s Gordok Courtyard Key", holder->GetName()) : std::string("no key (as in a normal run)"));
            advance(StepTrap);
            return true;
        }
        case StepTrap:
        {
            Creature* slipkik = FindSpawned(leader, NpcSlipkik);
            if (!slipkik || !IsTributeSpared(run, slipkik) || slipkik->HasAura(SpellIceLock) || run.tributeTrapAt)
            {
                advance(StepTannin);
                return true;
            }
            GameObject* trap = FindSpawnedObject(leader, GoBrokenTrap);
            if (!trap || !trap->isSpawned())
            {
                // Repaired already (the broken trap is gone while the fixed one stands): Slip'kik is waited for below.
                run.tributeTrapAt = now;
                advance(StepTannin);
                return true;
            }
            std::string const missing = TakeMaterials(leader, guild.ownerAccount, TrapMaterials, std::size(TrapMaterials));
            if (!missing.empty())
            {
                fightAfterAll(NpcSlipkik, Acore::StringFormat("the trap cannot be repaired, the guild storage lacks {}", missing));
                advance(StepTannin);
                return true;
            }
            // Not while Slip'kik walks near the trap.
            if (Creature* walker = patrolNear(NpcSlipkik, trap->GetPositionX(), trap->GetPositionY(), 22.0f))
            {
                if (leader->GetExactDist2d(trap) < 38.0f)
                {
                    if (!run.tributeWaitSince)
                        run.tributeWaitSince = now;
                    if (getMSTimeDiff(run.tributeWaitSince, now) < 3 * MINUTE * IN_MILLISECONDS)
                    {
                        waitOffRound(walker, Acore::StringFormat("waiting for {} to walk away from the trap", walker->GetName()));
                        return true;
                    }
                }
            }
            if (leader->GetExactDist(trap) > 5.0f)
            {
                goTo(trap->GetPositionX(), trap->GetPositionY(), trap->GetPositionZ(), "to the Broken Trap");
                return true;
            }
            Quest const* quest = sObjectMgr->GetQuestTemplate(QuestTrap);
            if (!quest)
            {
                fightAfterAll(NpcSlipkik, "the trap's quest is not known");
                advance(StepTannin);
                return true;
            }
            leader->StopMoving();
            if (leader->GetQuestStatus(QuestTrap) == QUEST_STATUS_NONE || leader->GetQuestStatus(QuestTrap) == QUEST_STATUS_REWARDED)
                leader->AddQuestAndCheckCompletion(quest, trap);
            bool const done = leader->GetQuestStatus(QuestTrap) == QUEST_STATUS_COMPLETE && HandInAt(leader, quest, trap);
            if (!done)
            {
                if (++run.tributeTries >= 3)
                {
                    fightAfterAll(NpcSlipkik, "the trap's quest could not be done");
                    advance(StepTannin);
                }
                return true;
            }
            run.tributeTrapAt = now;
            LOG_INFO(ActivityLog, "activity: {} repairs the Broken Trap in {} with Frost Oil and a Thorium Widget of the guild storage: "
                "Guard Slip'kik freezes when he walks into it", bot.name, plan.name);
            advance(StepTannin);
            return true;
        }
        case StepTannin:
        {
            // The trap is repaired: off Slip'kik's round until he freezes in it (at most six minutes).
            if (Creature* slipkik = FindSpawned(leader, NpcSlipkik); run.tributeTrapAt && slipkik && IsTributeSpared(run, slipkik) &&
                !slipkik->HasAura(SpellIceLock) && getMSTimeDiff(run.tributeTrapAt, now) < FreezeWaitMs)
            {
                waitOffRound(slipkik, "waiting for Guard Slip'kik to walk into the trap");
                return true;
            }
            Player* maker = ObjectAccessor::FindPlayer(run.tributeMaker);
            if (leader->HasItemCount(ItemSuit, 1) || leader->HasAura(SpellSuit) || Holding(group, ItemSuit) ||
                (maker && maker->HasItemCount(ItemTannin, 1)) || run.tributeKill.count(NpcKromcrush))
            {
                advance(StepSuit);
                return true;
            }
            if (!maker || !maker->IsAlive() || maker->GetMapId() != leader->GetMapId())
            {
                fightAfterAll(NpcKromcrush, run.tributeMaker.IsEmpty() ?
                    std::string("every member of the group had Knot's ogre suit quest before (once per character)") :
                    std::string("the member that would make the ogre suit is not there"));
                advance(StepSuit);
                return true;
            }
            GameObject* basket = FindSpawnedObject(leader, GoTanninBasket);
            if (!basket || !basket->isSpawned() || run.tributeTries >= 3)
            {
                fightAfterAll(NpcKromcrush, "no Ogre Tannin for the suit");
                advance(StepSuit);
                return true;
            }
            if (leader->GetExactDist(basket) > 10.0f)
            {
                goTo(basket->GetPositionX(), basket->GetPositionY(), basket->GetPositionZ(), "to the Ogre Tannin Basket");
                return true;
            }
            // The tannin binds when it is picked up: the member that makes the suit opens the basket itself.
            if (!basket->IsAtInteractDistance(maker))
            {
                stand(Acore::StringFormat("{} goes to the Ogre Tannin Basket", maker->GetName()));
                bring(maker, basket->GetPositionX(), basket->GetPositionY(), basket->GetPositionZ());
                return true;
            }
            maker->StopMoving();
            // The tannin drops only for a character on Knot's quest: the maker takes it first (as from Knot, who stands
            // 80 yd from the basket; the sixth test run's leader had it in its log from a failed try and got the tannin,
            // the eighth run's maker had not and got none).
            if (Quest const* quest = sObjectMgr->GetQuestTemplate(QuestSuit); quest && maker->GetQuestStatus(QuestSuit) == QUEST_STATUS_NONE)
                maker->AddQuestAndCheckCompletion(quest, FindSpawned(leader, NpcKnot));
            std::string const how = QuestInteraction::UseGameObject(maker, basket);
            QuestInteraction::TakeObjectLoot(maker, basket, ItemTannin);
            ++run.tributeTries;
            LOG_INFO(ActivityLog, "activity: {} opens the Ogre Tannin Basket in {} for {}'s group: {} ({}; a Gordok Bushwhacker comes out of it)",
                maker->GetName(), plan.name, bot.name, maker->HasItemCount(ItemTannin, 1) ? "it has the Ogre Tannin" : "no tannin", how);
            return true;
        }
        case StepSuit:
        {
            if (leader->HasItemCount(ItemSuit, 1) || leader->HasAura(SpellSuit) || run.tributeKill.count(NpcKromcrush))
            {
                advance(StepInner);
                return true;
            }
            // A suit another member made (or carries from before) goes to the leader: it binds to nobody, as in a trade.
            if (Player* holder = Holding(group, ItemSuit); holder && holder != leader)
            {
                ItemPosCountVec dest;
                if (leader->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, ItemSuit, 1) == EQUIP_ERR_OK)
                {
                    holder->DestroyItemCount(ItemSuit, 1, true);
                    if (Item* item = leader->StoreNewItem(dest, ItemSuit, true))
                        leader->SendNewItem(item, 1, true, false);
                    LOG_INFO(ActivityLog, "activity: {} hands the Gordok Ogre Suit to {} in {}", holder->GetName(), bot.name, plan.name);
                }
                else
                    fightAfterAll(NpcKromcrush, "no room in the leader's bags for the ogre suit");
                advance(StepInner);
                return true;
            }
            Player* maker = ObjectAccessor::FindPlayer(run.tributeMaker);
            if (!maker || !maker->IsAlive() || maker->GetMapId() != leader->GetMapId())
            {
                fightAfterAll(NpcKromcrush, "the member that would make the ogre suit is not there");
                advance(StepInner);
                return true;
            }
            std::string const missing = TakeMaterials(maker, guild.ownerAccount, SuitMaterials, std::size(SuitMaterials));
            if (!missing.empty() || !maker->HasItemCount(ItemTannin, 1))
            {
                fightAfterAll(NpcKromcrush, Acore::StringFormat("no Gordok Ogre Suit: {}", missing.empty() ? std::string("no Ogre Tannin") :
                    Acore::StringFormat("the guild storage lacks {}", missing)));
                advance(StepInner);
                return true;
            }
            Creature* knot = FindSpawned(leader, NpcKnot);
            Quest const* quest = sObjectMgr->GetQuestTemplate(QuestSuit);
            if (!knot || !quest)
            {
                fightAfterAll(NpcKromcrush, "Knot Thimblejack is not there");
                advance(StepInner);
                return true;
            }
            if (leader->GetExactDist(knot) > 10.0f)
            {
                goTo(knot->GetPositionX(), knot->GetPositionY(), knot->GetPositionZ(), "to Knot Thimblejack for the ogre suit");
                return true;
            }
            if (maker->GetExactDist(knot) > 5.0f)
            {
                stand(Acore::StringFormat("{} goes to Knot Thimblejack for the ogre suit", maker->GetName()));
                bring(maker, knot->GetPositionX(), knot->GetPositionY(), knot->GetPositionZ());
                return true;
            }
            maker->StopMoving();
            if (maker->GetQuestStatus(QuestSuit) == QUEST_STATUS_NONE)
                maker->AddQuestAndCheckCompletion(quest, knot);
            bool const done = maker->GetQuestStatus(QuestSuit) == QUEST_STATUS_COMPLETE && HandInAt(maker, quest, knot);
            if (!done || !maker->HasItemCount(ItemSuit, 1))
            {
                if (++run.tributeTries >= 3)
                {
                    fightAfterAll(NpcKromcrush, Acore::StringFormat("Knot's suit quest could not be done for {} (quest status {})",
                        maker->GetName(), uint32(maker->GetQuestStatus(QuestSuit))));
                    advance(StepInner);
                }
                return true;
            }
            LOG_INFO(ActivityLog, "activity: {} gets the Gordok Ogre Suit from Knot Thimblejack in {} (the guild storage's runecloth, leather "
                "and thread, and the Ogre Tannin)", maker->GetName(), plan.name);
            return true;
        }
        case StepInner:
        {
            // Slip'kik walks his round into the repaired trap: waited for, at most six minutes, where Knot stands.
            Creature* slipkik = FindSpawned(leader, NpcSlipkik);
            if (run.tributeTrapAt && slipkik && IsTributeSpared(run, slipkik) && !slipkik->HasAura(SpellIceLock))
            {
                if (getMSTimeDiff(run.tributeTrapAt, now) < FreezeWaitMs)
                {
                    if (Creature* knot = FindSpawned(leader, NpcKnot); knot && leader->GetExactDist(knot) > 8.0f)
                        goTo(knot->GetPositionX(), knot->GetPositionY(), knot->GetPositionZ(), "waiting for Guard Slip'kik to walk into the trap");
                    else
                        stand("waiting for Guard Slip'kik to walk into the trap");
                    return true;
                }
                fightAfterAll(NpcSlipkik, "he did not walk into the trap in six minutes");
                return true;
            }
            GameObject* door = FindSpawnedObject(leader, GoInnerDoor);
            if (!door || DoorOpen(door))
            {
                advance(StepKromcrush);
                return true;
            }
            if (leader->GetExactDist(door) > 8.0f)
            {
                goTo(door->GetPositionX(), door->GetPositionY(), door->GetPositionZ(), "to the inner door");
                return true;
            }
            Player* opener = ObjectAccessor::FindPlayer(run.tributeOpener);
            std::string how;
            switch (run.tributeDoor)
            {
                case DoorLockpick:
                    if (opener && opener->IsAlive() && opener->GetMapId() == leader->GetMapId())
                        how = Acore::StringFormat("{} picks its lock (Lockpicking {})", opener->GetName(), opener->GetSkillValue(SKILL_LOCKPICKING));
                    break;
                case DoorCharge:
                case DoorSkeletonKey:
                {
                    uint32 const item = run.tributeDoor == DoorCharge ? ItemCharge : ItemSkeletonKey;
                    if (!opener || !opener->IsAlive() || opener->GetMapId() != leader->GetMapId())
                        break;
                    std::string error;
                    if (opener->HasItemCount(item, 1) || sStorageService.Withdraw(opener, item, 0, 1, error))
                    {
                        opener->DestroyItemCount(item, 1, true);
                        how = Acore::StringFormat("{} uses a {} of the guild storage on it", opener->GetName(), ItemName(item));
                    }
                    break;
                }
                default:
                    if (Player* holder = Holding(group, ItemInnerKey))
                        how = Acore::StringFormat("{} unlocks it with Guard Mol'dar's Gordok Inner Door Key", holder->GetName());
                    break;
            }
            if (how.empty())
            {
                // (The means is gone: the opener died or left, the storage was emptied meanwhile. The door is opened as in a
                // normal run; nothing is lost but the honesty of one door.)
                how = "nobody can open it as planned, so it is opened as in a normal run";
            }
            door->UseDoorOrButton(0, false, leader);
            LOG_INFO(ActivityLog, "activity: {}'s group opens the Gordok Inner Door in {}: {}", bot.name, plan.name, how);
            advance(StepKromcrush);
            return true;
        }
        case StepKromcrush:
        {
            Creature* kromcrush = FindSpawned(leader, NpcKromcrush);
            if (!kromcrush || !IsTributeSpared(run, kromcrush))
            {
                advance(StepThrone);
                return false;
            }
            // Fooled: he walks off (his script: passive, immune) and comes back after a long round.
            if (kromcrush->HasUnitFlag(UNIT_FLAG_IMMUNE_TO_PC))
            {
                if (!run.tributeWaitSince)
                    run.tributeWaitSince = now;
                if (getMSTimeDiff(run.tributeWaitSince, now) < 8 * IN_MILLISECONDS && kromcrush->GetExactDist(kromcrush->GetHomePosition()) < 15.0f)
                {
                    run.tributeHoldUntil = now + 3 * IN_MILLISECONDS;
                    stand("Captain Kromcrush storms off");
                    return true;
                }
                if (leader->HasAura(SpellSuit))
                    leader->RemoveAurasDueToSpell(SpellSuit);
                advance(StepThrone);
                return false;
            }
            if (!leader->HasAura(SpellSuit))
            {
                Item* suit = leader->GetItemByEntry(ItemSuit);
                if (!suit || run.tributeTries >= 3)
                {
                    fightAfterAll(NpcKromcrush, "nobody wears the Gordok Ogre Suit");
                    advance(StepThrone);
                    return false;
                }
                // Into the suit within 70 yd of him, and from there straight to him, no pulls on the way: the trash of his
                // room is of his side, and a fight beside him brings him (the third full test: he joined a pull of his
                // room's Doomguards before the leader got within 30 yd, 2026-11-21).
                if (leader->GetExactDist(kromcrush) > 70.0f)
                {
                    goTo(kromcrush->GetPositionX(), kromcrush->GetPositionY(), kromcrush->GetPositionZ(), "to Captain Kromcrush's room");
                    return true;
                }
                leader->StopMoving();
                SpellCastTargets targets;
                targets.SetUnitTarget(leader);
                leader->CastItemUseSpell(suit, targets, 1, 0);
                ++run.tributeTries;
                LOG_INFO(ActivityLog, "activity: {} puts on the Gordok Ogre Suit in {} to talk to Captain Kromcrush", bot.name, plan.name);
                return true;
            }
            run.tributeHoldUntil = now + 3 * IN_MILLISECONDS;
            if (leader->GetExactDist(kromcrush) > 5.0f)
            {
                rt.why = Acore::StringFormat("In {}: tribute run: to Captain Kromcrush in the ogre suit", plan.name);
                PauseStuck(rt);
                run.bossStuckSince = now;
                if (getMSTimeDiff(rt.task.lastAssert, now) >= 2 * IN_MILLISECONDS || !leader->isMoving())
                {
                    if (leader->GetExactDist(kromcrush) < 15.0f)
                    {
                        if (BotDriver::GetRpgState(leader) == RpgState::GoCamp)
                            BotDriver::Rest(leader);
                        leader->GetMotionMaster()->MovePoint(0, kromcrush->GetPositionX(), kromcrush->GetPositionY(), kromcrush->GetPositionZ());
                    }
                    else
                        BotDriver::GoTo(leader, plan.mapId, kromcrush->GetPositionX(), kromcrush->GetPositionY(), kromcrush->GetPositionZ(), false);
                    rt.task.lastAssert = now;
                }
                return true;
            }
            leader->StopMoving();
            if (kromcrush->AI())
                kromcrush->AI()->sGossipSelect(leader, MenuKromcrush, 0);
            run.tributeWaitSince = now;
            LOG_INFO(ActivityLog, "activity: {} in the Gordok Ogre Suit tells Captain Kromcrush in {} that Guard Fengus called him a fat, "
                "useless gnoll lover: he storms off", bot.name, plan.name);
            return true;
        }
        default:
            return false;
    }
}

bool OrderService::StepTributeEnd(AdventureGuild& guild, GuildBot& bot, Player* leader, BotRuntime& rt, DungeonPlan const& plan,
    std::vector<Player*> const& group, Player* guest)
{
    BotRuntime::DungeonRun& run = rt.dungeon;
    if (!run.tribute || run.tribute >= StepDone)
        return false;
    if (leader->IsInCombat() || !leader->IsAlive())
        return true;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    if (run.tribute < StepMizzle)
    {
        run.tribute = StepMizzle;
        run.tributeSince = now;
        run.tributeWaitSince = 0;
        run.tributeTries = 0;
    }
    auto advance = [&](uint8 step)
    {
        run.tribute = step;
        run.tributeSince = now;
        run.tributeTries = 0;
        run.tributeWaitSince = 0;
    };
    auto walkTo = [&](float x, float y, float z, std::string const& why)
    {
        rt.why = Acore::StringFormat("In {}: tribute run: {}", plan.name, why);
        PauseStuck(rt);
        run.bossStuckSince = now;
        if (leader->GetExactDist(x, y, z) < 15.0f)
        {
            if (getMSTimeDiff(rt.task.lastAssert, now) >= 2 * IN_MILLISECONDS || !leader->isMoving())
            {
                if (BotDriver::GetRpgState(leader) == RpgState::GoCamp)
                    BotDriver::Rest(leader);
                leader->GetMotionMaster()->MovePoint(0, x, y, z);
                rt.task.lastAssert = now;
            }
            return;
        }
        // (The way with its look ahead: the Gordok Mastiffs are no ogres and stay hostile to the new king; the walk to
        // the guards' buffs met them unpulled, and a fight beside Fengus drew him in, 2026-11-21.)
        // A leader that has not got on for half a minute out of a fight walks the core's path itself for a while (the
        // eighth test run stood 20 minutes 105 yd from the Ogre Tannin Basket with a whole path to it, 2026-11-21).
        if (leader->IsInCombat() || leader->GetExactDist2d(run.tributeStillX, run.tributeStillY) > 3.0f)
        {
            run.tributeStillX = leader->GetPositionX();
            run.tributeStillY = leader->GetPositionY();
            run.tributeStillSince = now;
        }
        else if (getMSTimeDiff(run.tributeStillSince, now) >= 30 * IN_MILLISECONDS && now >= run.tributeDirectUntil)
        {
            run.tributeDirectUntil = now + 25 * IN_MILLISECONDS;
            run.tributeStillSince = now;
            LOG_INFO(ActivityLog, "activity: {} has not got on for 30 s in {} on its tribute run ({}): it walks the core's path there itself "
                "for a while", bot.name, plan.name, why);
        }
        if (now < run.tributeDirectUntil)
        {
            if (getMSTimeDiff(rt.task.lastAssert, now) >= 2 * IN_MILLISECONDS || !leader->isMoving())
            {
                if (BotDriver::GetRpgState(leader) == RpgState::GoCamp)
                    BotDriver::Rest(leader);
                leader->GetMotionMaster()->MovePoint(0, x, y, z);
                rt.task.lastAssert = now;
            }
            return;
        }
        float wayX = x, wayY = y, wayZ = z;
        if (StepDungeonWay(guild, bot, leader, rt, plan, group, guest, x, y, z, wayX, wayY, wayZ))
            return;
        if (BotDriver::GetRpgState(leader) != RpgState::GoCamp || getMSTimeDiff(rt.task.lastAssert, now) >= AssertMs ||
            std::fabs(wayX - run.wayX) + std::fabs(wayY - run.wayY) > 6.0f)
        {
            BotDriver::GoTo(leader, plan.mapId, wayX, wayY, wayZ, false);
            rt.task.lastAssert = now;
            run.wayX = wayX;
            run.wayY = wayY;
        }
    };
    std::vector<Player*> everyone = group;
    if (guest && guest->GetMapId() == leader->GetMapId())
        everyone.push_back(guest);

    switch (run.tribute)
    {
        case StepMizzle:
        {
            Creature* mizzle = leader->FindNearestCreature(NpcMizzle, 250.0f, true);
            if (!mizzle || !mizzle->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP))
            {
                if (getMSTimeDiff(run.tributeSince, now) > 3 * MINUTE * IN_MILLISECONDS)
                {
                    LOG_INFO(ActivityLog, "activity: {}'s group sees no Mizzle the Crafty in {} three minutes after King Gordok: no tribute",
                        bot.name, plan.name);
                    advance(StepBuffs);
                    return true;
                }
                rt.why = Acore::StringFormat("In {}: tribute run: waiting for Mizzle the Crafty", plan.name);
                PauseStuck(rt);
                run.bossStuckSince = now;
                return true;
            }
            if (leader->GetExactDist(mizzle) > 5.0f)
            {
                walkTo(mizzle->GetPositionX(), mizzle->GetPositionY(), mizzle->GetPositionZ(), "to Mizzle the Crafty");
                return true;
            }
            leader->StopMoving();
            // Every member becomes King of the Gordok (the ogres are friendly then), the leader asks for the tribute.
            uint32 crowned = 0;
            for (Player* member : everyone)
                if (member->IsAlive() && member->GetExactDist(mizzle) <= 40.0f && !member->HasAura(SpellKing) && mizzle->AI())
                {
                    mizzle->AI()->sGossipSelect(member, MenuMizzleKing, 0);
                    ++crowned;
                }
            if (mizzle->AI())
                mizzle->AI()->sGossipSelect(leader, MenuMizzleTribute, 0);
            uint32 alive = 0;
            std::string living;
            for (uint32 entry : Spared)
                if (FindSpawned(leader, entry))
                {
                    ++alive;
                    living += (living.empty() ? "" : ", ") + CreatureName(entry);
                }
            LOG_INFO(ActivityLog, "activity: Mizzle the Crafty crowns {} of {}'s group King of the Gordok in {} and sets up the Gordok Tribute: "
                "{} of 6 named ogres live ({})", crowned, bot.name, plan.name, alive, living.empty() ? std::string("none") : living);
            advance(StepChest);
            return true;
        }
        case StepChest:
        {
            GameObject* chest = leader->FindNearestGameObject(GoTribute, 80.0f);
            if (!chest || !chest->isSpawned() || run.tributeTries >= 2)
            {
                if (getMSTimeDiff(run.tributeSince, now) < 20 * IN_MILLISECONDS && run.tributeTries < 2)
                    return true;
                advance(StepBuffs);
                return true;
            }
            if (!chest->IsAtInteractDistance(leader))
            {
                walkTo(chest->GetPositionX(), chest->GetPositionY(), chest->GetPositionZ(), "to the Gordok Tribute");
                return true;
            }
            leader->StopMoving();
            std::string const how = QuestInteraction::UseGameObject(leader, chest);
            uint32 const taken = QuestInteraction::TakeObjectLoot(leader, chest, 0);
            ++run.tributeTries;
            LOG_INFO(ActivityLog, "activity: {} opens the Gordok Tribute in {}: {} item{} taken by it, the rest goes to the group's rolls ({})",
                bot.name, plan.name, taken, taken == 1 ? "" : "s", how);
            advance(StepBuffs);
            return true;
        }
        case StepBuffs:
        {
            while (run.tributeBuffAt < std::size(BuffGuards))
            {
                Guard const& guard = BuffGuards[run.tributeBuffAt];
                Creature* creature = FindSpawned(leader, guard.entry);
                bool anyNeeds = false;
                for (Player* member : everyone)
                    anyNeeds = anyNeeds || (member->IsAlive() && member->HasAura(SpellKing) && !member->HasAura(guard.buff));
                // Only a guard near the throne: the walks across the halls to Fengus and Mol'dar brought them down on the
                // group although its members wore the crown (the sixth test run, 2026-11-21: both killed, two members dead).
                if (!creature || creature->IsInCombat() || !anyNeeds || leader->GetExactDist(creature) > BuffYards ||
                    getMSTimeDiff(run.tributeSince, now) > 4 * MINUTE * IN_MILLISECONDS)
                {
                    ++run.tributeBuffAt;
                    run.tributeSince = now;
                    continue;
                }
                if (leader->GetExactDist(creature) > 5.0f)
                {
                    walkTo(creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ(),
                        Acore::StringFormat("to {} for his buff", creature->GetName()));
                    return true;
                }
                leader->StopMoving();
                // Each member that stands at him (12 yd); the others come along, 20 s at most.
                if (!run.tributeWaitSince)
                    run.tributeWaitSince = now;
                uint32 waiting = 0;
                for (Player* member : everyone)
                    if (member->IsAlive() && member->HasAura(SpellKing) && !member->HasAura(guard.buff) && creature->AI())
                    {
                        if (member->GetExactDist(creature) <= 12.0f)
                        {
                            creature->AI()->sGossipSelect(member, guard.menu, 0);
                            ++run.tributeTries;
                        }
                        else
                            ++waiting;
                    }
                if (waiting && getMSTimeDiff(run.tributeWaitSince, now) < 20 * IN_MILLISECONDS)
                {
                    rt.why = Acore::StringFormat("In {}: tribute run: {}'s buff for the group", plan.name, creature->GetName());
                    PauseStuck(rt);
                    run.bossStuckSince = now;
                    return true;
                }
                LOG_INFO(ActivityLog, "activity: {} gives his buff to {} of {}'s group in {}", creature->GetName(), run.tributeTries, bot.name, plan.name);
                ++run.tributeBuffAt;
                run.tributeSince = now;
                run.tributeTries = 0;
                run.tributeWaitSince = 0;
                return true;
            }
            advance(StepDone);
            return false;
        }
        default:
            return false;
    }
}
