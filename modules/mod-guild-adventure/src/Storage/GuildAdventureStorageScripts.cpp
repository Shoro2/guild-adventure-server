/*
 * Hooks and commands of the guild storage, the loot filter, auto loot and crafting from the storage
 * (StorageService, LootService, CraftService; docs/storage.md).
 *
 * Hooks used:
 *   PlayerScript  OnPlayerLootItem (queued for the loot filter), OnPlayerUpdate (auto loot, the character's own map
 *                 thread), OnPlayerSpellCast (crafting queue, queued), OnPlayerCheckReagent / OnPlayerTakeReagent
 *                 (core patch CP-7: reagents from the guild storage)
 *   WorldScript   OnStartup, OnUpdate, OnShutdown
 *   AuctionHouseScript  OnAuctionRemove, OnBeforeAuctionHouseMgrSendAuctionExpiredMail / SuccessfulMail (the storage's
 *                       surplus auctions: unsold back into the storage, the gold of a sale into the guild bank)
 *   PlayerScript        OnPlayerMoneyChanged (the guild tax, GuildBank), OnPlayerQuestComputeXP / OnPlayerCanSellItem
 *                       (they name the next gain in the tax line: a quest reward, a vendor sale)
 *   GuildScript         OnMemberWitdrawMoney (a withdrawal from the WoW guild bank is the guild's own gold: no tax)
 *
 * Commands (they join the ".ga" tree of the other command scripts):
 *   .ga storage [character]                      the guild storage: kinds, items, the biggest stacks (GM; console with
 *                                                a character)
 *   .ga storage deposit                          your character's materials into the guild storage (GM)
 *   .ga storage take <item id> [count]           items from the guild storage into your character's bags (GM)
 *   .ga loot [character]                         loot settings, totals and rules of the guild (GM; console with a
 *                                                character)
 *   .ga loot recent [character]                  the guild's last loot (GM; console with a character)
 *   .ga admin storage add <character> <item id> [count]   puts items into a guild's storage (administrator, console,
 *                                                SOAP; tests without a client)
 *   .ga admin loot <character> <item id> [count]  the character "loots" an item: it is created in its bags and goes
 *                                                through the loot filter like real loot (administrator, console, SOAP)
 *   .ga admin lootrule <character> <action> <conditions>   adds a loot rule to the character's guild; conditions in
 *                                                the GADV form "type:op:value:value2:text[:group];..." (administrator)
 *   .ga admin lootrules clear <character>        deletes every loot rule of the character's guild (administrator)
 *   .ga admin lootoptions <character> <filter 0|1> <default ai> <default player> <auto loot 0|1>  (administrator)
 *   .ga admin learn <character> <spell id>       the character learns a spell (administrator; recipes for tests)
 *   .ga admin craft <character> <spell id> [count]  the character crafts from its bags and the guild storage
 *                                                (administrator, console, SOAP)
 *   .ga admin sweep <character>                  the bags of a character the AI plays go through the loot filter now
 *                                                (administrator, console, SOAP; it also runs by itself)
 *   .ga auction [character]                      the guild's auction rules and its auctions in the auction house (GM;
 *                                                console with a character)
 *   .ga admin auction rule <character> <item id> <keep> [stack] [price]   sets an auction rule (administrator, console,
 *                                                SOAP; price in copper per item, 0 = default)
 *   .ga admin auction norule <character> <item id>   deletes it
 *   .ga admin auction post <character>           posts the guild's surplus now (it also runs every few minutes)
 *   .ga admin auction expire <character>         the guild's auctions expire at the next auction house update (tests)
 *   .ga admin auction catrule <character> <category> <keep> [stack]   an auction rule for a whole storage category
 *   .ga admin auction nocatrule <character> <category>               deletes it
 *   .ga guildbank [character]                    the guild bank's money, the guild tax and what is held back (GM, console)
 *   .ga admin guildbank add <character> <copper> puts copper into the guild bank (test aid; administrator, console)
 *   .ga shopping [character]                     the guild's shopping list, what the storage holds, today's spending (GM)
 *   .ga admin shopping set <character> <item id> <minimum>   puts an item on the guild's shopping list (administrator)
 *   .ga admin shopping del <character> <item id>             takes it off
 *   .ga admin shopping buy <character>           test aid: the character buys for the list at the vendor it stands at
 */

#include "AuctionHouseMgr.h"
#include "AuctionService.h"
#include "Bag.h"
#include "BotSettings.h"
#include "GuildBank.h"
#include "GuildBudget.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Creature.h"
#include "CraftOrders.h"
#include "CraftService.h"
#include "GadvProtocol.h"
#include "Group.h"
#include "GuildAdventureConfig.h"
#include "GuildAdventureMgr.h"
#include "GuildAdventureSettings.h"
#include "Item.h"
#include "Log.h"
#include "LootService.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "ShoppingList.h"
#include "StorageService.h"
#include "StringFormat.h"
#include "WorldIndex.h"
#include "WorldSession.h"
#include <algorithm>
#include <limits>

using namespace Acore::ChatCommands;
using namespace GuildAdventure;
namespace LR = GuildAdventure::LootRules;

namespace
{
    char const* ActionName(uint8 action)
    {
        switch (action)
        {
            case LR::ActionKeep:       return "keep";
            case LR::ActionSell:       return "sell";
            case LR::ActionDisenchant: return "disenchant";
            case LR::ActionDelete:     return "delete";
            case LR::ActionStore:      return "store";
            default:                   return "?";
        }
    }

    // An outcome of the storage's log: a loot action, or what only the log knows (docs/protocol.md section 8.8).
    char const* OutcomeName(uint8 outcome)
    {
        switch (outcome)
        {
            case LootService::LogTaken:     return "taken out";
            case LootService::LogDeposited: return "put in";
            case LootService::LogOpened:    return "opened";
            case LootService::LogBought:    return "bought";
            default:                        return ActionName(outcome);
        }
    }

    std::optional<uint8> ActionFromName(std::string name)
    {
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        for (uint8 action = 0; action < LR::ActionCount; ++action)
            if (name == ActionName(action) || name == std::to_string(action))
                return action;
        return std::nullopt;
    }

