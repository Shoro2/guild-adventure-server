/*
 * QuestInteraction: see QuestInteraction.h and docs/orders.md "Quest interactions".
 */

#include "QuestInteraction.h"
#include "ConditionMgr.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "Item.h"
#include "Log.h"
#include "LootMgr.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "QuestDef.h"
#include "SmartScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "WorldIndex.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>

using namespace GuildAdventure;

char const* GuildAdventure::ToString(InteractionKind kind)
{
    switch (kind)
    {
        case InteractionKind::None:       return "none";
        case InteractionKind::UseItem:    return "use item";
        case InteractionKind::CastSpells: return "cast spells";
        case InteractionKind::UseObject:  return "use object";
        case InteractionKind::Explore:    return "explore";
        case InteractionKind::Provoke:    return "provoke";
    }
    return "?";
}

namespace
{
    // Acting distances: the client's interaction distance for objects, a bit inside spell range for casts.
    constexpr float ObjectUseRange = 4.0f;
    constexpr float DefaultSpellRange = 20.0f;
    constexpr std::size_t MaxHelpfulSpells = 12;

    std::unordered_map<uint64, std::optional<InteractionPlan>> _plans;   // (quest << 5 | objective << 1 | team)
    std::unordered_map<uint32, std::optional<InteractionPlan>> _explore; // (quest << 1 | team)
    std::unordered_map<uint64, std::optional<FollowerAid>> _aids;        // (quest << 32 | follower entry)
    std::unordered_map<uint32, std::vector<uint32>> _questTriggers;      // quest -> area triggers
    bool _triggersLoaded = false;
    std::unordered_map<uint32, std::vector<uint32>> _focusObjects;       // spell focus id -> game object entries
    bool _focusLoaded = false;
    std::unordered_map<uint32, std::vector<uint32>> _creditSources;      // credit entry -> creatures whose kill gives it
    // credit entry -> (creature, spell): the credit comes when that spell hits that creature (SMART_EVENT_SPELLHIT)
    std::unordered_map<uint32, std::vector<std::pair<uint32, uint32>>> _spellHitCredits;
    bool _creditLoaded = false;
    std::unordered_map<uint32, std::vector<uint32>> _keyObjects;         // key item -> game objects whose lock it opens
    bool _keysLoaded = false;
    std::vector<uint32> const NoEntries;

    // What a spell script checks and no data names: the caster near a creature. The Ooze Buster of "Corrosion Prevention"
    // (spell 42489, spell_ooze_zap) works only with the aura Energized!, which a Zeppelin Power Core (23832, its periodic
    // spell_energize_aoe) gives every player with the quest within 40 yd, for 3 s at a time. Out of it the use does nothing
    // and the core's check of the cast beforehand still says yes.
    struct NearRule
    {
        uint32 spellId;
        uint32 creature;
        float range;
        uint32 aura;    // what the creature gives the caster (the use waits for it)
    };
    NearRule const NearRules[] = { { 42489, 23832, 40.0f, 42490 } };
    std::vector<std::pair<uint32, uint32>> const NoCreators;

    template <typename T>
    void AddUnique(std::vector<T>& list, T value)
    {
        if (std::find(list.begin(), list.end(), value) == list.end())
            list.push_back(value);
    }

    // The kill credit an action of a creature's death gives (SMART_ACTION_CALL_KILLEDMONSTER, or a cast of a spell with a
    // kill credit effect), 0 if none.
    uint32 DeathCredit(SmartScriptHolder const& holder)
    {
        switch (holder.GetActionType())
        {
            case SMART_ACTION_CALL_KILLEDMONSTER:
                return holder.action.raw.param1;
            case SMART_ACTION_CAST:
            case SMART_ACTION_SELF_CAST:
            case SMART_ACTION_INVOKER_CAST:
                if (SpellInfo const* spell = sSpellMgr->GetSpellInfo(holder.action.raw.param1))
                    for (SpellEffectInfo const& effect : spell->Effects)
                        if ((effect.Effect == SPELL_EFFECT_KILL_CREDIT || effect.Effect == SPELL_EFFECT_KILL_CREDIT2) && effect.MiscValue > 0)
                            return uint32(effect.MiscValue);
                return 0;
            default:
                return 0;
        }
    }

    // Game objects whose lock opens with this key item (a lock case LOCK_KEY_ITEM naming it): the Blackhoof Cages of
    // "Prisoners of the Grimtotems" open with a Grimtotem Key.
    std::vector<uint32> const& KeyObjects(uint32 itemId)
    {
        if (!_keysLoaded)
        {
            _keysLoaded = true;
            for (auto const& [entry, info] : *sObjectMgr->GetGameObjectTemplates())
                if (LockEntry const* lock = sLockStore.LookupEntry(info.GetLockId()))
                    for (uint8 i = 0; i < MAX_LOCK_CASE; ++i)
                        if (lock->Type[i] == LOCK_KEY_ITEM && lock->Index[i])
                            AddUnique(_keyObjects[lock->Index[i]], entry);
        }
        auto const itr = _keyObjects.find(itemId);
        return itr != _keyObjects.end() ? itr->second : NoEntries;
    }

    // Creatures whose kill gives another entry's credit: creature_template KillCredit1/2, and SmartAI scripts that give a
    // kill credit when the creature dies (the event or the events linked to it). The crocolisks of "The Grimtotem Weapon"
    // credit the Captured Totem (23811) with a spell on their death, the Risen Husks and Spirits of "What's Haunting Witch
    // Hill?" the Restless Apparition (23861); neither credit creature stands anywhere to be killed.
    void LoadCreditSources()
    {
        if (_creditLoaded)
            return;
        _creditLoaded = true;
        for (auto const& [entry, info] : *sObjectMgr->GetCreatureTemplates())
        {
            for (uint32 credit : info.KillCredit)
                if (credit && credit != entry)
                    AddUnique(_creditSources[credit], entry);
            if (info.AIName != "SmartAI")
                continue;
            SmartAIEventList const script = sSmartScriptMgr->GetScript(int32(entry), SMART_SCRIPT_TYPE_CREATURE);
            for (SmartScriptHolder const& holder : script)
            {
                // A spell that hits the creature gives the credit ("Thirst Unending": Arcane Torrent near a Mana Wyrm, whose
                // script casts the credit spell on the caster).
                if (holder.GetEventType() == SMART_EVENT_SPELLHIT && holder.event.spellHit.spell)
                {
                    if (uint32 const credit = DeathCredit(holder))
                        if (credit != entry)
                            AddUnique(_spellHitCredits[credit], std::make_pair(entry, holder.event.spellHit.spell));
                    continue;
                }
                if (holder.GetEventType() != SMART_EVENT_DEATH)
                    continue;
                // The death event and the chain of events linked to it (SMART_EVENT_LINK).
                SmartScriptHolder const* step = &holder;
                for (int depth = 0; step && depth < 8; ++depth)
                {
                    if (uint32 const credit = DeathCredit(*step))
                        if (credit != entry)
                            AddUnique(_creditSources[credit], entry);
                    if (!step->link)
                        break;
                    SmartScriptHolder const* next = nullptr;
                    for (SmartScriptHolder const& other : script)
                        if (other.event_id == step->link && other.GetEventType() == SMART_EVENT_LINK)
                        {
                            next = &other;
                            break;
                        }
                    step = next;
                }
            }
        }
    }

