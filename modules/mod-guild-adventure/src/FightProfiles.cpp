/*
 * FightProfiles (FightProfiles.h, docs/fight-profiles.md): the strategies "ga rotation" and "ga buffs", their triggers
 * and actions, conditions and targets, the module's bookkeeping of which character plays which profile, the fallback
 * of FC9 and the counting of casts. The commands are FightProfileCommands.cpp.
 */

#include "FightProfiles.h"
#include "AiFactory.h"
#include "AiObjectContext.h"
#include "BotDriver.h"
#include "BotSettings.h"
#include "CharacterCache.h"
#include "Creature.h"
#include "Engine.h"
#include "GenericSpellActions.h"
#include "Group.h"
#include "GuildAdventureMgr.h"
#include "GuildEvents.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Timer.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace FP = GuildAdventure::FightProfiles;
namespace BotDriver = GuildAdventure::BotDriver;

namespace
{
    constexpr char const* ActivityLog = "module.ga.activity";
    constexpr uint32 FallbackMs = 10 * IN_MILLISECONDS;  // FC9
    constexpr float GroupReach = 40.0f;                  // who counts as a member near the character

    // The two engines a list can stand in: the fight (its rotation) and the time between fights (its buffs, FC2).
    struct EngineInfo
    {
        ::BotState state;
        char const* strategy;      // the profile's strategy in that engine
        char const* rulePrefix;    // the names of its rules' triggers in the bot's context
        char const* actionPrefix;  // and of its rules' own actions
        char const* list;          // what the lines call it
    };

    EngineInfo const& Info(uint8 engine)
    {
        static EngineInfo const infos[2] = {
            { BOT_STATE_COMBAT, "ga rotation", "ga rule c", "ga do c", "rotation" },
            { BOT_STATE_NON_COMBAT, "ga buffs", "ga rule n", "ga do n", "buffs" } };
        return infos[engine ? 1 : 0];
    }

    // The strategies the fight list stands for (AiFactory::AddDefaultCombatStrategies): each talent tree's own, and the
    // class's strategies for several foes, crowd control and cooldowns. Not: targeting and movement ("dps assist",
    // "tank assist", "pull", "pull back", "behind", "tank face", "formation"), buffs, the shaman's totems (the module
    // keeps them, BotDriver::KeepTotems), cures ("cure" stays, docs/fight-profiles.md section 4.3) and the generic ones
    // ("racials", "chat", "default", "cast time", "potions", "duel", "save mana", "healer dps", "avoid aoe").
    std::unordered_set<std::string> const& RotationSet()
    {
        static std::unordered_set<std::string> const set = {
            // the talent trees
            "arms", "fury", "prot", "holy heal", "disc", "shadow", "arcane", "fire", "frostfire", "frost", "ret",
            "holy", "balance", "resto", "cat", "bear", "bm", "mm", "surv", "combat", "assassin", "affli", "demo",
            "destro", "ele", "enh", "blood", "unholy",
            // what a tree adds to them
            "shadow debuff", "shadow aoe", "frost aoe", "unholy aoe", "holy dps", "caster aoe", "meta melee",
            "curse of agony", "curse of elements", "feral charge", "tranquility",
            // the class's rows for several foes, crowd control and cooldowns
            "aoe", "cc", "boost" };
        return set;
    }

    // The strategies of the non-combat engine the buff list stands for (AiFactory::AddDefaultNonCombatStrategies): the
    // class's own ("nc": its buffs among other rows), the party buffs ("buff"), armour, auras and resistances. Not the
    // paladin's blessings ("bmight", "bwisdom", "bkings", "bsanc": the module's buff plan steers them in a group), cures
    // and the generic ones ("food", "loot", "mount" ...). Rows that are no buffs stay as they are (FC2).
    std::unordered_set<std::string> const& BuffSet()
    {
        static std::unordered_set<std::string> const set = { "nc", "buff", "bmana", "bdps", "bcast", "baoe", "bthreat",
            "barmor", "rshadow", "rfire", "rfrost", "rnature" };
        return set;
    }

    std::unordered_set<std::string> const& StandSet(uint8 engine)
    {
        return engine ? BuffSet() : RotationSet();
    }

    // Rows of the replaced strategies a profile does not own (docs/fight-profiles.md sections 4.3, 4.11): movement and
    // target housekeeping, the auto attacks ("melee", a hunter's "auto shot"), what the module's own rules do (the
    // wand, oils, stones, poisons, a caster's step back), and the pets'. ("auto shot" is the default action of the
    // hunter's tree strategies, and no template has it: a new level-1 hunter on the Beast Mastery template stood
    // beside its quest's Young Nightsaber with nothing it could do, Auto Shot gone and Raptor Strike out of reach,
    // 2026-11-28.)
    bool IsModuleRow(std::string const& action)
    {
        static std::unordered_set<std::string> const rows = { "reach spell", "reach melee", "drop target",
            "check mount state", "reset", "set facing", "set behind", "flee", "blink back", "apply oil", "apply stone",
            "apply poison", "shoot", "melee", "auto shot", "avoid aoe", "clean quest log" };
        return rows.count(action) || action.rfind("pet ", 0) == 0 || action.find(" pet") != std::string::npos;
    }

    // The objects a bot's context has made (AiObjectContext keeps the lists protected; a pointer to the member named
    // through a derived class is the language's own way to them, as in BotDriver.cpp).
    struct ContextAccess : AiObjectContext
    {
        static NamedObjectContextList<Strategy>& Strategies(AiObjectContext* context)
        {
            return context->*(&ContextAccess::strategyContexts);
        }
        static NamedObjectContextList<Action>& Actions(AiObjectContext* context)
        {
            return context->*(&ContextAccess::actionContexts);
        }
        static NamedObjectContextList<Trigger>& Triggers(AiObjectContext* context)
        {
            return context->*(&ContextAccess::triggerContexts);
        }
    };

    // Whether a profile owns a row of the template (the rest is emitted as it is).
    bool OwnsRow(uint8 engine, AiObjectContext* context, std::string const& action)
    {
        if (IsModuleRow(action))
            return false;
        if (!engine)
            return true;
        // Between fights only the buffs: "focus magic on party" is cast as a spell, a mana gem is no buff.
        if (action == "focus magic on party")
            return true;
        if (action == "conjure mana gem")
            return false;
        return dynamic_cast<CastBuffSpellAction*>(context->GetAction(action)) != nullptr;
    }