    std::string ItemName(uint32 entry)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        return proto ? proto->Name1 : Acore::StringFormat("item {}", entry);
    }

    // The owner whose guild 'target' (or, without one, the command's own character) belongs to; 0 with an error sent.
    uint32 ResolveOwner(ChatHandler* handler, Optional<PlayerIdentifier> const& target)
    {
        if (target)
        {
            uint32 owner = 0;
            if (Player* player = target->GetConnectedPlayer())
                owner = sStorageService.GetOwnerOf(player);
            if (!owner)
            {
                ObjectGuid const guid = target->GetGUID();
                if (AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfLeader(guid))
                    owner = guild->ownerAccount;
                else if (AdventureGuild* bots = sGuildAdventureMgr.FindGuildOfBot(guid))
                    owner = bots->ownerAccount;
            }
            if (!owner)
                handler->SendErrorMessage(Acore::StringFormat("{} is neither the leader nor a bot of an adventure guild.",
                    target->GetName()), false);
            return owner;
        }
        if (WorldSession* session = handler->GetSession())
        {
            if (sGuildAdventureMgr.FindGuild(session->GetAccountId()))
                return session->GetAccountId();
            handler->SendErrorMessage("Your account has no adventure guild yet.", false);
            return 0;
        }
        handler->SendErrorMessage("Name a guild character.", false);
        return 0;
    }

    Player* InWorldGuildCharacter(ChatHandler* handler, PlayerIdentifier const& target)
    {
        Player* player = target.GetConnectedPlayer();
        if (!player || !player->IsInWorld())
        {
            handler->SendErrorMessage(Acore::StringFormat("{} is not in the world.", target.GetName()), false);
            return nullptr;
        }
        if (!sStorageService.GetOwnerOf(player))
        {
            handler->SendErrorMessage(Acore::StringFormat("{} is no character of an adventure guild (or the guild storage is "
                "off).", target.GetName()), false);
            return nullptr;
        }
        return player;
    }
}

class GuildAdventureStoragePlayerScript : public PlayerScript
{
public:
    GuildAdventureStoragePlayerScript() : PlayerScript("GuildAdventureStoragePlayerScript", {
        PLAYERHOOK_ON_LOOT_ITEM,
        PLAYERHOOK_ON_GROUP_ROLL_REWARD_ITEM,
        PLAYERHOOK_ON_QUEST_REWARD_ITEM,
        PLAYERHOOK_ON_EQUIP,
        PLAYERHOOK_ON_UPDATE,
        PLAYERHOOK_ON_CREATURE_KILL_CREDIT,
        PLAYERHOOK_ON_SPELL_CAST,
        PLAYERHOOK_ON_CHECK_REAGENT,
        PLAYERHOOK_ON_TAKE_REAGENT,
        PLAYERHOOK_ON_MONEY_CHANGED,
        PLAYERHOOK_ON_QUEST_COMPUTE_EXP,
        PLAYERHOOK_CAN_SELL_ITEM
    }) { }

    // The guild tax: a share of every gain of a roster bot goes into the guild bank (GuildBank).
    void OnPlayerMoneyChanged(Player* player, int32& amount) override
    {
        GuildBank::OnMoneyChanged(player, amount);
    }

    // Right before a quest's reward money: the tax line names the quest.
    void OnPlayerQuestComputeXP(Player* player, Quest const* quest, uint32& /*xpValue*/) override
    {
        if (quest)
            GuildBank::OnQuestReward(player, quest->GetQuestId());
    }

    // Right before a vendor pays for an item: the tax line names the item.
    bool OnPlayerCanSellItem(Player* player, Item* item, Creature* /*creature*/) override
    {
        if (item)
            GuildBank::OnVendorSale(player, item->GetEntry());
        return true;
    }

    void OnPlayerLootItem(Player* player, Item* item, uint32 /*count*/, ObjectGuid /*lootguid*/) override
    {
        if (IsModuleEnabled())
            sStorageService.MarkUnworn(player, item, "looted");
        sLootService.OnLooted(player, item);
    }

    // A quest's reward item is a never-worn piece as much as loot is (DL1; Wardps carried eight bind-on-pickup quest
    // rewards it never put on, which the guild storage refused as soulbound).
    void OnPlayerQuestRewardItem(Player* player, Item* item, uint32 /*count*/) override
    {
        if (IsModuleEnabled())
            sStorageService.MarkUnworn(player, item, "a quest reward");
    }

    // A piece that is put on is worn: the guild storage does not take it as never-worn loot any more (DL1).
    void OnPlayerEquip(Player* /*player*/, Item* item, uint8 /*bag*/, uint8 /*slot*/, bool /*update*/) override
    {
        if (item && item->IsSoulBound())
            sStorageService.ForgetUnworn(item, "worn");
    }

    // What a character won in a roll is loot like any other: the loot filter decides (what it will wear stays).
    void OnPlayerGroupRollRewardItem(Player* player, Item* item, uint32 count, RollVote voteType, Roll* /*roll*/) override
    {
        if (!player || !item)
            return;
        if (IsModuleEnabled() && sStorageService.GetOwnerOf(player))
            LOG_INFO("module.ga.activity", "activity: {} won {} x {} in its group's roll ({})", player->GetName(), count,
                item->GetTemplate()->Name1, voteType == NEED ? "need" : voteType == GREED ? "greed" : "disenchant");
        if (IsModuleEnabled())
            sStorageService.MarkUnworn(player, item, "won in a roll");
        sLootService.OnLooted(player, item);
    }

    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        sLootService.OnCharacterUpdate(player, diff);
    }

    // A creature the character (or its group) has the loot of died: auto loot looks for corpses (PF-3).
    void OnPlayerCreatureKillCredit(Player* player, Creature* killed) override
    {
        sLootService.OnKillCredit(player, killed);
    }

    void OnPlayerSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override
    {
        if (player && spell && sCraftService.IsCraftRecipe(spell->GetSpellInfo()))
            sCraftService.OnSpellCast(player, spell->GetSpellInfo()->Id);
    }

    void OnPlayerCheckReagent(Player* player, uint32 itemId, uint32 count, bool& enough) override
    {
        sStorageService.OnCheckReagent(player, itemId, count, enough);
    }

    void OnPlayerTakeReagent(Player* player, uint32 itemId, uint32& count) override
    {
        sStorageService.OnTakeReagent(player, itemId, count);
    }
};