    std::string CreatureName(uint32 entry)
    {
        CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(entry);
        return info ? info->Name : Acore::StringFormat("creature {}", entry);
    }

    std::string ObjectName(uint32 entry)
    {
        GameObjectTemplate const* info = sObjectMgr->GetGameObjectTemplate(entry);
        return info ? info->name : Acore::StringFormat("object {}", entry);
    }

    std::string ItemName(uint32 entry)
    {
        ItemTemplate const* info = sObjectMgr->GetItemTemplate(entry);
        return info ? info->Name1 : Acore::StringFormat("item {}", entry);
    }

    std::vector<uint32> const& FocusObjects(uint32 focusId)
    {
        if (!_focusLoaded)
        {
            _focusLoaded = true;
            for (auto const& [entry, info] : *sObjectMgr->GetGameObjectTemplates())
                if (info.type == GAMEOBJECT_TYPE_SPELL_FOCUS && info.spellFocus.focusId)
                    _focusObjects[info.spellFocus.focusId].push_back(entry);
        }
        auto itr = _focusObjects.find(focusId);
        return itr != _focusObjects.end() ? itr->second : NoEntries;
    }

    // Items whose on-use spell creates an item: created item -> (item, spell). Built once from the item templates.
    std::vector<std::pair<uint32, uint32>> const& CreatorsOf(uint32 itemId)
    {
        static std::unordered_map<uint32, std::vector<std::pair<uint32, uint32>>> creators;
        static bool built = false;
        if (!built)
        {
            built = true;
            for (auto const& [entry, proto] : *sObjectMgr->GetItemTemplateStore())
                for (auto const& use : proto.Spells)
                {
                    if (use.SpellId <= 0 || use.SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
                        continue;
                    if (SpellInfo const* spell = sSpellMgr->GetSpellInfo(uint32(use.SpellId)))
                        for (SpellEffectInfo const& effect : spell->Effects)
                            if ((effect.Effect == SPELL_EFFECT_CREATE_ITEM || effect.Effect == SPELL_EFFECT_CREATE_ITEM_2) && effect.ItemType)
                                creators[effect.ItemType].emplace_back(entry, uint32(use.SpellId));
                }
        }
        auto itr = creators.find(itemId);
        return itr != creators.end() ? itr->second : NoCreators;
    }

    // The quest's own items: its start item and its source items (quest_template.ItemDrop).
    std::vector<uint32> QuestItems(Quest const* quest)
    {
        std::vector<uint32> items;
        if (quest->GetSrcItemId())
            items.push_back(quest->GetSrcItemId());
        for (uint32 item : quest->ItemDrop)
            if (item)
                AddUnique(items, item);
        return items;
    }

    // The spell an item casts when the player uses it (0 if none).
    uint32 UseSpellOf(uint32 itemId)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (!proto)
            return 0;
        for (auto const& spell : proto->Spells)
            if (spell.SpellId > 0 && spell.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
                return uint32(spell.SpellId);
        return 0;
    }

    // What a spell's conditions say about its target and where it may be cast.
    struct SpellConditions
    {
        std::vector<uint32> creatures;       // the target must be one of these
        std::vector<uint32> gameObjects;
        std::vector<uint32> nearObjects;     // cast near one of these game objects
        std::vector<uint32> nearCreatures;
        float nearDistance = 0.0f;
        bool mustBeDead = false;             // "target must be dead": a corpse (capture vessels, ooze jars)
        std::vector<uint32> targetAuras;     // the target must have these auras (a Lazy Peon asleep)
        std::vector<uint32> targetNoAuras;   // ... and must not have these
    };

    void Collect(ConditionList const& conditions, SpellConditions& out)
    {
        for (Condition const* condition : conditions)
        {
            if (!condition)
                continue;
            if (condition->ConditionType == CONDITION_ALIVE && condition->NegativeCondition)
                out.mustBeDead = true;
            // An aura on the spell's target (condition target 1): the Foreman's Blackjack of "Lazy Peons" wakes only a peon
            // that sleeps (aura 17743); used on one that works it failed with SPELL_FAILED_BAD_TARGETS (12), and a bot
            // took nine minutes for the five peons.
            if (condition->ConditionType == CONDITION_AURA && condition->ConditionTarget == 1 && condition->ConditionValue1)
                AddUnique(condition->NegativeCondition ? out.targetNoAuras : out.targetAuras, condition->ConditionValue1);
            if (condition->NegativeCondition)
                continue;
            switch (condition->ConditionType)
            {
                case CONDITION_OBJECT_ENTRY_GUID:
                    if (!condition->ConditionValue2)
                        break;
                    if (condition->ConditionValue1 == TYPEID_UNIT)
                        AddUnique(out.creatures, condition->ConditionValue2);
                    else if (condition->ConditionValue1 == TYPEID_GAMEOBJECT)
                        AddUnique(out.gameObjects, condition->ConditionValue2);
                    break;
                case CONDITION_NEAR_GAMEOBJECT:
                    AddUnique(out.nearObjects, condition->ConditionValue1);
                    out.nearDistance = std::max(out.nearDistance, float(condition->ConditionValue2));
                    break;
                case CONDITION_NEAR_CREATURE:
                    AddUnique(out.nearCreatures, condition->ConditionValue1);
                    out.nearDistance = std::max(out.nearDistance, float(condition->ConditionValue2));
                    break;
                default:
                    break;
            }
        }
    }

    SpellConditions ConditionsOf(SpellInfo const* spell)
    {
        SpellConditions result;
        Collect(sConditionMgr->GetConditionsForNotGroupedEntry(CONDITION_SOURCE_TYPE_SPELL, spell->Id), result);
        for (SpellEffectInfo const& effect : spell->Effects)
            if (effect.ImplicitTargetConditions)
                Collect(*effect.ImplicitTargetConditions, result);
        return result;
    }

    bool NeedsUnitTarget(SpellInfo const* spell)
    {
        return (spell->GetExplicitTargetMask() & TARGET_FLAG_UNIT_MASK) != 0;
    }

    bool NeedsObjectTarget(SpellInfo const* spell)
    {
        return (spell->GetExplicitTargetMask() & TARGET_FLAG_GAMEOBJECT_MASK) != 0;
    }

    float RangeOf(SpellInfo const* spell)
    {
        float const range = spell->GetMaxRange(spell->IsPositive());
        if (range <= 0.0f)
            return ObjectUseRange;
        return std::clamp(range - 2.0f, ObjectUseRange, 30.0f);
    }

    bool HasEffect(SpellInfo const* spell, SpellEffects effect, int32 miscValue)
    {
        for (SpellEffectInfo const& info : spell->Effects)
            if (info.Effect == effect && info.MiscValue == miscValue)
                return true;
        return false;
    }

    bool CreatesItem(SpellInfo const* spell, uint32 itemId)
    {
        for (SpellEffectInfo const& info : spell->Effects)
            if ((info.Effect == SPELL_EFFECT_CREATE_ITEM || info.Effect == SPELL_EFFECT_CREATE_ITEM_2) && info.ItemType == itemId)
                return true;
        return false;
    }

    // An item plan without a target: stand next to what the spell needs (spell focus, a "near" condition).
    void WhereToStand(SpellInfo const* spell, SpellConditions const& conditions, InteractionPlan& plan)
    {
        plan.target = InteractionTarget::None;
        if (spell->RequiresSpellFocus)
        {
            // The focus works within its own distance (a moonwell's 13 yards): stand a bit inside it. Only foci that stand
            // somewhere count ("Bank" shares the Cliffspring River's focus without a spawn and named the plan).
            for (uint32 entry : FocusObjects(spell->RequiresSpellFocus))
                if (!sWorldIndex.GetGameObjectSpawns(entry).empty())
                    plan.walkObjects.push_back(entry);
            if (plan.walkObjects.empty())
                plan.walkObjects = FocusObjects(spell->RequiresSpellFocus);
            float reach = 0.0f;
            for (uint32 entry : plan.walkObjects)
                if (GameObjectTemplate const* info = sObjectMgr->GetGameObjectTemplate(entry))
                    reach = reach == 0.0f ? float(info->spellFocus.dist) : std::min(reach, float(info->spellFocus.dist));
            plan.range = std::max(ObjectUseRange, reach - 3.0f);
        }
        else if (!conditions.nearObjects.empty())
        {
            plan.walkObjects = conditions.nearObjects;
            plan.range = std::clamp(conditions.nearDistance - 3.0f, ObjectUseRange, 20.0f);
        }
    }

    // A chest the bot may open without a skill (no lock, or only "open"-type locks).
    bool OpensWithoutSkill(GameObjectTemplate const* info)
    {
        uint32 lockId = 0;
        switch (info->type)
        {
            case GAMEOBJECT_TYPE_CHEST:  lockId = info->chest.lockId; break;
            case GAMEOBJECT_TYPE_GOOBER: lockId = info->goober.lockId; break;
            default: return false;
        }
        if (!lockId)
            return true;
        LockEntry const* lock = sLockStore.LookupEntry(lockId);
        if (!lock)
            return true;
        // The cases of a lock are alternatives: one the bot can use is enough. Most quest herbs and mushrooms (lock 259:
        // "open kneeling" or herbalism) open for anyone; the herbalism case alone made them profession objects, and
        // NewRpg stood next to them without opening them (Hyacinth Mushroom, Moonpetal Lily, Death Cap ...).
        bool anyCase = false;
        for (uint8 i = 0; i < MAX_LOCK_CASE; ++i)
        {
            if (lock->Type[i] == LOCK_KEY_NONE)
                continue;
            anyCase = true;
            if (lock->Type[i] == LOCK_KEY_ITEM)
                continue; // a key the planner does not look for
            if (lock->Type[i] != LOCK_KEY_SKILL)
                continue;
            switch (lock->Index[i])
            {
                case LOCKTYPE_PICKLOCK:
                case LOCKTYPE_HERBALISM:
                case LOCKTYPE_MINING:
                case LOCKTYPE_DISARM_TRAP:
                case LOCKTYPE_FISHING:
                case LOCKTYPE_INSCRIPTION:
                    continue; // gathering and lockpicking stay with the professions
                default:
                    return true;
            }
        }
        return !anyCase;
    }

    // A friendly SmartAI creature whose gossip choice leads to it turning on the player: its script sets a faction hostile
    // to players or starts attacking. If it also follows, the place where it turns is a "distance to creature" event.
    std::optional<InteractionPlan> PlanProvoke(Player* player, uint32 entry)
    {
        CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(entry);
        if (!info || info->AIName != "SmartAI")
            return std::nullopt;
        SmartAIEventList script = sSmartScriptMgr->GetScript(int32(entry), SMART_SCRIPT_TYPE_CREATURE);
        if (script.empty())
            return std::nullopt;
        // The timed action lists it runs belong to it.
        std::vector<uint32> lists;
        for (std::size_t i = 0; i < script.size(); ++i)
        {
            SmartScriptHolder const& holder = script[i];
            if (holder.GetActionType() == SMART_ACTION_CALL_TIMED_ACTIONLIST)
                AddUnique(lists, holder.action.raw.param1);
            else if (holder.GetActionType() == SMART_ACTION_CALL_RANDOM_TIMED_ACTIONLIST)
                for (uint32 id : { holder.action.raw.param1, holder.action.raw.param2, holder.action.raw.param3,
                    holder.action.raw.param4, holder.action.raw.param5, holder.action.raw.param6 })
                    if (id)
                        AddUnique(lists, id);
            else if (holder.GetActionType() == SMART_ACTION_CALL_RANDOM_RANGE_TIMED_ACTIONLIST &&
                holder.action.raw.param1 && holder.action.raw.param2 >= holder.action.raw.param1 &&
                holder.action.raw.param2 - holder.action.raw.param1 < 20)
                for (uint32 id = holder.action.raw.param1; id <= holder.action.raw.param2; ++id)
                    AddUnique(lists, id);
        }
        FactionTemplateEntry const* own = sFactionTemplateStore.LookupEntry(player->GetFaction());
        std::vector<SmartScriptHolder> all(script.begin(), script.end());
        for (uint32 id : lists)
        {
            SmartAIEventList const timed = sSmartScriptMgr->GetScript(int32(id), SMART_SCRIPT_TYPE_TIMED_ACTIONLIST);
            all.insert(all.end(), timed.begin(), timed.end());
        }

        InteractionPlan plan;
        bool gossip = false, turns = false, follows = false;
        for (SmartScriptHolder const& holder : all)
        {
            if (holder.GetEventType() == SMART_EVENT_GOSSIP_SELECT && holder.source_type == SMART_SCRIPT_TYPE_CREATURE && !gossip)
            {
                gossip = true;
                plan.gossipMenu = holder.event.raw.param1;
                plan.gossipOption = holder.event.raw.param2;
            }
            switch (holder.GetActionType())
            {
                case SMART_ACTION_SET_FACTION:
                    if (FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(holder.action.raw.param1))
                        if (faction->IsHostileToPlayers() || (own && faction->IsHostileTo(*own)))
                            turns = true;
                    break;
                case SMART_ACTION_ATTACK_START:
                    turns = true;
                    break;
                case SMART_ACTION_FOLLOW:
                    follows = true;
                    break;
                default:
                    break;
            }
            // Where a follower turns: within param3 yards of the creature with spawn id param1 (or entry param2).
            if (holder.GetEventType() == SMART_EVENT_DISTANCE_CREATURE && holder.source_type == SMART_SCRIPT_TYPE_CREATURE &&
                holder.event.raw.param3)
            {
                if (CreatureData const* data = holder.event.raw.param1 ? sObjectMgr->GetCreatureData(holder.event.raw.param1) : nullptr)
                {
                    plan.leadMap = data->mapid;
                    plan.leadX = data->posX;
                    plan.leadY = data->posY;
                    plan.leadZ = data->posZ;
                }
                else if (holder.event.raw.param2)
                    for (SpawnPoint const& spawn : sWorldIndex.GetCreatureSpawns(holder.event.raw.param2))
                    {
                        plan.leadMap = spawn.mapId;
                        plan.leadX = spawn.x;
                        plan.leadY = spawn.y;
                        plan.leadZ = spawn.z;
                        break;
                    }
                plan.leadRange = float(holder.event.raw.param3);
            }
        }
        if (!gossip || !turns)
            return std::nullopt;
        if (!follows)
            plan.leadMap = 0, plan.leadRange = 0.0f;
        plan.kind = InteractionKind::Provoke;
        plan.target = InteractionTarget::Creature;
        plan.targetEntries = { entry };
        plan.range = ObjectUseRange;
        plan.describe = plan.leadRange > 0.0f ?
            Acore::StringFormat("start {}, bring it to its place and fight it", info->Name) :
            Acore::StringFormat("talk to {} and fight it", info->Name);
        return plan;
    }

    // A creature that cannot be killed and is released by an object instead: its SmartAI makes an object near it
    // selectable once it is low (SMART_EVENT_HEALTH_PCT -> SMART_ACTION_REMOVE_GO_FLAG GO_FLAG_NOT_SELECTABLE on a game
    // object entry within a distance), and that object's spell kills it. "The Stones That Bind Us" (2681): the Servants
    // of Razelikh, Grol, Allistarj and Sevine are invincible at 1 HP, at 0-5 % they stun themselves and free their Stone
    // of Binding, whose use casts Shackle Shatter on them and they die. As a kill objective the fight never ended
    // (Retadk and Bloodyo, 2026-11-14).
    std::optional<InteractionPlan> PlanReleaseObject(uint32 entry)
    {
        CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(entry);
        if (!info || info->AIName != "SmartAI")
            return std::nullopt;
        for (SmartScriptHolder const& holder : sSmartScriptMgr->GetScript(int32(entry), SMART_SCRIPT_TYPE_CREATURE))
        {
            if (holder.GetEventType() != SMART_EVENT_HEALTH_PCT || holder.GetActionType() != SMART_ACTION_REMOVE_GO_FLAG ||
                !(holder.action.goFlag.flag & GO_FLAG_NOT_SELECTABLE) || holder.GetTargetType() != SMART_TARGET_GAMEOBJECT_DISTANCE ||
                !holder.target.goDistance.entry || !sObjectMgr->GetGameObjectTemplate(holder.target.goDistance.entry))
                continue;
            InteractionPlan plan;
            plan.kind = InteractionKind::UseObject;
            plan.target = InteractionTarget::GameObject;
            plan.targetEntries = { holder.target.goDistance.entry };
            plan.range = ObjectUseRange;
            plan.beatEntries = { entry };
            plan.beatBelow = uint8(std::clamp<uint32>(holder.event.minMaxRepeat.max, 1, 99));
            plan.describe = Acore::StringFormat("bring {} below {}% health and use {}", CreatureName(entry), uint32(plan.beatBelow),
                ObjectName(holder.target.goDistance.entry));
            return plan;
        }
        return std::nullopt;
    }

    std::optional<InteractionPlan> PlanCreatureObjective(Player* player, Quest const* quest, uint32 entry)
    {
        if (std::optional<InteractionPlan> released = PlanReleaseObject(entry))
            return released;
        // A quest item whose spell is meant for this objective.
        for (uint32 itemId : QuestItems(quest))
        {
            uint32 const spellId = UseSpellOf(itemId);
            SpellInfo const* spell = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
            if (!spell)
                continue;
            SpellConditions const conditions = ConditionsOf(spell);
            bool const credits = HasEffect(spell, SPELL_EFFECT_KILL_CREDIT, int32(entry)) ||
                HasEffect(spell, SPELL_EFFECT_KILL_CREDIT2, int32(entry));
            bool related = credits || std::find(conditions.creatures.begin(), conditions.creatures.end(), entry) != conditions.creatures.end();
            // The spell names another creature as target (the owlkin to inoculate, credit for the inoculated one): the
            // quest gave the bot this item for this objective.
            if (!related && (!conditions.creatures.empty() || NeedsUnitTarget(spell)))
                related = itemId == quest->GetSrcItemId() || std::find(std::begin(quest->ItemDrop), std::end(quest->ItemDrop), itemId) != std::end(quest->ItemDrop);
            // The quest's own item brings the objective's creature, which stands nowhere: its spell needs a spell focus,
            // sends an event or summons that creature. "Cleansing Witch Hill" lights the Witchbane Torch at the end of the
            // dock (spell focus) and Zelfrax comes; the bot waited 314 s in "the quest area" for him, gave the quest up
            // and took it again (Nochnwar). The plan uses the item where its spell wants it; the AI fights what comes.
            // Not when the credit comes from other creatures' deaths (CreditSources): the Captured Totem of "The Grimtotem
            // Weapon" summons the credit creature itself, and the credit comes from the crocolisks killed.
            bool bringsCreature = false;
            LoadCreditSources();
            if (!related && itemId == quest->GetSrcItemId() && sWorldIndex.GetCreatureSpawns(entry).empty() && !NeedsUnitTarget(spell) &&
                !_creditSources.count(entry))
            {
                bool sendsOrSummons = HasEffect(spell, SPELL_EFFECT_SUMMON, int32(entry));
                for (SpellEffectInfo const& effect : spell->Effects)
                    if (effect.Effect == SPELL_EFFECT_SEND_EVENT)
                        sendsOrSummons = true;
                bringsCreature = spell->RequiresSpellFocus || sendsOrSummons;
                related = bringsCreature;
            }
            // A key of the quest that opens a lock on an object (KeyObjects), for an objective creature without a spawn: the
            // object's use brings it. "Spirits of Stonemaul Hold": the Energized Totem opens the Ogre Remains, whose trap
            // summons a Stonemaul Spirit to put to rest; Retadin rode up and down "the quest area" (the owner, 2026-11-14).
            if (!related && NeedsObjectTarget(spell) && !KeyObjects(itemId).empty() && (itemId == quest->GetSrcItemId() ||
                std::find(std::begin(quest->ItemDrop), std::end(quest->ItemDrop), itemId) != std::end(quest->ItemDrop)))
                for (SpellEffectInfo const& effect : spell->Effects)
                    if (effect.Effect == SPELL_EFFECT_OPEN_LOCK)
                    {
                        related = true;
                        bringsCreature = sWorldIndex.GetCreatureSpawns(entry).empty() && !_creditSources.count(entry);
                        break;
                    }
            if (!related)
                continue;

            InteractionPlan plan;
            plan.kind = InteractionKind::UseItem;
            plan.itemId = itemId;
            plan.spellId = spellId;
            plan.range = RangeOf(spell);
            plan.targetDead = conditions.mustBeDead;
            plan.targetAuras = conditions.targetAuras;
            plan.targetNoAuras = conditions.targetNoAuras;
            // A key: its spell opens a lock on a game object (its explicit target), and the creature its conditions name is
            // the one set free. Used on the prisoner, the Grimtotem Key of "Prisoners of the Grimtotems" failed with
            // SPELL_FAILED_BAD_TARGETS (12). The key goes on a game object whose lock it opens (KeyObjects).
            bool opensLock = false;
            for (SpellEffectInfo const& effect : spell->Effects)
                if (effect.Effect == SPELL_EFFECT_OPEN_LOCK)
                    opensLock = true;
            std::vector<uint32> const& locked = opensLock && NeedsObjectTarget(spell) ? KeyObjects(itemId) : NoEntries;
            if (!locked.empty())
            {
                plan.target = InteractionTarget::GameObject;
                plan.targetEntries = locked;
                plan.range = ObjectUseRange;
                plan.targetDead = false;
            }
            else if (!conditions.creatures.empty())
            {
                plan.target = InteractionTarget::Creature;
                // One item for several objectives (Umi's Mechanical Yeti on Legacki, Sprinkle and Quixxil): each objective
                // acts on its own creature. Another creature than the objective's (inoculate the owlkin, credit for the
                // inoculated one) keeps the list.
                bool const named = std::find(conditions.creatures.begin(), conditions.creatures.end(), entry) != conditions.creatures.end();
                plan.targetEntries = named ? std::vector<uint32>{ entry } : conditions.creatures;
            }
            else if (NeedsUnitTarget(spell))
            {
                plan.target = InteractionTarget::Creature;
                plan.targetEntries = { entry };
            }
            else
                WhereToStand(spell, conditions, plan);
            plan.bringsCreature = bringsCreature;
            if (plan.target == InteractionTarget::None && plan.walkObjects.empty() && !bringsCreature)
            {
                // Nowhere named: use it next to the objective's creature (an area effect, a credit by proximity).
                plan.target = InteractionTarget::Creature;
                plan.targetEntries = { entry };
                plan.range = ObjectUseRange * 2.0f;
            }
            // A spell for a weakened creature only (TargetAuraState: below 20 or 35 % health): "capture a raptor" with the
            // Sturdy Rope failed on raptors at full health, SPELL_FAILED_TARGET_AURASTATE (111), a hundred times (Duduheal,
            // "Raptor Captor"). The bot beats it down first (OrderInteraction).
            if (plan.target == InteractionTarget::Creature && !plan.targetDead)
            {
                if (spell->TargetAuraState == AURA_STATE_HEALTHLESS_20_PERCENT)
                    plan.weakenBelow = 20;
                else if (spell->TargetAuraState == AURA_STATE_HEALTHLESS_35_PERCENT)
                    plan.weakenBelow = 35;
            }
            for (NearRule const& rule : NearRules)
                if (rule.spellId == spellId)
                {
                    plan.nearCreature = rule.creature;
                    plan.nearRange = rule.range;
                    plan.nearAura = rule.aura;
                }
            plan.describe = Acore::StringFormat("use {} on {}{}{}", ItemName(itemId), plan.target == InteractionTarget::Creature ?
                CreatureName(plan.targetEntries.front()) : plan.target == InteractionTarget::GameObject ?
                ObjectName(plan.targetEntries.front()) : (plan.walkObjects.empty() ? std::string("the spot") :
                Acore::StringFormat("next to {}", ObjectName(plan.walkObjects.front()))),
                plan.weakenBelow ? Acore::StringFormat(" below {}% health", uint32(plan.weakenBelow)) : std::string(),
                bringsCreature ? Acore::StringFormat(" and fight {}", CreatureName(entry)) : std::string());
            return plan;
        }

        // Credit that comes with the kill of other creatures (KillCredit, a credit on their death): a kill objective, the
        // planner looks for those creatures (QuestInteraction::CreditSources). Before, the Restless Apparition of "What's
        // Haunting Witch Hill?", a friendly SmartAI creature that only appears for a moment after a Risen dies, got a
        // "help with spells" plan, and two bots spent ten minutes waiting for one ("no target in sight").
        LoadCreditSources();
        if (_creditSources.count(entry))
            return std::nullopt;

        // Credit that comes when a spell hits a creature (its SmartAI spell hit casts the credit on the caster): the bot
        // casts that spell at such a creature. "Thirst Unending" (8346), the blood elves' Arcane Torrent near a Mana Wyrm:
        // before, an objective at "the quest area" for the credit creature, which stands nowhere; NewRpg gave it up after
        // five minutes, twice, and the quest was blocked.
        if (auto const hits = _spellHitCredits.find(entry); hits != _spellHitCredits.end())
        {
            InteractionPlan plan;
            plan.kind = InteractionKind::CastSpells;
            plan.target = InteractionTarget::Creature;
            plan.range = DefaultSpellRange;
            for (auto const& [creature, spellId] : hits->second)
            {
                AddUnique(plan.targetEntries, creature);
                AddUnique(plan.castSpells, spellId);
                // A spell around the caster (Arcane Torrent) hits within its radius only.
                if (SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId))
                {
                    float reach = spell->GetMaxRange(false);
                    for (SpellEffectInfo const& effect : spell->Effects)
                        if (effect.Effect && effect.HasRadius() && (!reach || effect.TargetA.GetTarget() == TARGET_SRC_CASTER ||
                            effect.TargetA.GetTarget() == TARGET_UNIT_SRC_AREA_ENEMY || effect.TargetB.GetTarget() == TARGET_UNIT_SRC_AREA_ENEMY))
                            reach = effect.CalcRadius();
                    if (reach > 0.0f)
                        plan.range = std::min(plan.range, std::max(3.0f, reach - 2.0f));
                }
            }
            SpellInfo const* first = sSpellMgr->GetSpellInfo(plan.castSpells.front());
            plan.describe = Acore::StringFormat("cast {} at {}", first ? first->SpellName[0] : "a spell",
                CreatureName(plan.targetEntries.front()));
            return plan;
        }

        // A friendly creature that killing cannot credit: a scripted NPC that wants the bot's help (heal, buff).
        CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(entry);
        if (!info)
            return std::nullopt;
        if (QuestInteraction::CanKill(player, entry))
            return std::nullopt;
        if (std::optional<InteractionPlan> provoke = PlanProvoke(player, entry))
            return provoke;
        if (!info->ScriptID && info->AIName.empty())
            return std::nullopt;
        // One to talk to is a gossip quest ("speak with", escorts start that way), not one for spells.
        if (info->npcflag & (UNIT_NPC_FLAG_GOSSIP | UNIT_NPC_FLAG_QUESTGIVER) || info->GossipMenuId)
            return std::nullopt;
        InteractionPlan plan;
        plan.kind = InteractionKind::CastSpells;
        plan.target = InteractionTarget::Creature;
        plan.targetEntries = { entry };
        plan.range = DefaultSpellRange;
        plan.describe = Acore::StringFormat("help {} with spells", info->Name);
        return plan;
    }