    std::string Lower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return text;
    }

    std::string SpellName(uint32 spellId)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        return info ? std::string(info->SpellName[0]) : Acore::StringFormat("spell {}", spellId);
    }

    // The highest rank of a spell's chain the character knows (0: none).
    uint32 HighestKnown(Player* player, uint32 spellId)
    {
        uint32 known = 0;
        for (uint32 id = sSpellMgr->GetFirstSpellInChain(spellId); id; id = sSpellMgr->GetNextSpellInChain(id))
            if (player->HasSpell(id))
                known = id;
        if (!known && player->HasSpell(spellId))
            known = spellId;
        return known;
    }

    uint32 OnUseSpell(Item const* item)
    {
        if (!item)
            return 0;
        for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
        {
            _Spell const& spell = item->GetTemplate()->Spells[i];
            if (spell.SpellId > 0 && spell.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
                return uint32(spell.SpellId);
        }
        return 0;
    }

    // The item a rule uses: by its entry, or whatever is worn in a trinket slot.
    Item* RuleItem(Player* player, FP::Rule const& rule)
    {
        if (rule.kind == FP::ActionKind::Trinket)
            return player->GetItemByPos(INVENTORY_SLOT_BAG_0, rule.ref == 2 ? EQUIPMENT_SLOT_TRINKET2 : EQUIPMENT_SLOT_TRINKET1);
        return rule.kind == FP::ActionKind::Item ? player->GetItemByEntry(rule.ref) : nullptr;
    }

    Unit* CurrentTarget(AiObjectContext* context)
    {
        Value<Unit*>* value = context->GetValue<Unit*>("current target");
        return value ? value->Get() : nullptr;
    }

    // The members of the character's group near it (itself included), alive and on its map.
    std::vector<Player*> MembersNear(Player* player)
    {
        std::vector<Player*> members;
        Group* group = player->GetGroup();
        if (!group)
        {
            members.push_back(player);
            return members;
        }
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member->IsInWorld() && member->IsAlive() && member->GetMapId() == player->GetMapId() &&
                (member == player || member->IsWithinDistInMap(player, GroupReach)))
                members.push_back(member);
        }
        return members;
    }

    Player* TankOf(Player* player)
    {
        Player* tank = nullptr;
        for (Player* member : MembersNear(player))
        {
            if (PlayerbotAI::IsMainTank(member))
                return member;
            if (!tank && PlayerbotAI::IsTank(member))
                tank = member;
        }
        return tank;
    }

    bool Compare(float measured, FP::CompareOp op, float value)
    {
        switch (op)
        {
            case FP::CompareOp::Less: return measured < value;
            case FP::CompareOp::LessEqual: return measured <= value;
            case FP::CompareOp::Equal: return std::fabs(measured - value) < 0.001f;
            case FP::CompareOp::GreaterEqual: return measured >= value;
            case FP::CompareOp::Greater: return measured > value;
            default: return measured != 0.0f;
        }
    }

    uint32 ParamNumber(FP::Condition const& condition)
    {
        try
        {
            return uint32(std::stoul(condition.param));
        }
        catch (...)
        {
            return 0;
        }
    }

    // What a condition measures for the character (a number; 1 or 0 for a condition without a value). False when it
    // cannot be known (no target, no pet, no mana pool): such a condition does not hold, also when it is turned round.
    bool Measure(PlayerbotAI* ai, Player* bot, AiObjectContext* context, FP::Condition const& condition, Unit* target,
        float& out)
    {
        std::string const& key = condition.key;
        auto flag = [&out](bool value) { out = value ? 1.0f : 0.0f; return true; };
        auto power = [&](Powers type, float divisor) -> bool
        {
            if (!bot->GetMaxPower(type))
                return false;
            out = float(bot->GetPower(type)) / divisor;
            return true;
        };
        if (key == "hp")
            return out = bot->GetHealthPct(), true;
        if (key == "mana")
            return bot->GetMaxPower(POWER_MANA) ? (out = bot->GetPowerPct(POWER_MANA), true) : false;
        if (key == "manapts")
            return power(POWER_MANA, 1.0f);
        if (key == "rage")
            return power(POWER_RAGE, 10.0f);
        if (key == "energy")
            return power(POWER_ENERGY, 1.0f);
        if (key == "runic")
            return power(POWER_RUNIC_POWER, 10.0f);
        if (key == "combo")
            return out = float(bot->GetComboPoints()), true;
        if (key == "combat")
            return flag(bot->IsInCombat());
        if (key == "aura")
            return flag(ai->HasAura(Lower(SpellName(ParamNumber(condition))), bot));
        if (key == "stacks")
        {
            Aura* aura = ai->GetAura(Lower(SpellName(ParamNumber(condition))), bot);
            return out = aura ? float(aura->GetStackAmount()) : 0.0f, true;
        }
        if (key == "moving")
            return flag(bot->isMoving());
        if (key == "casting")
            return flag(bot->IsNonMeleeSpellCast(false));
        if (key == "stunned")
            return flag(bot->HasUnitState(UNIT_STATE_STUNNED));
        if (key == "swimming")
            return flag(bot->GetMap()->IsInWater(bot->GetPhaseMask(), bot->GetPositionX(), bot->GetPositionY(),
                bot->GetPositionZ(), bot->GetCollisionHeight()));
        if (key == "level")
            return out = float(bot->GetLevel()), true;
        if (key == "attackers")
        {
            Value<uint8>* attackers = context->GetValue<uint8>("attacker count");
            return out = attackers ? float(attackers->Get()) : 0.0f, true;
        }
        if (key == "myattackers")
            return out = float(bot->getAttackers().size()), true;
        if (key == "enemies")
        {
            float const yards = float(ParamNumber(condition));
            uint32 count = 0;
            if (Value<GuidVector>* attackers = context->GetValue<GuidVector>("attackers"))
                for (ObjectGuid const& guid : attackers->Get())
                    if (Unit* unit = ObjectAccessor::GetUnit(*bot, guid); unit && unit->IsAlive() && bot->IsWithinDistInMap(unit, yards))
                        ++count;
            return out = float(count), true;
        }
        if (key == "pet")
            return flag(bot->GetPet() && bot->GetPet()->IsAlive());
        if (key == "pethp")
            return bot->GetPet() ? (out = bot->GetPet()->GetHealthPct(), true) : false;
        if (key == "ingroup")
            return flag(bot->GetGroup() != nullptr);
        if (key == "indungeon")
            return flag(bot->GetMap()->IsDungeon());
        if (key == "membersbelow")
        {
            float const below = float(ParamNumber(condition));
            uint32 count = 0;
            for (Player* member : MembersNear(bot))
                if (member->GetHealthPct() < below)
                    ++count;
            return out = float(count), true;
        }
        if (key == "tankhp")
        {
            Player* tank = TankOf(bot);
            return tank ? (out = tank->GetHealthPct(), true) : false;
        }
        if (key == "known")
            return flag(HighestKnown(bot, ParamNumber(condition)) != 0);
        if (key == "ready")
        {
            uint32 const rank = HighestKnown(bot, ParamNumber(condition));
            return flag(rank && !bot->HasSpellCooldown(rank));
        }
        if (key == "itemcount")
            return out = float(bot->GetItemCount(ParamNumber(condition))), true;
        if (key == "itemready")
        {
            uint32 const spell = OnUseSpell(bot->GetItemByEntry(ParamNumber(condition)));
            return flag(spell && !bot->HasSpellCooldown(spell));
        }
        if (key == "opt")
            return flag(condition.value != 0.0f);
        if (key == "trig")
        {
            Trigger* trigger = context->GetTrigger(condition.param);
            return flag(trigger && trigger->IsActive());
        }

        // Conditions on the rule's target.
        if (!target || !target->IsInWorld())
            return false;
        Creature* creature = target->ToCreature();
        if (key == "thp")
            return out = target->GetHealthPct(), true;
        if (key == "tdebuff")
            return flag(ai->HasAura(Lower(SpellName(ParamNumber(condition))), target, false, true));
        if (key == "taura")
            return flag(ai->HasAura(Lower(SpellName(ParamNumber(condition))), target));
        if (key == "tcasting")
            return flag(target->IsNonMeleeSpellCast(false));
        if (key == "tinterrupt")
        {
            Spell const* generic = target->GetCurrentSpell(CURRENT_GENERIC_SPELL);
            Spell const* channel = target->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
            return flag((generic && (generic->GetSpellInfo()->InterruptFlags & SPELL_INTERRUPT_FLAG_INTERRUPT)) ||
                (channel && (channel->GetSpellInfo()->ChannelInterruptFlags & CHANNEL_INTERRUPT_FLAG_INTERRUPT)));
        }
        if (key == "telite")
            return flag(creature && creature->isElite());
        if (key == "tboss")
            return flag(creature && (creature->isWorldBoss() || creature->IsDungeonBoss()));
        if (key == "tdist")
            return out = bot->GetDistance(target), true;
        if (key == "ttargetsme")
            return flag(target->GetVictim() == bot);
        if (key == "tenemies")
        {
            // The creatures of the fight within N yd of the target, the target included (mod-playerbots' "aoe" counts so).
            float const yards = float(ParamNumber(condition));
            uint32 count = 0;
            bool listed = false;
            if (Value<GuidVector>* attackers = context->GetValue<GuidVector>("attackers"))
                for (ObjectGuid const& guid : attackers->Get())
                {
                    listed = listed || guid == target->GetGUID();
                    if (Unit* unit = ObjectAccessor::GetUnit(*bot, guid); unit && unit->IsAlive() && target->IsWithinDistInMap(unit, yards))
                        ++count;
                }
            if (!listed && target->IsAlive() && !target->IsFriendlyTo(bot))
                ++count;
            return out = float(count), true;
        }
        if (key == "tdebuffleft")
        {
            // Seconds left of the bot's own aura on the target (0 when it has none; a lasting one counts as 10 minutes).
            Aura* aura = ai->GetAura(Lower(SpellName(ParamNumber(condition))), target, true);
            if (!aura)
                return out = 0.0f, true;
            int32 const duration = aura->GetDuration();
            return out = duration < 0 ? 600.0f : float(duration) / 1000.0f, true;
        }
        if (key == "ttype")
            return flag(target->GetCreatureType() == ParamNumber(condition));
        if (key == "behind")
            return flag(target->isInBack(bot));
        if (key == "facing")
            return flag(bot->isInFront(target));
        return false;
    }

    bool Holds(PlayerbotAI* ai, Player* bot, AiObjectContext* context, FP::Condition const& condition, Unit* target)
    {
        float measured = 0.0f;
        if (!Measure(ai, bot, context, condition, target, measured))
            return false;
        bool const result = Compare(measured, condition.op, condition.value);
        return condition.negate ? !result : result;
    }

    bool AllHold(PlayerbotAI* ai, Player* bot, AiObjectContext* context, std::vector<FP::Condition> const& conditions,
        Unit* target, bool targetOnly = false)
    {
        for (FP::Condition const& condition : conditions)
        {
            FP::ConditionType const* type = FP::FindCondition(condition.key);
            if (targetOnly && (!type || !type->onTarget))
                continue;
            if (!Holds(ai, bot, context, condition, target))
                return false;
        }
        return true;
    }

    // The target of a rule of the module's own (nullptr: none, the rule does not fire).
    Unit* FindTarget(PlayerbotAI* ai, Player* bot, AiObjectContext* context, FP::Rule const& rule)
    {
        switch (rule.target)
        {
            case FP::TargetKind::Self:
                return bot;
            case FP::TargetKind::Pet:
                return bot->GetPet() && bot->GetPet()->IsAlive() ? bot->GetPet() : nullptr;
            case FP::TargetKind::Lowest:
            {
                Player* lowest = nullptr;
                for (Player* member : MembersNear(bot))
                    if (!lowest || member->GetHealthPct() < lowest->GetHealthPct())
                        lowest = member;
                return lowest;
            }
            case FP::TargetKind::Tank:
                return TankOf(bot);
            case FP::TargetKind::Enemy:
            {
                Unit* best = nullptr;
                if (Value<GuidVector>* attackers = context->GetValue<GuidVector>("attackers"))
                    for (ObjectGuid const& guid : attackers->Get())
                    {
                        Unit* unit = ObjectAccessor::GetUnit(*bot, guid);
                        if (!unit || !unit->IsAlive() || !AllHold(ai, bot, context, rule.conditions, unit, true))
                            continue;
                        if (!best || bot->GetDistance(unit) < bot->GetDistance(best))
                            best = unit;
                    }
                return best;
            }
            default:
                return CurrentTarget(context);
        }
    }

    // What the character last cast (any thread: the casts come from its map's thread), the profile's fight line and the
    // fallback's clock. Only for characters that play a profile of the store, and for those being counted.
    struct Watch
    {
        std::string profile;      // empty: only counted (".ga admin fight ... stats")
        bool counting = false;
        uint32 lastCastMs = 0;
        uint32 lastSpell = 0;     // the first rank of the spell it cast last
        uint32 fightStart = 0;
        uint32 fightCasts = 0;
        std::map<std::string, uint32> fightSpells;
        // what ".ga admin fight ... stats" shows
        uint32 fights = 0;
        uint64 fightMs = 0;
        uint32 casts = 0;
        std::map<std::string, uint32> bySpell;
    };

    std::mutex& WatchLock()
    {
        static std::mutex lock;
        return lock;
    }

    std::unordered_map<uint64, Watch>& Watches()
    {
        static std::unordered_map<uint64, Watch> watches;
        return watches;
    }

    std::atomic<uint32>& WatchCount()
    {
        static std::atomic<uint32> count{ 0 };
        return count;
    }

    void SetWatched(Player* player, std::string const& profile)
    {
        std::lock_guard<std::mutex> guard(WatchLock());
        Watch& watch = Watches()[player->GetGUID().GetRawValue()];
        watch.profile = profile;
        WatchCount() = uint32(Watches().size());
    }

    void Unwatch(Player* player)
    {
        std::lock_guard<std::mutex> guard(WatchLock());
        auto it = Watches().find(player->GetGUID().GetRawValue());
        if (it == Watches().end())
            return;
        it->second.profile.clear();
        if (!it->second.counting)
            Watches().erase(it);
        WatchCount() = uint32(Watches().size());
    }

    uint32 LastSpell(Player* player)
    {
        std::lock_guard<std::mutex> guard(WatchLock());
        auto it = Watches().find(player->GetGUID().GetRawValue());
        return it == Watches().end() ? 0 : it->second.lastSpell;
    }

    uint32 LastCast(Player* player)
    {
        std::lock_guard<std::mutex> guard(WatchLock());
        auto it = Watches().find(player->GetGUID().GetRawValue());
        return it == Watches().end() ? 0 : it->second.lastCastMs;
    }

    // A rule of the module's own, shared by its trigger and its action (both live on the bot's map thread).
    struct RuleCore
    {
        FP::Rule rule;
        std::size_t index = 0;
        std::string spell;        // Spell: the name mod-playerbots casts it by (the highest rank the bot knows)
        ObjectGuid chosen;        // the target the last check found
        ObjectGuid lastTarget;    // "once per target"
        uint32 lastCastMs = 0;    // the interval
    };

    bool OwnCast(FP::Rule const& rule)
    {
        return rule.kind == FP::ActionKind::Spell || rule.kind == FP::ActionKind::Item || rule.kind == FP::ActionKind::Trinket;
    }

    // Whether a rule may fire now, and on what.
    bool Ready(PlayerbotAI* ai, Player* bot, AiObjectContext* context, RuleCore& core)
    {
        FP::Rule const& rule = core.rule;
        core.chosen.Clear();
        if (!rule.trigger.empty())
        {
            Trigger* base = context->GetTrigger(rule.trigger);
            if (!base || !base->IsActive())
                return false;
        }
        uint32 castSpell = 0;
        if (rule.kind == FP::ActionKind::Spell)
        {
            if (!HighestKnown(bot, rule.ref))
                return false; // a spell it has not learned yet: skipped
            castSpell = sSpellMgr->GetFirstSpellInChain(rule.ref);
        }
        else if (rule.kind == FP::ActionKind::Item || rule.kind == FP::ActionKind::Trinket)
        {
            castSpell = OnUseSpell(RuleItem(bot, rule));
            if (!castSpell)
                return false; // it neither wears nor carries the item
        }
        if (rule.minInterval && core.lastCastMs && getMSTimeDiff(core.lastCastMs, getMSTime()) < uint32(rule.minInterval) * IN_MILLISECONDS)
            return false;
        if ((rule.flags & FP::RuleNotTwiceInARow) && castSpell && LastSpell(bot) == sSpellMgr->GetFirstSpellInChain(castSpell))
            return false;
        Unit* target = OwnCast(rule) ? FindTarget(ai, bot, context, rule) : CurrentTarget(context);
        if (OwnCast(rule) && !target)
            return false;
        if ((rule.flags & FP::RuleOncePerTarget) && target && core.lastTarget == target->GetGUID())
            return false;
        if (!AllHold(ai, bot, context, rule.conditions, target))
            return false;
        if (target)
            core.chosen = target->GetGUID();
        return true;
    }

    Unit* Chosen(Player* bot, RuleCore const& core)
    {
        if (core.chosen.IsEmpty())
            return nullptr;
        return core.chosen == bot->GetGUID() ? bot : ObjectAccessor::GetUnit(*bot, core.chosen);
    }

    void NoteFired(RuleCore& core)
    {
        core.lastCastMs = getMSTime();
        core.lastTarget = core.chosen;
    }

    // The trigger of a rule of the module's own: its trigger of mod-playerbots, if it names one, its flags, its target
    // and its conditions.
    class RuleTrigger : public Trigger
    {
    public:
        RuleTrigger(PlayerbotAI* ai, std::string const& name, std::shared_ptr<RuleCore> core)
            : Trigger(ai, name, 1), _core(std::move(core)) { }

        bool IsActive() override { return Ready(botAI, bot, context, *_core); }

    private:
        std::shared_ptr<RuleCore> _core;
    };

    // A spell of a rule: mod-playerbots' own cast (its range, its "spell cast useful", the highest rank it knows) on the
    // rule's target; the rule is checked again when the queue gets to it (a queued action waits up to five seconds).
    class RuleSpellAction : public CastSpellAction
    {
    public:
        RuleSpellAction(PlayerbotAI* ai, std::shared_ptr<RuleCore> core) : CastSpellAction(ai, core->spell), _core(std::move(core))
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(_core->rule.ref);
            _nextSwing = info && (info->HasAttribute(SPELL_ATTR0_ON_NEXT_SWING_NO_DAMAGE) || info->HasAttribute(SPELL_ATTR0_ON_NEXT_SWING));
        }

        Unit* GetTarget() override { return Chosen(bot, *_core); }
        bool isUseful() override { return Ready(botAI, bot, context, *_core) && CastSpellAction::isUseful(); }
        bool isPossible() override { return GetTarget() && CastSpellAction::isPossible(); }

        bool Execute(Event event) override
        {
            SwingAt(GetTarget());
            if (!CastSpellAction::Execute(event))
                return false;
            NoteFired(*_core);
            return true;
        }

    private:
        // A spell "on next swing" (Raptor Strike, Heroic Strike, Cleave, Maul) is cast with the next swing of the auto
        // attack and not before. mod-playerbots' own action of such a spell gets the auto attack from its action node (the
        // node of "raptor strike" has the prerequisite "melee"); a rule's action of the module's own has no node, so the
        // spell was queued again and again and never cast: a level-1 hunter on the Beast Mastery template stood beside a
        // Young Thistle Boar 3 yd away with "0 casts" until the fight was ended for it (2026-11-28; its strategies start
        // the auto attack through that prerequisite alone). The rule starts the swings on its own target when it is in
        // melee reach.
        void SwingAt(Unit* target)
        {
            if (!_nextSwing || !target || target == bot || !target->IsAlive() || !bot->IsWithinMeleeRange(target) ||
                !bot->IsValidAttackTarget(target))
                return;
            if (!bot->HasInArc(float(M_PI) / 2.0f, target))
                bot->SetFacingToObject(target);
            if (bot->GetVictim() != target || !bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
                bot->Attack(target, true);
        }

        std::shared_ptr<RuleCore> _core;
        bool _nextSwing = false;
    };

    // A usable item or a trinket of a rule: used as the client does (CMSG_USE_ITEM, as mod-playerbots' "use trinket").
    class RuleItemAction : public Action
    {
    public:
        RuleItemAction(PlayerbotAI* ai, std::shared_ptr<RuleCore> core)
            : Action(ai, Acore::StringFormat("ga use {}", core->index)), _core(std::move(core)) { }

        Unit* GetTarget() override { return Chosen(bot, *_core); }
        bool isUseful() override { return Ready(botAI, bot, context, *_core); }

        bool isPossible() override
        {
            Item* item = RuleItem(bot, _core->rule);
            uint32 const spellId = OnUseSpell(item);
            Unit* target = GetTarget();
            return spellId && target && bot->CanUseItem(item) == EQUIP_ERR_OK && !bot->IsNonMeleeSpellCast(true) &&
                !bot->HasSpellCooldown(spellId) && botAI->CanCastSpell(spellId, target, false, nullptr, item);
        }

        bool Execute(Event /*event*/) override
        {
            Item* item = RuleItem(bot, _core->rule);
            uint32 const spellId = OnUseSpell(item);
            Unit* target = GetTarget();
            if (!spellId || !target)
                return false;
            WorldPacket packet(CMSG_USE_ITEM);
            packet << uint8(item->GetBagSlot()) << uint8(item->GetSlot()) << uint8(1) << spellId << item->GetGUID()
                   << uint32(0) << uint8(0);
            if (target == bot)
                packet << uint32(TARGET_FLAG_NONE) << bot->GetPackGUID();
            else
                packet << uint32(TARGET_FLAG_UNIT) << target->GetPackGUID();
            bot->GetSession()->HandleUseItemOpcode(packet);
            NoteFired(*_core);
            return true;
        }

    private:
        std::shared_ptr<RuleCore> _core;
    };

    std::string RuleTriggerName(uint8 engine, uint32 generation, std::size_t index)
    {
        return Acore::StringFormat("{}{}.{}", Info(engine).rulePrefix, generation, index);
    }

    std::string RuleActionName(uint8 engine, uint32 generation, std::size_t index)
    {
        return Acore::StringFormat("{}{}.{}", Info(engine).actionPrefix, generation, index);
    }

    // A rule the engine can take as it is: a row of mod-playerbots without conditions of the module's own.
    bool IsPlainRow(FP::Rule const& rule)
    {
        return rule.kind == FP::ActionKind::Row && !rule.trigger.empty() && rule.conditions.empty();
    }

    bool IsDefaultRow(FP::Rule const& rule)
    {
        return rule.kind == FP::ActionKind::Row && rule.trigger.empty() && rule.conditions.empty();
    }

    // The strategy "ga rotation" (or "ga buffs" between fights): the rules of what the bot plays in place of the
    // strategies it stands for. What else those strategies give the engine comes from them: their action nodes
    // (prerequisites, alternatives), their multipliers, their type (PlayerbotAI::IsRanged / IsTank ask the engine for it)
    // and their target exclusions.
    class FightRotationStrategy : public Strategy
    {
    public:
        FightRotationStrategy(PlayerbotAI* ai, uint8 engine, std::shared_ptr<FP::Played const> played, uint32 generation,
            std::vector<Strategy*> stands)
            : Strategy(ai), _engine(engine), _played(std::move(played)), _generation(generation), _stands(std::move(stands))
        {
            for (Strategy* strategy : _stands)
            {
                _type |= strategy->GetType();
                for (auto const& [name, creator] : strategy->actionNodeFactories.creators)
                    actionNodeFactories.creators[name] = creator;
            }
        }

        std::string const getName() override { return Info(_engine).strategy; }
        uint32 GetType() const override { return _type; }
        uint32 Generation() const { return _generation; }

        std::vector<NextAction> getDefaultActions() override
        {
            std::vector<NextAction> actions;
            for (FP::PlayedRule const& played : _played->rules)
                if (IsDefaultRow(played.rule))
                    actions.emplace_back(played.rule.action, played.rule.relevance);
            return actions;
        }

        void InitTriggers(std::vector<TriggerNode*>& triggers) override
        {
            // The rows of one trigger node of the template go back into one node (a node's trigger can add handlers of
            // its own, TriggerNode::getHandlers: once per node).
            std::string nodeTrigger;
            uint32 nodeNumber = 0;
            bool nodeHidden = false;
            std::vector<NextAction> handlers;
            auto flush = [&]()
            {
                if (!handlers.empty())
                    triggers.push_back(new TriggerNode(nodeTrigger, handlers));
                handlers.clear();
            };
            for (std::size_t i = 0; i < _played->rules.size(); ++i)
            {
                FP::PlayedRule const& played = _played->rules[i];
                FP::Rule const& rule = played.rule;
                if (IsDefaultRow(rule))
                    continue;
                if (IsPlainRow(rule))
                {
                    if (handlers.empty() || !rule.node || rule.node != nodeNumber || rule.trigger != nodeTrigger ||
                        played.hidden != nodeHidden)
                    {
                        flush();
                        nodeTrigger = rule.trigger;
                        nodeNumber = rule.node;
                        nodeHidden = played.hidden;
                    }
                    handlers.emplace_back(rule.action, rule.relevance);
                    continue;
                }
                flush();
                std::string const action = OwnCast(rule) ? RuleActionName(_engine, _generation, i) : rule.action;
                triggers.push_back(new TriggerNode(RuleTriggerName(_engine, _generation, i), { NextAction(action, rule.relevance) }));
            }
            flush();
        }

        void InitMultipliers(std::vector<Multiplier*>& multipliers) override
        {
            for (Strategy* strategy : _stands)
                strategy->InitMultipliers(multipliers);
        }

        bool HasTargetExclusions() const override
        {
            for (Strategy* strategy : _stands)
                if (strategy->HasTargetExclusions())
                    return true;
            return false;
        }

        void AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type) override
        {
            for (Strategy* strategy : _stands)
                strategy->AppendTargetExclusions(exclusions, type);
        }

    private:
        uint8 _engine = 0;
        std::shared_ptr<FP::Played const> _played;
        uint32 _generation = 0;
        std::vector<Strategy*> _stands;
        uint32 _type = STRATEGY_TYPE_GENERIC;
    };

    // Which character plays what (world thread: the planner and the commands).
    struct BotProfile
    {
        std::shared_ptr<FP::Played const> played;
        uint32 generation = 0;
        std::vector<std::string> stands;  // the strategies taken out of the engine
        uint32 version = 0;               // the store's version of the bot when it went in
    };

    uint64 Key(ObjectGuid guid, uint8 engine)
    {
        return guid.GetRawValue() * 2 + (engine ? 1 : 0);
    }

    std::unordered_map<uint64, BotProfile>& Profiles()
    {
        static std::unordered_map<uint64, BotProfile> profiles;
        return profiles;
    }

    // What the bot does not play for now and why (FC9: a fallback holds until the profile is saved or assigned again).
    struct Suspension
    {
        uint32 profileId = 0;
        uint32 version = 0;
        uint32 revision = 0;
    };

    std::unordered_map<uint64, Suspension>& Suspended()
    {
        static std::unordered_map<uint64, Suspension> suspended;
        return suspended;
    }

    // The fallback's clock and what was said about a profile the bot cannot play (world thread).
    struct FightWatch
    {
        uint32 idleSince = 0;
        std::string refusal;      // the last "plays its default because ..." line, said once
    };

    std::unordered_map<uint64, FightWatch>& FightWatches()
    {
        static std::unordered_map<uint64, FightWatch> watches;
        return watches;
    }

    uint32 NextGeneration()
    {
        static uint32 generation = 0;
        return ++generation;
    }

    // Takes the module's objects of an older generation out of the context and deletes them. Only after the engine has
    // been initialised again (ChangeStrategy): until then its trigger nodes and its queue may point at them.
    struct Leftovers
    {
        std::vector<std::unique_ptr<Strategy>> strategies;
        std::vector<std::unique_ptr<Trigger>> triggers;
        std::vector<std::unique_ptr<Action>> actions;
    };

    template <class T>
    void TakeOut(std::unordered_map<std::string, T*>& created, std::string const& prefix, std::vector<std::unique_ptr<T>>& into)
    {
        for (auto it = created.begin(); it != created.end();)
        {
            if (it->first.compare(0, prefix.size(), prefix) == 0)
            {
                if (it->second)
                    into.emplace_back(it->second);
                it = created.erase(it);
            }
            else
                ++it;
        }
    }

    void TakeOutAll(AiObjectContext* context, uint8 engine, Leftovers& leftovers)
    {
        EngineInfo const& info = Info(engine);
        auto& strategies = ContextAccess::Strategies(context).created;
        if (auto it = strategies.find(info.strategy); it != strategies.end())
        {
            if (it->second)
                leftovers.strategies.emplace_back(it->second);
            strategies.erase(it);
        }
        TakeOut(ContextAccess::Triggers(context).created, info.rulePrefix, leftovers.triggers);
        TakeOut(ContextAccess::Actions(context).created, info.actionPrefix, leftovers.actions);
    }

    std::vector<std::string> PresentStands(PlayerbotAI* ai, uint8 engine)
    {
        std::vector<std::string> present;
        for (std::string const& name : ai->GetStrategies(Info(engine).state))
            if (StandSet(engine).count(name))
                present.push_back(name);
        std::sort(present.begin(), present.end());
        return present;
    }

    bool OursInEngine(PlayerbotAI* ai, uint8 engine)
    {
        return ai->HasStrategy(Info(engine).strategy, Info(engine).state);
    }

    std::string Join(std::vector<std::string> const& names)
    {
        std::string text;
        for (std::string const& name : names)
            text += (text.empty() ? "" : ", ") + name;
        return text;
    }

    // The rows of the strategies an engine's list stands for (the template), each marked whether a profile owns it.
    bool ReadTemplate(PlayerbotAI* ai, uint8 engine, std::vector<std::string> const& stands, FP::Played& out)
    {
        AiObjectContext* context = ai->GetAiObjectContext();
        out = FP::Played();
        out.replaced = stands;
        uint16 node = 0;
        for (std::string const& name : stands)
        {
            Strategy* strategy = context->GetStrategy(name);
            if (!strategy)
                continue;
            std::vector<TriggerNode*> nodes;
            strategy->InitTriggers(nodes);
            for (TriggerNode* triggerNode : nodes)
            {
                ++node;
                for (NextAction handler : triggerNode->getHandlers())
                {
                    FP::PlayedRule row;
                    row.rule.list = FP::ListKind(engine);
                    row.rule.kind = FP::ActionKind::Row;
                    row.rule.source = name;
                    row.rule.node = node;
                    row.rule.trigger = triggerNode->getName();
                    row.rule.action = handler.getName();
                    row.rule.relevance = handler.getRelevance();
                    row.hidden = !OwnsRow(engine, context, row.rule.action);
                    out.rules.push_back(row);
                }
                delete triggerNode;
            }
            for (NextAction handler : strategy->getDefaultActions())
            {
                FP::PlayedRule row;
                row.rule.list = FP::ListKind(engine);
                row.rule.kind = FP::ActionKind::Row;
                row.rule.source = name;
                row.rule.action = handler.getName();
                row.rule.relevance = handler.getRelevance();
                row.hidden = !OwnsRow(engine, context, row.rule.action);
                out.rules.push_back(row);
            }
        }
        return !stands.empty();
    }

    // A stored profile's list for one bot: its own rules (enabled, the options filled in with the bot's own value first)
    // and the template's rows the profile does not own.
    void Compile(Player* player, uint8 engine, FP::StoredProfile const& profile, FP::Slot slot, FP::Played const& live, FP::Played& out)
    {
        out = FP::Played();
        out.replaced = live.replaced;
        out.profileId = profile.id;
        out.revision = profile.revision;
        out.name = profile.name;
        out.slot = slot;
        auto optionValue = [&](std::string const& key) -> float
        {
            float value = 0.0f;
            if (sFightProfileStore.BotOption(player->GetGUID(), profile.id, key, value))
                return value;
            FP::Option const* option = profile.FindOption(key);
            return option ? option->value : 0.0f;
        };
        for (FP::Rule const& stored : profile.rules)
        {
            if (stored.list != FP::ListKind(engine) || !stored.enabled)
                continue;
            FP::PlayedRule played;
            played.rule = stored;
            for (FP::Condition& condition : played.rule.conditions)
            {
                if (!condition.option.empty())
                {
                    condition.value = optionValue(condition.option);
                    condition.option.clear();
                }
                if (condition.key == "opt")
                    condition.value = optionValue(condition.param);
            }
            out.rules.push_back(std::move(played));
        }
        for (FP::PlayedRule const& row : live.rules)
            if (row.hidden)
                out.rules.push_back(row);
    }

    // Puts what the bot plays into its engine in place of the strategies it stands for.
    bool Apply(Player* player, uint8 engine, FP::Played const& played, std::vector<std::string> const& stands, uint32 version,
        std::string& why)
    {
        PlayerbotAI* ai = BotDriver::GetAI(player);
        AiObjectContext* context = ai ? ai->GetAiObjectContext() : nullptr;
        if (!context)
        {
            why = "it has no AI";
            return false;
        }
        EngineInfo const& info = Info(engine);
        if (stands.empty())
        {
            why = engine ? "it has none of the strategies of the buff list in its non-combat engine" :
                "it has no rotation strategy in its combat engine";
            return false;
        }

        std::vector<Strategy*> standObjects;
        for (std::string const& name : stands)
            if (Strategy* strategy = context->GetStrategy(name))
                standObjects.push_back(strategy);

        auto shared = std::make_shared<FP::Played const>(played);
        uint32 const generation = NextGeneration();

        Leftovers leftovers;
        TakeOutAll(context, engine, leftovers);
        ContextAccess::Strategies(context).created[info.strategy] =
            new FightRotationStrategy(ai, engine, shared, generation, standObjects);
        auto& triggers = ContextAccess::Triggers(context).created;
        auto& actions = ContextAccess::Actions(context).created;
        for (std::size_t i = 0; i < shared->rules.size(); ++i)
        {
            FP::Rule const& rule = shared->rules[i].rule;
            if (IsPlainRow(rule) || IsDefaultRow(rule))
                continue;
            auto core = std::make_shared<RuleCore>();
            core->rule = rule;
            core->index = i;
            if (rule.kind == FP::ActionKind::Spell)
                core->spell = Lower(SpellName(sSpellMgr->GetFirstSpellInChain(rule.ref)));
            triggers[RuleTriggerName(engine, generation, i)] = new RuleTrigger(ai, RuleTriggerName(engine, generation, i), core);
            if (rule.kind == FP::ActionKind::Spell)
                actions[RuleActionName(engine, generation, i)] = new RuleSpellAction(ai, core);
            else if (rule.kind == FP::ActionKind::Item || rule.kind == FP::ActionKind::Trinket)
                actions[RuleActionName(engine, generation, i)] = new RuleItemAction(ai, core);
        }

        std::string change;
        for (std::string const& name : PresentStands(ai, engine))
            change += "-" + name + ",";
        change += std::string("+") + info.strategy;
        ai->ChangeStrategy(change, info.state);
        // (The engine has been initialised again: nothing of it points at the leftovers any more.)
        leftovers = Leftovers();

        uint64 const key = Key(player->GetGUID(), engine);
        if (!OursInEngine(ai, engine))
        {
            why = Acore::StringFormat("the {} engine did not take the strategy", engine ? "non-combat" : "combat");
            Profiles().erase(key);
            return false;
        }
        BotProfile& entry = Profiles()[key];
        entry.played = shared;
        entry.generation = generation;
        entry.stands = stands;
        entry.version = version;
        if (!engine && played.profileId)
            SetWatched(player, played.name);
        return true;
    }

    // Takes what the bot plays in an engine out: the strategies it stood for come back (unless a reset put the class's
    // own strategies in already).
    bool Remove(Player* player, uint8 engine)
    {
        uint64 const key = Key(player->GetGUID(), engine);
        auto it = Profiles().find(key);
        if (it == Profiles().end())
            return false;
        std::vector<std::string> const stands = it->second.stands;
        Profiles().erase(it);
        if (!engine)
            Unwatch(player);
        PlayerbotAI* ai = BotDriver::GetAI(player);
        AiObjectContext* context = ai ? ai->GetAiObjectContext() : nullptr;
        if (!context)
            return true;
        if (OursInEngine(ai, engine))
        {
            std::string change = std::string("-") + Info(engine).strategy;
            for (std::string const& name : stands)
                change += ",+" + name;
            ai->ChangeStrategy(change, Info(engine).state);
        }
        Leftovers leftovers;
        TakeOutAll(context, engine, leftovers);
        return true;
    }

    // The strategies an engine's list stands for now: those in the engine, and, while the list is in it, those it took
    // out (a strategy mod-playerbots added since then joins them).
    std::vector<std::string> StandsNow(Player* player, PlayerbotAI* ai, uint8 engine)
    {
        std::vector<std::string> stands = PresentStands(ai, engine);
        auto it = Profiles().find(Key(player->GetGUID(), engine));
        if (it == Profiles().end())
            return stands;
        if (OursInEngine(ai, engine))
        {
            for (std::string const& name : it->second.stands)
                if (std::find(stands.begin(), stands.end(), name) == stands.end())
                    stands.push_back(name);
            std::sort(stands.begin(), stands.end());
            return stands;
        }
        return stands.empty() ? it->second.stands : stands;
    }

    // The owner's addon shows what the bot plays (BOT^fpn, fpw): a change of it is pushed.
    void PushPlaying(Player* player)
    {
        if (GuildAdventure::AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfMember(player->GetGUID()))
            if (guild->FindBot(player->GetGUID()))
                GuildAdventure::PushBot(*guild, player->GetGUID(), false);
    }

    char const* SlotName(FP::Slot slot)
    {
        return slot == FP::Slot::Group ? "group" : "solo";
    }

    std::string TargetText(FP::Rule const& rule)
    {
        switch (rule.target)
        {
            case FP::TargetKind::Self: return " on itself";
            case FP::TargetKind::Pet: return " on its pet";
            case FP::TargetKind::Lowest: return " on the member with the least health";
            case FP::TargetKind::Tank: return " on the tank";
            case FP::TargetKind::Enemy: return " on an enemy that meets the conditions";
            default: return OwnCast(rule) ? " on its target" : "";
        }
    }
}

