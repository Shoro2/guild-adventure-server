/*
 * Professions: the profession plan of a guild bot acts (docs/orders.md "Professions", the design log §7 "Classic content at
 * 60: plan", package K1).
 *
 * The guild master sets the professions per bot (the addon's Professions tab, `ga_bot_profession_plan`). This file
 * makes the bot follow that plan while it levels:
 *   - Learning: what the trainers of a planned profession would teach the bot now (the profession itself, its ranks
 *     as level and skill allow, the recipes) is learned where it stands, free, like class spells
 *     (GuildAdventure.Professions.LearnInPlace). The list comes from the trainer of that profession that teaches the
 *     most; a profession that is not in the plan is never learned.
 *   - Gathering: a node the bot's skill opens, close to its way, is worth a detour (FindNode; the planner walks there,
 *     auto loot gathers it, the guild storage gets it).
 *   - Crafting: at a hub, after a hand-in, the bot crafts what still gives skill in a planned profession from its bags
 *     and the guild storage (CraftForSkill; the casts are CraftService's, the reagents come through core patch CP-7).
 *
 * World thread only.
 */

#ifndef MOD_GUILD_ADVENTURE_PROFESSIONS_H
#define MOD_GUILD_ADVENTURE_PROFESSIONS_H

#include "Define.h"
#include "WorldIndex.h"
#include "ObjectGuid.h"
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Creature;
class GameObject;
class Player;
struct ItemTemplate;
struct VendorItemData;

namespace Trainer
{
    class Trainer;
}

namespace GuildAdventure
{
    struct AdventureGuild;
    struct GuildBot;
}

namespace GuildAdventure
{
    // A place where a mining or herb node spawns (often one of a pool: not every place has its node at a time).
    struct NodeSpawn
    {
        SpawnPoint spawn;
        uint16 skill = 0;      // SKILL_MINING or SKILL_HERBALISM
        uint16 required = 0;   // skill value the node asks for
    };

    // A place where an object stands that recipes ask for (an anvil, a forge, a cooking fire).
    struct FocusSpawn
    {
        SpawnPoint spawn;
        uint32 focus = 0;      // SpellFocusObject.dbc
        float reach = 0.0f;    // how close the crafter has to stand
    };

    // What the best recipe that cannot be made where the bot stands lacks (Professions::CraftForSkill).
    struct CraftNeed
    {
        uint32 spell = 0;      // the recipe; 0: no recipe is only a walk or a purchase away
        uint32 focus = 0;      // the object it has to be made at, when none is near
        uint32 buyItem = 0;    // a material vendors sell and neither the bags nor the guild storage hold ...
        uint32 buyCount = 0;   // ... and how many of it
        uint32 makeItem = 0;   // a material no vendor sells that the bot can make out of the guild storage: an
        uint32 makeCount = 0;  // enchanter's dust and essences (disenchanting), a scribe's pigments (milling)
        uint32 makeItem2 = 0;  // a second such material the recipe lacks (a rod takes dust and an essence)
        uint32 farmItem = 0;   // a material creatures drop (cloth, meat) that neither the bags nor the storage hold ...
        uint32 farmCount = 0;  // ... and how many of it a batch lacks (the profession mode hunts for it)
        uint32 matSpell = 0;   // a material it makes itself with a recipe it knows, out of its bags and the guild storage
        uint32 matItem = 0;    // (bars out of ore, a bolt out of cloth; one step deeper too): the recipe to cast here and
        uint32 matCount = 0;   // now, the material, and how many casts a batch wants
        uint32 matFocus = 0;   // the anvil or forge such a material is made at, when none is near (it is `focus` then)
        // A green recipe the bot could make now, passed over for the orange or yellow `spell` whose lack it can see to
        // itself (the owner, 2026-11-14: no materials for green recipes while orange or yellow ones can be made): the
        // caller makes it (a look without `need`) only when it cannot see to that lack here.
        uint32 greenSpell = 0;
    };