    std::optional<InteractionPlan> PlanObjectObjective(Quest const* quest, uint32 entry)
    {
        for (uint32 itemId : QuestItems(quest))
        {
            uint32 const spellId = UseSpellOf(itemId);
            SpellInfo const* spell = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
            if (!spell)
                continue;
            SpellConditions const conditions = ConditionsOf(spell);
            bool const named = std::find(conditions.gameObjects.begin(), conditions.gameObjects.end(), entry) != conditions.gameObjects.end();
            bool const nearby = std::find(conditions.nearObjects.begin(), conditions.nearObjects.end(), entry) != conditions.nearObjects.end();
            if (!named && !nearby && !NeedsObjectTarget(spell))
                continue;

            InteractionPlan plan;
            plan.kind = InteractionKind::UseItem;
            plan.itemId = itemId;
            plan.spellId = spellId;
            if (named || NeedsObjectTarget(spell))
            {
                plan.target = InteractionTarget::GameObject;
                plan.targetEntries = named ? conditions.gameObjects : std::vector<uint32>{ entry };
                plan.range = std::min(RangeOf(spell), 10.0f);
            }
            else
            {
                plan.target = InteractionTarget::None;
                plan.walkObjects = { entry };
                plan.range = std::clamp(conditions.nearDistance - 3.0f, ObjectUseRange, 20.0f);
            }
            plan.describe = Acore::StringFormat("use {} at {}", ItemName(itemId), ObjectName(entry));
            return plan;
        }

        GameObjectTemplate const* info = sObjectMgr->GetGameObjectTemplate(entry);
        if (!info)
            return std::nullopt;
        InteractionPlan plan;
        plan.kind = InteractionKind::UseObject;
        plan.target = InteractionTarget::GameObject;
        plan.targetEntries = { entry };
        plan.range = ObjectUseRange;
        plan.describe = Acore::StringFormat("use {}", info->name);
        return plan;
    }