std::string FP::RuleText(Rule const& rule)
{
    std::string text;
    std::string const when = !rule.trigger.empty() ? rule.trigger : rule.conditions.empty() ? std::string("(default)") :
        std::string("(when)");
    switch (rule.kind)
    {
        case ActionKind::Row:
        case ActionKind::Action:
            text = when + " -> " + rule.action;
            break;
        case ActionKind::Spell:
            text = (rule.trigger.empty() ? std::string("(always)") : rule.trigger) + " -> cast " + SpellName(rule.ref) +
                Acore::StringFormat(" ({})", rule.ref);
            break;
        case ActionKind::Item:
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(rule.ref);
            text = (rule.trigger.empty() ? std::string("(always)") : rule.trigger) + " -> use " +
                (proto ? proto->Name1 : Acore::StringFormat("item {}", rule.ref));
            break;
        }
        case ActionKind::Trinket:
            text = (rule.trigger.empty() ? std::string("(always)") : rule.trigger) + Acore::StringFormat(" -> use trinket {}", rule.ref);
            break;
    }
    text += TargetText(rule);
    text += Acore::StringFormat(" ({:.1f})", rule.relevance);
    if (!rule.enabled)
        text += " [off]";
    if (rule.flags & RuleOncePerTarget)
        text += " [once per target]";
    if (rule.flags & RuleNotTwiceInARow)
        text += " [not twice in a row]";
    if (rule.minInterval)
        text += Acore::StringFormat(" [every {} s at most]", rule.minInterval);
    for (Condition const& condition : rule.conditions)
        text += " [" + DescribeCondition(condition) + "]";
    return text;
}