    // What a look at gear for the guild came to (Professions::CraftGearFor).
    struct GearStep
    {
        enum class Kind : uint8
        {
            None,      // nothing to make
            Piece,     // the piece itself is being made
            Material,  // a material of it is being made first (bars out of ore, a bolt out of cloth, a cured hide)
            Buy,       // a material a vendor sells is missing (thread, salt, dye, flux)
            Focus      // the piece, or a material of it, is made at an anvil or forge that is not here
        };
        Kind kind = Kind::None;
        uint32 spell = 0;  // Piece, Material: the recipe whose casts were started
        uint32 item = 0;   // Material: what is being made; Buy: what the vendor sells
        uint32 count = 0;  // Material: casts started; Buy: how many are missing
        uint32 focus = 0;  // Focus: the object it is made at
        uint32 gain = 0;   // item levels the piece is above what its wearer has in that place
    };
}

class Professions
{
public:
    static Professions& Instance();

    // Reads which trainer teaches which profession (startup, .ga reload).
    void Load();

    // Teaches the bot what the trainers of its planned professions have for it now. Returns the number of spells.
    uint32 LearnPlanned(GuildAdventure::GuildBot const& bot, Player* player) const;

    // Starts crafting, up to `batch` times, the recipe of a planned profession that gives the most skill for the
    // least material. False when no recipe that still gives skill can be made. `text` says what was started. With
    // `need`, a false answer also says what the best recipe lacks that the bot can get by itself: the anvil, forge or
    // fire it is made at, or a material vendors sell.
    // Gear for the guild (package K5; second version 2026-11-14): of the recipes of the bot's planned professions that
    // make a piece of equipment one of `wearers` would put on (mod-playerbots' rating; the bot itself is one of them; a
    // piece that binds when made only for itself), the one whose wearer gains most: the item levels above what it has
    // in that place. Not while such a piece lies in the storage or in the wearer's bags already, not without the
    // recipe's tool, not a piece in `failed`. `item` and `wearer` say what it is and for whom.
    // The piece is made when its materials lie in the bags and the guild storage and its anvil or forge is here. Else
    // what it lacks comes first, as far as the bot can get it by itself: a material it has a recipe for is made (bars
    // out of the storage's ore, a bolt out of its cloth, a cured hide; two steps deep), a material a vendor sells is
    // named in `step` for the caller to buy, and so is the place it is made at (`mayWalk`: else a piece whose place
    // is not here is no choice). True when casts were started: `step` says of what.
    // A piece that improves the dungeon set of one of them (DungeonRoles.h: a tank's or healer's set, rated for the
    // role) counts too, after every piece somebody would level in; `forRole` says so.
    bool CraftGearFor(GuildAdventure::GuildBot const& bot, Player* player, std::vector<Player*> const& wearers, std::string& text,
        uint32& item, Player*& wearer, bool& forRole, GuildAdventure::GearStep* step = nullptr, bool mayWalk = false,
        std::unordered_set<uint32> const* failed = nullptr) const;
    // `notSkill`: a profession to pass over while another one has a recipe to make (a bot's two professions take
    // turns); `skillOut` gets the profession of what was started; `onlySkill`: only the recipes of that profession
    // (and `need` is what the best blocked recipe of that profession lacks).
    bool CraftForSkill(GuildAdventure::GuildBot const& bot, Player* player, uint32 batch, std::string& text,
        GuildAdventure::CraftNeed* need = nullptr, uint32 notSkill = 0, uint32* skillOut = nullptr, uint32 onlySkill = 0) const;
    // The planned skills of a bot: its primaries, then its secondaries.
    [[nodiscard]] static std::vector<uint32> PlannedSkills(GuildAdventure::GuildBot const& bot) { return Planned(bot); }

    // Food for the guild (the owner, 2026-11-13: [quote left out]): starts up to
    // `batch` casts of a cooking recipe the character knows (planned or not, grey or not) whose food one of the
    // guild's characters eats at its level (`levels`; the food's required level at or up to twelve below it) and of
    // which the guild storage holds fewer than `stock`. Food with a buff first, then the kind more characters eat,
    // then the higher one. Only what can be made here and now: the materials in its bags and the guild storage, a
    // fire where it stands. False when there is nothing to make; `focus` then gets the fire (SpellFocusObject.dbc)
    // a recipe that could be made otherwise lacks, 0 when none.
    bool CookForGuild(Player* player, std::vector<uint8> const& levels, uint32 stock, uint32 batch, std::string& text,
        uint32* focus = nullptr) const;

