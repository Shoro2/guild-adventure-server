/*
 * BotDriver: the one place where the order planner touches mod-playerbots internals (docs/orders.md "Executor").
 *
 * The module decides what a guild bot does; mod-playerbots carries it out. Three levers, all used on the world
 * thread outside map updates (the bot AI runs in map updates, so nothing here races it):
 *   - Strategy profiles of the bot's non-combat engine (adventure / follow / hold).
 *   - The NewRpg status of the bot (botAI->rpgInfo): "go to a position" (GO_GRIND / GO_CAMP), "work this quest"
 *     (DO_QUEST), "visit this NPC" (WANDER_NPC with a fixed target), "rest". NewRpg's actions do the movement,
 *     talk to quest givers, sell, repair and buy; the "grind" strategy fights. On this server NewRpg never picks a
 *     status by itself: every Playerbots.RpgStatusProbWeight.* is 0, so an idle NewRpg falls back to REST until the
 *     planner gives it the next status.
 *   - Master and group: only bots that follow have a master and sit in the owner's party.
 */

#ifndef MOD_GUILD_ADVENTURE_BOT_DRIVER_H
#define MOD_GUILD_ADVENTURE_BOT_DRIVER_H

#include "ObjectGuid.h"
#include <list>
#include <source_location>
#include <string>
#include <vector>

class Creature;
class DynamicObject;
class Item;
class Player;
class SpellInfo;
class PlayerbotAI;
class Quest;
class Unit;
class WorldObject;
struct ItemTemplate;

namespace GuildAdventure
{
    enum class StrategyProfile : uint8
    {
        None,
        Adventure, // quest, grind, travel through NewRpg; no master
        Travel,    // NewRpg without "grind": walks and talks, fights only what attacks it
        Follow,    // follow the master
        Hold       // stand still, fight back when attacked
    };

    char const* ToString(StrategyProfile profile);

    // NewRpg statuses the planner uses, mirrored so callers need no playerbots header.
    enum class RpgState : uint8
    {
        Idle,
        GoGrind,
        GoCamp,
        WanderRandom,
        WanderNpc,
        DoQuest,
        TravelFlight,
        Rest,
        Other
    };

    char const* ToString(RpgState state);

    namespace BotDriver
    {
        PlayerbotAI* GetAI(Player* bot);

        // Whether mod-playerbots fights with this character in melee (its class and talents), and the distance it
        // keeps to its target when it does not.
        bool FightsInMelee(Player* bot);
        float StandRange(Player* bot);
        // A strategy of the bot's combat engine on or off; true when that changed something.
        bool SetCombatStrategy(Player* bot, char const* name, bool on);
        bool HasCombatStrategy(Player* bot, char const* name);
        // A strategy of mod-playerbots in and out of a fight (a paladin's or priest's "rshadow": shadow resistance
        // aura, shadow protection); true when it changed.
        bool SetStrategyEverywhere(Player* bot, char const* name, bool on);
        // The blessing mod-playerbots' plan of the group leaves to this paladin next (its spell; 0: none, or not yet).
        // Also for a character its owner plays by hand (its placeholder AI never casts, and the bots of its group
        // leave its share of the blessings to it).
        uint32 BlessingLeftTo(Player* paladin, ObjectGuid& target);
        // A second shaman of a group: of each element the other totem than the first shaman's, where it knows it.
        // The changes for the log ("stoneskin for strength of earth, ..."); empty: nothing changed.
        std::string ComplementTotems(Player* first, Player* second);
        // The spell mod-playerbots knows by a name for this bot ("throw", "shoot bow"), 0: none.
        uint32 SpellIdByName(Player* bot, std::string const& name);

        // True if the bot's non-combat engine matches the profile (cheap; checked every planner tick).
        bool HasProfile(Player* bot, StrategyProfile profile);
        void ApplyProfile(Player* bot, StrategyProfile profile);

        // The bot's playerbots master (nullptr if none).
        Player* GetMaster(Player* bot);
        void SetMaster(Player* bot, Player* master);

        // Group policy: an autonomous bot leaves a normal party or raid (never a battleground or dungeon-finder
        // group); a following bot joins its master's party (no summon).
        void LeaveNormalGroup(Player* bot);
        bool JoinGroupOf(Player* bot, Player* master);