std::vector<std::string> FP::StandStrategies(Player* player, uint8 engine)
{
    PlayerbotAI* ai = BotDriver::GetAI(player);
    return ai ? StandsNow(player, ai, engine) : std::vector<std::string>();
}

bool FP::Template(Player* player, uint8 engine, Played& out, std::string& why)
{
    PlayerbotAI* ai = BotDriver::GetAI(player);
    if (!ai || !ai->GetAiObjectContext())
    {
        why = "it has no AI";
        return false;
    }
    if (!ReadTemplate(ai, engine, StandsNow(player, ai, engine), out))
    {
        why = engine ? "it has none of the strategies of the buff list in its non-combat engine" :
            "it has no rotation strategy in its combat engine";
        return false;
    }
    return true;
}

bool FP::CopyTemplate(Player* player, StoredProfile& out, std::string& why)
{
    out = StoredProfile();
    out.classId = player->getClass();
    out.tab = AiFactory::GetPlayerSpecTab(player);
    std::string key;
    for (uint8 engine = 0; engine < 2; ++engine)
    {
        Played played;
        if (!Template(player, engine, played, why))
        {
            if (!engine)
                return false;
            continue; // a class without a buff list keeps an empty one
        }
        key += (engine ? "|" : "") + Join(played.replaced);
        for (PlayedRule const& row : played.rules)
            if (!row.hidden)
                out.rules.push_back(row.rule);
    }
    out.templateKey = key.size() > 128 ? key.substr(0, 128) : key;
    return true;
}

