/*
 * QuestInteraction: quest objectives that killing and looting do not complete (docs/orders.md "Quest interactions").
 *
 * mod-playerbots' NewRpg walks to a quest's spot and lets the grind and loot strategies work; it never uses a quest item,
 * casts a spell on a quest NPC or uses a quest object. The order planner does these itself:
 *   - UseItem:    a quest item (the quest's start item or one of its source items) whose spell gives the credit: on the
 *                 creature or game object its spell conditions name, next to the object a "near" condition names, at a
 *                 spell focus (a moonwell), or on the objective's creature;
 *   - CastSpells: a friendly, scripted creature as objective (an injured guard, a draenei survivor): the bot casts its own
 *                 helpful spells on it, heals first, until the credit comes;
 *   - UseObject:  a game object objective (a quest object), or a chest that holds the quest item and that needs no skill;
 *   - Explore:    the quest's area trigger;
 *   - Provoke:    a friendly objective creature that a gossip choice turns into a foe, now or after it followed the bot
 *                 to a place (The Threshwackonator 4100 to Gelkak Gyromast): talk, lead, then kill it.
 * Resolve() reads only static data (quest, item, spell, condition, creature and game object templates) and caches the
 * result per quest, objective and team; the acting helpers send the client's own packets through the bot's session, so
 * the core's checks (range, item, conditions, locks, quest status) apply as for a player.
 */

#ifndef MOD_GUILD_ADVENTURE_QUEST_INTERACTION_H
#define MOD_GUILD_ADVENTURE_QUEST_INTERACTION_H

#include "Define.h"
#include "ObjectGuid.h"
#include <string>
#include <vector>

class Creature;
class GameObject;
class Player;
class Quest;
class WorldObject;

namespace GuildAdventure
{
    enum class InteractionKind : uint8
    {
        None,
        UseItem,
        CastSpells,
        UseObject,
        Explore,
        Provoke
    };
    char const* ToString(InteractionKind kind);

    // What the item, the spells or the use act on.
    enum class InteractionTarget : uint8
    {
        None,       // nothing: the bot only has to stand at the right place (next to walkObjects, at the spot)
        Creature,
        GameObject
    };

    // An item that works only near a creature (InteractionPlan::nearCreature): the bot stands this far from it, towards
    // the target, and takes targets within this plus the item's range of one.
    constexpr float NearStandYards = 8.0f;

    struct InteractionPlan
    {
        InteractionKind kind = InteractionKind::None;
        uint32 itemId = 0;                       // UseItem
        uint32 spellId = 0;                      // UseItem: the item's spell
        InteractionTarget target = InteractionTarget::None;
        std::vector<uint32> targetEntries;       // creatures or game objects to act on
        std::vector<uint32> walkObjects;         // target None: game objects to stand next to (spell focus, "near" condition)
        float range = 5.0f;                      // act from this close (yards)
        bool targetDead = false;                 // the creature must be dead (use the item on its corpse; kill one first)
        std::vector<uint32> targetAuras;         // the creature must have these auras (its spell's conditions: a Lazy Peon
        std::vector<uint32> targetNoAuras;       // asleep) and must not have these
        uint32 brought = 0;                      // UseItem with bringsCreature for an item objective: the creature the use
                                                 // brings, which drops the item (Echeyakee and his hide)
        bool bringsCreature = false;             // UseItem: the use brings the objective's creature (a summon, an event);
                                                 // the fight with it gives the credit, so its result is waited for longer
        uint8 weakenBelow = 0;                   // UseItem: the creature must be below this health % first (20 or 35:
                                                 // the spell's TargetAuraState); the bot beats it down, then uses the item
        bool anywhere = false;                   // UseItem: no target, no place (combine pages, break an essence): where it stands
        uint32 nearCreature = 0;                 // UseItem: the caster must stand within nearRange of such a creature (a
        float nearRange = 0.0f;                  // spell script's check no data names: the Ooze Buster's Zeppelin Power Core)
        uint32 nearAura = 0;                     // ... which gives it this aura (the use waits for it)
        std::vector<uint32> beatEntries;         // UseObject: the objective's creature, brought below beatBelow % health
        uint8 beatBelow = 0;                     // first; at that line its script makes the object usable, and the object's
                                                 // spell kills it for the credit (the Stones of Binding of 2681)
        uint32 areaTrigger = 0;                  // Explore
        uint32 gossipMenu = 0;                   // Provoke: the menu and option whose choice starts it
        uint32 gossipOption = 0;
        uint32 leadMap = 0;                      // Provoke: where it turns (0: it turns where it stands)
        float leadX = 0.0f, leadY = 0.0f, leadZ = 0.0f;
        float leadRange = 0.0f;                  // ... within this distance of that place
        std::vector<uint32> castSpells;          // CastSpells: these spells (the credit comes when one hits the creature, a
                                                 // SmartAI spell hit: Arcane Torrent on a Mana Wyrm); empty: helpful spells
        std::string describe;                    // for the activity log ("use Inoculating Crystal on Nestlewood Owlkin")
        // Other items that do the same (another phial, the cover instead of the page); Usable() picks the first the bot has.
        std::vector<InteractionPlan> alternatives;
    };

