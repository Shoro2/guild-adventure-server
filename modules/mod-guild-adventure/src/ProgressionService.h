/*
 * ProgressionService: guild renown, renown levels and unlocks (the design log section 2.2, decision D8).
 *
 * Renown comes from what the guild's bots achieve (ga_renown_source). Reaching a renown level makes its unlocks
 * available; unlocks without a gold cost are owned at once, the others are bought (UNLOCK_BUY, BuyUnlock) with the
 * gold of the guild bank or the leader (bot setting Unlocks.PayFrom). Owned unlocks set the slot count, the offline
 * window and the feature keys the addon sees in STATE^feat.
 *
 * Slot and offline unlocks name the total they give (10 bot slots, 4 h), so a paid tier the guild already has more than
 * (10 slots for 50g when renown gave it 30) is "covered": nothing to buy, the addon shows it as included (the design log
 * section 7 UL1). World thread only.
 */

#ifndef MOD_GUILD_ADVENTURE_PROGRESSION_SERVICE_H
#define MOD_GUILD_ADVENTURE_PROGRESSION_SERVICE_H

#include "Define.h"
#include "ObjectGuid.h"
#include <string>
#include <vector>

namespace GuildAdventure
{
    struct AdventureGuild;
    struct UnlockTemplate;

    enum class UnlockState : uint8
    {
        Owned,
        Available, // level reached, can be bought
        Locked,
        Covered    // a slots / offline unlock below what the guild has already: nothing to buy
    };

    // Who pays for an unlock that costs gold (bot setting Unlocks.PayFrom).
    enum class UnlockPayFrom : uint8
    {
        Bank = 0,
        BankThenLeader = 1,
        Leader = 2
    };

    char const* ToToken(UnlockPayFrom from);

    struct UnlockBuyResult
    {
        bool ok = false;
        std::string code;    // error code for the addon (docs/protocol.md section 8.5) when !ok
        std::string message;
    };

    char const* ToToken(UnlockState state);
}

class ProgressionService
{
public:
    static ProgressionService& Instance();

    // Adds renown for an event of 'eventType' caused by 'bot'; returns the points added (0 if the event gives none).
    int32 AwardForEvent(GuildAdventure::AdventureGuild& guild, std::string const& eventType, ObjectGuid bot);
    // Adds (or with a negative value removes) renown, raises the renown level and owns new free unlocks.
    void AddRenown(GuildAdventure::AdventureGuild& guild, int32 points);

    // Owns every free unlock up to the current level and recomputes slots and offline window. Returns true if
    // anything changed. Called on startup for every guild and after renown changes.
    bool ApplyUnlocks(GuildAdventure::AdventureGuild& guild);

    // Buys an unlock: checks its state and the guild's gold, takes the gold (guild bank and / or the leader's purse),
    // owns it, raises the slots or the offline window, records the event and pushes STATE and UNLOCKS. `who` names
    // the buyer in the activity log ("the owner", "an administrator's command"). Codes: BAD_REQUEST, LOCKED, COVERED,
    // NOT_ENOUGH_GOLD.
    GuildAdventure::UnlockBuyResult BuyUnlock(GuildAdventure::AdventureGuild& guild, GuildAdventure::UnlockTemplate const& unlock,
        std::string const& who);
    GuildAdventure::UnlockPayFrom GetPayFrom(GuildAdventure::AdventureGuild const& guild) const;
    // The copper the guild can pay for an unlock right now by its Unlocks.PayFrom (STATE^ulgold).
    uint64 GetUnlockFunds(GuildAdventure::AdventureGuild const& guild) const;

    GuildAdventure::UnlockState GetUnlockState(GuildAdventure::AdventureGuild const& guild, GuildAdventure::UnlockTemplate const& unlock) const;
    bool HasFeature(GuildAdventure::AdventureGuild const& guild, std::string const& key) const;
    std::vector<std::string> GetFeatures(GuildAdventure::AdventureGuild const& guild) const;
    // Renown points inside the current level.
    uint32 GetPointsInLevel(GuildAdventure::AdventureGuild const& guild) const;
    // The largest offline window any unlock can give (STATE^owmax).
    uint16 GetMaxOfflineWindow() const;
    // Death Knight recruits: some guild member reached level 55 (the design log section 2.2).
    bool IsDeathKnightUnlocked(GuildAdventure::AdventureGuild const& guild) const;

    // GM overrides (.ga slots set / .ga offline set); they persist until the next unlock changes the value.
    void SetSlots(GuildAdventure::AdventureGuild& guild, uint8 slots);
    void SetOfflineWindow(GuildAdventure::AdventureGuild& guild, uint16 minutes);

private:
    ProgressionService() = default;

    void RecalculateLevel(GuildAdventure::AdventureGuild& guild);
};

#define sProgressionService ProgressionService::Instance()

#endif
