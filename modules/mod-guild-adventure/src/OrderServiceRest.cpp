/*
 * OrderService, rest and fights (docs/orders.md "Rest and provisions", "Activity log"):
 *   - StepRest / EndRest: an AI-played guild character that is out of combat with little health or mana stops, eats and
 *     drinks what it carries and goes on when it has recovered. mod-playerbots alone eats below 45 % health only, and
 *     its "attack anything" action looks at neither health nor mana.
 *   - TrackFight / EndFight: one activity log line per fight (foes at once, health and mana at its start and end) and
 *     the counters of the status line.
 *
 * World thread only, called from OrderService::UpdateBot once a second.
 */

#include "BotSettings.h"
#include "DungeonRoles.h"
#include "OrderService.h"
#include "TemporarySummon.h"
#include "Bag.h"
#include "CombatManager.h"
#include "Creature.h"
#include "FightEscape.h"
#include "Group.h"
#include "MotionMaster.h"
#include "GuildAdventureMgr.h"
#include "GuildAdventureSettings.h"
#include "Item.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Pet.h"
#include "Player.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "RoadMap.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StorageSupply.h"
#include "StringFormat.h"
#include "Timer.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <vector>

using namespace GuildAdventure;

namespace
{
    // A bot walks to a quieter place for its rest for at most this long.
    constexpr uint32 RestWalkMaxMs = 12 * IN_MILLISECONDS;
    // A wet bot looks for dry land up to this far away (for its rest, or to breathe: GuildAdventure.Rest.AirAfterSeconds).
    constexpr float AirSearchYards = 70.0f;
    // A rest is due and the bot is in the water: dry ground is looked for up to this far (rings at 60, 100 and 150
    // yards after the near ones), every ShoreLookMs while none is found.
    constexpr float ShoreSearchYards = 150.0f;
    constexpr uint32 ShoreLookMs = 5 * IN_MILLISECONDS;
    constexpr uint32 ShoreNoteMs = 2 * MINUTE * IN_MILLISECONDS;
    constexpr float SwimYardsPerSecond = 4.0f;
    // The planner's tick (OrderService::Update): a rest step runs once per tick.
    constexpr uint32 TickMs = 1000;
    constexpr char const* ActivityLog = "module.ga.activity";
    // Spell categories of food and drink items (the same playerbots' eat and drink actions look for).
    constexpr uint32 FoodSpellCategory = 11;
    constexpr uint32 DrinkSpellCategory = 59;
    // After taking food or drink the next one is not tried before this time (its aura is on by then).
    constexpr uint32 RestUseGapMs = 3 * IN_MILLISECONDS;