    std::optional<InteractionPlan> PlanItemObjective(Quest const* quest, uint32 itemId)
    {
        // An item whose spell creates the required item (fill the phial at the moonwell, bind the pages, capture the
        // spirit of a slain beast): the quest's own items first, then any other item that does (handed out by the quest
        // giver, from a container, from an earlier quest). The bot uses the first one it carries (Usable()).
        std::vector<std::pair<uint32, uint32>> creators;
        for (uint32 sourceItem : QuestItems(quest))
            if (uint32 const spellId = UseSpellOf(sourceItem))
                if (SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId); spell && CreatesItem(spell, itemId))
                    creators.emplace_back(sourceItem, spellId);
        for (auto const& creator : CreatorsOf(itemId))
            if (std::find(creators.begin(), creators.end(), creator) == creators.end())
                creators.push_back(creator);

        std::optional<InteractionPlan> result;
        for (auto const& [sourceItem, spellId] : creators)
        {
            SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId);
            if (!spell)
                continue;
            SpellConditions const conditions = ConditionsOf(spell);
            InteractionPlan plan;
            plan.kind = InteractionKind::UseItem;
            plan.itemId = sourceItem;
            plan.spellId = spellId;
            plan.range = RangeOf(spell);
            plan.targetDead = conditions.mustBeDead;
            plan.targetAuras = conditions.targetAuras;
            plan.targetNoAuras = conditions.targetNoAuras;
            if (!conditions.creatures.empty())
            {
                plan.target = InteractionTarget::Creature;
                plan.targetEntries = conditions.creatures;
            }
            else if (!conditions.gameObjects.empty())
            {
                plan.target = InteractionTarget::GameObject;
                plan.targetEntries = conditions.gameObjects;
                plan.range = std::min(plan.range, 10.0f);
            }
            else
                WhereToStand(spell, conditions, plan);
            // Nothing to stand next to and no area: wherever the bot stands (it only combines what it carries).
            plan.anywhere = plan.target == InteractionTarget::None && plan.walkObjects.empty() && spell->AreaGroupId <= 0;
            plan.describe = Acore::StringFormat("use {} for {}{}", ItemName(sourceItem), ItemName(itemId),
                plan.walkObjects.empty() ? (plan.target == InteractionTarget::Creature ? Acore::StringFormat(" on {}{}",
                plan.targetDead ? "a slain " : "", CreatureName(plan.targetEntries.front())) : std::string()) :
                Acore::StringFormat(" next to {}", ObjectName(plan.walkObjects.front())));
            if (!result)
                result = std::move(plan);
            else
                result->alternatives.push_back(std::move(plan));
        }
        if (result)
            return result;

        // A creature that drops the item stands nowhere, and the quest's own item brings it: its spell needs a spell
        // focus, sends an event or summons the creature. "Echeyakee" (881): the Horn of Echeyakee is blown at Echeyakee's
        // Lair (spell focus, an event) and Echeyakee comes with the hide; the plan was "the quest area", and NewRpg gave the
        // quest up after five minutes (Hordeboss, the test guild). The plan blows the horn where its spell wants it and
        // fights what comes (OrderInteraction); the auto loot takes the hide.
        if (sWorldIndex.GetItemSources(itemId).empty())
            if (QueryResult rows = WorldDatabase.Query("SELECT DISTINCT ct.entry FROM creature_template ct JOIN creature_loot_template clt "
                "ON clt.Entry = ct.lootid WHERE ct.lootid <> 0 AND clt.Item = {} AND clt.Reference = 0", itemId))
            {
                std::vector<uint32> droppers;
                do
                {
                    uint32 const entry = rows->Fetch()[0].Get<uint32>();
                    if (sWorldIndex.GetCreatureSpawns(entry).empty())
                        droppers.push_back(entry);
                } while (rows->NextRow());
                for (uint32 sourceItem : QuestItems(quest))
                {
                    uint32 const spellId = UseSpellOf(sourceItem);
                    SpellInfo const* spell = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
                    if (!spell || NeedsUnitTarget(spell) || droppers.empty())
                        continue;
                    uint32 brought = 0;
                    for (uint32 entry : droppers)
                        if (HasEffect(spell, SPELL_EFFECT_SUMMON, int32(entry)))
                            brought = entry;
                    bool sends = false;
                    for (SpellEffectInfo const& effect : spell->Effects)
                        if (effect.Effect == SPELL_EFFECT_SEND_EVENT)
                            sends = true;
                    if (!brought && (spell->RequiresSpellFocus || sends) && droppers.size() == 1)
                        brought = droppers.front();
                    if (!brought)
                        continue;
                    SpellConditions const conditions = ConditionsOf(spell);
                    InteractionPlan plan;
                    plan.kind = InteractionKind::UseItem;
                    plan.itemId = sourceItem;
                    plan.spellId = spellId;
                    plan.range = RangeOf(spell);
                    WhereToStand(spell, conditions, plan);
                    plan.bringsCreature = true;
                    plan.brought = brought;
                    plan.describe = Acore::StringFormat("use {}{} and fight {} for {}", ItemName(sourceItem), plan.walkObjects.empty() ?
                        std::string() : Acore::StringFormat(" next to {}", ObjectName(plan.walkObjects.front())), CreatureName(brought),
                        ItemName(itemId));
                    return plan;
                }
            }

        // Only game objects hold the item: open them (the planner loots the quest item itself; NewRpg stands next to a
        // chest with an "open" lock without opening it).
        // A creature that drops it too does not hand the item to killing alone: the planner picks the nearest source, and
        // the 67 Hyacinth Mushrooms beside a few Grells were objects NewRpg stood next to without opening them. The
        // objects are opened when they are at least as many as the creatures that drop it.
        std::vector<SpawnPoint> const sources = sWorldIndex.GetItemSources(itemId);
        if (sources.empty())
            return std::nullopt;
        InteractionPlan plan;
        plan.kind = InteractionKind::UseObject;
        plan.target = InteractionTarget::GameObject;
        plan.range = ObjectUseRange;
        uint32 objects = 0, creatures = 0;
        for (SpawnPoint const& source : sources)
        {
            if (!source.gameObject)
            {
                ++creatures;
                continue;
            }
            GameObjectTemplate const* info = sObjectMgr->GetGameObjectTemplate(source.entry);
            if (!info || !OpensWithoutSkill(info))
                continue; // a profession's node or a locked box: not for the planner
            ++objects;
            AddUnique(plan.targetEntries, source.entry);
        }
        if (!objects || creatures > objects)
            return std::nullopt;
        plan.describe = Acore::StringFormat("open {} for {}", ObjectName(plan.targetEntries.front()), ItemName(itemId));
        return plan;
    }

    void LoadQuestTriggers()
    {
        _triggersLoaded = true;
        if (QueryResult result = WorldDatabase.Query("SELECT `id`, `quest` FROM `areatrigger_involvedrelation`"))
        {
            do
            {
                Field* fields = result->Fetch();
                _questTriggers[fields[1].Get<uint32>()].push_back(fields[0].Get<uint32>());
            } while (result->NextRow());
        }
    }
}