    // Ammunition for the guild (the owner, 2026-11-22: [quote left out]): for each of `shooters` (the guild's hunters with a gun) the best ammunition of the recipes the
    // character knows that the shooter can use; of such a kind the guild storage holds fewer than `stock`, it starts up
    // to `batch` casts, the kind more shooters use first, then the better one. What a recipe lacks that the character
    // makes itself out of its bags and the storage (blasting powder, bars) is made first; nothing is bought. False when
    // there is nothing to make; `focus` then gets the forge or anvil (SpellFocusObject.dbc) a material lacks, 0 when none.
    // Without `start` only the look: true when there is something to make here or at a forge.
    bool AmmoForGuild(Player* player, std::vector<Player*> const& shooters, uint32 stock, uint32 batch, std::string& text,
        uint32* focus = nullptr, bool start = true) const;

    // Endgame consumables for the guild's dungeon runs (the owner, 2026-11-25: [quote left out]): of each kind its recipes make that a run uses (an
    // elixir or flask, a scroll, a weapon's stone, weightstone or oil, a scope: DungeonSupplies::Classify; a healing or
    // mana potion), the kind being the effect, the best it can make for `topLevel` (the guild's highest), nothing more
    // than 15 item levels below it, while the guild storage holds fewer than `stock`. `step` says what comes next (the
    // piece or a material started when `start`, a vendor's material to buy, a place to walk to); `skip`: items not to
    // look at. True when there is such a step.
    bool ConsumablesForGuild(Player* player, uint8 topLevel, uint32 stock, uint32 batch, std::string& text, GuildAdventure::GearStep& step,
        bool start, std::unordered_set<uint32> const* skip = nullptr) const;
    // Raw materials in large amounts processed for the guild (the owner, 2026-11-25: ore into bars when the storage holds
    // more than 1000; processed materials on stock when more than 1000 of their source lie there). Of the recipes the
    // character knows whose product is a trade good the storage takes: those whose every material that no vendor sells
    // lies in the guild storage above `above`; the casts take only what is above it, and a product other than bars out
    // of ore up to `stock` (0 = no cap). The one with the most to spare first. A vendor's material, a place: the step,
    // as for gear. `text`: what it makes. True when there is a step.
    bool ProcessForGuild(Player* player, uint32 above, uint32 stock, uint32 batch, std::string& text, GuildAdventure::GearStep& step,
        bool start) const;
    // Skeleton keys for the lockboxes of the guild storage (the owner, 2026-11-26: [quote left out]): of the key recipes a blacksmith knows, each lockbox kind of the storage counted for the
    // weakest key that opens it; keys made up to that count, at most 10 of a kind in the storage. The same steps as
    // ProcessForGuild (a material made first, a vendor's bought, an anvil walked to). The key goes into the guild
    // storage, and a blacksmith takes it out to open a lockbox (LootService::OpenStoredContainers).
    bool KeysForGuild(Player* player, uint32 batch, std::string& text, GuildAdventure::GearStep& step, bool start) const;

