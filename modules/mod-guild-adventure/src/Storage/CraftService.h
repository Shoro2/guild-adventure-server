/*
 * CraftService: crafting from the guild storage on request (docs/storage.md "Crafting").
 *
 * The reagents themselves come from the storage through core patch CP-7 for every spell a guild character casts, the
 * normal Create button included, as long as the client lets the cast through. The 3.3.5a client checks its bags for
 * the reagents before it sends a tradeskill cast, so a recipe whose materials are only in the storage needs the
 * server to start the cast: the addon's "Create (storage)" button in the profession window asks for it (GADV CRAFT),
 * and for a bot the owner asks from the bot's professions page (or .ga admin craft). The service then casts the
 * recipe the asked number of times, one cast after the other, like the client's "Create All": the casts are normal
 * casts (cast time, tools, spell focus, skill-ups), and the queue stops when a cast fails or is interrupted
 * (moving, a fight).
 *
 * A bot that crafts stands still: the order planner leaves it alone while its queue runs (IsCrafting) and resumes when
 * the queue ends.
 *
 * World thread only, except OnSpellCast (the cast hook, map threads), which only queues.
 */

#ifndef MOD_GUILD_ADVENTURE_CRAFT_SERVICE_H
#define MOD_GUILD_ADVENTURE_CRAFT_SERVICE_H

#include "Define.h"
#include "ObjectGuid.h"
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class Player;
class SpellInfo;

class CraftService
{
public:
    static CraftService& Instance();

    // Starts crafting 'count' times (1-100). Error codes: UNKNOWN_SPELL, NOT_A_RECIPE, NOT_KNOWN, DEAD, BUSY, REAGENTS (none
    // left in the bags and the storage), CANNOT_CAST.
    bool Start(Player* player, uint32 spellId, uint32 count, std::string& error, std::string& text);
    void Stop(ObjectGuid character, char const* why);
    [[nodiscard]] bool IsCrafting(ObjectGuid character) const;
    // "Crafting <item>: 3 of 10" for the roster's why line, empty when not crafting.
    [[nodiscard]] std::string Describe(ObjectGuid character) const;

    // How many times the character could craft the recipe with the reagents in its bags and the guild storage.
    static uint32 CraftableCount(Player* player, SpellInfo const* spell);
    // A recipe the service crafts: creates an item and needs no item target (enchanting onto an item is not one).
    static bool IsCraftRecipe(SpellInfo const* spell);

    // Hook (any thread): a spell was cast.
    void OnSpellCast(Player* player, uint32 spellId);
    void Update(uint32 diff);

private:
    CraftService() = default;

    struct Job
    {
        uint32 spellId = 0;
        uint32 total = 0;
        uint32 done = 0;
        bool casting = false;     // a cast of ours was started and has not ended yet
        uint32 owner = 0;
        uint32 stillSince = 0;    // getMSTime() when the job first waited for the character to stand still (0 = not)
    };

    void Finish(ObjectGuid character, Job const& job, std::string const& why);

    std::unordered_map<ObjectGuid, Job> _jobs;

    mutable std::mutex _castLock;
    std::vector<std::pair<ObjectGuid, uint32>> _casts; // finished casts, from the hook
};

#define sCraftService CraftService::Instance()

#endif