std::vector<std::string> FP::DumpEngine(Player* player, uint8 engine)
{
    std::vector<std::string> lines;
    PlayerbotAI* ai = BotDriver::GetAI(player);
    AiObjectContext* context = ai ? ai->GetAiObjectContext() : nullptr;
    if (!context)
        return { "it has no AI" };
    ::BotState const state = Info(engine).state;
    std::vector<std::string> names = ai->GetStrategies(state);
    std::sort(names.begin(), names.end());
    for (std::string const& name : names)
    {
        Strategy* strategy = ai->GetStrategy(name, state);
        if (!strategy)
            continue;
        std::vector<TriggerNode*> nodes;
        strategy->InitTriggers(nodes);
        std::vector<Multiplier*> multipliers;
        strategy->InitMultipliers(multipliers);
        lines.push_back(Acore::StringFormat("[{}] type {}, {} trigger nodes, {} multipliers, {} action nodes{}", name,
            strategy->GetType(), nodes.size(), multipliers.size(), strategy->actionNodeFactories.creators.size(),
            StandSet(engine).count(name) ? (engine ? ", buff list" : ", rotation") : ""));
        for (TriggerNode* triggerNode : nodes)
        {
            std::string text = "  " + triggerNode->getName() + " ->";
            for (NextAction handler : triggerNode->getHandlers())
            {
                Action* action = context->GetAction(handler.getName());
                char const* kind = !action ? "?" : dynamic_cast<CastBuffSpellAction*>(action) ? "buff" :
                    dynamic_cast<CastSpellAction*>(action) ? "spell" : "action";
                text += Acore::StringFormat(" {} ({:.1f}, {})", handler.getName(), handler.getRelevance(), kind);
            }
            lines.push_back(text);
            delete triggerNode;
        }
        for (Multiplier* multiplier : multipliers)
            delete multiplier;
        std::string defaults;
        for (NextAction handler : strategy->getDefaultActions())
            defaults += Acore::StringFormat(" {} ({:.1f})", handler.getName(), handler.getRelevance());
        if (!defaults.empty())
            lines.push_back("  default:" + defaults);
    }
    return lines;
}