    // Enchanting has few recipes that make an item: its skill comes from enchantments. The bot puts the enchantment
    // that still gives skill for the least material on a piece of its own gear, up to `batch` times (a piece without
    // an enchantment, or with one of its own recipes on it; materials from its bags and the guild storage; the rod
    // the recipe asks for in its bags). False when none can be cast; with `need`, what the best one lacks.
    bool EnchantForSkill(GuildAdventure::GuildBot const& bot, Player* player, uint32 batch, std::string& text,
        GuildAdventure::CraftNeed* need = nullptr);
    // Enchantments for the guild's gear (the owner, 2026-11-25: [quote left out]). The core puts an enchantment on another player's item
    // only through a trade window; the game's other way is a scroll: the enchanter writes its recipe on a vellum, and
    // whoever holds the scroll puts it on its own piece.
    //
    // The enchanter's part: of the pieces the characters in `wearers` wear (blue quality or better, item level 60 or
    // more), the one whose enchantment its recipes would improve most, by the stat weights of the wearer's dungeon role,
    // else of its talents: the best enchantment for that piece it can make now (its materials in its bags or the guild
    // storage, the rod in its bags), better than the one on the piece and than a scroll for it in the guild storage.
    // With `start` it takes a vellum out of the guild storage and begins the cast (StepEnchanting); the scroll goes into
    // the guild storage. `lackVellum`: the vellum the best such enchantment lacks, when neither its bags nor the storage
    // hold one (0 = none lacks). True when there is one to make.
    bool EnchantScrollForGuild(Player* player, std::vector<Player*> const& wearers, std::string& text, uint32& lackVellum, bool start);
    // The wearer's part: a scroll of the guild storage on a piece it wears (blue or better, item level 60 or more) when
    // its enchantment is better for its role than the one on the piece, the best such scroll first; the cast is queued
    // (StepEnchanting). True when one was taken.
    bool UseEnchantScroll(Player* player, std::string& text);
    // The queue of those casts: true while the character is at it (the planner leaves it alone until then).
    bool StepEnchanting(Player* player);
    // An enchantment of the character is under way (its scroll or vellum lies in its bags until the cast). World thread.
    bool IsEnchanting(ObjectGuid character) const { return _enchanting.count(character) != 0; }
    [[nodiscard]] std::string DescribeEnchanting(ObjectGuid character) const;

    // Milling for a scribe: herbs of the guild storage (five at a time, as the spell takes them, those its skill
    // allows) become the pigment `pigment` in the guild storage, until `count` of it are there or `maxStacks` stacks
    // are milled. Returns the stacks milled; each leaves a line in the activity log.
    uint32 MillStored(uint32 owner, Player* player, uint32 pigment, uint32 count, uint32 maxStacks) const;
    // Recipes that lie in the guild storage are taken out and learned by the guild's AI-played characters in the
    // world that can learn them now (the profession, its skill, the level, whatever else the recipe asks; not known
    // yet). Several could: the one with the highest skill in that profession, on a tie the one that knows more of its
    // recipes (the owner, 2026-11-13). One copy per character; a recipe nobody can learn yet stays. Each leaves a line in
    // the activity log (the withdrawal and what was learned). Returns the recipes learned.
    uint32 LearnStoredRecipes(GuildAdventure::AdventureGuild const& guild) const;
    // Recipes at vendors (the owner, 2026-11-13: [quote left out]). What a vendor's list holds that the
    // character can learn now and pay: a recipe of a profession it has, its skill and level reached, not known yet,
    // at or below Professions.RecipeMaxPrice and what the guild's budget allows it. `vendor`: the creature itself
    // (its prices and its stock of recipes in limited supply), nullptr for the list alone.
    struct RecipeOffer
    {
        uint32 slot = 0;
        ItemTemplate const* proto = nullptr;
        uint32 price = 0;
        uint32 teaches = 0;
    };
    static std::vector<RecipeOffer> RecipeOffers(Player* player, VendorItemData const* items, Creature* vendor);
    // Whether a vendor (by entry) sells such a recipe.
    static bool SellsRecipe(Player* player, uint32 vendorEntry);
    // At the vendor: buys those recipes (five a visit) and learns them at once. Returns what was bought, with price
    // and payer; empty when nothing.
    static std::string BuyRecipesAt(Player* player, Creature* vendor);
    // Recipes in the character's bags that it can learn now are learned (mod-playerbots buys recipes at vendors
    // itself, with the bot's own money, and leaves them in the bags). Returns their names; empty when none.
    static std::string LearnCarriedRecipes(Player* player);