    // A quest NPC that follows the character and has to be woken or called back with a quest item: Kerlonian falls
    // asleep (Horn of Awakening), Ringo faints (Spraggle's Canteen), Shay wanders off (Shay's Bell). The item is the one
    // whose on-use spell names that NPC in its conditions.
    struct FollowerAid
    {
        uint32 itemId = 0;
        uint32 spellId = 0;
        float range = 4.0f;          // use it from this close (yards)
        int32 itemObjective = -1;    // 4-9 when the quest asks for the item too (it is fetched as that objective: Resolve)
    };

    namespace QuestInteraction
    {
        // The interaction an objective (0-3 creature or game object, 4-9 item) needs, or nullptr when killing and
        // looting do it. Static data only, cached; HasMeans() checks the bot.
        InteractionPlan const* Resolve(Player* player, Quest const* quest, int32 objective);
        // Exploration quests: the interaction for the quest's area trigger (nullptr if the quest has none).
        InteractionPlan const* ResolveExplore(Player* player, Quest const* quest);
        // The player may attack creatures of this entry (faction and unit flags of the template).
        bool CanKill(Player* player, uint32 creatureEntry);
        // Creatures whose kill gives this entry's credit: creature_template KillCredit1/2 and SmartAI scripts that give the
        // credit on their death (a spell with a kill credit effect, SMART_ACTION_CALL_KILLEDMONSTER). Static data, cached.
        std::vector<uint32> const& CreditSources(uint32 creditEntry);
        // The bot can do it now: it carries the item and the spell's reagents; for CastSpells it knows a helpful spell.
        bool HasMeans(Player* player, InteractionPlan const& plan);
        // The plan or the first of its alternatives the bot has the means for (nullptr if none).
        InteractionPlan const* Usable(Player* player, InteractionPlan const& plan);
        // Creatures, game objects or spell foci to walk to, by entry (for the planner's spot search).
        std::vector<uint32> const& SpotCreatures(InteractionPlan const& plan);
        std::vector<uint32> const& SpotGameObjects(InteractionPlan const& plan);

        // The bot's helpful spells that can be cast on another creature (heals first, then buffs), at most a dozen.
        std::vector<uint32> HelpfulSpells(Player* player);

        // Corpses an interaction wants (an item used on a slain creature): the bot does not skin them meanwhile, because a
        // skinned corpse is gone at the core's next update (Creature::AllLootRemovedFromCorpse). Set by the interaction's
        // step for `forMs`, asked by auto loot on the map threads and by the gather detour (thread-safe).
        void KeepCorpses(ObjectGuid bot, std::vector<uint32> const& entries, uint32 forMs);
        bool KeepsCorpse(ObjectGuid bot, uint32 creatureEntry);

        // Uses the item like the client does (CMSG_USE_ITEM through the bot's session), on the target if any. Returns
        // the core's check of the cast beforehand (SpellCastResult, 255 = SPELL_CAST_OK; the use is sent either way, the
        // core refuses it itself), or 0xFFFF when the bot has no such item or the item no spell.
        uint32 UseItem(Player* player, uint32 itemId, uint32 spellId, WorldObject* target);
        // Uses the game object like the client does: a locked one with the character's open-lock spell for its lock type
        // ("Opening", the loot window opens), any other with CMSG_GAMEOBJ_USE.
        // Returns what it did, for the activity log ("open-lock spell 6478 (result 255), loot window yes").
        std::string UseGameObject(Player* player, GameObject* object);
        // After UseGameObject on a chest: takes what the bot may loot and closes the loot window like the client. With
        // `neededItem`: when nothing came out although the bot still lacks that item, the chest's loot is made anew at
        // its next opening (the old loot remembers that this character took the quest item before it gave the quest up).
        uint32 TakeObjectLoot(Player* player, GameObject* object, uint32 neededItem = 0);

        // The quest's item that wakes or calls back this follower (its start item, source items or required items),
        // nullptr if none. Static data only, cached.
        FollowerAid const* GetFollowerAid(Quest const* quest, uint32 followerEntry);
        // The follower needs its aid now: it sleeps (stand state), or it stopped following and is not fighting.
        bool NeedsAid(Creature* follower);

        // .ga reload and tests.
        void ClearCache();
    }
}

#endif