InteractionPlan const* QuestInteraction::Resolve(Player* player, Quest const* quest, int32 objective)
{
    if (!player || !quest || objective < 0 || objective >= QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT)
        return nullptr;
    uint64 const key = (uint64(quest->GetQuestId()) << 5) | (uint64(objective) << 1) | uint64(player->GetTeamId() == TEAM_HORDE ? 1 : 0);
    auto itr = _plans.find(key);
    if (itr == _plans.end())
    {
        std::optional<InteractionPlan> plan;
        if (objective < QUEST_OBJECTIVES_COUNT)
        {
            int32 const target = quest->RequiredNpcOrGo[objective];
            if (target > 0)
                plan = PlanCreatureObjective(player, quest, uint32(target));
            else if (target < 0)
                plan = PlanObjectObjective(quest, uint32(-target));
        }
        else if (uint32 const item = quest->RequiredItemId[objective - QUEST_OBJECTIVES_COUNT])
            plan = PlanItemObjective(quest, item);
        itr = _plans.emplace(key, std::move(plan)).first;
    }
    return itr->second ? &*itr->second : nullptr;
}

InteractionPlan const* QuestInteraction::ResolveExplore(Player* player, Quest const* quest)
{
    if (!player || !quest || !quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT))
        return nullptr;
    if (!_triggersLoaded)
        LoadQuestTriggers();
    uint32 const key = (quest->GetQuestId() << 1) | (player->GetTeamId() == TEAM_HORDE ? 1 : 0);
    auto itr = _explore.find(key);
    if (itr == _explore.end())
    {
        std::optional<InteractionPlan> plan;
        auto triggers = _questTriggers.find(quest->GetQuestId());
        if (triggers != _questTriggers.end())
            for (uint32 trigger : triggers->second)
                if (sObjectMgr->GetAreaTrigger(trigger))
                {
                    plan = InteractionPlan();
                    plan->kind = InteractionKind::Explore;
                    plan->areaTrigger = trigger;
                    plan->describe = Acore::StringFormat("explore area trigger {}", trigger);
                    break;
                }
        itr = _explore.emplace(key, std::move(plan)).first;
    }
    return itr->second ? &*itr->second : nullptr;
}

