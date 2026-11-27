/*
 * AuctionService: the guild storage's surplus goes to the auction house (the owner, 2026-11-12: [quote left out]; docs/storage.md "Auction house").
 *
 * Per guild and item kind (entry + random property) the owner sets an auction rule in the Storage tab: keep X in the
 * storage, auction what is above it in stacks of S, at a price per item (0 = the server's default price). Every few
 * minutes the service posts full stacks of the surplus as normal auctions in the name of the guild leader (in the
 * leader's faction auction house, also while the leader is offline), so players and the AH bot (mod-ah-bot-plus, set to
 * buy only) can buy them. The proceeds go into the guild bank. An auction that expires unsold is sold at the vendor
 * price (since 2026-11-26; what its rule or reserve keeps, and what may not be sold, back into the guild storage), not
 * mailed to the leader; one the leader cancels by hand is mailed to him by the core.
 *
 * The default price follows the AH bot's own valuation, so its buyer accepts it: the item's vendor price, at least a
 * minimum per item class, times a quality multiplier, times GuildAdventure.Auction.PriceFactor (below 1: a little under
 * what the buyer pays at most).
 *
 * Which auctions are the service's: ga_auction_listing (auction id -> guild, kind, count); the leader's own auctions are
 * never touched. World thread only (the auction house updates and the auction hooks run there too).
 */

#ifndef MOD_GUILD_ADVENTURE_AUCTION_SERVICE_H
#define MOD_GUILD_ADVENTURE_AUCTION_SERVICE_H

#include "DatabaseEnvFwd.h"
#include "Define.h"
#include "ObjectGuid.h"
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class AuctionHouseObject;
class Item;
struct AuctionEntry;
struct ItemTemplate;

namespace GuildAdventure
{
    struct AdventureGuild;

    struct AuctionRule
    {
        uint32 entry = 0;
        int32 randomProperty = 0;
        uint32 keep = 0;           // stays in the storage
        uint32 stack = 0;          // items per auction (clamped to the item's stack size)
        uint32 price = 0;          // copper per item, 0 = the default price
        bool enabled = true;
    };

    // One rule for a whole storage category (StorageCategory: "cloth", "herb", "weapons", ...): every kind of it that
    // has no rule of its own is auctioned above `keep`, at the default price (the owner, 2026-11-13: [quote left out]).
    struct AuctionCategoryRule
    {
        std::string category;
        uint32 keep = 0;           // per kind of item
        uint32 stack = 0;          // items per auction (0 = up to 20, clamped to the item's stack size)
        bool enabled = true;
    };

    struct AuctionListing
    {
        uint32 auctionId = 0;
        uint32 owner = 0;
        uint32 entry = 0;
        int32 randomProperty = 0;
        uint32 count = 0;
        uint32 createdAt = 0;
    };
}

class AuctionService
{
public:
    static AuctionService& Instance();

    // Startup (world thread, after the auction house and the guilds are loaded): rules and listings; listings whose
    // auction no longer exists are dropped.
    void Load();
    // World thread: posts the surplus every GuildAdventure.Auction.IntervalSeconds, deletes returned items.
    void Update(uint32 diff);

    // --- rules (world thread; the protocol and the commands) ---
    std::vector<GuildAdventure::AuctionRule> GetRules(uint32 owner) const;
    // Error codes: NOT_AUCTIONABLE (quest item, conjured, timed, key, bind on pickup), INVALID (stack 0 or above 1000,
    // keep above 1000000), LIMIT (GuildAdventure.Auction.MaxRules).
    bool SetRule(uint32 owner, GuildAdventure::AuctionRule rule, std::string& error);
    bool DeleteRule(uint32 owner, uint32 entry, int32 randomProperty);
    // Category rules (error codes: CATEGORY for an unknown one, INVALID, LIMIT).
    std::vector<GuildAdventure::AuctionCategoryRule> GetCategoryRules(uint32 owner) const;
    bool SetCategoryRule(uint32 owner, GuildAdventure::AuctionCategoryRule rule, std::string& error);
    bool DeleteCategoryRule(uint32 owner, std::string const& category);
    // The rule that applies to a kind in the storage: its own rule, else its category's (nullopt: none).
    std::optional<GuildAdventure::AuctionRule> EffectiveRule(uint32 owner, uint32 entry, int32 randomProperty) const;
    // Items of a kind the guild has in the auction house right now.
    uint32 GetListedCount(uint32 owner, uint32 entry, int32 randomProperty) const;
    std::vector<GuildAdventure::AuctionListing> GetListings(uint32 owner) const;

    // The default price per item (copper) and why an item kind cannot be auctioned (nullptr = it can).
    static uint32 DefaultPrice(ItemTemplate const* proto);
    static char const* RefuseReason(ItemTemplate const* proto);

    // Posts the surplus of one guild now (also what Update does every interval); returns the auctions created.
    uint32 PostSurplus(uint32 owner);
    // Tests: the guild's listings expire at the next auction house update (they come back into the storage).
    uint32 ExpireListings(uint32 owner);

    // Auction hooks (world thread)
    void OnAuctionRemove(AuctionEntry* entry);
    // An auction expires unsold: one of the service's is sold at the vendor price into the guild bank (Auction.SellUnsold;
    // what the kind's rule or reserve keeps, tools, Darkmoon cards and reputation turn-ins back into the guild storage)
    // instead of the leader's mail.
    void OnBeforeExpiredMail(AuctionEntry* entry, bool& sendMail);
    // One of the service's auctions sold: the gold goes into the guild bank instead of the leader's mail (2026-11-13).
    void OnBeforeSuccessfulMail(AuctionEntry* entry, uint32 profit, bool& sendMail);

private:
    AuctionService() = default;

    using Key = std::pair<uint32, int32>;

    void SaveRule(uint32 owner, GuildAdventure::AuctionRule const& rule) const;
    // Posts the surplus of one kind by `rule`; returns the auctions created (summary gets "n x stack Name").
    uint32 PostKind(uint32 owner, GuildAdventure::AdventureGuild const& guild, GuildAdventure::AuctionRule const& rule,
        CharacterDatabaseTransaction trans, std::string& summary);

    std::unordered_map<uint32, std::map<Key, GuildAdventure::AuctionRule>> _rules;   // by owner account
    std::unordered_map<uint32, std::map<std::string, GuildAdventure::AuctionCategoryRule>> _categoryRules; // by owner
    std::unordered_map<uint32, GuildAdventure::AuctionListing> _listings;             // by auction id
    std::vector<Item*> _returned;   // items of expired auctions put back into the storage; deleted next tick
    uint32 _timer = 0;
};

#define sAuctionService AuctionService::Instance()

#endif