        RpgState GetRpgState(Player* bot);
        // Seconds the current NewRpg status has lasted.
        uint32 GetRpgStateSeconds(Player* bot);
        // The quest of a DO_QUEST status (0 otherwise).
        uint32 GetRpgQuest(Player* bot);

        // Walk to a position. grindOnArrival: GO_GRIND (fights around the spot afterwards) instead of GO_CAMP
        // (visits NPCs around the spot afterwards). Only quest mobs and attackers are fought on the way.
        void GoTo(Player* bot, uint32 mapId, float x, float y, float z, bool grindOnArrival);
        // Work one objective of a quest in the log at a known spot (objectiveIndex: 0-3 creature/GO, 4-9 item);
        // without a spot NewRpg picks one from the quest's POIs.
        void DoQuest(Player* bot, Quest const* quest, int32 objectiveIndex, bool hasSpot, float x, float y, float z);
        // Walk to and interact with a creature or game object in sight: quest accept / reward, sell, repair, buy.
        void Visit(Player* bot, ObjectGuid target);
        // True while NewRpg's visit goes to this target (after a while at one it picks the next NPC by itself).
        bool IsVisiting(Player* bot, ObjectGuid target);
        // True once NewRpg stood at the visit target (it has talked, sold, repaired and bought by then).
        bool HasReachedVisitTarget(Player* bot, ObjectGuid target);
        void Rest(Player* bot);
        // playerbots' list of "real" guilds (a guild whose leader plays on an account of a person): their bots run their
        // whole AI, every other bot alone gets the share Playerbots.BotActiveAlone gives (10 %). playerbots reads the list
        // at startup and once an hour, so a guild founded since is not on it (RosterService::Update reads it again).
        bool IsRealGuild(uint32 guildId);
        void ReloadRealGuilds();
        // Walk to a flight master, learn its flight path and fly the route (nodes the bot knows; NewRpg pays).
        void Fly(Player* bot, uint32 flightMasterEntry, uint32 mapId, float x, float y, float z, std::vector<uint32> const& route);

        // Boarding and leaving a ship: the bot cannot path onto a moving model, so it steps the last yards with a
        // short same-map teleport (from the pier onto the deck, from the deck onto the pier). Never used for travel.
        void Hop(Player* bot, float x, float y, float z);
        void LeaveTransport(Player* bot);

        // NewRpg gave up on the quest (no progress at its spot for 5 minutes, or it could not be turned in).
        bool HasAIGivenUp(Player* bot, uint32 questId);
        void ForgetAIGivenUp(Player* bot, uint32 questId);
        // Removes a quest from the bot's quest log like the client's "abandon quest".
        void AbandonQuest(Player* bot, uint32 questId);

        // playerbots counts deaths since the last repair and, for a bot without a client master, revives it after five
        // and teleports it to a random grinding spot for its level (RandomPlayerbotMgr::Revive), after ten it picks a
        // start-zone graveyard. Guild bots never get that teleport-for-level: their count is reset while they live.
        void ResetDeathCount(Player* bot);
        uint32 GetDeathCount(Player* bot);

        // NewRpg's long-distance walk teleports a bot straight to its destination when it has not come 5 yd closer for
        // 90 s (NewRpgBaseAction::MoveFarTo, stuck recovery). For a guild bot that is a teleport by order; the planner
        // has its own stuck rules (OrderService::UpdateStuck) and gives such a target up instead, so the counter's clock
        // is held back here (the progress it measures is kept).
        void HoldOffStuckTeleport(Player* bot);
        // The quest giver NewRpg is walking to "in passing" stands at (x, y, z) and cannot be reached: NewRpg skips it
        // from now on (as it does by itself after a minute at a giver, which never happens while its walk is cancelled
        // over and over). The bot's map thread, or the world thread outside map updates. True when there was one.
        bool GiveUpQuestGiverAt(Player* bot, float x, float y, float z);

        // playerbots mounts a bot for long walks. A bot that leads a follower on foot must not ride ahead of it: the
        // "mount" strategy is taken out while it does, and the bot gets off its mount.
        void SetMountAllowed(Player* bot, bool allowed);
        // playerbots' loot strategy has a target within `range` of the bot that it can loot (a chest, a pumpkin, a
        // corpse). It gets the bot off its mount for that; SetMountAllowed(false) keeps the bot off it meanwhile.
        [[nodiscard]] bool HasLootTarget(Player* bot, float range, std::string* name = nullptr, WorldObject** found = nullptr);
        // That target is put off: playerbots turns to it again later, if at all.
        void DeferLootTarget(Player* bot);