    // The best food or drink in the bags (highest required level the bot can use), nullptr if it carries none.
    Item* FindConsumable(Player* player, uint32 spellCategory)
    {
        Item* best = nullptr;
        auto consider = [&](Item* item)
        {
            ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
            if (!proto || proto->Spells[0].SpellCategory != spellCategory || !proto->Spells[0].SpellId ||
                proto->RequiredLevel > player->GetLevel() || player->CanUseItem(item) != EQUIP_ERR_OK)
                return;
            if (!best || proto->RequiredLevel > best->GetTemplate()->RequiredLevel)
                best = item;
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            consider(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = player->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    consider(bag->GetItemByPos(uint8(slot)));
        return best;
    }

    // Uses an item from the bags on the bot itself, the way a client does (CMSG_USE_ITEM), so the core runs its own
    // checks (cooldown, stand state, combat).
    // onSelf: with the bot as the spell's target (a bandage). castCount 0: the core lets such a use through while
    // another cast runs, which it ends (a healing potion at low health must not wait for a Frostbolt).
    void UseOnSelf(Player* player, Item* item, bool onSelf = false, uint8 castCount = 1)
    {
        WorldPacket packet(CMSG_USE_ITEM, 1 + 1 + 1 + 4 + 8 + 4 + 1 + 4 + 9);
        packet << uint8(item->GetBagSlot()) << uint8(item->GetSlot()) << castCount;
        packet << uint32(item->GetTemplate()->Spells[0].SpellId) << item->GetGUID() << uint32(0) << uint8(0);
        if (onSelf)
            packet << uint32(TARGET_FLAG_UNIT) << player->GetPackGUID();
        else
            packet << uint32(TARGET_FLAG_NONE);
        player->GetSession()->HandleUseItemOpcode(packet);
    }

    // Pieces of food or drink in the bags the character can use at its level.
    uint32 CountUsable(Player const* player, uint32 spellCategory)
    {
        uint32 count = 0;
        auto add = [&](Item const* item)
        {
            ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
            if (proto && proto->Spells[0].SpellCategory == spellCategory && proto->RequiredLevel <= player->GetLevel())
                count += item->GetCount();
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            add(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag const* bag = player->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    add(bag->GetItemByPos(uint8(slot)));
        return count;
    }

    // True while the bot sits over food (SPELL_AURA_MOD_REGEN) or drink (SPELL_AURA_MOD_POWER_REGEN): a regeneration
    // aura that ends when it stands up.
    bool IsTaking(Player const* player, AuraType type)
    {
        for (AuraEffect const* effect : player->GetAuraEffectsByType(type))
            if (effect->GetSpellInfo()->AuraInterruptFlags & AURA_INTERRUPT_FLAG_NOT_SEATED)
                return true;
        return false;
    }

    // The forms a character travels in and cannot fight in; nullptr for every other form.
    char const* FormName(uint8 form)
    {
        switch (form)
        {
            case FORM_AQUA: return "aquatic form";
            case FORM_TRAVEL: return "travel form";
            case FORM_GHOSTWOLF: return "ghost wolf";
            case FORM_FLIGHT:
            case FORM_FLIGHT_EPIC: return "flight form";
            default: return nullptr;
        }
    }

    // The highest rank the character knows of a spell, given its first rank (0 = none).
    uint32 KnownRank(Player const* player, uint32 firstRank)
    {
        for (uint32 spell = sSpellMgr->GetLastSpellInChain(firstRank); spell; spell = sSpellMgr->GetPrevSpellInChain(spell))
        {
            if (player->HasSpell(spell))
                return spell;
            if (spell == firstRank)
                break;
        }
        return player->HasSpell(firstRank) ? firstRank : 0;
    }

    // The spell a character heals itself with (0 = none, or not now: a priest in Shadowform, a druid in a form).
    // quick: in a fight the shorter cast comes first where there is one.
    uint32 SelfHealSpell(Player const* player, bool quick)
    {
        // Paladin: Flash of Light, Holy Light. Priest: Flash Heal, Greater Heal, Heal, Lesser Heal. Shaman: Lesser
        // Healing Wave, Healing Wave. Druid: Regrowth, Healing Touch.
        std::vector<uint32> spells;
        switch (player->getClass())
        {
            case CLASS_PALADIN:
            {
                // the owner, 2026-11-14: [quote left out]. In a fight: Holy Light, and Flash of Light when
                // The Art of War makes it instant; out of a fight: Flash of Light for the last quarter of the health,
                // Holy Light for more.
                constexpr uint32 ArtOfWarProc[] = { 53489, 59578 };
                bool const proc = player->HasAura(ArtOfWarProc[0]) || player->HasAura(ArtOfWarProc[1]);
                bool const flash = quick ? proc : player->GetHealthPct() >= 75.0f;
                spells = flash ? std::vector<uint32>{ 19750, 635 } : std::vector<uint32>{ 635, 19750 };
                break;
            }
            case CLASS_PRIEST:
                if (player->GetShapeshiftForm() == FORM_SHADOW)
                    return 0;
                spells = { 2061, 2060, 2054, 2050 };
                break;
            case CLASS_SHAMAN:
                spells = quick ? std::vector<uint32>{ 8004, 331 } : std::vector<uint32>{ 331, 8004 };
                break;
            case CLASS_DRUID:
                if (player->GetShapeshiftForm() != FORM_NONE)
                    return 0;
                spells = { 8936, 5185 };
                break;
            default:
                return 0;
        }
        for (uint32 first : spells)
            if (uint32 const spell = KnownRank(player, first))
                return spell;
        return 0;
    }

    // Casts the heal on the character itself; returns the cast time in ms (0 = it did not go out: no mana, the global
    // cooldown, silenced).
    uint32 CastSelfHeal(Player* player, uint32 spell)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
        if (!info)
            return 0;
        player->StopMoving();
        if (player->CastSpell(player, spell, false) != SPELL_CAST_OK)
            return 0;
        return std::max<uint32>(info->CalcCastTime(player), 500);
    }

    uint8 HealthPercent(Player const* player)
    {
        return uint8(std::clamp(player->GetHealthPct(), 0.0f, 100.0f));
    }

    bool UsesMana(Player const* player)
    {
        return player->getPowerType() == POWER_MANA && player->GetMaxPower(POWER_MANA) > 0;
    }

    // 100 for a character that does not use mana (a warrior, a rogue, a druid in cat or bear form).
    uint8 ManaPercent(Player const* player)
    {
        if (!UsesMana(player))
            return 100;
        return uint8(uint64(player->GetPower(POWER_MANA)) * 100 / player->GetMaxPower(POWER_MANA));
    }
}

namespace
{
    // Why a creature cannot reach the one it fights: both places and what the core's path finder gives the creature
    // from its place to that one (its chase asks the same). The test guild's Shadowfang Keep group was held in a fight
    // for 14 minutes by a Shadowfang Whitescalp that "could not reach its target" 2 to 8 yd beside the group, where the
    // navmesh joins both places (2026-11-24); this names the cause the next time.
    std::string UnreachableDetail(Creature* creature)
    {
        Unit* target = ObjectAccessor::GetUnit(*creature, creature->GetCannotReachTarget());
        if (!target)
            return ", cannot reach its target";
        PathGenerator path(creature);
        path.CalculatePath(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(), false);
        uint32 const type = path.GetPathType();
        std::string kind;
        auto add = [&](uint32 flag, char const* name)
        {
            if (type & flag)
                kind += Acore::StringFormat("{}{}", kind.empty() ? "" : "+", name);
        };
        add(PATHFIND_NORMAL, "normal");
        add(PATHFIND_SHORTCUT, "shortcut");
        add(PATHFIND_INCOMPLETE, "incomplete");
        add(PATHFIND_NOPATH, "no path");
        add(PATHFIND_NOT_USING_PATH, "not using a path");
        add(PATHFIND_SHORT, "short");
        add(PATHFIND_FARFROMPOLY_START, "start far from the navmesh");
        add(PATHFIND_FARFROMPOLY_END, "end far from the navmesh");
        Movement::PointsArray const& points = path.GetPath();
        G3D::Vector3 const end = points.empty() ? G3D::Vector3(creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ()) :
            points.back();
        return Acore::StringFormat(", cannot reach {} at ({:.1f}, {:.1f}, {:.1f}) from ({:.1f}, {:.1f}, {:.1f}): its path there is {} with {} "
            "points, ending at ({:.1f}, {:.1f}, {:.1f}){}{}", target->GetName(), target->GetPositionX(), target->GetPositionY(),
            target->GetPositionZ(), creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ(),
            kind.empty() ? std::string("of no kind") : kind, points.size(), end.x, end.y, end.z,
            creature->IsWithinLOSInMap(target) ? "" : ", no line of sight", target->isInAccessiblePlaceFor(creature) ? "" :
                ", the target stands where the creature may not go");
    }

    // Who a unit is in combat with, for the line of a fight in which nothing happens: each creature with its level,
    // how far it is, what it does and whether the bot could do anything about it.
    std::string CombatHolders(Player* player, Unit* unit)
    {
        std::string text;
        uint32 count = 0;
        for (auto const& [guid, ref] : unit->GetCombatManager().GetPvECombatRefs())
        {
            Unit* other = ref->GetOther(unit);
            if (!other)
                continue;
            if (++count > 6)
                continue;
            Creature* creature = other->ToCreature();
            Unit const* victim = other->GetVictim();
            text += Acore::StringFormat("{}{} (level {}{}, {:.0f} yd away, {:.0f} yd higher, health {:.0f}%, {}{}{}{}{})", text.empty() ? "" : ", ",
                other->GetName(), uint32(other->GetLevel()), creature ? Acore::StringFormat(", entry {}", creature->GetEntry()) : std::string(),
                player->GetExactDist2d(other), other->GetPositionZ() - player->GetPositionZ(), other->GetHealthPct(),
                !other->IsAlive() ? "dead" : (victim ? Acore::StringFormat("on {}", victim->GetName()) : std::string("on nobody")),
                creature && creature->IsInEvadeMode() ? ", evading" : "",
                creature && creature->CanNotReachTarget() ? UnreachableDetail(creature) : std::string(),
                player->CanSeeOrDetect(other) ? "" : ", the bot cannot see it",
                ref->IsSuppressedFor(unit) ? ", suppressed" : "");
        }
        if (count > 6)
            text += Acore::StringFormat(" and {} more", count - 6);
        return text.empty() ? std::string("nothing") : text;
    }
}

void OrderService::TrackFight(Player* player, BotRuntime& rt)
{
    if (!player->IsAlive())
        return; // OnDied ends the fight

    uint8 const health = HealthPercent(player);
    if (!player->IsInCombat())
    {
        if (rt.fight.active)
        {
            EndFight(player, rt, false);
            Escape::BackOutAfterFight(player, rt);
        }
        rt.calmHealth = health;
        rt.calmMana = ManaPercent(player);
        return;
    }

    BotRuntime::Fight& fight = rt.fight;
    if (!fight.active)
    {
        fight = BotRuntime::Fight();
        fight.active = true;
        fight.startedAt = getMSTime();
        fight.healthStart = rt.calmHealth;
        fight.manaStart = rt.calmMana;
        fight.form = uint8(player->GetShapeshiftForm());
        fight.budgetStart = Escape::BudgetOf(player);
        // Its own pull (the pull check let the target pass a moment ago) or an attack on it.
        constexpr uint32 OwnPullMs = 10 * IN_MILLISECONDS;
        fight.expected = rt.pullOkAt && getMSTimeDiff(rt.pullOkAt, fight.startedAt) <= OwnPullMs ? std::max<uint8>(rt.pullOkFoes, 1) : 0;
        fight.possible = fight.expected ? rt.pullOkMaybe : 0;
    }
    fight.healthLow = std::min(fight.healthLow, health);

    // Who is on the bot right now (its own target counts when nothing has reached it yet).
    uint8 foes = 0;
    bool victimCounted = false;
    Unit const* victim = player->GetVictim();
    float unitsNow = 0.0f;
    auto count = [&](Unit const* unit)
    {
        Creature const* creature = unit ? unit->ToCreature() : nullptr;
        if (!creature || !creature->IsAlive() || creature->IsPet() || creature->IsTotem())
            return;
        ++foes;
        unitsNow += creature->GetHealthPct();
        if (std::find(fight.seen.begin(), fight.seen.end(), creature->GetGUID()) == fight.seen.end())
        {
            fight.seen.push_back(creature->GetGUID());
            fight.kinds.emplace_back(creature->GetEntry(), uint8(creature->GetLevel()));
            fight.unitsJoined += creature->GetHealthPct();
            if (!fight.firstKind)
                fight.firstKind = creature->GetEntry();
            else if (creature->GetEntry() != fight.firstKind)
                fight.oneKind = false;
            // Its own pull: who comes into the fight beyond what the pull check expected, from where and when (with
            // the debug setting every joiner): what the check's shares are measured against.
            // (Not what says nothing about the pull check: a creature twelve or more levels below the bot, which the
            // check leaves out on purpose; a creature that comes when the fight has moved on from where it began or
            // has lasted long. Of the first live round's 39 lines 12 were such.)
            constexpr uint32 JoinerMaxMs = 45 * IN_MILLISECONDS;
            constexpr float JoinerMaxYards = 45.0f;
            bool const ofThePull = creature->GetLevel() + 12 > player->GetLevel() &&
                getMSTimeDiff(fight.startedAt, getMSTime()) <= JoinerMaxMs &&
                std::hypot(player->GetPositionX() - rt.pullOkX, player->GetPositionY() - rt.pullOkY) <= JoinerMaxYards;
            if ((GetSettings().debug || (fight.seen.size() > fight.expected && ofThePull)) && fight.expected &&
                creature->GetGUID() != rt.pullOkTarget)
            {
                Position const& home = creature->GetHomePosition();
                LOG_INFO(ActivityLog, "activity: {} pull joiner {} of {} expected after {} s: {} (level {}), its home {:.0f} yd from "
                    "where the target stood, it wanders {:.0f} yd, now {:.0f} yd from there and {:.0f} yd from the bot, on {}; "
                    "the fight is {:.0f} yd from where the target stood",
                    rt.name, fight.seen.size(), uint32(fight.expected), getMSTimeDiff(fight.startedAt, getMSTime()) / IN_MILLISECONDS,
                    creature->GetName(), uint32(creature->GetLevel()),
                    std::hypot(home.GetPositionX() - rt.pullOkX, home.GetPositionY() - rt.pullOkY),
                    creature->GetDefaultMovementType() == RANDOM_MOTION_TYPE ? creature->GetWanderDistance() : 0.0f,
                    std::hypot(creature->GetPositionX() - rt.pullOkX, creature->GetPositionY() - rt.pullOkY),
                    player->GetExactDist(creature), creature->GetVictim() == player ? "the bot" : "its pet",
                    std::hypot(player->GetPositionX() - rt.pullOkX, player->GetPositionY() - rt.pullOkY));
            }
            // Attacked (no pull of its own), with the debug setting: where the bot stood and where each attacker came
            // from (its home, whether it was summoned, whether it walks a round), to tell a walk into a camp from a
            // patrol or a summoned minion that came to it.
            if (GetSettings().debug && !fight.expected)
            {
                Position const& home = creature->GetHomePosition();
                Unit const* owner = creature->IsSummon() ? creature->ToTempSummon()->GetSummonerUnit() : nullptr;
                LOG_INFO(ActivityLog, "activity: {} attacked, foe {} after {} s at ({:.0f}, {:.0f}){}: {} (level {}) {:.0f} yd from the bot, its "
                    "home {:.0f} yd from the bot{}{}", rt.name, fight.seen.size(), getMSTimeDiff(fight.startedAt, getMSTime()) / IN_MILLISECONDS,
                    player->GetPositionX(), player->GetPositionY(), player->IsMounted() ? ", riding" : "", creature->GetName(),
                    uint32(creature->GetLevel()), player->GetExactDist(creature),
                    std::hypot(home.GetPositionX() - player->GetPositionX(), home.GetPositionY() - player->GetPositionY()),
                    creature->GetDefaultMovementType() == WAYPOINT_MOTION_TYPE ? ", on a round" : "",
                    owner ? Acore::StringFormat(", summoned by {} {:.0f} yd from the bot", owner->GetName(), player->GetExactDist(owner)) : std::string());
            }
        }
        if (unit == victim)
            victimCounted = true;
        if (creature->GetLevel() > fight.foeLevel || !fight.foeEntry)
        {
            fight.foeEntry = creature->GetEntry();
            fight.foeLevel = creature->GetLevel();
        }
    };
    for (Unit const* attacker : player->getAttackers())
        count(attacker);
    if (!victimCounted)
        count(victim);
    // What hits its pet is in the fight too.
    if (Pet const* pet = player->GetPet())
        for (Unit const* attacker : pet->getAttackers())
            if (attacker != victim && attacker->GetVictim() == pet)
                count(attacker);
    fight.foes = std::max(fight.foes, foes);
    // Attacked in the saddle and still in it after four seconds: off the mount, so that the AI fights. mod-playerbots
    // gets off an ordinary mount by itself, but not off a mount a quest puts the character on: two bots sat on the
    // elekk of "The Kessel Run" for a minute each while a single creature of their level killed them, without a hit,
    // a heal or a step (the run on the elekk is lost by this; the bot is not).
    constexpr uint32 SaddleMs = 4 * IN_MILLISECONDS;
    if (foes && !rt.escape.active && (player->IsMounted() || player->GetVehicle()) && !player->IsInFlight() &&
        getMSTimeDiff(fight.startedAt, getMSTime()) >= SaddleMs)
    {
        LOG_INFO(ActivityLog, "activity: {} gets off its mount to fight: attacked in the saddle for {} s", rt.name,
            getMSTimeDiff(fight.startedAt, getMSTime()) / IN_MILLISECONDS);
        if (player->GetVehicle())
            player->ExitVehicle();
        player->Dismount();
        player->RemoveAurasByType(SPELL_AURA_MOUNTED);
    }
    // Attacked and not fighting back (a paladin stood 62 seconds in a fight against one creature of its level, its
    // target at full health to the end, and died): after twelve seconds in which the foes lost nothing the fight line
    // says what the bot was doing. (Whether it has a target does not say it: a caster's AI casts without one.)
    constexpr uint32 NoDamageAfterMs = 12 * IN_MILLISECONDS;
    if (fight.idleNote.empty() && foes && !rt.escape.active && getMSTimeDiff(fight.startedAt, getMSTime()) >= NoDamageAfterMs &&
        fight.unitsJoined - unitsNow < 3.0f)
        fight.idleNote = Acore::StringFormat("last action \"{}\", {}{}{}{}{}", BotDriver::LastAction(player),
            victim ? "with a target" : "without a target", player->HasUnitState(UNIT_STATE_STUNNED) ? ", stunned" : "",
            player->HasUnitState(UNIT_STATE_ROOT) ? ", rooted" : "",
            player->HasUnitState(UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING) ? ", feared" : "",
            player->IsNonMeleeSpellCast(false) ? ", casting" : "") + (player->IsMounted() ? ", mounted" : "");
    // A fight in which nothing happens: the bot loses and gains nothing, its foes lose nothing, it does not move and
    // casts nothing, for StaleFightMs, by the clock on the wall (the task's clock stands still in a fight, so neither
    // the stuck rule nor the rescue ends this). The game counts a character as fighting for as long as any creature
    // holds a combat reference to it, also one that does not attack, cannot be seen or cannot be reached: a warlock
    // stood "in combat" at 45 % health beside Manor Mistmantle for over half an hour, no fight, no step. The module
    // says who holds the fight and ends it; the planner goes on from there. In a dungeon after 120 s and not while a
    // boss of the dungeon holds it (a boss that talks), and the bot stays with its group: a hunter of the Sunken Temple's
    // group stood "in combat" for 850 s with two dragonkin 37 yd off that could not reach it, and its group waited for it.
    bool const inDungeon = player->GetMap()->IsDungeon();
    uint32 const StaleFightMs = (inDungeon ? 120 : 45) * IN_MILLISECONDS;
    uint32 const quietNow = getMSTime();
    bool bossHolds = false;
    if (inDungeon)
        for (auto const& [guid, ref] : player->GetCombatManager().GetPvECombatRefs())
            if (Creature const* creature = ref->GetOther(player) ? ref->GetOther(player)->ToCreature() : nullptr;
                creature && creature->IsAlive() && creature->IsDungeonBoss())
                bossHolds = true;
    // (Health that comes back by itself and a foe that regenerates are nothing happening; what is lost is measured
    // against the most seen since, so slow damage adds up and a point of rounding does not count.)
    // (Nor how many are on the bot, as such: a creature that cannot reach it leaves and comes back, again and again,
    // and every change began the quiet anew. A shaman stood "in combat" for 480 s at the Ogre Remains of Dustwallow
    // Marsh, "2 foes at once", 0 yards in five minutes. What counts is a foe that is hurt: a new low of its own health.)
    bool foeHurt = false;
    auto lookAtFoe = [&](Unit const* unit)
    {
        Creature const* creature = unit ? unit->ToCreature() : nullptr;
        if (!creature || !creature->IsAlive() || creature->IsPet() || creature->IsTotem())
            return;
        float const pct = creature->GetHealthPct();
        auto low = std::find_if(fight.foeLows.begin(), fight.foeLows.end(),
            [&](std::pair<ObjectGuid, float> const& entry) { return entry.first == creature->GetGUID(); });
        if (low == fight.foeLows.end())
            fight.foeLows.emplace_back(creature->GetGUID(), pct);
        else if (pct < low->second - 1.0f)
        {
            low->second = pct;
            foeHurt = true;
        }
    };
    for (Unit const* attacker : player->getAttackers())
        lookAtFoe(attacker);
    lookAtFoe(victim);
    // A foe that does not die. Bloodyo, a death knight new that day, stood 297 s beside a Servant of Grol at 0 %
    // health: its SmartAI holds it at one health point ("Set Invincibility at 1 HP") until the Stone of Binding is
    // used on it, which a kill objective does not do; the quiet rule below never ran, because the bot was hit and
    // cast all the while (both count as something happening). The same holds for a creature that is immune or held
    // at a higher level by its script. So: no foe on the bot or its target has reached a new low for NoProgressMs,
    // or one of them has stood at one health point for UndyingMs: the module ends the fight, the bot walks away,
    // leaves that creature alone for UndyingAsideMs and ends a task that was about it (twice within ten minutes and
    // the quest waits a quarter of an hour, IsBlockedEnd). Not in a dungeon (a boss with a phase), not on a run, and
    // the NoProgressMs part not while another character of its group fights (a healer beside its squad's fight).
    constexpr uint32 NoProgressMs = 60 * IN_MILLISECONDS;
    constexpr uint32 UndyingMs = 15 * IN_MILLISECONDS;
    // (An interaction task gets longer: a plan that brings a creature low and then uses an object on it, the Stone of
    // Binding on a Servant, stops attacking, walks to the object and uses it, six to eight seconds and the use.)
    constexpr uint32 UndyingInteractMs = 30 * IN_MILLISECONDS;
    constexpr uint32 UndyingAsideMs = 15 * MINUTE * IN_MILLISECONDS;
    // A fight in which the bot hurts nobody and is hurt: it cannot get at what fights it. Hordeboss, a level-20 warrior
    // of the test guild, stood 47 s in a lake of the Stonetalon Mountains below a Venture Co. Deforester, a caster on
    // the bank, whose Fireballs took all its health; the Deforester ended the fight at full health, and the 60 s of no
    // progress came too late. When no foe has been hurt at all after UnhitMs and the bot has lost UnhitLossPct of its
    // health meanwhile, the same as for a foe that does not die.
    constexpr uint32 UnhitMs = 15 * IN_MILLISECONDS;
    constexpr uint8 UnhitLossPct = 15;
    bool const unhit = !fight.firstLossAt && getMSTimeDiff(fight.startedAt, quietNow) >= UnhitMs &&
        fight.healthStart >= health + UnhitLossPct;
    if (foeHurt || !fight.foeLossAt)
    {
        fight.foeLossAt = quietNow;
        fight.foeLossHealth = health;
    }
    fight.foeLossHealth = std::max(fight.foeLossHealth, health);
    // A fight it loses without hurting anyone any more: no foe has reached a new low for LosingMs and the bot has lost
    // LosingLossPct of its health meanwhile (its potions counted against it). Brakthar, a level-21 warrior, took a
    // Stormsnout to 42 % and then hurt nothing for 60 s while it lost two thirds of its health, and a minute later
    // fought a Thunderhead for 80 s, "its target at 100% health", and died: the 60 s of no progress came too late.
    constexpr uint32 LosingMs = 20 * IN_MILLISECONDS;
    constexpr uint8 LosingLossPct = 20;
    bool const losing = getMSTimeDiff(fight.foeLossAt, quietNow) >= LosingMs && fight.foeLossHealth >= health + LosingLossPct;
    // The creature it is about: one at one health point first, then the bot's target, then the nearest on it.
    Creature const* held = nullptr;
    bool atOnePoint = false;
    auto consider = [&](Unit const* unit)
    {
        Creature const* creature = unit ? unit->ToCreature() : nullptr;
        if (!creature || !creature->IsAlive() || creature->IsPet() || creature->IsTotem())
            return;
        bool const onePoint = creature->GetHealth() <= 1;
        bool const better = !held || (onePoint != atOnePoint ? onePoint :
            ((unit == victim) != (static_cast<Unit const*>(held) == victim) ? unit == victim :
                creature->GetExactDist(player) < held->GetExactDist(player)));
        if (better)
        {
            held = creature;
            atOnePoint = onePoint;
        }
    };
    consider(victim);
    for (Unit const* attacker : player->getAttackers())
        consider(attacker);
    bool groupFights = false;
    if (Group* group = player->GetGroup())
        for (GroupReference* ref = group->GetFirstMember(); ref && !groupFights; ref = ref->next())
            if (Player const* member = ref->GetSource(); member && member != player && member->IsInCombat() &&
                member->IsInMap(player) && member->GetExactDist(player) <= 100.0f)
                groupFights = true;
    bool const timedOut = getMSTimeDiff(fight.foeLossAt, quietNow) >= (atOnePoint ?
        (rt.task.kind == TaskKind::Interact ? UndyingInteractMs : UndyingMs) : NoProgressMs);
    // (The bot that hurts nobody also beside its group's fight: what it fights is hurt by the others when they reach it.)
    if (held && !rt.escape.active && !player->GetMap()->IsDungeon() && !player->IsInFlight() &&
        (((atOnePoint || !groupFights) && timedOut) || unhit || (losing && !groupFights)))
    {
        ObjectGuid const heldGuid = held->GetGUID();
        if (losing && !timedOut && !unhit)
        {
            // What the bot was at, for the cause: its target, where it stands to it, what holds its AI.
            Unit const* target = victim ? victim : ObjectAccessor::GetUnit(*player, player->GetTarget());
            LOG_INFO(ActivityLog, "activity: {} has hurt none of its foes for {} s and has lost {}% of its health meanwhile: the module "
                "ends the fight with {} (level {}, {:.0f}% health) and the bot leaves that creature alone for {} min [its target {}; "
                "{}{}{}; AI {}]", rt.name, getMSTimeDiff(fight.foeLossAt, quietNow) / IN_MILLISECONDS,
                uint32(fight.foeLossHealth - health), held->GetName(), uint32(held->GetLevel()), held->GetHealthPct(),
                UndyingAsideMs / (MINUTE * IN_MILLISECONDS),
                target ? Acore::StringFormat("{} at {:.0f}% health, {:.0f} yd{}", target->GetName(), target->GetHealthPct(),
                    player->GetExactDist(target), target == static_cast<Unit const*>(held) ? " (the same)" : "") : std::string("none"),
                player->HasUnitState(UNIT_STATE_MELEE_ATTACKING) ? "swinging" : "not swinging",
                target && player->IsWithinMeleeRange(target) ? ", in melee reach" : ", out of melee reach",
                target && !player->HasInArc(float(M_PI), target) ? ", not facing it" : "", BotDriver::AIState(player));
        }
        else if (!timedOut)
            LOG_INFO(ActivityLog, "activity: {} has hurt none of its foes in {} s of fight{} and has lost {}% of its health meanwhile: it "
                "cannot get at {} (level {}, {:.0f} yd away, last action \"{}\"): the module ends the fight and the bot leaves that "
                "creature alone for {} min", rt.name, getMSTimeDiff(fight.startedAt, quietNow) / IN_MILLISECONDS,
                fight.water ? " in the water" : "", uint32(fight.healthStart - health), held->GetName(), uint32(held->GetLevel()),
                player->GetExactDist(held), BotDriver::LastAction(player), UndyingAsideMs / (MINUTE * IN_MILLISECONDS));
        else
            LOG_INFO(ActivityLog, "activity: {} has been in a fight for {} s in which its foes have lost nothing for {} s: {} (level {}) "
            "{} (health {}%, last action \"{}\"): the module ends the fight and the bot leaves that creature alone for {} min",
            rt.name, getMSTimeDiff(fight.startedAt, quietNow) / IN_MILLISECONDS, getMSTimeDiff(fight.foeLossAt, quietNow) / IN_MILLISECONDS,
            held->GetName(), uint32(held->GetLevel()), atOnePoint ? "stays at one health point (a script holds it there)" :
            Acore::StringFormat("stays at {:.0f}% health (immune, or a script holds it)", held->GetHealthPct()), uint32(health),
            BotDriver::LastAction(player), UndyingAsideMs / (MINUTE * IN_MILLISECONDS));
        fight.stale = true;
        rt.undyingFoes[heldGuid] = quietNow + UndyingAsideMs;
        // (The spawn too, so the planner does not choose it again as the task's spot.)
        if (held->GetSpawnId())
        {
            SpawnPoint spawn;
            spawn.spawnId = held->GetSpawnId();
            spawn.entry = held->GetEntry();
            spawn.mapId = uint16(held->GetMapId());
            held->GetRespawnPosition(spawn.x, spawn.y, spawn.z);
            rt.Avoid(spawn, UndyingAsideMs);
        }
        if (Pet* pet = player->GetPet())
            pet->CombatStop(true);
        player->CombatStop(true);
        fight.foeLossAt = quietNow;
        fight.foeLossHealth = health;
        fight.quietSince = quietNow;
        if (rt.task.kind == TaskKind::Objective || rt.task.kind == TaskKind::Interact || rt.task.kind == TaskKind::Grind)
            ClearTask(rt, 0, "a foe that does not die");
        if (Creature* from = ObjectAccessor::GetCreature(*player, heldGuid); from && !player->IsInCombat())
            Escape::WalkAway(player, rt, from);
        return;
    }
    // (Health lost with nothing on the bot is no fight going on: a periodic effect, or a creature that cannot be
    // reached. Retadk, a death knight, stood "in combat" for over a quarter of an hour beside the rooted Servant of
    // Sevine and lost a few points of health a minute, which began the quiet anew every time.)
    bool const busy = !fight.quietSince || (foes && health + 2 <= fight.quietHealth) || foeHurt || fight.seen.size() != fight.quietSeen ||
        std::hypot(player->GetPositionX() - fight.quietX, player->GetPositionY() - fight.quietY) > 3.0f ||
        player->IsNonMeleeSpellCast(false) || rt.escape.active || player->IsInFlight() || player->GetTransport() ||
        bossHolds || player->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING);
    if (busy)
    {
        fight.quietSince = quietNow;
        fight.quietSeen = fight.seen.size();
        fight.quietX = player->GetPositionX();
        fight.quietY = player->GetPositionY();
        fight.quietHealth = health;
    }
    fight.quietHealth = std::max(fight.quietHealth, health);
    if (!busy && getMSTimeDiff(fight.quietSince, quietNow) >= StaleFightMs)
    {
        Pet* pet = player->GetPet();
        LOG_INFO(ActivityLog, "activity: {} has been in a fight for {} s in which nothing has happened for {} s (health {}%, {} on it, "
            "at ({:.0f}, {:.0f}, {:.0f}), last action \"{}\"): in combat with {}{}: the module ends the fight",
            rt.name, getMSTimeDiff(fight.startedAt, quietNow) / IN_MILLISECONDS, getMSTimeDiff(fight.quietSince, quietNow) / IN_MILLISECONDS,
            uint32(health), uint32(foes), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
            BotDriver::LastAction(player), CombatHolders(player, player),
            pet ? Acore::StringFormat("; its pet {} with {}", pet->GetName(), CombatHolders(player, pet)) : std::string());
        fight.stale = true;
        // The creature that held it, to walk away from (the nearest alive): where the bot stands it would begin again.
        Creature* holder = nullptr;
        for (Unit* unit : { static_cast<Unit*>(player), static_cast<Unit*>(pet) })
        {
            if (!unit)
                continue;
            for (auto const& [guid, ref] : unit->GetCombatManager().GetPvECombatRefs())
                if (Creature* creature = ref->GetOther(unit) ? ref->GetOther(unit)->ToCreature() : nullptr; creature &&
                    creature->IsAlive() && !creature->IsPet() && (!holder || creature->GetExactDist(player) < holder->GetExactDist(player)))
                    holder = creature;
        }
        ObjectGuid const holderGuid = holder ? holder->GetGUID() : ObjectGuid::Empty;
        if (pet)
            pet->CombatStop(true);
        player->CombatStop(true);
        fight.quietSince = quietNow; // (if the game still counts it as fighting: again in StaleFightMs)
        if (Creature* from = holderGuid && !inDungeon ? ObjectAccessor::GetCreature(*player, holderGuid) : nullptr; from &&
            !player->IsInCombat())
            Escape::WalkAway(player, rt, from);
        return;
    }
    // How the fight goes: the foes' health now, and how long how many of them have been on the bot.
    fight.unitsNow = unitsNow;
    uint32 const sampleAt = getMSTime();
    if (fight.measuredAt)
        fight.attackerSeconds += float(foes) * std::min(2.0f, float(getMSTimeDiff(fight.measuredAt, sampleAt)) / float(IN_MILLISECONDS));
    fight.measuredAt = sampleAt;
    if (!fight.firstLossAt && fight.unitsJoined - unitsNow >= 3.0f)
        fight.firstLossAt = sampleAt;

    // Where it fights and how far it gets, for the fight line: a fight in the water that ends with its target at
    // full health says the bot could not hit it.
    fight.water = std::max(fight.water, Escape::WaterAt(player, player->GetPositionX(), player->GetPositionY(),
        player->GetPositionZ()));
    if (victim && victim->IsAlive())
        fight.targetHealth = uint8(std::clamp(victim->GetHealthPct(), 0.0f, 100.0f));
    // (Debug) How a character that fights at range fights: every two seconds what it casts, whether it moves, and
    // how far the nearest creature that is on it (or on its pet) stands.
    if (GetSettings().debug && !fight.water && player->getPowerType() == POWER_MANA && player->getClass() != CLASS_PALADIN &&
        getMSTimeDiff(fight.debugAt, getMSTime()) >= 2 * IN_MILLISECONDS)
    {
        fight.debugAt = getMSTime();
        Unit const* nearest = nullptr;
        for (Unit const* attacker : player->getAttackers())
            if (attacker->IsAlive() && (!nearest || player->GetExactDist(attacker) < player->GetExactDist(nearest)))
                nearest = attacker;
        Pet const* pet = player->GetPet();
        if (!nearest && pet)
            for (Unit const* attacker : pet->getAttackers())
                if (attacker->IsAlive() && (!nearest || player->GetExactDist(attacker) < player->GetExactDist(nearest)))
                    nearest = attacker;
        if (!nearest)
            nearest = victim;
        Spell const* spell = player->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        if (!spell)
            spell = player->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
        if (!spell)
            spell = player->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL);
        LOG_INFO(ActivityLog, "activity: {} fight trace {} s: health {}%, mana {}%, {}, {}{}; {}", rt.name,
            getMSTimeDiff(fight.startedAt, getMSTime()) / IN_MILLISECONDS, uint32(health), uint32(ManaPercent(player)),
            spell ? Acore::StringFormat("casts {}", spell->GetSpellInfo()->SpellName[0]) : std::string("casts nothing"),
            player->isMoving() ? "moving" : "standing",
            pet ? Acore::StringFormat(", pet at {:.0f}%", pet->IsAlive() ? pet->GetHealthPct() : 0.0f) : std::string(),
            nearest ? Acore::StringFormat("{} at {:.0f}% health, {:.0f} yd away, on {}{}", nearest->GetName(), nearest->GetHealthPct(),
                player->GetExactDist(nearest), nearest->GetVictim() == player ? "the bot" : (nearest->GetVictim() ? "something else" : "nothing"),
                nearest->HasUnitState(UNIT_STATE_ROOT) ? ", rooted" : (nearest->isMoving() ? ", moving" : "")) : std::string("no foe"));
    }
    if (GetSettings().debug && fight.water && getMSTimeDiff(fight.debugAt, getMSTime()) >= 3 * IN_MILLISECONDS)
    {
        fight.debugAt = getMSTime();
        Spell const* spell = player->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        Spell const* channel = player->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
        LOG_INFO(ActivityLog, "activity: {} water fight: last action \"{}\", form {}, power {} {}%, at z {:.1f}{}{}, movement {}{}; victim {}",
            rt.name, BotDriver::LastAction(player), uint32(player->GetShapeshiftForm()), uint32(player->getPowerType()),
            player->GetMaxPower(player->getPowerType()) ? uint32(uint64(player->GetPower(player->getPowerType())) * 100 /
                player->GetMaxPower(player->getPowerType())) : 0u,
            player->GetPositionZ(), player->isMoving() ? ", moving" : "",
            player->HasUnitMovementFlag(MOVEMENTFLAG_SWIMMING) ? ", swim flag" : "",
            uint32(player->GetMotionMaster()->GetCurrentMovementGeneratorType()),
            spell ? Acore::StringFormat(", casts {}", spell->GetSpellInfo()->Id) : (channel ?
                Acore::StringFormat(", channels {}", channel->GetSpellInfo()->Id) : std::string()),
            victim ? Acore::StringFormat("{} at {:.0f}% health, {:.1f} yd away ({:.1f} flat, {:.1f} up), melee range {}, in "
                "sight {}, attacks {}, it {}moves (movement {})", victim->GetName(), victim->GetHealthPct(),
                player->GetExactDist(victim), player->GetExactDist2d(victim), victim->GetPositionZ() - player->GetPositionZ(),
                player->IsWithinMeleeRange(victim) ? "yes" : "no", player->IsWithinLOSInMap(victim) ? "yes" : "no",
                victim->GetVictim() == player ? "the bot" : (victim->GetVictim() ? "something else" : "nothing"),
                victim->isMoving() ? "" : "not ", uint32(victim->GetMotionMaster()->GetCurrentMovementGeneratorType())) :
                std::string("none"));
    }
}

void OrderService::LeaveTravelForm(Player* player, BotRuntime& rt)
{
    if (!player->IsAlive() || !player->IsInCombat() || rt.escape.active)
        return;
    uint8 const form = uint8(player->GetShapeshiftForm());
    char const* name = form == FORM_FLIGHT || form == FORM_FLIGHT_EPIC ? nullptr : FormName(form);
    if (!name)
        return;
    player->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
    LOG_INFO(ActivityLog, "activity: {} leaves its {} to fight", rt.name, name);
}

void OrderService::StepSelfHeal(Player* player, BotRuntime& rt)
{
    Settings const& settings = GetSettings();
    uint32 const now = getMSTime();
    if (!settings.selfHealBelow || !player->IsAlive() || !player->IsInCombat() || rt.escape.active || now < rt.nextSelfHeal ||
        HealthPercent(player) >= settings.selfHealBelow || player->IsNonMeleeSpellCast(false) ||
        player->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING))
        return;
    uint32 const spell = SelfHealSpell(player, true);
    if (!spell)
        return;