namespace
{
    // The corpses an interaction keeps (KeepCorpses): per bot the creature entries and until when (getMSTime).
    std::shared_mutex keptCorpsesLock;
    std::unordered_map<ObjectGuid, std::pair<std::vector<uint32>, uint32>> keptCorpses;
}

void QuestInteraction::KeepCorpses(ObjectGuid bot, std::vector<uint32> const& entries, uint32 forMs)
{
    std::unique_lock<std::shared_mutex> lock(keptCorpsesLock);
    keptCorpses[bot] = { entries, getMSTime() + forMs };
}

bool QuestInteraction::KeepsCorpse(ObjectGuid bot, uint32 creatureEntry)
{
    std::shared_lock<std::shared_mutex> lock(keptCorpsesLock);
    auto const found = keptCorpses.find(bot);
    if (found == keptCorpses.end() || int32(found->second.second - getMSTime()) <= 0)
        return false;
    std::vector<uint32> const& entries = found->second.first;
    return std::find(entries.begin(), entries.end(), creatureEntry) != entries.end();
}

bool QuestInteraction::CanKill(Player* player, uint32 creatureEntry)
{
    CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(creatureEntry);
    return info && WorldIndex::IsAttackableFaction(info->faction, player) &&
        !(info->unit_flags & (UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_IMMUNE_TO_PC));
}