class GuildAdventureAuctionScript : public AuctionHouseScript
{
public:
    GuildAdventureAuctionScript() : AuctionHouseScript("GuildAdventureAuctionScript", {
        AUCTIONHOUSEHOOK_ON_AUCTION_REMOVE,
        AUCTIONHOUSEHOOK_ON_BEFORE_AUCTIONHOUSEMGR_SEND_AUCTION_EXPIRED_MAIL,
        AUCTIONHOUSEHOOK_ON_BEFORE_AUCTIONHOUSEMGR_SEND_AUCTION_SUCCESSFUL_MAIL
    }) { }

    void OnBeforeAuctionHouseMgrSendAuctionSuccessfulMail(AuctionHouseMgr* /*mgr*/, AuctionEntry* auction, Player* /*owner*/,
        uint32& /*ownerAccountId*/, uint32& profit, bool& /*sendNotification*/, bool& /*updateAchievementCriteria*/, bool& sendMail) override
    {
        sAuctionService.OnBeforeSuccessfulMail(auction, profit, sendMail);
    }

    void OnAuctionRemove(AuctionHouseObject* /*ah*/, AuctionEntry* entry) override
    {
        sAuctionService.OnAuctionRemove(entry);
    }

    void OnBeforeAuctionHouseMgrSendAuctionExpiredMail(AuctionHouseMgr* /*mgr*/, AuctionEntry* auction, Player* /*owner*/,
        uint32& /*ownerAccountId*/, bool& /*sendNotification*/, bool& sendMail) override
    {
        sAuctionService.OnBeforeExpiredMail(auction, sendMail);
    }
};

class GuildAdventureStorageWorldScript : public WorldScript
{
public:
    GuildAdventureStorageWorldScript() : WorldScript("GuildAdventureStorageWorldScript", {
        WORLDHOOK_ON_STARTUP,
        WORLDHOOK_ON_UPDATE,
        WORLDHOOK_ON_SHUTDOWN
    }) { }

    void OnStartup() override
    {
        // After GuildAdventureRosterWorldScript::OnStartup (registered first), which loads the guilds.
        sStorageService.Load();
        StorageService::IsDarkmoonCard(0); // finds the Darkmoon Faire cards now (one line in Server.log)
        sLootService.Load();
        sAuctionService.Load();
        sShoppingList.Load();
        _started = true;
    }

    void OnUpdate(uint32 diff) override
    {
        if (!_started)
            return;
        sStorageService.Update(diff);
        sLootService.Update(diff);
        sCraftService.Update(diff);
        sAuctionService.Update(diff);
        sGadvProtocol.PushStorageChanges(diff);
        GuildBank::Update(diff);
    }

    void OnShutdown() override
    {
        if (!_started)
            return;
        GuildBank::Flush();
        sLootService.FlushNow();
        sStorageService.FlushNow();
    }

private:
    bool _started = false;
};

class GuildAdventureStorageCommandScript : public CommandScript
{
public:
    GuildAdventureStorageCommandScript() : CommandScript("GuildAdventureStorageCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable storageTable =
        {
            { "deposit", HandleStorageDeposit, SEC_GAMEMASTER, Console::No },
            { "take",    HandleStorageTake,    SEC_GAMEMASTER, Console::No },
            { "junk",    HandleStorageJunk,    SEC_GAMEMASTER, Console::Yes },
            { "selljunk", HandleStorageSellJunk, SEC_GAMEMASTER, Console::No },
            { "",        HandleStorage,        SEC_GAMEMASTER, Console::Yes }
        };
        static ChatCommandTable lootTable =
        {
            { "recent", HandleLootRecent, SEC_GAMEMASTER, Console::Yes },
            { "",       HandleLoot,       SEC_GAMEMASTER, Console::Yes }
        };
        static ChatCommandTable adminStorageTable =
        {
            { "add",        HandleAdminStorageAdd,        SEC_ADMINISTRATOR, Console::Yes },
            { "sell",       HandleAdminStorageSell,       SEC_ADMINISTRATOR, Console::Yes },
            { "disenchant", HandleAdminStorageDisenchant, SEC_ADMINISTRATOR, Console::Yes },
            { "reserve",    HandleAdminStorageReserve,    SEC_ADMINISTRATOR, Console::Yes }
        };
        static ChatCommandTable adminLootRulesTable =
        {
            { "clear", HandleAdminLootRulesClear, SEC_ADMINISTRATOR, Console::Yes }
        };
        static ChatCommandTable adminAuctionTable =
        {
            { "rule",   HandleAdminAuctionRule,   SEC_ADMINISTRATOR, Console::Yes },
            { "norule", HandleAdminAuctionNoRule, SEC_ADMINISTRATOR, Console::Yes },
            { "post",   HandleAdminAuctionPost,   SEC_ADMINISTRATOR, Console::Yes },
            { "expire", HandleAdminAuctionExpire, SEC_ADMINISTRATOR, Console::Yes },
            { "catrule",   HandleAdminAuctionCatRule,   SEC_ADMINISTRATOR, Console::Yes },
            { "nocatrule", HandleAdminAuctionNoCatRule, SEC_ADMINISTRATOR, Console::Yes }
        };
        static ChatCommandTable adminGuildBankTable =
        {
            { "add", HandleAdminGuildBankAdd, SEC_ADMINISTRATOR, Console::Yes }
        };
        static ChatCommandTable adminShoppingTable =
        {
            { "set", HandleAdminShoppingSet, SEC_ADMINISTRATOR, Console::Yes },
            { "del", HandleAdminShoppingDel, SEC_ADMINISTRATOR, Console::Yes },
            { "buy", HandleAdminShoppingBuy, SEC_ADMINISTRATOR, Console::Yes }
        };
        static ChatCommandTable adminTable =
        {
            { "auction",     adminAuctionTable },
            { "guildbank",   adminGuildBankTable },
            { "shopping",    adminShoppingTable },
            { "storage",     adminStorageTable },
            { "loot",        HandleAdminLoot,        SEC_ADMINISTRATOR, Console::Yes },
            { "lootrule",    HandleAdminLootRule,    SEC_ADMINISTRATOR, Console::Yes },
            { "lootrules",   adminLootRulesTable },
            { "lootoptions", HandleAdminLootOptions, SEC_ADMINISTRATOR, Console::Yes },
            { "learn",       HandleAdminLearn,       SEC_ADMINISTRATOR, Console::Yes },
            { "craft",       HandleAdminCraft,       SEC_ADMINISTRATOR, Console::Yes },
            { "sweep",       HandleAdminSweep,       SEC_ADMINISTRATOR, Console::Yes },
            { "worngear",    HandleAdminWornGear,    SEC_ADMINISTRATOR, Console::Yes },
            { "selljunk",    HandleAdminSellJunk,    SEC_ADMINISTRATOR, Console::Yes }
        };
        static ChatCommandTable gaTable =
        {
            { "auction", HandleAuction, SEC_GAMEMASTER, Console::Yes },
            { "guildbank", HandleGuildBank, SEC_GAMEMASTER, Console::Yes },
            { "shopping", HandleShopping, SEC_GAMEMASTER, Console::Yes },
            { "storage", storageTable },
            { "loot",    lootTable },
            { "admin",   adminTable }
        };
        static ChatCommandTable commandTable =
        {
            { "ga", gaTable }
        };
        return commandTable;
    }