    // Whether the item is a tool this module hands out with a profession (a mining pick, a jeweler's kit, ...). Some
    // of them are no tools by their item data (TotemCategory 0: the Jeweler's Kit), so whoever decides what a bot
    // keeps has to ask here as well.
    [[nodiscard]] static bool IsTool(uint32 itemId);
    // Whether milling gives the item at all.
    [[nodiscard]] bool IsPigment(uint32 itemId) const { return _pigmentHerbs.count(itemId) != 0; }

    // Whether an object recipes ask for (SpellFocusObject.dbc) is close enough to craft at, as the core checks it.
    static bool FocusNear(Player* player, uint32 focus);
    // The places of a map where such objects stand.
    [[nodiscard]] std::vector<GuildAdventure::FocusSpawn> const& GetFocusSpawns(uint32 mapId) const;

    // Skinning: whether the character's skill is enough for a creature of this level (the spell's own rule), and
    // whether a creature of this level still teaches it something.
    static bool CanSkin(Player const* player, uint8 level);
    static bool SkinTeaches(Player const* player, uint8 level);
    // A beast the skinning skill is for (skin loot, not one that is "skinned" with mining, herbalism or engineering).
    static bool IsSkinKind(uint32 creatureEntry);
    // A corpse the character killed and can skin: skinnable now, or still holding its loot (auto loot takes that
    // first, then it is skinnable).
    static bool IsSkinnableCorpse(Player const* player, Creature const* creature);
    // The nearest such corpse within `range`.
    static Creature* FindSkinnable(Player* player, float range);

    // The nearest mining or herb node within `range` that is ready and that the character's skill opens.
    static GameObject* FindNode(Player* player, float range);

    // Whether a game object is a mining or herb node (not a quest object that opens with the same lock: a Plant
    // Bundle, a Tear of Tilloa).
    [[nodiscard]] bool IsNode(uint32 gameObjectEntry) const { return _nodeEntries.count(gameObjectEntry) != 0; }
    // The places of a map where gathering nodes spawn.
    [[nodiscard]] std::vector<GuildAdventure::NodeSpawn> const& GetNodeSpawns(uint32 mapId) const;
    // The lowest skill anything of a gathering skill asks for in a zone: its mining or herb nodes (Mining,
    // Herbalism), its skinnable creatures by their level (Skinning); 0: the zone has nothing of it. Looked up once per
    // zone and skill (world thread).
    [[nodiscard]] uint32 ZoneGatherFloor(uint32 mapId, uint32 zoneId, uint32 skill) const;
    // What most of a zone's nodes (or skinnable creatures) ask for: the middle one of them all by the skill they need.
    // The lowest says little: a Silver Vein (75) stands somewhere in almost every zone up to level 50.
    [[nodiscard]] uint32 ZoneGatherUsual(uint32 mapId, uint32 zoneId, uint32 skill) const;
    // Whether the bot's plan has work left in a profession: the skill is planned and below its target (300 when the
    // plan names none).
    static bool HasWork(GuildAdventure::GuildBot const& bot, Player const* player, uint32 skill);
    // Whether a character could put a piece of that kind on now, by every requirement of the item: the core's check
    // (side, class and race, a required profession and its rank, a spell, the level), the weapon or armour
    // proficiency, the reputation.
    static bool CanWear(Player const* wearer, ItemTemplate const* proto);
    // The Skinning skill a creature of a level asks for (Spell::CheckCast, SPELL_EFFECT_SKINNING).
    static uint32 SkinSkillFor(uint8 level) { return level <= 10 ? 1u : level <= 20 ? (uint32(level) - 10) * 10 : uint32(level) * 5; }

    // Fishing: the zones and areas an item is fished in (fishing_loot_template, with its references), and the schools
    // of fish of a map (they mark water).
    [[nodiscard]] std::vector<uint32> const& GetFishZones(uint32 itemId) const;
    [[nodiscard]] std::vector<GuildAdventure::SpawnPoint> const& GetFishingPools(uint32 mapId) const;

    // The profession recipes (spells) that create an item; empty for most items.
    [[nodiscard]] std::vector<uint32> const& RecipesFor(uint32 itemId) const;
    // The ammunition vendors sell for gold (any vendor of the world; read at startup).
    [[nodiscard]] std::vector<uint32> const& VendorAmmo() const { return _vendorAmmo; }