std::vector<std::string> FP::DescribePlayed(Played const& played)
{
    std::vector<std::string> lines;
    std::size_t hidden = 0;
    for (PlayedRule const& row : played.rules)
        hidden += row.hidden ? 1 : 0;
    lines.push_back(Acore::StringFormat("{}: {} rules of its own and {} rows left to mod-playerbots and the module, in place of {}",
        played.profileId ? Acore::StringFormat("profile #{} \"{}\" ({}, revision {})", played.profileId, played.name,
            SlotName(played.slot), played.revision) : std::string("template"),
        played.rules.size() - hidden, hidden, Join(played.replaced)));
    std::size_t index = 0;
    for (PlayedRule const& row : played.rules)
        if (!row.hidden)
            lines.push_back(Acore::StringFormat("#{} [{}] {}", index++, row.rule.source.empty() ? std::string("own") : row.rule.source,
                RuleText(row.rule)));
    std::string left;
    for (PlayedRule const& row : played.rules)
        if (row.hidden)
            left += (left.empty() ? "" : ", ") + row.rule.action;
    if (!left.empty())
        lines.push_back("left as they are: " + left);
    return lines;
}

std::vector<std::string> FP::DescribeStored(StoredProfile const& profile)
{
    static char const* const classes[] = { "", "warrior", "paladin", "hunter", "rogue", "priest", "death knight", "shaman",
        "mage", "warlock", "", "druid" };
    std::vector<std::string> lines;
    lines.push_back(Acore::StringFormat("#{} \"{}\": {}, talent tree {}, revision {}, from the template of {}", profile.id,
        profile.name, profile.classId < 12 ? classes[profile.classId] : "?", uint32(profile.tab), profile.revision,
        profile.templateKey.empty() ? std::string("nothing") : profile.templateKey));
    if (!profile.note.empty())
        lines.push_back("note: " + profile.note);
    for (Option const& option : profile.options)
        lines.push_back(Acore::StringFormat("option {} \"{}\": {} ({} to {}, {})", option.key, option.label,
            Acore::StringFormat("{:.1f}", option.value), Acore::StringFormat("{:.1f}", option.min), Acore::StringFormat("{:.1f}", option.max),
            option.kind == OptionKind::Switch ? "switch" : option.kind == OptionKind::Percent ? "percent" : "number"));
    for (uint8 list = 0; list < 2; ++list)
    {
        lines.push_back(Acore::StringFormat("{} ({} rules):", list ? "between fights" : "the fight", profile.CountRules(ListKind(list))));
        uint32 index = 0;
        for (Rule const& rule : profile.rules)
            if (rule.list == ListKind(list))
                lines.push_back(Acore::StringFormat("  {}{} {}", list ? "n" : "c", index++, RuleText(rule)));
    }
    std::string users;
    for (ObjectGuid const& bot : sFightProfileStore.UsedBy(profile.id))
    {
        std::string name;
        sCharacterCache->GetCharacterNameByGuid(bot, name);
        users += (users.empty() ? "" : ", ") + (name.empty() ? Acore::StringFormat("guid {}", bot.GetCounter()) : name);
    }
    lines.push_back("used by: " + (users.empty() ? std::string("nobody") : users));
    return lines;
}

std::vector<std::string> FP::DescribeBot(Player* player)
{
    std::vector<std::string> lines;
    ObjectGuid const guid = player->GetGUID();
    for (Slot slot : { Slot::Solo, Slot::Group })
    {
        uint32 const id = sFightProfileStore.Assigned(guid, slot);
        StoredProfile const* profile = id ? sFightProfileStore.Get(id) : nullptr;
        StoredProfile const* byDefault = id ? nullptr : sFightProfileStore.Get(DefaultFor(player, slot == Slot::Group));
        std::string const empty = slot == Slot::Group ? std::string("as the solo slot") : byDefault ?
            Acore::StringFormat("by default the template #{} \"{}\" of its class and talent tree", byDefault->id, byDefault->name) :
            std::string("the default of mod-playerbots");
        lines.push_back(Acore::StringFormat("{} slot: {}", SlotName(slot), profile ? Acore::StringFormat("#{} \"{}\" (talent tree {})",
            profile->id, profile->name, uint32(profile->tab)) : empty));
        if (profile)
            for (Option const& option : profile->options)
            {
                float value = 0.0f;
                bool const own = sFightProfileStore.BotOption(guid, profile->id, option.key, value);
                lines.push_back(Acore::StringFormat("  option {}: {} ({})", option.key, Acore::StringFormat("{:.1f}", own ? value : option.value),
                    own ? "its own value" : "the profile's"));
            }
    }
    lines.push_back(Acore::StringFormat("talent tree now: {}", uint32(AiFactory::GetPlayerSpecTab(player))));
    for (uint8 engine = 0; engine < 2; ++engine)
        if (Played const* active = Active(player, engine); active && active->byDefault)
        {
            uint32 const own = sFightProfileStore.Assigned(guid, active->slot);
            if (StoredProfile const* mine = own ? sFightProfileStore.Get(own) : nullptr)
                lines.push_back(Acore::StringFormat("its {} slot's profile #{} is made for talent tree {}: it plays the template of its "
                    "tree now by default", SlotName(active->slot), mine->id, uint32(mine->tab)));
            break;
        }
    if (auto it = Suspended().find(guid.GetRawValue()); it != Suspended().end())
        lines.push_back(Acore::StringFormat("profile #{} is suspended: it did nothing for 10 s in a fight under it (until it is saved "
            "or assigned again)", it->second.profileId));
    if (auto it = FightWatches().find(guid.GetRawValue()); it != FightWatches().end() && !it->second.refusal.empty())
        lines.push_back("not played: " + it->second.refusal);
    for (uint8 engine = 0; engine < 2; ++engine)
    {
        Played const* active = Active(player, engine);
        if (!active)
        {
            lines.push_back(Acore::StringFormat("{}: the default of mod-playerbots", Info(engine).list));
            continue;
        }
        lines.push_back(Acore::StringFormat("{}:", Info(engine).list));
        for (std::string const& line : DescribePlayed(*active))
            lines.push_back("  " + line);
    }
    return lines;
}