    // --- storage ---

    static bool HandleStorage(ChatHandler* handler, Optional<PlayerIdentifier> target)
    {
        uint32 const owner = ResolveOwner(handler, target);
        if (!owner)
            return false;
        auto const [kinds, items] = sStorageService.GetTotals(owner);
        handler->PSendSysMessage("Guild storage of account {}: {} kinds, {} items (storage {}, reagents from it {}).", owner, kinds,
            items, GetSettings().storageEnable ? "on" : "off", GetSettings().storageReagents ? "on" : "off");

        std::vector<StorageItem> list = sStorageService.ListAll(owner);
        std::sort(list.begin(), list.end(), [](StorageItem const& a, StorageItem const& b) { return a.amount > b.amount; });
        for (std::size_t i = 0; i < list.size() && i < 15; ++i)
            handler->PSendSysMessage("  {} x {} ({}{}, {}{})", list[i].amount, ItemName(list[i].entry), list[i].entry,
                list[i].randomProperty ? Acore::StringFormat(", property {}", list[i].randomProperty) : std::string(),
                StorageCategory(list[i].itemClass, list[i].subClass, list[i].entry),
                list[i].reserve ? Acore::StringFormat(", reserve {}", list[i].reserve) : std::string());
        return true;
    }

    static bool HandleStorageDeposit(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        if (!player || !sStorageService.GetOwnerOf(player))
        {
            handler->SendErrorMessage("This character is no character of an adventure guild.", false);
            return false;
        }
        std::vector<StorageItem> moved;
        auto const [stacks, items] = sStorageService.DepositMaterials(player, &moved);
        for (StorageItem const& kind : moved)
            sLootService.LogStorageMove(sStorageService.GetOwnerOf(player), player->GetGUID(), player->GetName(), kind.entry,
                kind.randomProperty, kind.amount, true, Acore::StringFormat("deposited by {} (.ga storage deposit)", player->GetName()));
        handler->PSendSysMessage("Deposited {} stacks ({} items) into the guild storage.", stacks, items);
        return true;
    }