    // The tools the orange and yellow recipes (and enchantments) of `skill` lack (a spell's tool category) and how the bot
    // gets one it cannot make by itself: `inStorage` such a tool lies in the guild storage (it takes it out); else
    // `part` is what another profession makes, `count` of it: a material of the tool's recipe the bot knows (a smith's
    // Silver Rod for the enchanter's Runed Silver Rod), or the tool itself when the bot knows no recipe for it. False:
    // nothing lacks a tool, or the bot sees to it by itself (CraftForSkill makes the tool first).
    struct ToolNeed
    {
        uint32 category = 0, tool = 0, part = 0, count = 0;
        bool inStorage = false;
    };
    bool LackedTool(GuildAdventure::GuildBot const& bot, Player* player, uint32 skill, std::vector<ToolNeed>& needs) const;

    // For .ga reload and the startup line.
    [[nodiscard]] std::size_t GetTeacherCount() const { return _teachers.size(); }

private:
    Professions() = default;

    // The planned skills of a bot: its primaries, then its secondaries.
    static std::vector<uint32> Planned(GuildAdventure::GuildBot const& bot);

    std::unordered_map<uint32, Trainer::Trainer const*> _teachers; // skill -> the trainer with the most to teach
    std::unordered_map<uint32, std::vector<uint32>> _recipes;       // item -> profession spells that create it
    std::unordered_map<uint32, std::vector<uint32>> _toolsMade;     // tool category -> items of it profession spells create
    std::unordered_map<uint32, std::vector<GuildAdventure::NodeSpawn>> _nodes; // map -> gathering node places
    std::unordered_map<uint32, std::vector<GuildAdventure::FocusSpawn>> _focus; // map -> anvils, forges, fires ...
    std::unordered_set<uint32> _nodeEntries;                                    // game objects that are nodes
    mutable std::unordered_map<uint64, uint32> _zoneFloors; // (zone << 16 | skill) -> ZoneGatherFloor
    mutable std::unordered_map<uint64, uint32> _zoneUsual;  // (zone << 16 | skill) -> ZoneGatherUsual
    std::unordered_map<uint32, std::vector<uint32>> _fishZones;                 // item -> zones and areas it is fished in
    std::unordered_map<uint32, std::vector<GuildAdventure::SpawnPoint>> _pools; // map -> schools of fish
    std::unordered_map<uint32, std::vector<uint32>> _pigmentHerbs;              // pigment -> the herbs milling gives it from
    std::vector<uint32> _vendorAmmo;                                            // ammunition vendors sell for gold

    struct EnchantJob
    {
        uint32 spell = 0;
        ObjectGuid item;
        uint32 left = 0;
        uint32 done = 0;
        uint32 nextAt = 0;     // getMSTime()
        uint32 skillBefore = 0;
        // A scroll for the guild (EnchantScrollForGuild): `item` is the vellum in its bags.
        bool vellum = false;
        uint32 vellumEntry = 0;
        // A scroll put on its own piece (UseEnchantScroll): `item` is the piece, `castItem` the scroll.
        bool apply = false;
        ObjectGuid castItem;
        uint32 scroll = 0;     // the scroll's item
        uint32 enchant = 0;    // what goes on, and what was on the piece before
        uint32 before = 0;
        ObjectGuid wearer;     // whose piece the scroll is for (the claim), and its place on the body
        uint8 slot = 0;
        std::string text;
        uint8 movingTries = 0; // casts refused because it was moving (the AI moved it again): tried again
    };
    std::unordered_map<ObjectGuid, EnchantJob> _enchanting;                     // by character (world thread)
    // A piece a scroll is being written for: (wearer, place on the body) -> (the enchanter, until when; getMSTime).
    std::map<std::pair<ObjectGuid, uint8>, std::pair<ObjectGuid, uint32>> _scrollClaims;
};

#define sProfessions Professions::Instance()

#endif