    constexpr uint32 GlobalCooldownMs = 1500;
    // A paladin at critical health presses what it has for that: Lay on Hands, else Divine Shield (it heals under it
    // with its next looks); neither with Forbearance on it. mod-playerbots has both on its "critical health" trigger,
    // but a level-40 paladin among four murlocs went down to 9 % with both ready, and the decision to stay in such a
    // fight counts on them (Escape: EmergencyReserve).
    constexpr uint8 CriticalHealth = 25;
    constexpr uint32 Forbearance = 25771;
    if (player->getClass() == CLASS_PALADIN && HealthPercent(player) < CriticalHealth && !player->HasAura(Forbearance))
        for (uint32 const first : { 633u /*Lay on Hands*/, 642u /*Divine Shield*/ })
            if (uint32 const emergency = KnownRank(player, first); emergency && !player->HasSpellCooldown(emergency))
            {
                uint8 const at = HealthPercent(player);
                if (player->CastSpell(player, emergency, false) != SPELL_CAST_OK)
                    continue;
                SpellInfo const* info = sSpellMgr->GetSpellInfo(emergency);
                LOG_INFO(ActivityLog, "activity: {} casts {} at {}% health", rt.name, info ? info->SpellName[0] : "its emergency spell", uint32(at));
                rt.nextSelfHeal = now + (first == 633 ? GlobalCooldownMs : 300);
                if (first == 633 && rt.fight.active && rt.fight.selfHeals < 255)
                    ++rt.fight.selfHeals;
                return;
            }

