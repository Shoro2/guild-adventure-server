/*
 * The GADV part of the guild's craft orders (docs/protocol.md §7.21, CraftOrders.h). Members of GadvProtocol, kept
 * apart from GadvProtocol.cpp.
 */

#include "GadvProtocol.h"
#include "CraftOrders.h"
#include "GuildAdventureMgr.h"

using namespace GuildAdventure;
using GuildAdventure::Gadv::Message;

void GadvProtocol::SendCraftOrders(uint32 accountId, AdventureGuild const& guild)
{
    std::vector<Message> records;
    for (CraftOrderView const& view : sCraftOrders.Describe(guild))
        records.push_back(Message("CORDER")
            .Set("id", view.order.id)
            .Set("item", view.order.item)
            .Set("n", view.itemName)
            .Set("amt", view.order.amount)
            .Set("made", view.order.made)
            .Set("note", view.note));
    SendBlock(accountId, "CRAFT_ORDERS", records, Message("CRAFT_ORDERS_BEGIN"));
}

void GadvProtocol::SendCraftables(uint32 accountId, AdventureGuild const& guild)
{
    std::vector<Message> records;
    for (Craftable const& entry : sCraftOrders.Craftables(guild))
        records.push_back(Message("CRAFTABLE")
            .Set("item", entry.item)
            .Set("n", entry.name)
            .Set("il", entry.itemLevel)
            .Set("cls", entry.itemClass)
            .Set("sub", entry.itemSubClass)
            .Set("by", entry.crafters));
    SendBlock(accountId, "CRAFTABLES", records, Message("CRAFTABLES_BEGIN"));
}

void GadvProtocol::PushCraftOrders(AdventureGuild const& guild)
{
    if (FindConnectedLink(guild.ownerAccount))
        SendCraftOrders(guild.ownerAccount, guild);
}

void GadvProtocol::OnCraftOrderAdd(uint32 accountId, AdventureGuild& guild, Message const& message)
{
    std::optional<int64> const r = message.GetInt("r");
    std::optional<int64> const item = message.GetInt("item");
    std::optional<int64> const count = message.GetInt("n");
    if (!item || *item <= 0 || !count || *count <= 0)
    {
        Error(accountId, r, "BAD_REQUEST", "CRAFT_ORDER_ADD needs an item and a count.");
        return;
    }
    std::string code;
    std::string text;
    if (!sCraftOrders.Add(guild, uint32(*item), uint32(*count), code, text))
    {
        Error(accountId, r, code, text);
        return;
    }
    SendCraftOrders(accountId, guild);
    Ack(accountId, r);
}

void GadvProtocol::OnCraftOrderDelete(uint32 accountId, AdventureGuild& guild, Message const& message)
{
    std::optional<int64> const r = message.GetInt("r");
    std::optional<int64> const id = message.GetInt("id");
    std::string text;
    if (!id || *id <= 0 || !sCraftOrders.Remove(guild, uint32(*id), text))
    {
        Error(accountId, r, "CRAFT_ORDER_INVALID", text.empty() ? "CRAFT_ORDER_DEL needs an order id." : text);
        return;
    }
    SendCraftOrders(accountId, guild);
    Ack(accountId, r);
}