bool QuestInteraction::HasMeans(Player* player, InteractionPlan const& plan)
{
    switch (plan.kind)
    {
        case InteractionKind::UseItem:
        {
            if (!player->HasItemCount(plan.itemId, 1))
                return false;
            // The spell's reagents too (the other pages of a chapter, the charm's other half): until the bot has them,
            // collecting them is its work (the quest's other objectives).
            if (SpellInfo const* spell = sSpellMgr->GetSpellInfo(plan.spellId))
                for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
                    if (spell->Reagent[i] > 0 && spell->ReagentCount[i] && !player->HasItemCount(uint32(spell->Reagent[i]), spell->ReagentCount[i]))
                        return false;
            return true;
        }
        case InteractionKind::CastSpells:
            if (!plan.castSpells.empty())
                return std::any_of(plan.castSpells.begin(), plan.castSpells.end(), [player](uint32 spellId)
                {
                    return player->HasSpell(spellId);
                });
            return !HelpfulSpells(player).empty();
        case InteractionKind::UseObject:
        case InteractionKind::Explore:
        case InteractionKind::Provoke:
            return true;
        case InteractionKind::None:
            break;
    }
    return false;
}

InteractionPlan const* QuestInteraction::Usable(Player* player, InteractionPlan const& plan)
{
    if (HasMeans(player, plan))
        return &plan;
    for (InteractionPlan const& alternative : plan.alternatives)
        if (HasMeans(player, alternative))
            return &alternative;
    return nullptr;
}

std::vector<uint32> const& QuestInteraction::SpotCreatures(InteractionPlan const& plan)
{
    if (!plan.beatEntries.empty())
        return plan.beatEntries;
    return plan.target == InteractionTarget::Creature ? plan.targetEntries : NoEntries;
}

std::vector<uint32> const& QuestInteraction::SpotGameObjects(InteractionPlan const& plan)
{
    if (!plan.beatEntries.empty())
        return NoEntries;
    if (plan.target == InteractionTarget::GameObject)
        return plan.targetEntries;
    return plan.target == InteractionTarget::None ? plan.walkObjects : NoEntries;
}