    // A paladin stuns what hits it first, so the heal is not pushed back (the heal follows with the next look).
    constexpr uint32 HammerOfJustice = 853;
    if (player->getClass() == CLASS_PALADIN)
        if (Unit* victim = player->GetVictim(); victim && victim->IsAlive() && !victim->HasUnitState(UNIT_STATE_STUNNED))
            if (uint32 const hammer = KnownRank(player, HammerOfJustice); hammer && !player->HasSpellCooldown(hammer))
                if (player->CastSpell(victim, hammer, false) == SPELL_CAST_OK)
                {
                    rt.nextSelfHeal = now + GlobalCooldownMs;
                    BotDriver::PauseAI(player, GlobalCooldownMs + 200);
                    return;
                }

    uint32 const castMs = CastSelfHeal(player, spell);
    if (!castMs)
    {
        rt.nextSelfHeal = now + 700; // the global cooldown, or no mana: look again shortly
        return;
    }
    // mod-playerbots must not cut the cast short with its own next spell or a step towards its target.
    BotDriver::PauseAI(player, castMs + 300);
    rt.nextSelfHeal = now + castMs + 500;
    if (rt.fight.active && rt.fight.selfHeals < 255)
        ++rt.fight.selfHeals;
}

void OrderService::StepPotion(Player* player, BotRuntime& rt)
{
    // the owner, 2026-11-13: [quote left out]: the
    // best potion the bot carries or its guild's storage has, one piece, taken out when it is drunk. The game allows
    // one potion per fight.
    constexpr uint32 LookMs = 1500;
    constexpr uint32 NoneMs = 10 * IN_MILLISECONDS;
    constexpr float TargetAbovePct = 25.0f;
    uint32 const now = getMSTime();
    if (now < rt.nextPotion || !player->IsAlive() || !player->IsInCombat())
        return;
    rt.nextPotion = now + LookMs;
    Settings const settings = sBotSettings.For(player);
    uint8 const health = HealthPercent(player);
    uint8 const mana = ManaPercent(player);
    bool const forHealth = settings.potionBelowHealth && health < settings.potionBelowHealth;
    // Mana: only while the fight still needs it (the rest's drink does the rest), and not into a running cast.
    Unit* const victim = player->GetVictim();
    bool const forMana = !forHealth && settings.potionBelowMana && UsesMana(player) && mana < settings.potionBelowMana && victim &&
        victim->IsAlive() && victim->GetHealthPct() > TargetAbovePct && !player->IsNonMeleeSpellCast(false);
    if ((!forHealth && !forMana) || player->GetLastPotionId() ||
        player->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING) ||
        (player->getClass() == CLASS_DRUID && player->GetShapeshiftForm() != FORM_NONE))
        return;

    Item* const potion = StorageSupply::TakeOne(player, forHealth ? StorageSupply::Kind::HealingPotion : StorageSupply::Kind::ManaPotion,
        Acore::StringFormat("for its fight ({} at {}%)", forHealth ? "health" : "mana", uint32(forHealth ? health : mana)).c_str());
    if (!potion)
    {
        rt.nextPotion = now + NoneMs; // neither its bags nor the storage have one: no need to ask every look
        return;
    }
    std::string const name = potion->GetTemplate()->Name1;
    uint32 const entry = potion->GetEntry();
    uint32 const before = player->GetItemCount(entry);
    UseOnSelf(player, potion, false, forHealth ? 0 : 1);
    if (player->GetItemCount(entry) < before)
    {
        if (rt.fight.active)
            rt.fight.potion = name;
        LOG_DEBUG(ActivityLog, "activity: {} drank {} in its fight ({} at {}%)", rt.name, name, forHealth ? "health" : "mana",
            uint32(forHealth ? health : mana));
    }
}

void OrderService::StepSelfBuff(Player* player, BotRuntime& rt)
{
    // mod-playerbots' mage casts an armour only once it knows Mage Armor (level 34) or Molten Armor: its triggers ask
    // for those spells, and nothing ever asks for Frost Armor or Ice Armor alone. On the integration server all three
    // mages (levels 16, 24, 25) fought without one: no extra armour, and nothing that slows what hits them, with half
    // of every fight spent in melee (a level-25 frost mage lost 15 to 59 % of its health per creature of its level).
    constexpr uint32 LookMs = 5 * IN_MILLISECONDS;
    constexpr uint32 FrostArmor = 168;
    constexpr uint32 IceArmor = 7302;
    constexpr uint32 MageArmor = 6117;
    constexpr uint32 MoltenArmor = 30482;

    // A shaman's totems follow the place (BotDriver::KeepTotems): one Searing Totem in the open world.
    if (player->getClass() == CLASS_SHAMAN)
    {
        constexpr uint32 TotemLookMs = 20 * IN_MILLISECONDS;
        uint32 const totemNow = getMSTime();
        if (totemNow >= rt.nextTotemLook && player->IsAlive() && !player->IsInCombat())
        {
            rt.nextTotemLook = totemNow + TotemLookMs;
            if (uint8 const kept = BotDriver::KeepTotems(player))
                LOG_INFO(ActivityLog, "activity: {} {}", rt.name, kept == 1 ?
                    "sets one Searing Totem per fight while it levels (the four totems of its talents cost it about a third of its "
                    "mana every time)" : "sets the totems of its talents again (it is in a dungeon)");
        }
        return;
    }
    if (player->getClass() != CLASS_MAGE)
        return;
    uint32 const now = getMSTime();
    if (now < rt.nextSelfBuff)
        return;
    rt.nextSelfBuff = now + LookMs;
    if (!player->IsAlive() || player->IsMounted() || player->IsInFlight() || rt.escape.active || player->IsNonMeleeSpellCast(false) ||
        player->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING | UNIT_STATE_CASTING))
        return;

    auto hasRankOf = [player](uint32 first)
    {
        for (uint32 spell = first; spell; spell = sSpellMgr->GetNextSpellInChain(spell))
            if (player->HasAura(spell))
                return true;
        return false;
    };
    if (hasRankOf(FrostArmor) || hasRankOf(IceArmor) || hasRankOf(MageArmor) || hasRankOf(MoltenArmor))
        return;
    // (With Mage Armor or Molten Armor known the choice is mod-playerbots'.)
    if (KnownRank(player, MageArmor) || KnownRank(player, MoltenArmor))
        return;
    uint32 spell = KnownRank(player, IceArmor);
    if (!spell)
        spell = KnownRank(player, FrostArmor);
    if (!spell || player->HasSpellCooldown(spell))
        return;
    if (player->CastSpell(player, spell, false) != SPELL_CAST_OK)
        return;
    SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
    LOG_INFO(ActivityLog, "activity: {} puts on {} (mod-playerbots does not at its level)", player->GetName(),
        info && info->SpellName[0] ? info->SpellName[0] : "its armour");
    BotDriver::PauseAI(player, 300);
}

void OrderService::StopLeftoverAutoShot(Player* player, BotRuntime& rt)
{
    // A target that is alive: only when neither the bot nor the target has fought for a few seconds and no pull of the
    // module runs (mod-playerbots opens a fight with Auto Shot out of combat; the first shot comes a moment later).
    // Every repeating shot, not only a hunter's Auto Shot: a mage's wand ("Shoot") went on at a Noxious Plaguebat that
    // the module had ended a fight with (it did not die), so the mage counted as casting for five minutes, its walks
    // did not start, and the rule that takes a walk over when it does not get on waits for a cast to end, until the
    // rescue near Light's Hope, round 157.
    constexpr uint32 IdleMs = 4 * IN_MILLISECONDS;
    Spell const* spell = player->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL);
    if (!spell || !spell->m_spellInfo)
    {
        rt.autoShotIdleSince = 0;
        return;
    }
    uint32 const now = getMSTime();
    // Never the spell's own target pointer: it is refreshed only when the spell updates (in the hunter's own update,
    // every Bots.UpdateInterval ms for a bot), and a target that logged out or despawned meanwhile leaves it pointing
    // at freed memory (a crash after 400 bots logged out at once). By guid, on the hunter's map.
    ObjectGuid const targetGuid = spell->m_targets.GetUnitTargetGUID();
    Unit const* target = targetGuid ? ObjectAccessor::GetUnit(*player, targetGuid) : nullptr;
    bool gone = !target || !target->IsInWorld() || !target->IsAlive();
    // (Nor at a creature the bot leaves alone: one that does not die. And a target "in combat" counts as a fight only
    // when it is on the bot: the plaguebat stayed in combat after the module had ended the bot's fight.)
    if (!gone && target)
        if (auto alone = rt.undyingFoes.find(targetGuid); alone != rt.undyingFoes.end() && now < alone->second)
            gone = true;
    bool const idle = !player->IsInCombat() && !rt.pull.active && !(target && target->IsInCombat() && target->GetVictim() == player);
    if (!idle)
        rt.autoShotIdleSince = 0;
    else if (!rt.autoShotIdleSince)
        rt.autoShotIdleSince = std::max<uint32>(now, 1);
    if (!gone && !(rt.autoShotIdleSince && getMSTimeDiff(rt.autoShotIdleSince, now) >= IdleMs))
        return;
    player->InterruptSpell(CURRENT_AUTOREPEAT_SPELL);
    rt.autoShotIdleSince = 0;
}