    // What of the guild storage is junk (LootService::IsJunk), and what a vendor pays for it.
    static bool HandleStorageJunk(ChatHandler* handler, Optional<PlayerIdentifier> target)
    {
        uint32 const owner = ResolveOwner(handler, target);
        if (!owner)
            return false;
        int32 const quality = LootService::JunkQualityOfGuild(owner);
        if (quality < 0)
        {
            handler->PSendSysMessage("Junk is not sold for this guild (bot setting LootFilter.SellJunk, or selling is off).");
            return true;
        }
        std::vector<std::pair<uint64, std::string>> lines;
        for (StorageItem const& item : sStorageService.List(owner))
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item.entry);
            if (proto && sLootService.IsJunk(owner, proto, item.randomProperty, quality, true))
                lines.emplace_back(uint64(proto->SellPrice) * item.amount,
                    Acore::StringFormat("{} x {} ({})", item.amount, ItemName(item.entry), item.entry));
        }
        JunkSale const sale = sLootService.SellStoredJunk(owner, nullptr, false);
        handler->PSendSysMessage("Junk in the guild storage of account {} ({}): {} kinds, {} items, worth {} copper at a vendor.",
            owner, quality ? "grey and white" : "grey only", sale.kinds, sale.items, sale.money);
        std::sort(lines.begin(), lines.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
        for (std::size_t i = 0; i < lines.size() && i < 15; ++i)
            handler->PSendSysMessage("  {}", lines[i].second);
        return true;
    }

    static bool SellJunkInto(ChatHandler* handler, Player* player)
    {
        uint32 const owner = player ? sStorageService.GetOwnerOf(player) : 0;
        if (!owner)
        {
            handler->SendErrorMessage("This character is no character of an adventure guild.", false);
            return false;
        }
        JunkSale const sale = sLootService.SellStoredJunk(owner, player, true);
        if (!sale.items)
        {
            handler->SendErrorMessage(sale.capped ? "Nothing sold: the gold limit." : "No junk in the guild storage.", false);
            return false;
        }
        handler->PSendSysMessage("{} sold {} junk items ({} kinds) from the guild storage for {} copper{}.", player->GetName(),
            sale.items, sale.kinds, sale.money, sale.capped ? " (stopped at the gold limit)" : "");
        return true;
    }

    // Test aids of the Storage tab's Sell / Disenchant buttons: what the owner's character `target` would order.
    // A test aid like the Storage tab's Reserve: the bots leave `reserve` of the kind in the storage (0: none).
    static bool HandleAdminStorageReserve(ChatHandler* handler, PlayerIdentifier target, uint32 entry, uint32 reserve)
    {
        uint32 const owner = ResolveOwner(handler, target);
        if (!owner)
            return false;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        if (!proto)
        {
            handler->SendErrorMessage(Acore::StringFormat("Unknown item {}.", entry), false);
            return false;
        }
        uint32 const before = sStorageService.GetReserve(owner, entry, 0);
        sStorageService.SetReserve(owner, entry, 0, reserve);
        LOG_INFO("module.ga.activity", "activity: an administrator sets the reserve of {} in the guild storage of owner {}: {} -> {}",
            proto->Name1, owner, before, reserve);
        handler->PSendSysMessage("Guild storage of account {}: {} x {}, reserve {} (the bots may take {}).", owner,
            sStorageService.GetStored(owner, entry, 0), proto->Name1, reserve, sStorageService.GetAmount(owner, entry, 0));
        return true;
    }

    static bool HandleAdminStorageSell(ChatHandler* handler, PlayerIdentifier target, uint32 entry, Optional<uint32> count)
    {
        StorageService::OwnerOrder ownerOrder; // as the owner's Sell
        Player* player = InWorldGuildCharacter(handler, target);
        uint32 const owner = player ? sStorageService.GetOwnerOf(player) : 0;
        if (!owner)
            return false;
        uint64 money = 0;
        std::string error;
        uint32 const sold = sLootService.SellStored(owner, player, entry, 0, count.value_or(0), money, error);
        if (!sold)
        {
            handler->SendErrorMessage(Acore::StringFormat("Nothing sold: {}.", error), false);
            return false;
        }
        handler->PSendSysMessage("{} sold {} x {} from the guild storage for {} copper.", player->GetName(), sold, ItemName(entry), money);
        return true;
    }

    static bool HandleAdminStorageDisenchant(ChatHandler* handler, PlayerIdentifier target, uint32 entry)
    {
        StorageService::OwnerOrder ownerOrder; // as the owner's DE
        Player* player = InWorldGuildCharacter(handler, target);
        uint32 const owner = player ? sStorageService.GetOwnerOf(player) : 0;
        if (!owner)
            return false;
        StorageDisenchant const result = sLootService.DisenchantFromStorage(owner, entry, 0, player->GetName());
        if (!result.done)
        {
            handler->SendErrorMessage(Acore::StringFormat("Not disenchanted ({}): {}", result.error, result.text), false);
            return false;
        }
        handler->PSendSysMessage("{} disenchanted 1 x {} from the guild storage: {}.", result.enchanter, ItemName(entry),
            result.materials);
        return true;
    }

    static bool HandleStorageSellJunk(ChatHandler* handler)
    {
        return SellJunkInto(handler, handler->GetPlayer());
    }

    static bool HandleAdminSellJunk(ChatHandler* handler, PlayerIdentifier target)
    {
        Player* player = InWorldGuildCharacter(handler, target);
        return player && SellJunkInto(handler, player);
    }

    static bool HandleStorageTake(ChatHandler* handler, uint32 entry, Optional<uint32> count)
    {
        Player* player = handler->GetPlayer();
        if (!player || !sStorageService.GetOwnerOf(player))
        {
            handler->SendErrorMessage("This character is no character of an adventure guild.", false);
            return false;
        }
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        uint32 const want = count.value_or(proto ? std::max<uint32>(1, proto->GetMaxStackSize()) : 1);
        std::string error;
        StorageService::OwnerOrder ownerOrder; // the owner's own take reaches into the reserve
        uint32 const taken = sStorageService.Withdraw(player, entry, 0, want, error);
        if (!taken)
        {
            handler->SendErrorMessage(Acore::StringFormat("Nothing taken: {}.", error), false);
            return false;
        }
        sLootService.LogStorageMove(sStorageService.GetOwnerOf(player), player->GetGUID(), player->GetName(), entry, 0, taken, false,
            Acore::StringFormat("taken by {} (.ga storage take)", player->GetName()));
        handler->PSendSysMessage("Took {} x {} from the guild storage.", taken, ItemName(entry));
        return true;
    }

    // --- loot ---

    static bool HandleLoot(ChatHandler* handler, Optional<PlayerIdentifier> target)
    {
        uint32 const owner = ResolveOwner(handler, target);
        if (!owner)
            return false;
        LootSettings const s = sLootService.GetSettingsOf(owner);
        handler->PSendSysMessage("Loot of account {}: filter {}, default for AI-played characters: {}, for your own: {}, auto loot "
            "{} (server: filter {}, auto loot {}).", owner, s.filterEnabled ? "on" : "off", ActionName(s.defaultAi),
            ActionName(s.defaultPlayer), s.autoLoot ? "on" : "off", GetSettings().lootFilterEnable ? "on" : "off",
            GetSettings().autoLootEnable ? "on" : "off");
        handler->PSendSysMessage("Totals: stored {}, sold for {} copper, disenchanted {}, deleted {}.", s.totalStored, s.totalSold,
            s.totalDisenchanted, s.totalDeleted);
        for (LR::Rule const& rule : sLootService.GetRules(owner))
            handler->PSendSysMessage("  rule {} #{} {} {}: {}", rule.id, rule.position, ActionName(rule.action),
                rule.enabled ? "on" : "off", LR::EncodeConditions(rule.conditions));
        return true;
    }

    static bool HandleLootRecent(ChatHandler* handler, Optional<PlayerIdentifier> target)
    {
        uint32 const owner = ResolveOwner(handler, target);
        if (!owner)
            return false;
        auto const& log = sLootService.GetLog(owner);
        handler->PSendSysMessage("Last {} loot of account {} (newest last):", log.size(), owner);
        std::size_t const from = log.size() > 15 ? log.size() - 15 : 0;
        for (std::size_t i = from; i < log.size(); ++i)
        {
            LootLogEntry const& e = log[i];
            std::string name;
            sCharacterCache->GetCharacterNameByGuid(e.character, name);
            handler->PSendSysMessage("  {}: {} x {} -> {}{}", name.empty() ? "?" : name, e.count, ItemName(e.entry),
                OutcomeName(e.outcome), e.why.empty() ? std::string() : " (" + e.why + ")");
        }
        return true;
    }

    // --- auction house ---

    static bool HandleAuction(ChatHandler* handler, Optional<PlayerIdentifier> target)
    {
        uint32 const owner = ResolveOwner(handler, target);
        if (!owner)
            return false;
        std::vector<AuctionRule> const rules = sAuctionService.GetRules(owner);
        std::vector<AuctionListing> const listings = sAuctionService.GetListings(owner);
        handler->PSendSysMessage("Auction house surplus of account {}: {} rules, {} auctions up (server: {}).", owner, rules.size(),
            listings.size(), GetSettings().auctionEnable ? "on" : "off");
        for (AuctionRule const& rule : rules)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(rule.entry);
            handler->PSendSysMessage("  {} ({}): keep {}, stacks of {}, {} copper each{}, {} in the storage, {} listed", ItemName(rule.entry),
                rule.entry, rule.keep, rule.stack, rule.price ? rule.price : (proto ? AuctionService::DefaultPrice(proto) : 0),
                rule.price ? "" : " (default)", sStorageService.GetStored(owner, rule.entry, rule.randomProperty),
                sAuctionService.GetListedCount(owner, rule.entry, rule.randomProperty));
        }
        return true;
    }

    static bool HandleAdminAuctionRule(ChatHandler* handler, PlayerIdentifier target, uint32 entry, uint32 keep, Optional<uint32> stack,
        Optional<uint32> price)
    {
        uint32 const owner = ResolveOwner(handler, target);
        if (!owner)
            return false;
        AuctionRule rule;
        rule.entry = entry;
        rule.keep = keep;
        rule.stack = stack.value_or(0);
        rule.price = price.value_or(0);
        std::string error;
        if (!sAuctionService.SetRule(owner, rule, error))
        {
            handler->SendErrorMessage(Acore::StringFormat("Rule refused: {}.", error), false);
            return false;
        }
        sGadvProtocol.PushAuctions(owner);
        handler->PSendSysMessage("Auction rule for {}: keep {}, the rest goes to the auction house.", ItemName(entry), keep);
        return true;
    }

    static bool HandleAdminAuctionNoRule(ChatHandler* handler, PlayerIdentifier target, uint32 entry)
    {
        uint32 const owner = ResolveOwner(handler, target);
        if (!owner)
            return false;
        if (!sAuctionService.DeleteRule(owner, entry, 0))
        {
            handler->SendErrorMessage("No such auction rule.", false);
            return false;
        }
        sGadvProtocol.PushAuctions(owner);
        handler->PSendSysMessage("Auction rule for {} deleted.", ItemName(entry));
        return true;
    }

    static bool HandleAdminAuctionCatRule(ChatHandler* handler, PlayerIdentifier target, std::string category, uint32 keep,
        Optional<uint32> stack)
    {
        uint32 const owner = ResolveOwner(handler, target);
        if (!owner)
            return false;
        AuctionCategoryRule rule;
        rule.category = category;
        rule.keep = keep;
        rule.stack = stack.value_or(0);
        std::string error;
        if (!sAuctionService.SetCategoryRule(owner, rule, error))
        {
            handler->SendErrorMessage(Acore::StringFormat("Rule refused: {}.", error), false);
            return false;
        }
        sGadvProtocol.PushAuctions(owner);
        handler->PSendSysMessage("Auction rule for the category {}: keep {} of each kind, the rest goes to the auction house.", category,
            keep);
        return true;
    }

    static bool HandleAdminAuctionNoCatRule(ChatHandler* handler, PlayerIdentifier target, std::string category)
    {
        uint32 const owner = ResolveOwner(handler, target);
        if (!owner)
            return false;
        if (!sAuctionService.DeleteCategoryRule(owner, category))
        {
            handler->SendErrorMessage("No such category rule.", false);
            return false;
        }
        sGadvProtocol.PushAuctions(owner);
        handler->PSendSysMessage("Auction rule for the category {} deleted.", category);
        return true;
    }

    // --- the guild bank (GuildBank) ---

    static bool HandleGuildBank(ChatHandler* handler, Optional<PlayerIdentifier> target)
    {
        uint32 const owner = ResolveOwner(handler, target);
        if (!owner)
            return false;
        handler->PSendSysMessage("Guild bank of account {}: {} copper; guild tax {}% of every gain of the roster bots ({} copper held "
            "back and waiting for its booking).", owner, GuildBank::Money(owner), sBotSettings.GetForGuild(owner, Setting::GuildTaxPercent),
            GuildBank::PendingTax(owner));
        return true;
    }

    // .ga shopping [character]: the guild's shopping list with what the storage holds, today's spending and the budgets.
    static bool HandleShopping(ChatHandler* handler, Optional<PlayerIdentifier> target)
    {
        uint32 const owner = ResolveOwner(handler, target);
        if (!owner)
            return false;
        std::vector<ShoppingEntry> const list = sShoppingList.Get(owner);
        handler->PSendSysMessage("Shopping list of account {}: {} items; spent today {} of {} per day, at most {} per vendor visit; "
            "the guild bank has {}{}.", owner, list.size(), GuildBudget::MoneyText(uint32(sShoppingList.SpentToday(owner))),
            GuildBudget::MoneyText(uint32(std::max(sBotSettings.GetForGuild(owner, Setting::ShoppingMaxPerDay), 0))),
            GuildBudget::MoneyText(uint32(std::max(sBotSettings.GetForGuild(owner, Setting::ShoppingMaxPerVisit), 0))),
            GuildBudget::MoneyText(uint32(std::min<uint64>(GuildBank::Money(owner), std::numeric_limits<uint32>::max()))),
            sBotSettings.GetForGuild(owner, Setting::ShoppingEnable) ? "" : " (Shopping.Enable is off for the guild)");
        for (ShoppingEntry const& entry : list)
        {
            ShoppingItem const* item = sShoppingList.FindItem(entry.entry);
            handler->PSendSysMessage("  {} (item {}): {} of at least {}{}{}", ItemName(entry.entry), entry.entry,
                sStorageService.GetStoredOfEntry(owner, entry.entry), entry.min, entry.enabled ? "" : ", paused",
                item ? Acore::StringFormat(", {} for {} at a vendor", GuildBudget::MoneyText(item->price), item->bundle) : std::string());
        }
        return true;
    }

    // .ga admin shopping set <character> <item> <minimum> / del <character> <item>: the guild's list (tests, setup).
    static bool HandleAdminShoppingSet(ChatHandler* handler, PlayerIdentifier target, uint32 entry, uint32 minimum)
    {
        uint32 const owner = ResolveOwner(handler, target);
        if (!owner)
            return false;
        AdventureGuild const* guild = sGuildAdventureMgr.FindGuild(owner);
        ShoppingEntry item;
        item.entry = entry;
        item.min = minimum;
        std::string error;
        if (!guild || !sShoppingList.Set(owner, guild->team, item, error))
        {
            handler->SendErrorMessage(Acore::StringFormat("Not on the shopping list: {}.", guild ? error : std::string("no adventure guild")),
                false);
            return false;
        }
        sGadvProtocol.PushShopping(owner);
        handler->PSendSysMessage("Shopping list of account {}: {} at least {}.", owner, ItemName(entry), minimum);
        return true;
    }

    static bool HandleAdminShoppingDel(ChatHandler* handler, PlayerIdentifier target, uint32 entry)
    {
        uint32 const owner = ResolveOwner(handler, target);
        if (!owner)
            return false;
        if (!sShoppingList.Delete(owner, entry))
        {
            handler->SendErrorMessage("That item is not on the shopping list.", false);
            return false;
        }
        sGadvProtocol.PushShopping(owner);
        handler->PSendSysMessage("Shopping list of account {}: {} taken off.", owner, ItemName(entry));
        return true;
    }

    // .ga admin shopping buy <character>: test aid, the character buys for the list at the vendor it stands at.
    static bool HandleAdminShoppingBuy(ChatHandler* handler, PlayerIdentifier target)
    {
        Player* player = target.GetConnectedPlayer();
        if (!player)
        {
            handler->SendErrorMessage(Acore::StringFormat("{} is not in the world.", target.GetName()), false);
            return false;
        }
        handler->SendSysMessage(Acore::StringFormat("{}: {}", player->GetName(), sShoppingList.BuyHere(player)));
        return true;
    }

    static bool HandleAdminGuildBankAdd(ChatHandler* handler, PlayerIdentifier target, uint32 copper)
    {
        uint32 const owner = ResolveOwner(handler, target);
        if (!owner)
            return false;
        if (!GuildBank::Deposit(owner, copper, "an administrator's command"))
        {
            handler->SendErrorMessage("No guild bank (the adventure guild has no WoW guild).", false);
            return false;
        }
        handler->PSendSysMessage("Guild bank of account {}: {} copper now.", owner, GuildBank::Money(owner));
        return true;
    }

    static bool HandleAdminAuctionPost(ChatHandler* handler, PlayerIdentifier target)
    {
        uint32 const owner = ResolveOwner(handler, target);
        if (!owner)
            return false;
        uint32 const created = sAuctionService.PostSurplus(owner);
        handler->PSendSysMessage("Posted {} auctions for account {}.", created, owner);
        return true;
    }

    static bool HandleAdminAuctionExpire(ChatHandler* handler, PlayerIdentifier target)
    {
        uint32 const owner = ResolveOwner(handler, target);
        if (!owner)
            return false;
        uint32 const count = sAuctionService.ExpireListings(owner);
        handler->PSendSysMessage("{} auctions of account {} expire at the next auction house update.", count, owner);
        return true;
    }

    // --- admin (tests without a client) ---

    static bool HandleAdminStorageAdd(ChatHandler* handler, PlayerIdentifier target, uint32 entry, Optional<uint32> count)
    {
        uint32 const owner = ResolveOwner(handler, target);
        if (!owner)
            return false;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        if (!proto)
        {
            handler->SendErrorMessage(Acore::StringFormat("Unknown item {}.", entry), false);
            return false;
        }
        if (char const* reason = StorageService::RefuseReason(proto))
        {
            handler->SendErrorMessage(Acore::StringFormat("{} cannot go into the storage: {}.", proto->Name1, reason), false);
            return false;
        }
        uint32 const added = std::max<uint32>(1, count.value_or(1));
        uint32 const amount = sStorageService.Add(owner, proto, 0, added);
        sLootService.LogStorageMove(owner, ObjectGuid::Empty, "an administrator", entry, 0, added, true,
            "added by an administrator (test aid)");
        handler->PSendSysMessage("The guild storage of account {} has {} x {} now.", owner, amount, proto->Name1);
        return true;
    }

    static bool HandleAdminLoot(ChatHandler* handler, PlayerIdentifier target, uint32 entry, Optional<uint32> count)
    {
        Player* player = InWorldGuildCharacter(handler, target);
        if (!player)
            return false;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        if (!proto)
        {
            handler->SendErrorMessage(Acore::StringFormat("Unknown item {}.", entry), false);
            return false;
        }
        uint32 const n = std::clamp<uint32>(count.value_or(1), 1, std::max<uint32>(1, proto->GetMaxStackSize()));
        ItemPosCountVec dest;
        if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, entry, n) != EQUIP_ERR_OK)
        {
            handler->SendErrorMessage(Acore::StringFormat("{} has no room for {} x {}.", player->GetName(), n, proto->Name1), false);
            return false;
        }
        Item* item = player->StoreNewItem(dest, entry, true, Item::GenerateItemRandomPropertyId(entry));
        if (!item)
        {
            handler->SendErrorMessage("The item could not be created.", false);
            return false;
        }
        player->SendNewItem(item, n, true, false);
        // As Player::StoreLootItem does: the loot filter sees it next tick.
        sScriptMgr->OnPlayerLootItem(player, item, n, ObjectGuid::Empty);
        handler->PSendSysMessage("{} looted {} x {}; the loot filter decides next tick (.ga loot recent {}).", player->GetName(), n,
            proto->Name1, player->GetName());
        return true;
    }

    static bool HandleAdminLootRule(ChatHandler* handler, PlayerIdentifier target, std::string action, std::string conditions)
    {
        uint32 const owner = ResolveOwner(handler, target);
        if (!owner)
            return false;
        std::optional<uint8> const act = ActionFromName(action);
        LR::Rule rule;
        if (!act || !LR::DecodeConditions(conditions, rule.conditions))
        {
            handler->SendErrorMessage("Usage: .ga admin lootrule <character> <keep|sell|disenchant|delete|store> "
                "<type:op:value:value2:text[:group];...>", false);
            return false;
        }
        rule.action = *act;
        std::string error;
        if (!sLootService.SetRule(owner, rule, error))
        {
            handler->SendErrorMessage(Acore::StringFormat("Rule refused: {}.", error), false);
            return false;
        }
        sGadvProtocol.PushLoot(owner);
        handler->PSendSysMessage("Added a rule ({} {}) to the loot filter of account {}.", ActionName(rule.action), conditions, owner);
        return true;
    }

    static bool HandleAdminLootRulesClear(ChatHandler* handler, PlayerIdentifier target)
    {
        uint32 const owner = ResolveOwner(handler, target);
        if (!owner)
            return false;
        sLootService.ClearRules(owner);
        sGadvProtocol.PushLoot(owner);
        handler->PSendSysMessage("Deleted every loot rule of account {}.", owner);
        return true;
    }

    static bool HandleAdminLootOptions(ChatHandler* handler, PlayerIdentifier target, uint32 filter, std::string defaultAi,
        std::string defaultPlayer, uint32 autoLoot)
    {
        uint32 const owner = ResolveOwner(handler, target);
        if (!owner)
            return false;
        std::optional<uint8> const ai = ActionFromName(defaultAi);
        std::optional<uint8> const own = ActionFromName(defaultPlayer);
        if (!ai || !own)
        {
            handler->SendErrorMessage("Actions: keep, sell, disenchant, delete, store.", false);
            return false;
        }
        sLootService.SetOptions(owner, filter != 0, *ai, *own, autoLoot != 0);
        sGadvProtocol.PushLoot(owner);
        handler->PSendSysMessage("Loot of account {}: filter {}, default AI {}, default own {}, auto loot {}.", owner,
            filter ? "on" : "off", ActionName(*ai), ActionName(*own), autoLoot ? "on" : "off");
        return true;
    }

    static bool HandleAdminLearn(ChatHandler* handler, PlayerIdentifier target, uint32 spellId)
    {
        Player* player = InWorldGuildCharacter(handler, target);
        if (!player)
            return false;
        if (!sSpellMgr->GetSpellInfo(spellId))
        {
            handler->SendErrorMessage(Acore::StringFormat("Unknown spell {}.", spellId), false);
            return false;
        }
        player->learnSpell(spellId);
        handler->PSendSysMessage("{} {} spell {}.", player->GetName(), player->HasSpell(spellId) ? "knows" : "could not learn", spellId);
        return player->HasSpell(spellId);
    }

    // What the worn-gear rule (WG1) makes of each soulbound weapon and armour piece in the bags of a guild character the
    // AI plays; with "go" (a test aid) it acts now, with the vendor it stands at for a sale.
    static bool HandleAdminWornGear(ChatHandler* handler, PlayerIdentifier target, Optional<std::string> go)
    {
        Player* player = InWorldGuildCharacter(handler, target);
        if (!player)
            return false;
        Creature* vendor = nullptr;
        for (ServicePoint const& service : sWorldIndex.GetServices(ServiceType::Vendor))
            if (service.spawn.mapId == player->GetMapId() && player->GetDistance2d(service.spawn.x, service.spawn.y) <= 12.0f)
                if ((vendor = player->FindNearestCreature(service.spawn.entry, INTERACTION_DISTANCE)))
                    break;
        uint32 const skill = player->HasSkill(SKILL_ENCHANTING) ? player->GetSkillValue(SKILL_ENCHANTING) : 0;
        handler->PSendSysMessage("{}: LootFilter.WornGear {} (0 keep, 1 disenchant or sell, 2 disenchant only), Enchanting {}, vendor "
            "within reach: {}", player->GetName(), sBotSettings.Get(player, Setting::LootWornGear), skill, vendor ? vendor->GetName() : "none");
        uint32 listed = 0;
        auto look = [&](Item* item)
        {
            ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
            if (!proto || !item->IsSoulBound() || (proto->Class != ITEM_CLASS_WEAPON && proto->Class != ITEM_CLASS_ARMOR))
                return;
            ++listed;
            char const* keeps = sLootService.WornGearKeeps(player, item);
            bool const disenchant = skill && CraftOrders::CanDisenchant(proto->ItemId) && uint32(proto->RequiredDisenchantSkill) <= skill;
            handler->PSendSysMessage("  {} (item level {}, quality {}): {}", proto->Name1, proto->ItemLevel, proto->Quality,
                keeps ? Acore::StringFormat("stays ({})", keeps) : disenchant ? std::string("goes: disenchanted by it") :
                proto->SellPrice ? std::string("goes: sold at its next vendor visit") : std::string("stays (no vendor price)"));
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            look(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = player->GetBagByPos(bagSlot))
                for (uint32 i = 0; i < bag->GetBagSize(); ++i)
                    look(bag->GetItemByPos(uint8(i)));
        if (!listed)
            handler->SendSysMessage("  no soulbound weapon or armour in its bags");
        // Quest items (QI1).
        auto lookQuest = [&](Item* item)
        {
            ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
            if (!proto || (proto->Class != ITEM_CLASS_QUEST && !proto->StartQuest && proto->Bonding != BIND_QUEST_ITEM &&
                proto->Bonding != BIND_QUEST_ITEM1))
                return;
            char const* keeps = sLootService.QuestItemKeeps(player, item);
            handler->PSendSysMessage("  quest item {} x {}: {}", item->GetCount(), proto->Name1, keeps ? Acore::StringFormat("stays ({})", keeps) :
                proto->SellPrice ? std::string("goes: sold at its next vendor visit") : std::string("goes: deleted"));
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            lookQuest(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = player->GetBagByPos(bagSlot))
                for (uint32 i = 0; i < bag->GetBagSize(); ++i)
                    lookQuest(bag->GetItemByPos(uint8(i)));
        if (go && *go == "go")
            handler->PSendSysMessage("{} pieces went (the activity log has a line each).", sLootService.HandleWornGear(player, vendor));
        return true;
    }

    static bool HandleAdminSweep(ChatHandler* handler, PlayerIdentifier target)
    {
        Player* player = InWorldGuildCharacter(handler, target);
        if (!player)
            return false;
        uint32 const stacks = sLootService.SweepBags(player);
        handler->PSendSysMessage("Looked at {} stacks in the bags of {} (only a character the AI plays is swept; .ga loot recent "
            "shows what moved).", stacks, player->GetName());
        return true;
    }

    static bool HandleAdminCraft(ChatHandler* handler, PlayerIdentifier target, uint32 spellId, Optional<uint32> count)
    {
        Player* player = InWorldGuildCharacter(handler, target);
        if (!player)
            return false;
        std::string error;
        std::string text;
        if (!sCraftService.Start(player, spellId, count.value_or(1), error, text))
        {
            handler->SendErrorMessage(Acore::StringFormat("{} ({})", text, error), false);
            return false;
        }
        handler->PSendSysMessage("{}", text);
        return true;
    }
};

// A withdrawal from the WoW guild bank is the guild's own gold: the guild tax leaves it alone.
class GuildAdventureGuildBankScript : public GuildScript
{
public:
    GuildAdventureGuildBankScript() : GuildScript("GuildAdventureGuildBankScript", { GUILDHOOK_ON_MEMBER_WITDRAW_MONEY }) { }

    void OnMemberWitdrawMoney(Guild* /*guild*/, Player* player, uint32& amount, bool isRepair) override
    {
        GuildBank::OnGuildBankWithdrawal(player, amount, isRepair);
    }
};

void AddGuildAdventureStorageScripts()
{
    new GuildAdventureStoragePlayerScript();
    new GuildAdventureGuildBankScript();
    new GuildAdventureAuctionScript();
    new GuildAdventureStorageWorldScript();
    new GuildAdventureStorageCommandScript();
}