        // Makes the bot attack a unit it would not pick by itself (what attacks the NPC of a quest event): playerbots'
        // "attack" action without its chat answers. False when the target cannot be attacked from where the bot stands.
        // Without requireSight a target behind a corner is taken too: the bot's own movement goes to it (a dungeon
        // group that goes to help one of its members).
        bool Attack(Player* bot, Unit* target, bool requireSight = true);
        // What the bot's AI is about to attack or attacks (its "current target", else its victim); nullptr = nothing.
        Unit* IntendedTarget(Player* bot);
        // The AI lets go of its target: no attack, no cast, no walk towards it (the planner decides what comes next).
        void DropTarget(Player* bot);
        // The raid target icon the bot's AI takes as its damage target first ("skull", "cross", ...; mod-playerbots' "rti"
        // value, "skull" unless somebody changed it).
        void SetRti(Player* bot, std::string const& icon);
        // Lets go of a target the AI picked that the module does not want fought, without stopping the bot's walk: only
        // a chase of that target and a cast at it end. (DropTarget stops every movement: called four times a second
        // for a creature mod-playerbots picks again at every look, it held two bots in Stranglethorn on the spot for
        // two minutes, "movement 0; rpg do quest", their NewRpg walk to the quest's spot cleared at every look.)
        void LetGo(Player* bot, Unit* target);
        // Whether the bot let go of that creature in the last 30 s (mod-playerbots' "attack anything" leaves it alone).
        bool IsLetGo(Player const* bot, Unit const* target);

        // Lets a ghost accept the spirit healer's resurrection the way a client does (CMSG_SPIRIT_HEALER_ACTIVATE): back
        // to life at the graveyard with half health and mana, resurrection sickness and the durability loss. False
        // when the healer is out of interaction range.
        bool ResurrectAtSpiritHealer(Player* bot, Creature* healer);

        // At a vendor: playerbots' "buy" (ammo, reagents and consumables the bot uses, within its money). NewRpg only buys
        // when the bot also has something to sell. One call buys only one kind of consumable (food and drink share an
        // inventory type), so food and drink are bought by the planner itself (OrderService, BuyProvisions).
        void BuyFromVendor(Player* bot);

        // Rest (OrderService::StepRest): the AI does nothing for that long, so it does not stand up from its food or
        // drink to buff or loot. An attack wakes it at once (PlayerbotAI::UpdateAI resets the delay when the combat
        // state changes).
        // (The caller's place is kept for a pause of 3 s or more, so AIState can name who holds the AI.)
        void PauseAI(Player* bot, uint32 ms, std::source_location where = std::source_location::current());
        // Ground effects (the owner, 2026-11-14: [quote left out]):
        // mod-playerbots' strategy "avoid aoe" steps out of a hostile cloud, fire or trap, in and out of combat. Its
        // switch Playerbots.AutoAvoidAoe is read by nothing in our pin, so no bot had it: KeepAvoidAoe puts it on.
        void KeepAvoidAoe(Player* bot);
        // The hostile ground effect the bot stands in (an area aura of a dynamic object: a cloud, a puddle, a fire), its
        // spell's name; empty when it stands in none.
        std::string GroundEffectOn(Player* bot);
        // The dynamic object of that effect (its middle, radius and time left), or none.
        DynamicObject const* GroundEffectObject(Player* bot);
        // Steps out of it now with mod-playerbots' own action, also while the module holds the AI (a rest, a hold).
        bool StepOutOfGroundEffect(Player* bot);

        // Talents changed (SpecService): mod-playerbots chooses its strategies (damage, tank, healer) from the talent
        // tree with the most points again. The planner puts its strategy profile back on its next tick.
        void ResetStrategies(Player* bot);
        // A shaman's totems follow the place: in the open world one Searing Totem per fight, in a dungeon what its
        // talents give (mod-playerbots sets four totems for every fight: Strength of Earth, Magma, Healing Stream
        // and Windfury for an enhancement shaman, about a third of its mana each time it has walked 30 yards; a
        // level-37 shaman that led a squad spent 20 to 48 % of its mana per creature and rested after every second
        // one). 0: nothing changed, 1: the one totem is set, 2: the full set is back.
        uint8 KeepTotems(Player* bot);
        // A shaman's water and air totems for a fight that wants them (the Temple of Ahn'Qiraj's poison: Cleansing Totem
        // and Nature Resistance Totem); the other totems of those two elements go. True when it changed something.
        bool SetTotems(Player* bot, char const* water, char const* air);
        // A healer's focus heal targets (mod-playerbots heals only them while the list is set; empty: everybody again).
        void SetFocusHeal(Player* bot, std::list<ObjectGuid> const& targets);