void OrderService::StepWand(Player* player, BotRuntime& rt)
{
    // mod-playerbots' casters reach "shoot" last: a level-20 shadow priest without Mind Flay opens with its pain and
    // a Mind Blast and then casts Smite until the creature is dead, 65 % of its mana for one creature of its level,
    // with a wand in its hand. Against one creature, while it is not hard pressed:
    //  - a priest without Mind Flay shoots its wand between its Mind Blasts once its pain is on the target (not at
    //    full mana: there the Smites cost nothing that matters);
    //  - a priest, mage or warlock finishes a creature that is nearly dead with the wand when its mana is not plentiful.
    constexpr uint32 ShootSpell = 5019;
    constexpr uint32 MindBlast = 8092;
    constexpr uint32 MindFlay = 15407;
    constexpr uint32 ShadowWordPain = 589;
    constexpr uint8 MinHealth = 50;
    constexpr uint8 FillerBelowMana = 85;
    constexpr uint8 FinisherBelowMana = 60;
    constexpr float FinisherTargetHealth = 20.0f;
    constexpr float WandYards = 28.0f;
    // What it stops in favour of the wand, by first rank: Smite, Holy Fire, Shadow Bolt, Frostbolt, Fireball.
    static constexpr uint32 Fillers[] = { 585, 14914, 686, 116, 133 };

    uint8 const cls = player->getClass();
    if ((cls != CLASS_PRIEST && cls != CLASS_MAGE && cls != CLASS_WARLOCK) || !player->IsAlive() || !player->IsInCombat() ||
        rt.escape.active || HealthPercent(player) < MinHealth || player->GetShapeshiftForm() == FORM_SHADOW)
        return;
    Item const* wand = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
    if (!wand || wand->GetTemplate()->Class != ITEM_CLASS_WEAPON || wand->GetTemplate()->SubClass != ITEM_SUBCLASS_WEAPON_WAND)
        return;

    // One creature only: with two on it every second counts more than the mana.
    Unit* target = nullptr;
    uint32 foes = 0;
    auto count = [&](Unit* unit)
    {
        if (unit->IsAlive() && unit->IsCreature() && !unit->IsPet() && !unit->IsTotem() && unit != target)
        {
            ++foes;
            target = unit;
        }
    };
    for (Unit* attacker : player->getAttackers())
        count(attacker);
    if (Pet* pet = player->GetPet())
        for (Unit* attacker : pet->getAttackers())
            count(attacker);
    if (foes > 1)
        return;
    if (!target)
        target = ObjectAccessor::GetUnit(*player, player->GetTarget());
    if (!target || !target->IsAlive() || !player->IsValidAttackTarget(target) || player->GetExactDist(target) > WandYards ||
        !player->IsWithinLOSInMap(target))
        return;

    uint8 const mana = ManaPercent(player);
    bool filler = false;
    if (cls == CLASS_PRIEST && mana <= FillerBelowMana && !KnownRank(player, MindFlay))
    {
        uint32 const blast = KnownRank(player, MindBlast);
        uint32 const pain = KnownRank(player, ShadowWordPain);
        filler = !(blast && !player->HasSpellCooldown(blast)) && (!pain || target->HasAura(pain, player->GetGUID()));
    }
    bool const finisher = target->GetHealthPct() <= FinisherTargetHealth && mana < FinisherBelowMana;
    if (!filler && !finisher)
        return;

    if (player->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
        return;
    if (Spell const* spell = player->GetCurrentSpell(CURRENT_GENERIC_SPELL))
    {
        uint32 const first = sSpellMgr->GetFirstSpellInChain(spell->GetSpellInfo()->Id);
        if (std::find(std::begin(Fillers), std::end(Fillers), first) == std::end(Fillers))
            return; // a Mind Blast, a heal, a shield: it lands
        player->InterruptSpell(CURRENT_GENERIC_SPELL);
    }
    if (!player->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
    {
        player->SetFacingToObject(target);
        if (player->CastSpell(target, ShootSpell, false) != SPELL_CAST_OK)
            return;
    }
    // mod-playerbots would start the next Smite within a moment.
    BotDriver::PauseAI(player, 1300);
    if (rt.fight.active)
        rt.fight.wand = true;
}

void OrderService::EndFight(Player* player, BotRuntime& rt, bool died)
{
    BotRuntime::Fight const fight = rt.fight;
    rt.fight = BotRuntime::Fight();
    if (!fight.active)
        return;
    rt.lastFightEndAt = std::max<uint32>(getMSTime(), 1);

    for (BotRuntime::Tally* tally : { &rt.window, &rt.total })
    {
        ++tally->fights;
        if (fight.foes >= 2)
            ++tally->crowded;
        if (fight.healthStart < 50)
            ++tally->weakStarts;
    }
    uint8 const healthEnd = died ? 0 : HealthPercent(player);
    // A fight the bot ran from (FightEscape.h) is not won; it teaches only when the bot left it by how it went
    // (Escape::OnFightEnd).
    bool const left = rt.escape.active && !died;
    // (Nor is one the module ended because nothing happened in it.)
    // What it cost is how far down it went, not where it ended: a potion or a heal in the fight brings the end back up.
    // The level-60 warriors of the guild pulled the Shadowmages of the Eastern Plaguelands in pairs at 95 % health,
    // went down to 20-30 %, drank a Major Healing Potion and ended at 60-70 %, which taught them that a pair costs a
    // third of what it does; one of them died to a pair the pack look had judged at 45 %, round 172.
    if (!fight.stale || died)
        Escape::OnFightEnd(player, rt, fight.foes, fight.foeLevel, fight.healthStart, std::min(fight.healthLow, healthEnd), fight.manaStart,
            died ? 0 : ManaPercent(player), died, getMSTimeDiff(fight.startedAt, getMSTime()) / IN_MILLISECONDS, fight.foeEntry,
            fight.oneKind && fight.seen.size() == fight.foes, fight.firstLossAt != 0,
            fight.seen.size() == fight.foes ? fight.kinds : std::vector<std::pair<uint32, uint8>>());
    // Its own pull brought two or more foes than it expected and it lived: its count does not hold at this place (a
    // creature that runs for help, a camp beyond its sight), and the next pull there would be counted the same way. The
    // place is set aside for a while. Again (60) pulled in Zul'Mashar "1 expected, 1 more possible", three came and it
    // won at 67 %; eleven seconds later its next pull there, counted the same way, brought four and it died, round 229.
    constexpr uint8 SurpriseFoes = 2;
    constexpr uint32 SurpriseAvoidMs = 20 * MINUTE * IN_MILLISECONDS;
    if (!died && fight.expected && fight.foes >= fight.expected + SurpriseFoes && !player->GetMap()->IsDungeon())
    {
        rt.AddAvoidArea(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), uint8(player->GetLevel()), SurpriseAvoidMs, false);
        LOG_INFO(ActivityLog, "activity: {} sets the place of its pull aside for {} min: it expected {} foe{} and {} came", rt.name,
            SurpriseAvoidMs / (MINUTE * IN_MILLISECONDS), uint32(fight.expected), fight.expected == 1 ? "" : "s", uint32(fight.foes));
    }
    if (!GetSettings().activityLogFights)
        return;

    CreatureTemplate const* foe = fight.foeEntry ? sObjectMgr->GetCreatureTemplate(fight.foeEntry) : nullptr;
    // A form the bot went into the fight with that it cannot fight in (FormName), and how far it got with a target
    // it did not kill.
    char const* form = FormName(fight.form);
    LOG_INFO(ActivityLog, "activity: {} fight {} s{}{}: {} foe{} at once{}, health {}% -> {}% (lowest {}%), mana {}% -> {}%, {}{}{}{}{}{}{}{}{}",
        rt.name, getMSTimeDiff(fight.startedAt, getMSTime()) / IN_MILLISECONDS,
        fight.water == 2 ? " under water" : (fight.water == 1 ? " in the water" : ""),
        form ? Acore::StringFormat(", begun in {}", form) : std::string(), uint32(fight.foes), fight.foes == 1 ? "" : "s",
        foe ? Acore::StringFormat(" ({} level {})", foe->Name, uint32(fight.foeLevel)) : "", uint32(fight.healthStart),
        uint32(healthEnd), uint32(std::min(fight.healthLow, healthEnd)), uint32(fight.manaStart),
        died ? 0u : uint32(ManaPercent(player)), died ? "died" : (fight.stale ? "ended by the module (nothing happened in it, or its foe did not die)" :
            (left ? "left" : "won")),
        (died || left) ? Acore::StringFormat(", its target at {}% health", uint32(fight.targetHealth)) : std::string(),
        // (Also when fewer came than the pull check expected: that is how its counting is checked.)
        // (A fight against one foe that the bot began as expected says nothing more.)
        fight.foes < 2 && fight.expected + fight.possible < 2 ? std::string(fight.expected ? "" : ", it was attacked") :
            (fight.expected ? Acore::StringFormat(", its own pull ({} expected{})", uint32(fight.expected),
                fight.possible ? Acore::StringFormat(", {} more possible", uint32(fight.possible)) : std::string()) :
                std::string(", it was attacked")),
        fight.selfHeals ? Acore::StringFormat(", healed itself {} time{}", uint32(fight.selfHeals), fight.selfHeals == 1 ? "" : "s") :
            std::string(),
        fight.potion.empty() ? std::string() : ", drank " + fight.potion,
        fight.stepsBack ? Acore::StringFormat(", ran to range {} time{}", uint32(fight.stepsBack), fight.stepsBack == 1 ? "" : "s") :
            std::string(),
        fight.defence.empty() ? std::string() : ", used " + fight.defence,
        fight.wand ? ", used its wand" : "",
        fight.interrupts ? Acore::StringFormat(", interrupted {} cast{}", uint32(fight.interrupts), fight.interrupts == 1 ? "" : "s") :
            std::string(),
        fight.idleNote.empty() ? std::string() : Acore::StringFormat(", no damage done in its first 12 s ({})", fight.idleNote));
}

bool OrderService::StepRest(GuildBot const& bot, Player* player, BotRuntime& rt)
{
    Settings settings = sBotSettings.For(player);
    // The gate of its dungeon group before a boss or a big pull asks for more (OrderService::DungeonGate, package P1;
    // the owner's thresholds: health 90 %, a healer's mana 80 %, a damage dealer's 60 %): it rests from below those values
    // and up to them, eating and drinking, while the gate asks (a few seconds at a time).
    if (rt.gateUntil && getMSTime() < rt.gateUntil)
    {
        settings.restBelowHealth = std::max(settings.restBelowHealth, rt.gateHealth);
        settings.restUntilHealth = std::max(settings.restUntilHealth, rt.gateHealth);
        settings.restBelowMana = std::max(settings.restBelowMana, rt.gateMana);
        settings.restUntilMana = std::max(settings.restUntilMana, rt.gateMana);
    }
    else
        rt.gateUntil = 0;
    // In a dungeon run a member rests with its group, not on its own (package P6b; the owner's E2: the group goes on until
    // somebody is below the trigger, then rests up together, OrderService::StepDungeonGroupRest): only what the group
    // asks starts a rest, and a rest ends when its group fights.
    bool const dungeonGroup = player->GetMap()->IsDungeon() && (bot.order.type == OrderType::Dungeon || rt.squad.following);
    if (dungeonGroup && rt.rest.active && !player->IsInCombat())
        if (Group* party = player->GetGroup())
            for (GroupReference* ref = party->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource(); member && member != player && member->IsInWorld() && member->IsAlive() &&
                    member->IsInCombat() && !member->getAttackers().empty() && member->IsWithinDistInMap(player, 40.0f))
                {
                    EndRest(player, rt, "its group fights");
                    return false;
                }
    // (Unless its group cannot rest: the group's rest is its leader's step, which does not come while the leader lies
    // dead. In the owner's Molten Core raid, 2026-11-27, the shaman that had brought the tank back stood at 7 % mana with
    // nothing to drink in its bags for a minute and a half and only ate: mod-playerbots' own drink finds nothing there,
    // and the one piece out of the guild storage comes with a rest. So it rests by itself while the party's leader or a
    // member near it lies dead and nobody near it fights.)
    if (dungeonGroup && !rt.rest.active && !rt.gateUntil)
    {
        bool groupDown = false;
        Group* party = player->GetGroup();
        if (party && !player->IsInCombat())
        {
            Player* partyLeader = ObjectAccessor::FindPlayer(party->GetLeaderGUID());
            groupDown = !partyLeader || !partyLeader->IsInWorld() || !partyLeader->IsAlive();
            for (GroupReference* ref = party->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == player || !member->IsInWorld() || member->GetMap() != player->GetMap())
                    continue;
                if (member->IsAlive() && member->IsInCombat() && !member->getAttackers().empty() && member->IsWithinDistInMap(player, 40.0f))
                {
                    groupDown = false;
                    break;
                }
                if (!member->IsAlive() && member->IsWithinDistInMap(player, 100.0f))
                    groupDown = true;
            }
        }
        if (!groupDown)
            return false;
    }
    // In an open-world squad no rest while one of it fights within reach and the bot is not badly hurt: it helps (the
    // squad's wait and follow turn it to the foe). A leader rested twice in half a minute, walking 59 and 44 yd away to
    // a quieter place, while its member fought two centaurs alone, could not get away and died, the Barrens, 2026-11-25.
    if (!dungeonGroup && !player->IsInCombat() && (rt.squad.leads || rt.squad.following) && HealthPercent(player) >= 30)
        if (Group* party = player->GetGroup())
            for (GroupReference* ref = party->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource(); member && member != player && member->IsInWorld() && member->IsAlive() &&
                    member->IsInCombat() && !member->getAttackers().empty() && member->IsWithinDistInMap(player, 40.0f))
                {
                    if (rt.rest.active)
                        EndRest(player, rt, "its squad fights");
                    return false;
                }
    BotRuntime::Rest& rest = rt.rest;
    // A following bot keeps up with whoever it follows.
    if (!settings.restEnable || bot.order.type == OrderType::Follow)
    {
        if (rest.active)
            EndRest(player, rt, "moved on");
        return false;
    }

    uint32 const now = getMSTime();
    uint8 const health = HealthPercent(player);
    uint8 const mana = ManaPercent(player);
    // A trip to a vendor for drink is asked for (below) or under way (PlanUpkeep): it does not sit down for its
    // mana meanwhile; standing gives no more mana than walking, and what it needs is at the vendor. Until it has a
    // drink again, or the time is over.
    if (rt.drinkTripUntil && (now >= rt.drinkTripUntil || CountUsable(player, DrinkSpellCategory) > 0))
        rt.drinkTripUntil = 0;
    // (A rest for its health runs as always: the first build cut it short at the start value, and a warlock that
    // taps its life for mana ate six pieces of food in twenty seconds, one per rest.)
    if (rt.drinkTripUntil && !player->IsInCombat() && (rest.active ? !rest.forHealth : health >= settings.restBelowHealth))
    {
        if (rest.active)
            EndRest(player, rt, "moved on");
        return false;
    }
    // What the next fight needs counts too: a bot whose fights against one foe of its level cost it most of what it
    // has rests until it has that much again (a level-21 warlock whose fights cost 80 % went into them at 85 % health
    // and 75 % mana, which is 74 %). A pull that was called off for a rest asks for its own amount (CheckPull).
    if (rt.restWant > 0.0f && now >= rt.restWantUntil)
        rt.restWant = 0.0f;
    float const fightLimit = std::max(0.3f, sBotSettings.GetFloat(player, Setting::RiskMaxFightCostPercent));
    float const want = !sBotSettings.IsOn(player, Setting::RiskEnable) ? 0.0f :
        std::max(rt.restWant, std::min(0.95f, Escape::FoeCost(rt, player->GetLevel(), 0) / fightLimit));
    float const has = Escape::BudgetOf(player);
    // In a dungeon a member of the group that has nothing to drink does not hold its group up for its mana (the
    // leader waits up to two minutes at a time for a member that rests; a hunter at 35 % mana held a group for
    // 120 s, 2026-11-14): it keeps up, its mana comes back as it walks, and a rest for its health runs as always. Not
    // the group's healer, whose mana the group waits for. With a drink in its bags, in the guild storage or with a
    // mage of the group it drinks, which takes 15 to 20 seconds.
    bool const dryInDungeon = rt.squad.following && player->GetMap()->IsDungeon() && UsesMana(player) &&
        sRoleService.EffectiveRole(bot) != DungeonRole::Healer && !CountUsable(player, DrinkSpellCategory) &&
        !StorageSupply::Has(player, StorageSupply::Kind::Drink) && [&]()
        {
            Group* group = player->GetGroup();
            if (!group)
                return true;
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource(); member && member != player && member->getClass() == CLASS_MAGE &&
                    member->IsInWorld() && member->IsAlive())
                    return false;
            return true;
        }();
    // (Only when a rest can give something by the leader's rules: food below his health threshold, drink up to his
    // mana value. A bot at 77 % health and full mana has nothing to gain by sitting down.)
    bool const restGains = health < settings.restBelowHealth ||
        (UsesMana(player) && !dryInDungeon && mana + 5 < settings.restUntilMana);
    bool const shortOfFight = has + 0.10f < want && restGains; // a rest starts
    // Before a walk through hostile ground the bot rests to more than the 65 % it rests at after a fight: a level-17
    // warrior set out at 80 % health for a target 211 yards away through the Riverpaw gnolls, was attacked at 65 %,
    // two became four, and it died. A few seconds with food. The walk: the straight line to the task's target, more
    // than WalkMinYards long, with hostile creatures of about the bot's level along it (the road map's danger; the
    // last yards belong to the target and its pull check). Not in the saddle, once per target, and not while
    // something hostile is near already (that is no place to sit down for it).
    // Its own setting, off by default (the owner, 2026-11-13: [quote left out]: 285 of 646 rests of one live window were these, at 90 %).
    float const WalkWant = float(settings.restBeforeWalk) / 100.0f;
    constexpr float WalkMinYards = 60.0f;
    constexpr float WalkStopBefore = 30.0f;
    constexpr int32 WalkLevelsBelow = 4;
    constexpr uint32 WalkLookMs = 3 * IN_MILLISECONDS;
    bool walkShort = false;
    uint64 const walkKey = !rt.task.hasSpot ? 0 : (uint64(rt.task.spot.spawnId) << 1 | (rt.task.spot.gameObject ? 1 : 0)) ^
        (uint64(uint32(int32(rt.task.spot.x))) << 40) ^ (uint64(uint32(int32(rt.task.spot.y))) << 20);
    if (settings.restBeforeWalk && !rest.active && !player->IsMounted() && !player->IsInCombat() && rt.task.kind != TaskKind::None &&
        rt.task.hasSpot && rt.task.spot.mapId == player->GetMapId() && walkKey != rt.walkRestedFor &&
        sBotSettings.IsOn(player, Setting::RiskEnable))
    {
        if (now >= rt.nextWalkLook)
        {
            rt.nextWalkLook = now + WalkLookMs;
            rt.walkDanger = 0;
            if (player->GetDistance2d(rt.task.spot.x, rt.task.spot.y) > WalkMinYards)
            {
                uint8 const danger = sRoadMap.DangerAlong(player->GetMapId(), player->GetPositionX(), player->GetPositionY(),
                    rt.task.spot.x, rt.task.spot.y, player->GetTeamId(), WalkStopBefore);
                if (danger && int32(danger) + WalkLevelsBelow >= int32(player->GetLevel()))
                    rt.walkDanger = danger;
            }
        }
        walkShort = rt.walkDanger && has < WalkWant && restGains && !Escape::HostilesAround(player, 30.0f);
    }
    // a rest may end
    bool const enoughForFight = has >= std::max(want, (walkShort || (rest.active && rest.forWalk)) ? WalkWant : 0.0f);
    // In the saddle: a journey goes on (the bot rests where it arrives), but a bot that rides to its next fight with
    // little health or mana gets off and rests first. From level 20 the bots ride everywhere; before this rule a
    // mounted bot was never rested, and the AI of a resting bot mounted up after a few seconds ("ended by: moved on" at
    // 3-11 % mana: a level-50 priest then fought on without mana and died).
    if (player->IsMounted())
    {
        bool const travelling = rt.task.kind == TaskKind::Travel || rt.task.kind == TaskKind::Voyage;
        bool const needs = health < settings.restBelowHealth || mana < settings.restBelowMana || shortOfFight;
        if (!rest.active && (travelling || !needs))
            return false;
        if (rest.active && travelling)
        {
            EndRest(player, rt, "moved on");
            return false;
        }
        player->Dismount();
        player->RemoveAurasByType(SPELL_AURA_MOUNTED);
    }
    // The water where it stands, asked from the map (0 dry, 1 in water, 2 head under water).
    uint8 const water = Escape::WaterAt(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());
    if (water == 2)
    {
        if (!rt.underWaterSince)
            rt.underWaterSince = now;
    }
    else
        rt.underWaterSince = 0;
    bool const needsAir = settings.restAirAfterSeconds && rt.underWaterSince &&
        getMSTimeDiff(rt.underWaterSince, now) >= settings.restAirAfterSeconds * IN_MILLISECONDS;
    // No place to sit down: water deeper than a character can sit in. By the depth under the bot, not by its feet:
    // the path finder keeps a bot at the surface of deep water, and there `water` is 0 by centimetres. (The owner,
    // 2026-11-14: [quote left out]: his paladin began a rest floating on 1.7 yards of swamp water, and the "quieter place" a rest walks to
    // could lie on the water as well.) Shallow water is ground.
    float const depth = Escape::WaterDepth(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());
    bool const wet = water == 2 || depth > Escape::RestWaterYards;

    // A mage keeps a small stock of water and food it conjures itself (the owner, 2026-11-13: [quote left out]): with fewer than Provisions.ConjureBelow pieces it stops
    // where nothing hostile is near and casts, up to twice that. What it conjures next (0: nothing):
    constexpr uint32 ConjureFood = 587;
    constexpr uint32 ConjureWater = 5504;
    constexpr float ConjureQuietYards = 25.0f;
    constexpr uint32 ConjureRetryMs = 15 * IN_MILLISECONDS;
    uint32 conjureSpell = 0;
    char const* conjureWhat = "";
    if (player->getClass() == CLASS_MAGE && settings.provisionsConjureBelow && now >= rt.nextConjure && !wet)
    {
        uint32 const stock = settings.provisionsConjureBelow * (rest.active && rest.conjuring ? 2 : 1);
        uint32 const waterSpell = KnownRank(player, ConjureWater);
        uint32 const foodSpell = KnownRank(player, ConjureFood);
        if (waterSpell && CountUsable(player, DrinkSpellCategory) < stock)
        {
            conjureSpell = waterSpell;
            conjureWhat = "water";
        }
        else if (foodSpell && CountUsable(player, FoodSpellCategory) < stock)
        {
            conjureSpell = foodSpell;
            conjureWhat = "food";
        }
    }

    // Right after a run from a fight only a place where nothing hostile stands is a place to rest: a shaman got
    // away from two Venture Co. creatures at 52 % health in the Barrens, rested 22 yd on "where 2 stood instead of
    // 5", was attacked by three there 8 s later and died on its next run, round 226.
    constexpr uint32 AfterRunMs = 90 * IN_MILLISECONDS;
    constexpr float AfterRunYards = 120.0f;
    bool const afterRun = !rt.escapeMarks.empty() && rt.escapeMarks.back().mapId == player->GetMapId() &&
        getMSTimeDiff(rt.escapeMarks.back().at, now) <= AfterRunMs &&
        player->GetExactDist2d(rt.escapeMarks.back().x, rt.escapeMarks.back().y) <= AfterRunYards;
    if (!rest.active)
    {
        // Asked for once (a login inside a camp); a bot that cannot rest now (in the water) just goes on.
        bool const retreat = rt.loginRetreat;
        rt.loginRetreat = false;
        bool const otherReason = health < settings.restBelowHealth || (mana < settings.restBelowMana && !dryInDungeon) || needsAir ||
            shortOfFight || retreat || walkShort;
        // A stop only to conjure: not on a ship, not while it leads or follows a quest's NPC, and only where nothing
        // hostile is near.
        bool conjureDue = !otherReason && conjureSpell && !player->IsInCombat() && !player->GetTransport() &&
            rt.task.kind != TaskKind::Escort && rt.task.kind != TaskKind::Event && rt.task.kind != TaskKind::Voyage;
        if (conjureDue && Escape::HostilesAround(player, ConjureQuietYards))
        {
            conjureDue = false;
            rt.nextConjure = now + ConjureRetryMs / 3;
        }
        if (!otherReason && !conjureDue)
            return false;
        // Not while it walks back the way it came (StartTrailOut): it rests at the end of that walk, not halfway.
        if (!rt.trailOut.empty() && !needsAir)
            return false;
        // Not in a hostile ground effect: the step out of it comes first (StepGroundEffect). The owner's rogue began a rest in
        // the Clouds of Disease of three Diseased Ghouls in Scholomance once a second, each ended by the cloud, and went
        // from 57 % to 7 % health in 17 s out of any fight, 2026-11-26.
        if (!BotDriver::GroundEffectOn(player).empty())
            return false;
        // Nobody sits down in the water, swimming, floating at the surface or under it: a wet bot goes to dry ground
        // first (the shore, a pier; the quietest spot the path finder has a whole way to), near or up to
        // ShoreSearchYards away, and rests there. With none in reach it goes on with its task and looks again every
        // few seconds (the search is two dozen paths).
        float x = 0.0f, y = 0.0f, z = 0.0f;
        uint32 here = 0, there = 0;
        bool dryLand = false;
        Movement::PointsArray shoreWay;
        // While a rest is due here and there is no place for it, it starts no fight (Escape::CheckPull holds the pull):
        // it waits for its health and mana where it stands. Mahiri, a level-21 shaman, "found no dry ground" among the
        // naga of the Zoram Strand at 25 % mana, went on, fought on to 2 % and pulled again at 4 %: two naga, dead,
        // round 242.
        constexpr uint32 NoPlaceHoldMs = 8 * IN_MILLISECONDS;
        if (wet)
        {
            if (now < rt.nextDryLook)
            {
                rt.restNoPlaceUntil = now + NoPlaceHoldMs;
                return false;
            }
            dryLand = Escape::FindRestSpot(player, rt, AirSearchYards / 1.5f, x, y, z, here, there, false, &shoreWay) ||
                Escape::FindRestSpot(player, rt, ShoreSearchYards / 1.5f, x, y, z, here, there, false, &shoreWay);
            if (!dryLand)
            {
                rt.restNoPlaceUntil = now + NoPlaceHoldMs;
                rt.nextDryLook = now + ShoreLookMs;
                if (now >= rt.nextDryNote)
                {
                    rt.nextDryNote = now + ShoreNoteMs;
                    LOG_INFO(ActivityLog, "activity: {} needs a rest (health {}%, mana {}%) in {:.1f} yd of water at ({:.0f}, {:.0f}, {:.0f}) and "
                        "finds no dry ground within {:.0f} yd: it does not rest in the water, starts no fight meanwhile and goes on", rt.name, uint32(health),
                        uint32(mana), depth, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), ShoreSearchYards);
                }
                return false;
            }
        }

        rest = BotRuntime::Rest();
        rest.active = true;
        rest.startedAt = now;
        rest.healthStart = health;
        rest.manaStart = mana;
        rest.usesMana = UsesMana(player);
        rest.forHealth = health < settings.restBelowHealth;
        rest.forMana = rest.usesMana && mana < settings.restBelowMana && !rt.drinkTripUntil && !dryInDungeon;
        rest.whyBefore = rt.why;
        rest.forAir = needsAir && health >= settings.restBelowHealth && mana >= settings.restBelowMana;
        if (walkShort)
        {
            rest.forWalk = true;
            rest.walkNote = Acore::StringFormat(", before a walk of {:.0f} yd past level-{} creatures",
                player->GetDistance2d(rt.task.spot.x, rt.task.spot.y), uint32(rt.walkDanger));
            rt.walkRestedFor = walkKey;
        }
        // Badly hurt and with food in the bags it eats where it stands (a few seconds) instead of walking to a quieter
        // place first: a rogue at 30 % health walked 59 yards through a camp and was attacked on the way.
        constexpr uint8 EatFirstBelowHealth = 50;
        // (A bot that entered the world inside a camp walks first, whatever its health: it would eat among them. So
        // does one that has just run from a fight: what it ran from stands around it.)
        if (!wet && health < EatFirstBelowHealth && !retreat && !afterRun &&
            (FindConsumable(player, FoodSpellCategory) || StorageSupply::Has(player, StorageSupply::Kind::Food)))
            rest.placeChecked = true;
        // In a dungeon it rests where it stands, beside its group (the owner, 2026-11-14: [quote left out]sicheres[quote left out]).
        if (!wet && player->GetMap()->IsDungeon())
        {
            rest.placeChecked = true;
            if (uint32 const hostiles = Escape::HostilesAround(player, 30.0f))
                rest.place = Acore::StringFormat(", where it stood in the dungeon ({} hostile creatures within 30 yd)", hostiles);
        }
        // A stop only to conjure is a few seconds where it stands (nothing hostile is near: looked at above).
        if (conjureDue)
        {
            rest.placeChecked = true;
            rest.onlyConjure = true;
        }
        player->StopMoving();
        Reconcile(bot, player, rt); // the hold profile, now
        if (dryLand)
        {
            // Out of the water first (twice the usual time, or what the way takes: it swims).
            rest.placeChecked = true;
            rest.walkUntil = now + std::max<uint32>(2 * RestWalkMaxMs,
                uint32(player->GetDistance2d(x, y) / SwimYardsPerSecond) * IN_MILLISECONDS + RestWalkMaxMs / 2);
            rest.walkX = x;
            rest.walkY = y;
            rest.walkZ = z;
            rest.place = Acore::StringFormat(", went {:.0f} yd ashore first{}{}", player->GetDistance2d(x, y),
                needsAir ? " (under water too long)" : "", shoreWay.size() >= 2 ? " (up and along the surface)" : "");
            rt.restNoPlaceUntil = 0;
            player->GetMotionMaster()->Clear();
            // From under the surface: the way the path finder gave from the surface above it, up and along (a point
            // movement from down there is a straight line through the sea floor's slope).
            if (shoreWay.size() >= 2)
                player->GetMotionMaster()->MoveSplinePath(&shoreWay, FORCED_MOVEMENT_RUN);
            else
                player->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_RUN);
        }
    }

    // Not in a camp or where something hostile walks about: a few steps away first, once per rest.
    if (!rest.placeChecked)
    {
        rest.placeChecked = true;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        uint32 here = 0, there = 0;
        // In a camp that reaches farther than the usual search (a bot that entered the world among gnolls sat down
        // to eat there), the search goes farther once.
        bool found = settings.restSaferPlaceYards > 0.0f &&
            (Escape::FindRestSpot(player, rt, settings.restSaferPlaceYards, x, y, z, here, there, afterRun) ||
                (here && Escape::FindRestSpot(player, rt, settings.restSaferPlaceYards * 2.5f, x, y, z, here, there, afterRun)));
        // No such place in reach: back the way it came, out of the place, before it sits down.
        if (!found && afterRun && here && !player->GetMap()->IsDungeon() &&
            StartTrailOut(player, rt, Acore::StringFormat("it ran from a fight a moment ago and {} hostile creatures stand within 30 yd: "
                "it rests farther back", here)))
        {
            EndRest(player, rt, "moved on");
            return true;
        }
        if (found)
        {
            rest.walkUntil = now + RestWalkMaxMs + (player->GetDistance2d(x, y) > settings.restSaferPlaceYards ? RestWalkMaxMs / 2 : 0);
            rest.walkX = x;
            rest.walkY = y;
            rest.walkZ = z;
            rest.place = Acore::StringFormat(", walked {:.0f} yd away {} ({} hostile creatures near, {} there)",
                player->GetDistance2d(x, y), rest.relocations ? "when something came near" : "first", here, there);
            if (!player->IsStandState())
                player->SetStandState(UNIT_STAND_STATE_STAND);
            player->GetMotionMaster()->Clear();
            player->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_RUN);
        }
    }
    if (rest.walkUntil)
    {
        float const dx = player->GetPositionX() - rest.walkX;
        float const dy = player->GetPositionY() - rest.walkY;
        if (now < rest.walkUntil && dx * dx + dy * dy > 4.0f * 4.0f)
        {
            // mod-playerbots must not turn it round on the way.
            BotDriver::PauseAI(player, 2 * std::max(TickMs, rt.stepGapMs));
            rt.why = wet ? "Getting out of the water to rest" : "Resting: walking to a quieter place first";
            if (rt.task.kind != TaskKind::None)
            {
                rt.task.startedAt += rt.stepGapMs;
                if (rt.task.nearSince)
                    rt.task.nearSince += rt.stepGapMs;
            }
            PauseStuck(rt);
            return true;
        }
        // Stopped within 4 yd of the dry place and still in the water: the last yards on to it, once. (Sunwhisper,
        // 2026-11-24, round 212: its rest walked 26 yd ashore at a lake in Ashenvale, ended "still in the water" short of
        // the shore, and the priest went on to its next quest target at 44 % mana and died there in a fight it lost with
        // the creature at 7 %.)
        if (wet && !rest.walkFinal && dx * dx + dy * dy <= 4.0f * 4.0f)
        {
            rest.walkFinal = true;
            rest.walkUntil = now + 4 * IN_MILLISECONDS;
            rest.walkX = rest.walkX + (rest.walkX - player->GetPositionX()) * 0.5f;
            rest.walkY = rest.walkY + (rest.walkY - player->GetPositionY()) * 0.5f;
            float z = rest.walkZ;
            player->UpdateAllowedPositionZ(rest.walkX, rest.walkY, z);
            player->GetMotionMaster()->Clear();
            player->GetMotionMaster()->MovePoint(0, rest.walkX, rest.walkY, z, FORCED_MOVEMENT_RUN);
            BotDriver::PauseAI(player, 2 * std::max(TickMs, rt.stepGapMs));
            return true;
        }
        rest.walkUntil = 0;
        player->StopMoving();
        // It did not get out: no rest in the water (it goes on, and tries again).
        if (wet)
        {
            rt.nextDryLook = now + ShoreLookMs;
            EndRest(player, rt, "still in the water");
            return false;
        }
    }
    // A rest only for air is over as soon as the bot stands on dry land.
    if (rest.forAir)
    {
        EndRest(player, rt, "breathed");
        return false;
    }

    // Something hostile walks up (a patrol, a wanderer, a respawn): the bot moves its rest before it is attacked
    // sitting down with half its health, twice per rest at most.
    constexpr uint32 RestWatchMs = 2 * IN_MILLISECONDS;
    constexpr uint8 MaxRelocations = 2;
    if (now >= rest.nextWatch && rest.relocations < MaxRelocations && settings.restSaferPlaceYards > 0.0f && !wet &&
        !player->GetMap()->IsDungeon())
    {
        rest.nextWatch = now + RestWatchMs;
        if (Escape::ThreatApproaching(player))
        {
            ++rest.relocations;
            rest.placeChecked = false; // the place is looked for again (it stands up only if there is a better one)
        }
    }

    bool const eating = IsTaking(player, SPELL_AURA_MOD_REGEN);
    bool const drinking = IsTaking(player, SPELL_AURA_MOD_POWER_REGEN);
    // Food is for health below the leader's threshold, drink for mana below his: each on its own. (Once a rest ran,
    // both went up to their "until" values: a rest begun for health at 60 % drank at 80 % mana. In 94 minutes on the
    // integration server 170 rests drank although they had begun at 40 % mana or more, against 77 below it.)
    if (health < settings.restBelowHealth)
        rest.forHealth = true;
    // (On its way to a vendor for drink a rest is for health alone: the mana comes at the vendor.)
    if (rest.usesMana && mana < settings.restBelowMana && !rt.drinkTripUntil && !dryInDungeon)
        rest.forMana = true;
    else if (dryInDungeon && rest.forMana)
        rest.forMana = false;
    // A rest that was only for mana is over when the group would wait for it without a drink (dryInDungeon).
    if (dryInDungeon && !rest.forHealth && !rest.forWalk && !rest.forAir && !rest.onlyConjure && rt.restWant <= 0.0f)
    {
        EndRest(player, rt, "nothing to drink: it keeps up with its group");
        return false;
    }
    // A rest for a fight (a pull asked for it, or the bot's own fights cost most of what it has) ends when the bot has
    // enough, or when it has all a rest gives by the leader's rules: mana at his value, and health too when the rest
    // is for health. It does not wait above his health threshold for health it is not allowed to eat for.
    bool const nothingMore = (!rest.usesMana || dryInDungeon || mana >= settings.restUntilMana) &&
        (!rest.forHealth || health >= settings.restUntilHealth);
    bool const rested = (!rest.forHealth || health >= settings.restUntilHealth) && (!rest.forMana || mana >= settings.restUntilMana) &&
        (enoughForFight || nothingMore || rt.drinkTripUntil) && !conjureSpell;
    // Without food and drink the bot's own regeneration has to do, up to the rest's targets (a mage without money went
    // on after 60 seconds at 56-68 % mana, pulled two foes and died without mana). After three times MaxSeconds it
    // goes on once it is above the values a rest starts at (below them it would only start again).
    constexpr uint32 UnfedRestFactor = 3;
    bool const timeOver = !eating && !drinking &&
        getMSTimeDiff(rest.startedAt, now) >= settings.restMaxSeconds * UnfedRestFactor * IN_MILLISECONDS &&
        health >= settings.restBelowHealth && mana >= settings.restBelowMana;
    if (rested || timeOver)
    {
        EndRest(player, rt, rested ? "rested" : "time over");
        // What a pull had asked for is served by this rest (it held for three minutes, for every rest in them).
        rt.restWant = 0.0f;
        return false;
    }

    // Eat and drink: what the bags hold, or one piece out of the guild storage at this moment when that has a better
    // kind or the bags none (StorageSupply::TakeOne; the owner, 2026-11-13: [quote left out]). A druid in a form or a silenced bot simply waits. An Auto Shot or wand that still
    // runs ends here (a rest is out of any fight): it counts as a cast, and a hunter sat out 155 s without its drink
    // (StopLeftoverAutoShot).
    if (player->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
        player->InterruptSpell(CURRENT_AUTOREPEAT_SPELL);
    if (now >= rest.nextUse && !player->IsNonMeleeSpellCast(false) && !player->isMoving())
    {
        // A mage's stock of water and food comes first: a cast would end its drinking.
        if (conjureSpell && !eating && !drinking)
        {
            if (!player->IsStandState())
                player->SetStandState(UNIT_STAND_STATE_STAND);
            SpellInfo const* info = sSpellMgr->GetSpellInfo(conjureSpell);
            if (player->CastSpell(player, conjureSpell, false) == SPELL_CAST_OK)
            {
                rest.nextUse = now + std::max<uint32>(info ? info->CalcCastTime(player) : 3000, 500) + 400;
                rest.conjuring = true;
                if (rest.conjured.find(conjureWhat) == std::string::npos)
                    rest.conjured += (rest.conjured.empty() ? "" : " and ") + std::string(conjureWhat);
            }
            else
                // No mana for it, silenced, no room in its bags: it rests on and tries later (much later when it had
                // stopped only for this).
                rt.nextConjure = now + ConjureRetryMs * (rest.onlyConjure ? 8 : 1);
        }
        // Food only for health below the leader's threshold (rest.forHealth), in a rest for a fight too (the owner,
        // 2026-11-14: [quote left out]: a rest that a pull had asked for ate at 77 % health; "not enough for the fight" had
        // been a reason to eat at any health below 100 %).
        else if (rest.forHealth && health < settings.restUntilHealth && !eating)
        {
            // A character with First Aid bandages itself first, once per rest (the owner, 2026-11-13: [quote left out]): six to eight seconds, then the food does the rest.
            constexpr uint32 RecentlyBandaged = 11196;
            Item* const bandage = rest.bandage.empty() && rest.forHealth && !drinking && player->HasSkill(SKILL_FIRST_AID) &&
                !player->HasAura(RecentlyBandaged) ? StorageSupply::TakeOne(player, StorageSupply::Kind::Bandage, "to bandage itself") : nullptr;
            if (bandage)
            {
                SpellInfo const* info = sSpellMgr->GetSpellInfo(bandage->GetTemplate()->Spells[0].SpellId);
                rest.bandage = bandage->GetTemplate()->Name1;
                if (!player->IsStandState())
                    player->SetStandState(UNIT_STAND_STATE_STAND);
                UseOnSelf(player, bandage, true);
                rest.nextUse = now + uint32(std::clamp<int32>(info ? info->GetDuration() : 8000, 3000, 10000)) + 500;
            }
            else if (Item* food = StorageSupply::TakeOne(player, StorageSupply::Kind::Food, "to eat"))
            {
                rest.ate = food->GetTemplate()->Name1;
                UseOnSelf(player, food);
                rest.nextUse = now + RestUseGapMs;
                for (BotRuntime::Tally* tally : { &rt.window, &rt.total })
                    ++tally->foodUsed;
            }
            // A mage makes its own food.
            else if (uint32 const conjure = player->getClass() == CLASS_MAGE && !drinking ? KnownRank(player, 587) : 0;
                conjure && player->CastSpell(player, conjure, false) == SPELL_CAST_OK)
                rest.nextUse = now + 3500;
            // Without food a character that can heal does (a paladin sat out its wounds with full mana). Not while it
            // drinks: the cast makes it stand up, which ends the drink, and the next drink would be another piece.
            else if (uint32 const spell = GetSettings().selfHealBelow && mana >= 30 && !drinking ? SelfHealSpell(player, false) : 0)
            {
                if (!player->IsStandState())
                    player->SetStandState(UNIT_STAND_STATE_STAND);
                if (uint32 const castMs = CastSelfHeal(player, spell))
                {
                    rest.nextUse = now + castMs + 300;
                    if (rest.ate.empty())
                        rest.ate = "nothing (healed itself)";
                }
            }
        }
        // A drink although mana is above the leader's threshold: at once when a pull waits for the rest (the pull
        // check said what the fight needs: "waits with ...: it rests first"); else only once the health is back and
        // the bot is still short (a rest begun for health at 48 % drank at 49 % mana because health and mana together
        // were too little for a fight, which the food alone would have mended: a mage, three times in one round).
        // And never above the mana a rest ends at: a warlock at 68 % health and 92 % mana drank for a pull that had
        // asked for a rest two minutes earlier (the health was what it lacked).
        // (A rest that is not for health has no food to wait for: it drinks at once.)
        bool const drinkForFight = !enoughForFight && mana < settings.restUntilMana &&
            (rt.restWant > 0.0f || !rest.forHealth || health >= settings.restUntilHealth);
        if (((rest.forMana && mana < settings.restUntilMana) || drinkForFight) && !drinking && now >= rest.nextUse)
        {
            if (Item* drink = StorageSupply::TakeOne(player, StorageSupply::Kind::Drink, "to drink"))
            {
                rest.drank = drink->GetTemplate()->Name1;
                UseOnSelf(player, drink);
                rest.nextUse = now + RestUseGapMs;
                for (BotRuntime::Tally* tally : { &rt.window, &rt.total })
                    ++tally->drinkUsed;
            }
            // The mage of its squad has water: a handful changes bags, as between two players (a shaman that led a
            // mage through Dustwallow drank the guild storage dry, a piece at a time, and then sat out its mana for
            // 100 to 190 seconds in every five minutes beside a mage that carried twelve conjured waters).
            else if ([&]()
                {
                    constexpr float GiveYards = 40.0f;
                    constexpr uint32 Pieces = 4;
                    constexpr uint32 MageKeeps = 2;
                    Group* group = player->GetGroup();
                    if (!group || player->getClass() == CLASS_MAGE)
                        return false;
                    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                    {
                        Player* mage = ref->GetSource();
                        if (!mage || mage == player || mage->getClass() != CLASS_MAGE || !mage->IsInWorld() || !mage->IsAlive() ||
                            mage->GetMapId() != player->GetMapId() || !player->IsWithinDistInMap(mage, GiveYards) ||
                            _runtime.find(mage->GetGUID()) == _runtime.end())
                            continue;
                        // Its best drink that the thirsty one can use: what it conjured first, else what it bought (it
                        // conjures its own again when it runs low).
                        Item* best = nullptr;
                        auto consider = [&](Item* item)
                        {
                            ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
                            if (!proto || proto->Spells[0].SpellCategory != DrinkSpellCategory || !proto->Spells[0].SpellId ||
                                proto->RequiredLevel > player->GetLevel() || item->IsSoulBound())
                                return;
                            ItemTemplate const* has = best ? best->GetTemplate() : nullptr;
                            if (!has || (proto->IsConjuredConsumable() && !has->IsConjuredConsumable()) ||
                                (proto->IsConjuredConsumable() == has->IsConjuredConsumable() && proto->RequiredLevel > has->RequiredLevel))
                                best = item;
                        };
                        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
                            consider(mage->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
                        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
                            if (Bag* bag = mage->GetBagByPos(bagSlot))
                                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                                    consider(bag->GetItemByPos(uint8(slot)));
                        if (!best)
                            continue;
                        uint32 const entry = best->GetEntry();
                        std::string const name = best->GetTemplate()->Name1;
                        uint32 const has = mage->GetItemCount(entry);
                        uint32 const give = std::min(Pieces, has > MageKeeps ? has - MageKeeps : 0);
                        ItemPosCountVec dest;
                        if (!give || player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, entry, give) != EQUIP_ERR_OK)
                            continue;
                        mage->DestroyItemCount(entry, give, true);
                        player->StoreNewItem(dest, entry, true);
                        LOG_INFO(ActivityLog, "activity: {} got {} x {} from {}, the mage of its squad, to drink ({} left with the mage)",
                            rt.name, give, name, mage->GetName(), mage->GetItemCount(entry));
                        return true;
                    }
                    return false;
                }())
                rest.nextUse = now + 500;
            // A mage makes its own water (it costs mana once and gives back many times that): a mage without water
            // rested 85 seconds after every fight.
            else if (uint32 const conjure = player->getClass() == CLASS_MAGE ? KnownRank(player, 5504) : 0)
            {
                if (!player->IsStandState())
                    player->SetStandState(UNIT_STAND_STATE_STAND);
                if (player->CastSpell(player, conjure, false) == SPELL_CAST_OK)
                    rest.nextUse = now + 3500;
            }
            // Nothing to drink anywhere: none in its bags, none in the guild storage, no mage beside it. It does not
            // sit its mana out (the guild leader rested 180 s, 93 s and 106 s in ten minutes, with 37 gold on it):
            // it goes to a vendor now (the owner, 2026-11-14: [quote left out]). Its task ends, the planner's next look finds the
            // vendor (PlanUpkeep; when none is in reach it does not ask again for ten minutes). Not a squad member
            // (it follows), not in a dungeon or on a dungeon order, not on a ship, not with a quest's NPC in tow.
            // (A hand-in or a pickup it is busy with is finished first: what stands next to the bot comes before a
            // trip to a vendor in the planner too. The first build ended such a task every second: the planner gave
            // it back each time.)
            else if (player->getClass() != CLASS_MAGE && settings.provisionsTripWhenOut && rest.forMana && now >= rt.nextRestock &&
                !rt.drinkTripUntil && !rt.squad.following && bot.order.type != OrderType::Dungeon && !player->GetMap()->IsDungeon() &&
                !player->GetTransport() && rt.task.kind != TaskKind::Vendor && rt.task.kind != TaskKind::Voyage &&
                rt.task.kind != TaskKind::Escort && rt.task.kind != TaskKind::Event && player->GetLevel() >= settings.provisionsMinLevel &&
                player->GetMoney() >= settings.provisionsMinMoney && !StorageSupply::HasDrink(player))
            {
                constexpr uint32 DrinkTripAskMs = 3 * MINUTE * IN_MILLISECONDS;
                LOG_INFO(ActivityLog, "activity: {} has nothing to drink at {}% mana (none in its bags, none in the guild storage): it "
                    "goes to a vendor now", rt.name, uint32(mana));
                rt.drinkTripUntil = std::max<uint32>(now + DrinkTripAskMs, 1);
                EndRest(player, rt, "no drink: to a vendor");
                if (rt.task.kind != TaskKind::None && rt.task.kind != TaskKind::TurnIn && rt.task.kind != TaskKind::Pickup)
                    ClearTask(rt, 0, "out of drink");
                return false;
            }
        }
    }

    // Nothing may make it stand up or ride off (a buff, a look at some loot, its mount); an attack wakes the AI at once.
    BotDriver::PauseAI(player, 2 * std::max(TickMs, rt.stepGapMs));
    if (!eating && !drinking && player->IsStandState() && rest.ate.empty() && rest.drank.empty() && !player->isMoving() &&
        !player->IsNonMeleeSpellCast(false) && !conjureSpell)
        // Sitting regenerates faster than standing.
        player->SetStandState(UNIT_STAND_STATE_SIT);

    rt.why = conjureSpell ? "Resting: conjuring water and food" :
        rest.ate.empty() && rest.drank.empty() ? (rest.bandage.empty() ? "Resting: catching its breath" : "Resting: bandaging itself") :
        (rest.drank.empty() ? "Resting: eating" : (rest.ate.empty() ? "Resting: drinking" : "Resting: eating and drinking"));

    // The task and its clocks wait.
    if (rt.task.kind != TaskKind::None)
    {
        rt.task.startedAt += rt.stepGapMs;
        if (rt.task.nearSince)
            rt.task.nearSince += rt.stepGapMs;
    }
    PauseStuck(rt);
    return true;
}