FP::Played const* FP::Active(Player* player, uint8 engine)
{
    auto it = Profiles().find(Key(player->GetGUID(), engine));
    return it == Profiles().end() ? nullptr : it->second.played.get();
}

std::string FP::Refusal(ObjectGuid guid)
{
    auto it = FightWatches().find(guid.GetRawValue());
    return it == FightWatches().end() ? std::string() : it->second.refusal;
}

uint8 FP::TalentTree(Player* player)
{
    return AiFactory::GetPlayerSpecTab(player);
}

namespace
{
    // Whether a template has something against a foe the bot can cast at its level: a spell of its fight list on its
    // target or an enemy that the bot knows. Below the level of its first such spell a template is not played by
    // default (FC12) and the bot plays mod-playerbots' rotation, which falls back on what the bot has: a level-1 mage
    // (Frost by mod-playerbots' tree below level 10) has Fireball, and the Frost template's first spell against a foe is
    // Frostbolt of level 4; it stood beside its first creature and never cast (a player's report, confirmed by the owner,
    // 2026-11-28: "with the setting off it worked"). (A melee class swings under any template: "melee" is a row a
    // profile does not own.)
    bool HasSpellAgainstFoes(Player* player, FP::StoredProfile const& profile)
    {
        for (FP::Rule const& rule : profile.rules)
            if (rule.list == FP::ListKind::Combat && rule.enabled && rule.kind == FP::ActionKind::Spell &&
                (rule.target == FP::TargetKind::Current || rule.target == FP::TargetKind::Enemy) && HighestKnown(player, rule.ref))
                return true;
        return false;
    }

    uint32 PlayableTemplate(Player* player, uint32 id)
    {
        FP::StoredProfile const* profile = id ? sFightProfileStore.Get(id) : nullptr;
        if (!profile || HasSpellAgainstFoes(player, *profile))
            return id;
        // (Said once per template and level.)
        static std::mutex lock;
        static std::unordered_map<uint64, uint64> noted;
        uint64 const said = (uint64(id) << 8) | player->GetLevel();
        std::lock_guard<std::mutex> guard(lock);
        if (noted[player->GetGUID().GetRawValue()] != said)
        {
            noted[player->GetGUID().GetRawValue()] = said;
            LOG_INFO(ActivityLog, "activity: {} plays the default of mod-playerbots, not the template #{} \"{}\" of its talent tree: "
                "it knows none of the template's spells against a foe yet (level {})", player->GetName(), profile->id, profile->name,
                uint32(player->GetLevel()));
        }
        return 0;
    }
}

uint32 FP::DefaultFor(Player* player, bool groupSlot)
{
    if (!sBotSettings.IsOn(player, GuildAdventure::Setting::FightTemplatesByDefault))
        return 0;
    uint8 const classId = player->getClass();
    uint8 const tab = AiFactory::GetPlayerSpecTab(player);
    // Its role by its talents (not by its strategies, which a profile replaces): in a group a tank plays the tank
    // template of its tree and a healer the healer template, or mod-playerbots' rotation when there is none; alone the
    // solo template of its tree first, else the one of its role (a healer that levels alone heals itself and fights).
    uint8 const role = PlayerbotAI::IsTank(player, true) ? TemplateTank : PlayerbotAI::IsHeal(player, true) ? TemplateHeal : TemplateSolo;
    if (groupSlot)
        return PlayableTemplate(player, sFightProfileStore.TemplateFor(classId, tab, role));
    if (uint32 const solo = sFightProfileStore.TemplateFor(classId, tab, TemplateSolo))
        return PlayableTemplate(player, solo);
    return role != TemplateSolo ? PlayableTemplate(player, sFightProfileStore.TemplateFor(classId, tab, role)) : 0;
}

void FP::Keep(Player* player, bool groupSlot)
{
    ObjectGuid const guid = player->GetGUID();
    uint32 const solo = sFightProfileStore.Assigned(guid, Slot::Solo);
    uint32 const group = sFightProfileStore.Assigned(guid, Slot::Group);
    bool const playing = Profiles().count(Key(guid, 0)) || Profiles().count(Key(guid, 1));
    // With nothing in the slot it plays, the default template of its class and talent tree (FC12).
    Slot slot = groupSlot && group ? Slot::Group : Slot::Solo;
    uint32 wanted = slot == Slot::Group ? group : solo;
    bool byDefault = !wanted && (wanted = DefaultFor(player, groupSlot)) != 0;
    // Its own profile is of another talent tree (a tank or healer whose dungeon role's talents are on only in a run):
    // the default template of the tree it has now, as for a bot with nothing of its own in that slot.
    if (!byDefault && wanted)
    {
        StoredProfile const* own = sFightProfileStore.Get(wanted);
        if (own && own->classId == player->getClass() && own->tab != AiFactory::GetPlayerSpecTab(player))
            if (uint32 const fallback = DefaultFor(player, groupSlot))
            {
                wanted = fallback;
                byDefault = true;
            }
    }
    // The template a group slot takes is the group slot's (its role), whatever its solo slot holds.
    if (byDefault && groupSlot)
        slot = Slot::Group;
    if (!wanted && !playing)
        return;
    PlayerbotAI* ai = BotDriver::GetAI(player);
    if (!ai || !ai->GetAiObjectContext() || player->IsInCombat())
        return;

    // Which profile, if any: the group slot on a dungeon or raid map and in a dungeon run, else (or when it is empty)
    // the solo slot, else the default template; not one of another class or talent tree, nor one suspended by the
    // fallback.
    StoredProfile const* profile = wanted ? sFightProfileStore.Get(wanted) : nullptr;
    uint32 const version = sFightProfileStore.BotVersion(guid);
    std::string refusal;
    if (profile && profile->classId != player->getClass())
        refusal = Acore::StringFormat("profile #{} \"{}\" is of another class", profile->id, profile->name);
    else if (profile && profile->tab != AiFactory::GetPlayerSpecTab(player))
        refusal = Acore::StringFormat("profile #{} \"{}\" is made for talent tree {}, its talents are of tree {}", profile->id,
            profile->name, uint32(profile->tab), uint32(AiFactory::GetPlayerSpecTab(player)));
    else if (auto s = Suspended().find(guid.GetRawValue()); profile && s != Suspended().end())
    {
        if (s->second.profileId == profile->id && s->second.version == version && s->second.revision == profile->revision)
            refusal = Acore::StringFormat("profile #{} \"{}\" is suspended after 10 s without an action in a fight", profile->id, profile->name);
        else
            Suspended().erase(s);
    }
    FightWatch& watch = FightWatches()[guid.GetRawValue()];
    bool changed = watch.refusal != refusal;
    if (!refusal.empty())
    {
        if (watch.refusal != refusal)
            LOG_INFO(ActivityLog, "activity: {} plays the default of mod-playerbots: {}", player->GetName(), refusal);
        watch.refusal = refusal;
        profile = nullptr;
    }
    else
        watch.refusal.clear();

    for (uint8 engine = 0; engine < 2; ++engine)
    {
        uint64 const key = Key(guid, engine);
        auto it = Profiles().find(key);
        if (!profile)
        {
            if (it != Profiles().end())
            {
                std::string const was = it->second.played ? it->second.played->name : std::string();
                Remove(player, engine);
                changed = true;
                if (!engine && refusal.empty())
                    LOG_INFO(ActivityLog, "activity: {} plays the default of mod-playerbots again (no fight profile in its {} slot{}; "
                        "it played \"{}\")", player->GetName(), SlotName(slot),
                        sBotSettings.IsOn(player, GuildAdventure::Setting::FightTemplatesByDefault) ? " and no template of its talent tree" : "", was);
            }
            continue;
        }
        bool const same = it != Profiles().end() && it->second.played && it->second.played->profileId == profile->id &&
            it->second.played->revision == profile->revision && it->second.played->slot == slot &&
            it->second.played->byDefault == byDefault && it->second.version == version;
        if (same)
        {
            // Still in the engine as it went in: nothing to do. A strategy reset (a login, a level-up, the module's own
            // strategy fixes) or a strategy mod-playerbots added since: in again.
            auto* ours = dynamic_cast<FightRotationStrategy*>(ai->GetStrategy(Info(engine).strategy, Info(engine).state));
            if (ours && ours->Generation() == it->second.generation && PresentStands(ai, engine).empty())
                continue;
        }
        std::vector<std::string> const stands = StandsNow(player, ai, engine);
        Played live;
        std::string why;
        if (!ReadTemplate(ai, engine, stands, live))
        {
            if (it != Profiles().end())
                Remove(player, engine);
            if (!engine)
                LOG_INFO(ActivityLog, "activity: {} cannot play fight profile \"{}\": it has no rotation strategy in its combat engine",
                    player->GetName(), profile->name);
            continue;
        }
        Played played;
        Compile(player, engine, *profile, slot, live, played);
        played.byDefault = byDefault;
        if (!Apply(player, engine, played, stands, version, why))
        {
            LOG_INFO(ActivityLog, "activity: {} cannot play fight profile \"{}\" ({}): {}", player->GetName(), profile->name,
                Info(engine).list, why);
            continue;
        }
        if (!same)
        {
            changed = true;
            std::size_t own = 0;
            for (PlayedRule const& row : played.rules)
                own += row.hidden ? 0 : 1;
            LOG_INFO(ActivityLog, "activity: {} plays fight profile #{} \"{}\" ({} slot{}, {}): {} rules of its own and {} rows left to "
                "mod-playerbots and the module, in place of {}", player->GetName(), profile->id, profile->name, SlotName(slot),
                byDefault ? ", by default: nothing of its own in it" : "", Info(engine).list, own, played.rules.size() - own,
                Join(stands));
        }
    }
    if (changed)
        PushPlaying(player);
}