        // Loot filter (docs/storage.md): true if the bot will use this looted item itself, by playerbots' item usage:
        // an equipment upgrade, a quest starter it can take, food, drink or potions it keeps, ammunition, a class
        // reagent it needs. Such an item stays in its bags instead of going to the guild storage.
        bool WantsItem(Player* bot, Item* item);
        // Guild storage (StorageSupply): true if playerbots rates this kind of item as equipment the bot should wear
        // instead of (or in addition to) what it wears now; EquipUpgrades makes it put on the upgrades in its bags.
        bool IsUpgrade(Player* bot, uint32 itemEntry, int32 randomProperty);
        // A necklace, ring, trinket, relic, or (for mages, priests, warlocks and druids) something held in the off
        // hand, that the bot can use and has a bare place for. The module's own rule: mod-playerbots never rates
        // such a piece as worth putting on where the bot wears nothing (its armour rule wants the armour kind of the
        // class for everything but cloaks, and these have none). Part of IsUpgrade and WantsItem.
        bool FillsEmptyPlace(Player* bot, ItemTemplate const* proto);
        bool IsJewellery(ItemTemplate const* proto);
        // True when the bot may not wear one more of this kind beside what it wears outside exceptSlot: a unique-
        // equipped piece it wears already, a limit category it has filled, an item it may carry only so many of
        // (MaxCount) that are all worn. mod-playerbots' own check counts wrong (its "equipped" count tells only the
        // bank apart) and rates such a piece as an upgrade for the other finger or trinket place; 13 of the owner's bots
        // took a second jewelcrafted ring out of the guild storage after every restart and put it back
        // (2026-11-14). Part of IsUpgrade, WantsItem and FillsEmptyPlace.
        bool WearsUniqueAlready(Player const* bot, ItemTemplate const* proto, uint8 exceptSlot = 255 /* NULL_SLOT */);
        // A piece only for the off hand (held in it, a shield, an off-hand weapon) while the bot holds a two-hand weapon
        // (without Titan's Grip): mod-playerbots calls it an upgrade for the empty off hand and tries to put it on at
        // every loot ("Equipping Antipodean Rod", a mage with its staff, the owner 2026-11-25). No upgrade (IsUpgrade, WantsItem).
        bool OffHandUnderTwoHand(Player const* bot, ItemTemplate const* proto);
        // A caster's hands as a whole (the owner, 2026-11-26: [quote left out]): mod-playerbots compares a two-hand weapon with the main hand alone, a staff counting half and a
        // caster's one-hand weapon 65 % of their stats, so a staff could take the place of a one-hander and an orb worth
        // more together. For a caster (mage, priest, warlock, a druid but a feral one, a shaman that cannot dual wield):
        // a two-hand weapon is an upgrade only when its stats are worth more than those of the main and off hand it would
        // replace, and a one-hand weapon in place of a two-hander only when it alone is worth more than the two-hander
        // (an off-hand piece then comes on top). Part of IsUpgrade and WantsItem.
        bool CasterHandsNoBetter(Player* bot, ItemTemplate const* proto, int32 randomProperty);
        // playerbots' score of that kind of item for this bot's class and spec (higher is better).
        float ItemScore(Player* bot, uint32 itemEntry, int32 randomProperty);
        // The same score for many kinds of item at once: one calculator, whose weights for the bot are made once
        // (scores[i] for items[i]: entry and random property). World thread only.
        void ItemScores(Player* bot, std::vector<std::pair<uint32, int32>> const& items, std::vector<float>& scores);
        // The talent tree the scores follow (mod-playerbots' spec tab of the bot).
        uint8 SpecTab(Player* bot);
        // The stat weights the scores follow are those of the owner's wowbot335 research (ga_stat_weight, generated by
        // tools/gen_stat_weights.py; docs/orders.md "Stat weights from wowbot335"): mod-playerbots patch 0002 lets the
        // module fill them in place of mod-playerbots' own, for its own item choices (loot, "equip upgrade") and the
        // module's alike. Startup (world thread): loads the table and puts the hook in (GuildAdventure.StatWeights.Enable).
        void LoadStatWeights();
        // The bot's dungeon set is on for a role (1 tank, 2 healer) or off (0): a Blood death knight and a Feral druid
        // are rated by their tree's tank profile while it is on. World thread (RoleService); read by the hook on any thread.
        void SetRoleMode(ObjectGuid bot, uint8 role);
        void EquipUpgrades(Player* bot);
        // mod-playerbots' own "drink" and "food" actions of this bot keep to the leader's thresholds (Rest.BelowMana,
        // Rest.BelowHealth): its grind strategy drinks below 85 % mana whatever the settings say. Puts the module's
        // versions of the two actions into the bot's AI context; cheap when they are there already (the context is
        // made anew now and then). And a paladin's "consecration" only against Fight.ConsecrationMinFoes foes (the owner:
        // no Consecration against a single target). World thread only.
        void GuardConsumables(Player* bot);
        // Dungeon roles (DungeonRoles.h): the worth of that kind of item for a role the bot does not play right now
        // (1 tank, 2 healer), from playerbots' reading of the item's stats and its stat weights for the tank or
        // healer tree of the class. ItemScore rates by the talents the bot has at the moment, so it cannot say what
        // a levelling warrior's tank set should hold. 0 = no use for the role.
        float RoleScore(Player const* bot, uint8 role, uint32 itemEntry, int32 randomProperty);
        // The worth of a permanent enchantment on `item` for the bot (0 for none): role 1 tank, 2 healer by the weights of
        // RoleScore, any other role by mod-playerbots' weights of the bot's talents (it needs an AI then). A weapon's
        // speed counts for weapon damage, a proc for those who strike (playerbots' reading of an enchantment).
        float EnchantScore(Player* bot, int8 role, uint32 enchantId, Item const* item);
        // The worth of a lasting buff (the use spell of an elixir, a flask or a scroll) for the bot: the stats the
        // buff gives, by the stat weights its gear is rated with. role 1 tank, 2 healer: the weights of RoleScore;
        // any other role: mod-playerbots' weights of the bot's talents (it needs an AI then; itemEntry is the item
        // that carries the buff). ItemScore and RoleScore say 0 for every elixir: mod-playerbots' reading of an
        // item's spells leaves out spells of another spell family than the class's, and elixirs are of the potion
        // family. 0 = no use.
        float BuffScore(Player* bot, int8 role, uint32 itemEntry, SpellInfo const* buff);
        // The bot votes on the loot rolls of its group it has not voted on yet (playerbots' "loot roll": need or
        // greed for what is an upgrade for it by the server's playerbots settings, pass on the rest). The module
        // calls it because playerbots only votes from its own tick, which a resting bot does not have (its AI is
        // paused), and a roll then waits a minute for it. True when it voted.
        bool VoteOnRolls(Player* bot);
        // Test aid (.ga admin boost): bags, uncommon gear for the bot's level and ammunition from playerbots' factory, as a
        // character has them that levelled there. Never used for a guild bot in play.
        // Gear of the bot's level (test aid): uncommon by default, rare for a test raid at the level of what dungeons give.
        bool EquipForLevel(Player* bot, uint32 quality = 2);
        // Debugging (.ga bot plan): the last action of the bot's current engine and its non-combat strategies.
        std::string DescribeAI(Player* bot);
        // Why playerbots' AI may not act now (held, casting, BotActiveAlone's share, its engine), for the logs.
        std::string AIState(Player* bot);
        // playerbots stores a bot's strategies when it logs out and loads them at every login
        // (PlayerbotRepository), after it has chosen them from the talents. So a bot keeps the combat strategies it
        // was first stored with (a priest that was holy then heals with shadow talents now; a druid stored before
        // its first talent point plays balance whatever its talents). This takes the strategies from the talents
        // again, stores them, and returns what changed ("holy heal, healer dps -> shadow, shadow aoe"; "" = nothing).
        // The planner's own non-combat profile comes back with its next tick (HasProfile / ApplyProfile).
        std::string SyncStrategies(Player* bot);
        // What the bot's talents make of it for playerbots: 2 a healer, 1 a tank (and nothing else), 0 anything else.
        uint8 SoloRole(Player* bot);
        // The last action of the engine that runs now (combat or non-combat), for diagnostics.
        std::string LastAction(Player* bot);
    }
}

#endif