void OrderService::EndRest(Player* player, BotRuntime& rt, char const* outcome)
{
    BotRuntime::Rest const rest = rt.rest;
    rt.rest = BotRuntime::Rest();
    if (!rest.active)
        return;

    uint32 const seconds = getMSTimeDiff(rest.startedAt, getMSTime()) / IN_MILLISECONDS;
    for (BotRuntime::Tally* tally : { &rt.window, &rt.total })
    {
        ++tally->rests;
        tally->restSeconds += seconds;
    }
    if (player->IsAlive() && !player->IsStandState() && !player->IsInCombat())
        player->SetStandState(UNIT_STAND_STATE_STAND);
    rt.why = rest.whyBefore;
    // Rests that had no food for the health or no drink for the mana they were for, in a row (PlanUpkeep sends the
    // bot to a vendor after three: a paladin that had never passed a hub rested 37 of 80 minutes, "no food, no
    // drink", up to 180 s a time).
    constexpr uint32 UnfedMinSeconds = 15;
    Settings const restSettings = sBotSettings.For(player);
    bool const forHealth = rest.healthStart < restSettings.restBelowHealth;
    bool const forMana = rest.usesMana && rest.manaStart < restSettings.restBelowMana;
    if ((forHealth && rest.ate.empty() && rest.bandage.empty()) || (forMana && rest.drank.empty()))
    {
        if (seconds >= UnfedMinSeconds && rt.unfedRests < 255)
            ++rt.unfedRests;
    }
    else if (forHealth || forMana)
        rt.unfedRests = 0;

    if (GetSettings().activityLogFights)
        LOG_INFO(ActivityLog, "activity: {} rested {} s: health {}% -> {}%{}{}{}{}{}{}{}, ended by: {}", rt.name, seconds,
            uint32(rest.healthStart), uint32(HealthPercent(player)),
            rest.usesMana ? Acore::StringFormat(", mana {}% -> {}%", uint32(rest.manaStart), uint32(ManaPercent(player))) : "",
            rest.bandage.empty() ? std::string() : Acore::StringFormat(", bandaged itself with {}", rest.bandage),
            rest.ate.empty() ? ", no food" : Acore::StringFormat(", ate {}", rest.ate),
            !rest.usesMana ? "" : (rest.drank.empty() ? ", no drink" : Acore::StringFormat(", drank {}", rest.drank)),
            rest.conjured.empty() ? std::string() : Acore::StringFormat(", conjured {}", rest.conjured), rest.place,
            rest.walkNote, outcome);
}