void FP::WatchFight(Player* player)
{
    auto it = Profiles().find(Key(player->GetGUID(), 0));
    if (it == Profiles().end() || !it->second.played || !it->second.played->profileId)
        return;
    FightWatch& watch = FightWatches()[player->GetGUID().GetRawValue()];
    uint32 const now = getMSTime();
    PlayerbotAI* ai = BotDriver::GetAI(player);
    Unit* target = ai && ai->GetAiObjectContext() ? CurrentTarget(ai->GetAiObjectContext()) : nullptr;
    if (!target)
        target = player->GetVictim();
    // Doing something: a cast, a swing, a shot, a step, or held by crowd control (FC9 counts only standing idle).
    uint32 const lastCast = LastCast(player);
    bool const busy = !player->IsInCombat() || !player->IsAlive() || !target || !target->IsAlive() ||
        player->IsNonMeleeSpellCast(false) || player->HasUnitState(UNIT_STATE_MELEE_ATTACKING) ||
        player->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL) || player->isMoving() || player->HasUnitState(UNIT_STATE_CONTROLLED) ||
        (lastCast && getMSTimeDiff(lastCast, now) < FallbackMs);
    if (busy)
    {
        watch.idleSince = 0;
        return;
    }
    if (!watch.idleSince)
    {
        watch.idleSince = now;
        return;
    }
    if (getMSTimeDiff(watch.idleSince, now) < FallbackMs)
        return;

    // FC9: back to the default, and the owner is told.
    watch.idleSince = 0;
    FP::Played const played = *it->second.played;
    StoredProfile const* profile = sFightProfileStore.Get(played.profileId);
    Suspension& suspension = Suspended()[player->GetGUID().GetRawValue()];
    suspension.profileId = played.profileId;
    suspension.version = sFightProfileStore.BotVersion(player->GetGUID());
    suspension.revision = profile ? profile->revision : played.revision;
    Remove(player, 0);
    Remove(player, 1);
    std::string const message = Acore::StringFormat("{} did nothing for 10 s in a fight against {} under its fight profile \"{}\": it "
        "plays the default of mod-playerbots until the profile is saved or assigned again.", player->GetName(), target->GetName(),
        played.name);
    LOG_INFO(ActivityLog, "activity: {}", message);
    if (GuildAdventure::AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfMember(player->GetGUID()))
        GuildAdventure::RecordEvent(*guild, "FIGHT", player->GetGUID(), message, uint8(player->GetLevel()), player->GetZoneId());
    PushPlaying(player);
}

void FP::Forget(Player* player)
{
    Remove(player, 0);
    Remove(player, 1);
}

void FP::StartCounting(Player* player)
{
    std::lock_guard<std::mutex> guard(WatchLock());
    Watch& watch = Watches()[player->GetGUID().GetRawValue()];
    watch.counting = true;
    watch.fights = 0;
    watch.fightMs = 0;
    watch.casts = 0;
    watch.bySpell.clear();
    WatchCount() = uint32(Watches().size());
}

void FP::NoteCast(Player* player, Spell const* spell)
{
    if (!WatchCount() || !player || !spell || spell->IsTriggered())
        return;
    std::lock_guard<std::mutex> guard(WatchLock());
    auto it = Watches().find(player->GetGUID().GetRawValue());
    if (it == Watches().end())
        return;
    Watch& watch = it->second;
    watch.lastCastMs = getMSTime();
    watch.lastSpell = sSpellMgr->GetFirstSpellInChain(spell->GetSpellInfo()->Id);
    if (!player->IsInCombat())
        return;
    std::string const name = spell->GetSpellInfo()->SpellName[0];
    ++watch.fightCasts;
    ++watch.fightSpells[name];
    if (watch.counting)
    {
        ++watch.casts;
        ++watch.bySpell[name];
    }
}

void FP::NoteCombat(Player* player, bool entered)
{
    if (!WatchCount() || !player)
        return;
    std::string line;
    {
        std::lock_guard<std::mutex> guard(WatchLock());
        auto it = Watches().find(player->GetGUID().GetRawValue());
        if (it == Watches().end())
            return;
        Watch& watch = it->second;
        if (entered)
        {
            watch.fightStart = getMSTime();
            watch.fightCasts = 0;
            watch.fightSpells.clear();
            return;
        }
        if (!watch.fightStart)
            return;
        uint32 const ms = getMSTimeDiff(watch.fightStart, getMSTime());
        watch.fightStart = 0;
        if (watch.counting)
        {
            ++watch.fights;
            watch.fightMs += ms;
        }
        if (!watch.profile.empty())
        {
            std::vector<std::pair<std::string, uint32>> spells(watch.fightSpells.begin(), watch.fightSpells.end());
            std::sort(spells.begin(), spells.end(), [](auto const& a, auto const& b) { return a.second > b.second; });
            std::string casts;
            for (auto const& [name, count] : spells)
                casts += Acore::StringFormat("{}{} {}", casts.empty() ? "" : ", ", name, count);
            line = Acore::StringFormat("activity: {} fought {:.0f} s by its fight profile \"{}\": {} casts{}", player->GetName(),
                ms / 1000.0, watch.profile, watch.fightCasts, casts.empty() ? std::string() : " (" + casts + ")");
        }
    }
    if (!line.empty())
        LOG_INFO(ActivityLog, "{}", line);
}

std::vector<std::string> FP::DescribeCounts(Player* player)
{
    std::lock_guard<std::mutex> guard(WatchLock());
    auto it = Watches().find(player->GetGUID().GetRawValue());
    if (it == Watches().end() || !it->second.counting)
        return { "not counted (\"stats reset\" starts it)" };
    Watch const& counts = it->second;
    std::vector<std::string> lines;
    lines.push_back(Acore::StringFormat("{} fights, {:.1f} s on average, {} casts ({:.1f} per fight)", counts.fights,
        counts.fights ? double(counts.fightMs) / counts.fights / 1000.0 : 0.0, counts.casts,
        counts.fights ? double(counts.casts) / counts.fights : 0.0));
    std::vector<std::pair<std::string, uint32>> spells(counts.bySpell.begin(), counts.bySpell.end());
    std::sort(spells.begin(), spells.end(), [](auto const& a, auto const& b) { return a.second > b.second; });
    for (auto const& [name, count] : spells)
        lines.push_back(Acore::StringFormat("  {}: {} ({:.1f} %)", name, count, counts.casts ? 100.0 * count / counts.casts : 0.0));
    return lines;
}

namespace
{
    class GuildAdventureFightProfilePlayerScript : public PlayerScript
    {
    public:
        GuildAdventureFightProfilePlayerScript() : PlayerScript("GuildAdventureFightProfilePlayerScript", {
            PLAYERHOOK_ON_SPELL_CAST,
            PLAYERHOOK_ON_PLAYER_ENTER_COMBAT,
            PLAYERHOOK_ON_PLAYER_LEAVE_COMBAT
        }) { }

        void OnPlayerSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override { FP::NoteCast(player, spell); }
        void OnPlayerEnterCombat(Player* player, Unit* /*enemy*/) override { FP::NoteCombat(player, true); }
        void OnPlayerLeaveCombat(Player* player) override { FP::NoteCombat(player, false); }
    };
}

void AddGuildAdventureFightProfileCommandScripts();

void AddGuildAdventureFightProfileScripts()
{
    new GuildAdventureFightProfilePlayerScript();
    AddGuildAdventureFightProfileCommandScripts();
}
