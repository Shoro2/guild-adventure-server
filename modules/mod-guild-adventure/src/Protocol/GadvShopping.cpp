/*
 * GADV: the guild storage's shopping list (ShoppingList.h, docs/storage.md "Shopping list", docs/protocol.md
 * section 7.25). Block SHOPPING (the list with what the storage holds, today's spending, the budgets), intents
 * SHOP_SET / SHOP_DEL, and SHOP_SEARCH with block SHOP_ITEMS (items vendors of the guild's side sell for gold).
 */

#include "GadvProtocol.h"
#include "BotSettings.h"
#include "GuildAdventureMgr.h"
#include "GuildBank.h"
#include "ObjectMgr.h"
#include "ShoppingList.h"
#include "StorageService.h"
#include <algorithm>
#include <cctype>

using namespace GuildAdventure;
using GuildAdventure::Gadv::Message;

namespace
{
    constexpr std::size_t MaxShopSearchResults = 30;
    constexpr std::size_t MinShopSearchLength = 2;

    int64 Clamp(int64 value, int64 low, int64 high)
    {
        return std::clamp<int64>(value, low, high);
    }
}

void GadvProtocol::SendShopping(uint32 accountId)
{
    std::vector<Message> records;
    for (ShoppingEntry const& entry : sShoppingList.Get(accountId))
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry.entry);
        ShoppingItem const* item = sShoppingList.FindItem(entry.entry);
        records.push_back(Message("SHOP")
            .Set("e", entry.entry)
            .Set("n", proto ? std::string(proto->Name1) : std::to_string(entry.entry))
            .Set("min", entry.min)
            .Set("on", entry.enabled)
            .Set("have", sStorageService.GetStoredOfEntry(accountId, entry.entry))
            .Set("price", item ? item->price : 0u)
            .Set("bc", item ? item->bundle : 1u));
    }
    SendBlock(accountId, "SHOPPING", records, Message("SHOPPING_BEGIN")
        .Set("srv", sBotSettings.GetForGuild(accountId, Setting::ShoppingEnable) != 0)
        .Set("day", uint32(std::min<uint64>(sShoppingList.SpentToday(accountId), 0xFFFFFFFFu)))
        .Set("cap", uint32(std::max(sBotSettings.GetForGuild(accountId, Setting::ShoppingMaxPerDay), 0)))
        .Set("visit", uint32(std::max(sBotSettings.GetForGuild(accountId, Setting::ShoppingMaxPerVisit), 0)))
        .Set("max", uint32(ShoppingList::MaxEntries)));
}

void GadvProtocol::PushShopping(uint32 ownerAccount)
{
    if (FindConnectedLink(ownerAccount))
        SendShopping(ownerAccount);
}

void GadvProtocol::OnShoppingSet(uint32 accountId, AdventureGuild& guild, Message const& message)
{
    std::optional<int64> const r = message.GetInt("r");
    std::optional<int64> const entry = message.GetInt("e");
    if (!entry || *entry <= 0 || *entry > 0x7FFFFFFF)
    {
        Error(accountId, r, "BAD_REQUEST", "SHOP_SET needs e (item).");
        return;
    }
    ShoppingEntry item;
    item.entry = uint32(*entry);
    item.min = uint32(Clamp(message.GetInt("min").value_or(0), 0, 0x7FFFFFFF));
    item.enabled = message.GetBool("on").value_or(true);
    std::string error;
    if (!sShoppingList.Set(accountId, guild.team, item, error))
    {
        // "CODE: text" from ShoppingList::Set.
        std::size_t const colon = error.find(": ");
        std::string const code = colon == std::string::npos ? error : error.substr(0, colon);
        std::string const text = colon == std::string::npos ? error : error.substr(colon + 2);
        Error(accountId, r, "SHOP_REFUSED", text, code);
        return;
    }
    SendShopping(accountId);
    Ack(accountId, r);
}

void GadvProtocol::OnShoppingDelete(uint32 accountId, Message const& message)
{
    std::optional<int64> const r = message.GetInt("r");
    std::optional<int64> const entry = message.GetInt("e");
    if (!entry || !sShoppingList.Delete(accountId, uint32(*entry)))
    {
        Error(accountId, r, "SHOP_REFUSED", "That item is not on the shopping list.", "NOT_FOUND");
        return;
    }
    SendShopping(accountId);
    Ack(accountId, r);
}

void GadvProtocol::OnShoppingSearch(uint32 accountId, AdventureGuild& guild, Message const& message)
{
    std::optional<int64> const r = message.GetInt("r");
    std::string const text = message.GetString("q");
    bool const number = !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
    if (text.size() < MinShopSearchLength && !number)
    {
        Error(accountId, r, "BAD_REQUEST", "Type at least two letters of the item's name, or its id.");
        return;
    }
    std::vector<Message> records;
    for (ShoppingItem const& item : sShoppingList.Search(text, guild.team, MaxShopSearchResults))
        records.push_back(Message("SHOP_ITEM")
            .Set("e", item.entry)
            .Set("n", item.name)
            .Set("q", item.quality)
            .Set("lvl", item.requiredLevel)
            .Set("price", item.price)
            .Set("bc", item.bundle));
    SendBlock(accountId, "SHOP_ITEMS", records, Message("SHOP_ITEMS_BEGIN").Set("q", text));
    Ack(accountId, r);
}