std::vector<uint32> QuestInteraction::HelpfulSpells(Player* player)
{
    std::vector<uint32> heals;
    std::vector<uint32> buffs;
    for (auto const& [spellId, spell] : player->GetSpellMap())
    {
        if (!spell || spell->State == PLAYERSPELL_REMOVED || !spell->Active)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info || info->IsPassive() || !info->IsPositive() || info->IsProfession() || info->IsPrimaryProfessionFirstRank() ||
            !NeedsUnitTarget(info) || info->HasAttribute(SPELL_ATTR0_DO_NOT_DISPLAY) || info->CalcCastTime() > 5 * IN_MILLISECONDS)
            continue;
        bool heal = false;
        bool buff = false;
        for (SpellEffectInfo const& effect : info->Effects)
        {
            if (effect.Effect == SPELL_EFFECT_HEAL ||
                (effect.Effect == SPELL_EFFECT_APPLY_AURA && effect.ApplyAuraName == SPELL_AURA_PERIODIC_HEAL))
                heal = true;
            else if (effect.Effect == SPELL_EFFECT_APPLY_AURA)
                buff = true;
        }
        if (heal)
            heals.push_back(spellId);
        else if (buff)
            buffs.push_back(spellId);
    }
    std::sort(heals.begin(), heals.end());
    std::sort(buffs.begin(), buffs.end());
    heals.insert(heals.end(), buffs.begin(), buffs.end());
    if (heals.size() > MaxHelpfulSpells)
        heals.resize(MaxHelpfulSpells);
    return heals;
}

uint32 QuestInteraction::UseItem(Player* player, uint32 itemId, uint32 spellId, WorldObject* target)
{
    Item* item = player->GetItemByEntry(itemId);
    SpellInfo const* spellInfo = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
    if (!item || !spellInfo)
        return 0xFFFF;

    WorldPacket packet(CMSG_USE_ITEM, 1 + 1 + 1 + 4 + 8 + 4 + 1 + 4 + 9);
    packet << uint8(item->GetBagSlot()) << uint8(item->GetSlot()) << uint8(1) << uint32(spellId) << item->GetGUID()
           << uint32(0) << uint8(0);
    SpellCastTargets targets;
    if (target && target->ToUnit())
        targets.SetUnitTarget(target->ToUnit());
    else if (target && target->ToGameObject())
        targets.SetGOTarget(target->ToGameObject());
    targets.Write(packet);

    // The core's verdict before the use, for the activity log (as playerbots' CanCastSpell checks a cast).
    Spell* check = new Spell(player, spellInfo, TRIGGERED_NONE);
    check->m_targets = targets;
    check->m_CastItem = item;
    uint32 const result = uint32(check->CheckCast(true));
    delete check;

    player->GetSession()->HandleUseItemOpcode(packet);
    return result;
}

std::string QuestInteraction::UseGameObject(Player* player, GameObject* object)
{
    // A locked object (a chest with an "open" lock): the client casts the character's open-lock spell for the lock's type
    // ("Opening"), which opens the loot; using the object would do nothing for a chest. Instant, as the planner's
    // gathering in passing does (AutoLoot).
    if (LockEntry const* lock = sLockStore.LookupEntry(object->GetGOInfo()->GetLockId()))
    {
        for (uint8 i = 0; i < MAX_LOCK_CASE; ++i)
        {
            if (lock->Type[i] != LOCK_KEY_SKILL)
                continue;
            for (auto const& [spellId, spell] : player->GetSpellMap())
            {
                if (!spell || spell->State == PLAYERSPELL_REMOVED)
                    continue;
                SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
                if (info && HasEffect(info, SPELL_EFFECT_OPEN_LOCK, int32(lock->Index[i])))
                {
                    SpellCastResult const result = player->CastSpell(object, spellId, true);
                    return Acore::StringFormat("open-lock spell {} for lock {} type {} (result {}), loot window {}", spellId,
                        lock->ID, lock->Index[i], uint32(result), player->GetLootGUID() == object->GetGUID() ? "yes" : "no");
                }
            }
        }
    }
    WorldPacket packet(CMSG_GAMEOBJ_USE, 8);
    packet << object->GetGUID();
    player->GetSession()->HandleGameObjectUseOpcode(packet);
    return Acore::StringFormat("object use (lock {}), loot window {}", object->GetGOInfo()->GetLockId(),
        player->GetLootGUID() == object->GetGUID() ? "yes" : "no");
}

uint32 QuestInteraction::TakeObjectLoot(Player* player, GameObject* object, uint32 neededItem)
{
    if (player->GetLootGUID() != object->GetGUID())
        return 0;
    Loot* loot = &object->loot;
    uint32 taken = 0;
    uint32 const maxSlot = loot->GetMaxSlotInLootFor(player);
    for (uint32 i = 0; i < maxSlot; ++i)
    {
        if (!loot->LootItemInSlot(i, player))
            continue;
        InventoryResult msg = EQUIP_ERR_OK;
        player->StoreLootItem(uint8(i), loot, msg);
        if (msg == EQUIP_ERR_OK)
            ++taken;
    }
    player->GetSession()->DoLootRelease(object->GetGUID());
    // The core makes a chest's loot anew only from GO_READY; an opened one keeps its loot and, per player, which quest
    // items that player took already (Hunterbm took the Horn of Awakening, gave the quest back with the horn and took it
    // again: "looted 0" three times, and Kerlonian could not be woken).
    if (!taken && neededItem && !player->HasItemCount(neededItem, 1) && object->GetGoType() == GAMEOBJECT_TYPE_CHEST &&
        object->getLootState() != GO_READY)
        object->SetLootState(GO_READY);
    return taken;
}

FollowerAid const* QuestInteraction::GetFollowerAid(Quest const* quest, uint32 followerEntry)
{
    if (!quest || !followerEntry)
        return nullptr;
    uint64 const key = (uint64(quest->GetQuestId()) << 32) | followerEntry;
    auto itr = _aids.find(key);
    if (itr == _aids.end())
    {
        std::vector<uint32> items = QuestItems(quest);
        for (uint32 item : quest->RequiredItemId)
            if (item)
                AddUnique(items, item);
        std::optional<FollowerAid> found;
        for (uint32 item : items)
        {
            uint32 const spellId = UseSpellOf(item);
            SpellInfo const* spell = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
            if (!spell)
                continue;
            SpellConditions const conditions = ConditionsOf(spell);
            if (std::find(conditions.creatures.begin(), conditions.creatures.end(), followerEntry) == conditions.creatures.end())
                continue;
            FollowerAid aid;
            aid.itemId = item;
            aid.spellId = spellId;
            aid.range = RangeOf(spell);
            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                if (quest->RequiredItemId[i] == item)
                    aid.itemObjective = QUEST_OBJECTIVES_COUNT + i;
            found = aid;
            break;
        }
        itr = _aids.emplace(key, found).first;
    }
    return itr->second ? &*itr->second : nullptr;
}

bool QuestInteraction::NeedsAid(Creature* follower)
{
    if (!follower || !follower->IsAlive() || follower->IsInCombat())
        return false;
    if (follower->getStandState() == UNIT_STAND_STATE_SLEEP)
        return true;
    return follower->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE;
}

std::vector<uint32> const& QuestInteraction::CreditSources(uint32 creditEntry)
{
    LoadCreditSources();
    auto const itr = _creditSources.find(creditEntry);
    return itr != _creditSources.end() ? itr->second : NoEntries;
}

void QuestInteraction::ClearCache()
{
    _creditSources.clear();
    _keyObjects.clear();
    _keysLoaded = false;
    _creditLoaded = false;
    _plans.clear();
    _explore.clear();
    _aids.clear();
    _questTriggers.clear();
    _triggersLoaded = false;
    _focusObjects.clear();
    _focusLoaded = false;
}
