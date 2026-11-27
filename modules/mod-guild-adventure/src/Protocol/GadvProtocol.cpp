/*
 * GadvProtocol implementation. See GadvProtocol.h and docs/protocol.md.
 */

#include "GadvProtocol.h"
#include "FightProfiles.h"
#include "AchievementService.h"
#include "BotSettings.h"
#include "MilestoneService.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "ControlSwapService.h"
#include "CraftOrders.h"
#include "DungeonRoles.h"
#include "Dungeons.h"
#include "GameTime.h"
#include "Guild.h"
#include "GuildAdventureMgr.h"
#include "GuildAdventureSettings.h"
#include "GuildAdventureUtil.h"
#include "GuildBotHolder.h"
#include "GuildEvents.h"
#include "GuildMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "OfflineService.h"
#include "OrderService.h"
#include "PerkService.h"
#include "Player.h"
#include "ProgressionService.h"
#include "Readiness.h"
#include "RosterService.h"
#include "SpecService.h"
#include "QuestPartners.h"
#include "Squads.h"
#include "StorageService.h"
#include "StringFormat.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"
#include <algorithm>
#include <limits>

using namespace GuildAdventure;
using GuildAdventure::Gadv::Message;

namespace
{
    constexpr char const* ModuleVersion = "0.2.0";
    constexpr uint32 PartialTimeoutMs = 15 * IN_MILLISECONDS;
    constexpr std::size_t MaxPartials = 4;
    constexpr uint32 DeltaIntervalMs = 2000;
    constexpr std::size_t MaxInboxPerTick = 256;

    // docs/protocol.md section 8.7
    constexpr auto const& PrimarySkills = GuildAdventure::PrimaryProfessionSkills;
    constexpr auto const& SecondarySkills = GuildAdventure::SecondaryProfessionSkills;
    constexpr uint16 TargetSkills[] = { 0, 75, 150, 225, 300, 375, 450 };
    constexpr int64 Continents[] = { -1, 0, 1, 530, 571 };

    std::string const PrefixTab = std::string(GuildAdventure::Gadv::Prefix) + "\t";

    template <typename Range, typename T>
    bool Contains(Range const& range, T value)
    {
        return std::find(std::begin(range), std::end(range), value) != std::end(range);
    }

    uint32 NowMs()
    {
        return uint32(GameTime::GetGameTimeMS().count());
    }

    std::string OrderWhy(AdventureGuild const& /*guild*/, GuildBot const& bot, char const* state)
    {
        std::string const s = state;
        if (s == "benched")
            return "Benched";
        bool const watched = s == "controlled" && sGuildBotHolder.IsWatched(bot.guid);
        if (s == "controlled" && !watched)
            return "Played by you";
        if (s == "offline")
            return "Offline";

        // What the bot is doing right now, from the order planner (docs/orders.md "Why line"); in watch mode the AI
        // plays the character the owner controls (docs/take-control.md §5.12).
        std::string live = sOrderService.GetWhy(bot.guid);
        if (watched)
            return live.empty() ? std::string("Watch mode: the AI plays") : "Watch mode: " + live;
        if (!live.empty())
            return live;

        // Not planned yet (the first second in the world): describe the order.
        switch (bot.order.type)
        {
            case OrderType::Auto:   return "Autopilot";
            case OrderType::Idle:   return "Idle";
            case OrderType::Follow: return "Following you";
            case OrderType::LevelZone:
            {
                ZoneTemplate const* zone = sGuildAdventureMgr.GetZone(bot.order.zoneId);
                return "Levelling in " + (zone ? zone->name : GetAreaName(bot.order.zoneId));
            }
            case OrderType::Farm:
            {
                ItemTemplate const* item = sObjectMgr->GetItemTemplate(bot.order.zoneId);
                return "Farming " + (item ? item->Name1 : std::to_string(bot.order.zoneId));
            }
            default:
                return ToToken(bot.order.type);
        }
    }

    std::string OrderDescription(BotOrder const& order)
    {
        switch (order.type)
        {
            case OrderType::Auto:   return "Autopilot";
            case OrderType::Idle:   return "Idle";
            case OrderType::Follow: return "Follow";
            case OrderType::LevelZone:
            {
                ZoneTemplate const* zone = sGuildAdventureMgr.GetZone(order.zoneId);
                return "Level in " + (zone ? zone->name : std::to_string(order.zoneId));
            }
            case OrderType::Farm:
            {
                ItemTemplate const* item = sObjectMgr->GetItemTemplate(order.zoneId);
                return "Farm " + (item ? item->Name1 : std::to_string(order.zoneId)) +
                    (order.farmTarget ? Acore::StringFormat(" (up to {} in the guild storage)", order.farmTarget) : std::string());
            }
            default:
                return ToToken(order.type);
        }
    }

    // The highest target stock of a farm order.
    constexpr int64 MaxFarmTarget = 1000000;

    std::optional<int64> RequestId(Message const& message)
    {
        return message.GetInt("r");
    }
}

GadvProtocol& GadvProtocol::Instance()
{
    static GadvProtocol instance;
    return instance;
}

void GadvProtocol::Init()
{
    sControlSwapService.AddListener([this](SwapEvent const& event) { OnSwapEvent(event); });
}

// ---------------------------------------------------------------------------------------------------------------
// Receive path
// ---------------------------------------------------------------------------------------------------------------

bool GadvProtocol::IsTransportText(std::string const& text)
{
    return text.compare(0, PrefixTab.size(), PrefixTab) == 0;
}

bool GadvProtocol::OnAddonWhisper(Player* player, std::string const& text)
{
    if (!IsTransportText(text))
        return false;

    WorldSession* session = player->GetSession();
    if (!session || session->IsHeadless())
        return true; // never echo a GADV message, not even from a bot

    Inbound inbound;
    inbound.accountId = session->GetAccountId();
    inbound.player = player->GetGUID();
    inbound.chunk = text.substr(PrefixTab.size());

    std::lock_guard<std::mutex> lock(_inboxLock);
    if (_inbox.size() < 4 * MaxInboxPerTick)
        _inbox.push_back(std::move(inbound));
    return true;
}

void GadvProtocol::Update(uint32 diff)
{
    std::vector<Inbound> inbox;
    {
        std::lock_guard<std::mutex> lock(_inboxLock);
        inbox.swap(_inbox);
    }

    for (Inbound const& inbound : inbox)
        HandleChunk(inbound.accountId, inbound.player, inbound.chunk);

    // Partial messages older than the reassembly timeout are dropped.
    uint32 const now = NowMs();
    for (auto& [account, link] : _links)
    {
        for (auto itr = link.partials.begin(); itr != link.partials.end();)
        {
            if (now - itr->second.firstAtMs > PartialTimeoutMs)
            {
                link.partialOrder.erase(std::remove(link.partialOrder.begin(), link.partialOrder.end(), itr->first), link.partialOrder.end());
                itr = link.partials.erase(itr);
            }
            else
                ++itr;
        }
    }

    _deltaTimer += diff;
    if (_deltaTimer >= DeltaIntervalMs)
    {
        _deltaTimer = 0;
        PushBotDeltas();
    }
}

void GadvProtocol::HandleChunk(uint32 accountId, ObjectGuid playerGuid, std::string const& text)
{
    std::optional<Gadv::Chunk> chunk = Gadv::ParseChunk(text);
    if (!chunk || chunk->direction != 'C')
    {
        if (GetSettings().debug)
            LOG_INFO("module", "mod-guild-adventure: GADV dropped malformed chunk from account {}: {}", accountId, text);
        return;
    }

    Link& link = _links[accountId];
    std::string encoded;
    if (chunk->total == 1)
        encoded = std::move(chunk->body);
    else
    {
        auto itr = link.partials.find(chunk->id);
        if (itr == link.partials.end())
        {
            if (link.partials.size() >= MaxPartials && !link.partialOrder.empty())
            {
                link.partials.erase(link.partialOrder.front());
                link.partialOrder.pop_front();
            }
            Partial partial;
            partial.total = chunk->total;
            partial.firstAtMs = NowMs();
            partial.parts.resize(chunk->total);
            itr = link.partials.emplace(chunk->id, std::move(partial)).first;
            link.partialOrder.push_back(chunk->id);
        }

        Partial& partial = itr->second;
        if (partial.total != chunk->total)
            return;
        if (!partial.parts[chunk->seq - 1])
        {
            partial.parts[chunk->seq - 1] = std::move(chunk->body);
            ++partial.received;
        }
        if (partial.received < partial.total)
            return;

        for (std::optional<std::string> const& part : partial.parts)
            encoded += *part;
        link.partialOrder.erase(std::remove(link.partialOrder.begin(), link.partialOrder.end(), itr->first), link.partialOrder.end());
        link.partials.erase(itr);
    }

    if (!CheckRate(link, accountId))
        return;

    std::optional<Message> message = Gadv::Decode(encoded);
    if (!message)
    {
        Error(accountId, std::nullopt, "BAD_REQUEST", "Malformed message.");
        return;
    }

    // The message counts only if it comes from the character the client plays right now.
    Player* player = GetClientPlayer(accountId);
    if (!player || player->GetGUID() != playerGuid)
        return;

    if (GetSettings().debug)
        LOG_INFO("module", "mod-guild-adventure: GADV <- account {}: {}", accountId, encoded);

    HandleMessage(accountId, player, *message);
}

bool GadvProtocol::CheckRate(Link& link, uint32 accountId)
{
    uint32 const now = NowMs();
    if (now - link.rateWindowStartMs >= IN_MILLISECONDS)
    {
        link.rateWindowStartMs = now;
        link.rateCount = 0;
    }

    if (++link.rateCount <= GetSettings().protocolMaxMsgPerSecond)
        return true;

    if (now - link.lastRateErrorMs >= IN_MILLISECONDS)
    {
        link.lastRateErrorMs = now;
        Error(accountId, std::nullopt, "RATE_LIMIT", "Too many messages; some were dropped.");
    }
    return false;
}

void GadvProtocol::HandleMessage(uint32 accountId, Player* player, Message const& message)
{
    std::string const& op = message.Opcode();

    // The owner's session may play the leader or one of the owner's guild bots.
    AdventureGuild* guild = sGuildAdventureMgr.FindGuild(accountId);
    bool const isLeader = guild && guild->leader == player->GetGUID();
    bool const isOwnBot = guild && guild->FindBot(player->GetGUID());

    if (op == "HELLO")
    {
        OnHello(accountId, player, (isLeader || isOwnBot) ? guild : nullptr, message);
        return;
    }

    Link* link = FindConnectedLink(accountId);
    if (!link || link->player != player->GetGUID())
        return; // intents before HELLO are ignored; the addon sends HELLO first

    if (!guild || !(isLeader || isOwnBot))
    {
        Error(accountId, RequestId(message), "NOT_OWNER", "This character does not lead an adventure guild.");
        return;
    }

    if (op == "GET")
        OnGet(accountId, player, *guild, message);
    else if (op == "ORDER_SET")
        OnOrderSet(accountId, *guild, message);
    else if (op == "BOT_STATE")
        OnBotState(accountId, *guild, message);
    else if (op == "PROF_SET")
        OnProfSet(accountId, *guild, message);
    else if (op == "RECRUIT")
        OnRecruit(accountId, *guild, message);
    else if (op == "CONTROL_TAKE")
        OnControl(accountId, player, *guild, message, true);
    else if (op == "CONTROL_RETURN")
        OnControl(accountId, player, *guild, message, false);
    else if (op == "CONTROL_WATCH")
        OnControlWatch(accountId, player, *guild, message);
    else if (op == "UNLOCK_BUY")
        OnUnlockBuy(accountId, *guild, message);
    else if (op == "AWAY_ACK")
        sOfflineService.AcknowledgeSummary(accountId);
    else if (op == "SPEC_SET")
        OnSpecSet(accountId, *guild, message);
    else if (op == "SQUAD_SET")
        OnSquadSet(accountId, *guild, message);
    else if (op == "ROLE_SET")
        OnRoleSet(accountId, *guild, message);
    else if (op == "FIGHT_ASSIGN")
        OnFightAssign(accountId, *guild, message);
    else if (op == "FIGHT_OPTION")
        OnFightOption(accountId, *guild, message);
    else if (op == "FIGHT_COPY")
        OnFightCopy(accountId, *guild, message);
    else if (op == "FIGHT_SAVE")
        OnFightSave(accountId, *guild, message);
    else if (op == "FIGHT_DEL")
        OnFightDelete(accountId, *guild, message);
    else if (op == "DUNGEON_START")
        OnDungeonStart(accountId, *guild, message);
    else if (op == "DUNGEON_STOP")
        OnDungeonStop(accountId, *guild, message);
    else if (op == "SPEC_SAVE")
        OnSpecSave(accountId, player, *guild, message);
    else if (op == "SPEC_DEL")
        OnSpecDelete(accountId, *guild, message);
    else if (op == "STORAGE_TAKE")
        OnStorageTake(accountId, player, message);
    else if (op == "STORAGE_DEPOSIT")
        OnStorageDeposit(accountId, player, message);
    else if (op == "STORAGE_SELL")
        OnStorageSell(accountId, player, message);
    else if (op == "STORAGE_DELETE")
        OnStorageDelete(accountId, player, message);
    else if (op == "STORAGE_RESERVE")
        OnStorageReserve(accountId, player, message);
    else if (op == "STORAGE_DISENCHANT")
        OnStorageDisenchant(accountId, player, message);
    else if (op == "STORAGE_SELL_JUNK")
        OnStorageSellJunk(accountId, player, message);
    else if (op == "LOOT_RULE_SET")
        OnLootRuleSet(accountId, message);
    else if (op == "LOOT_RULE_DEL")
        OnLootRuleDelete(accountId, message);
    else if (op == "LOOT_RULE_MOVE")
        OnLootRuleMove(accountId, message);
    else if (op == "LOOT_RULE_ENABLE")
        OnLootRuleEnable(accountId, message);
    else if (op == "LOOT_OPTIONS")
        OnLootOptions(accountId, message);
    else if (op == "CRAFT")
        OnCraft(accountId, player, *guild, message);
    else if (op == "AUCTION_RULE_SET")
        OnAuctionRuleSet(accountId, message);
    else if (op == "AUCTION_RULE_DEL")
        OnAuctionRuleDelete(accountId, message);
    else if (op == "AUCTION_CAT_SET")
        OnAuctionCategorySet(accountId, message);
    else if (op == "AUCTION_CAT_DEL")
        OnAuctionCategoryDelete(accountId, message);
    else if (op == "PERK_BUY")
        OnPerkBuy(accountId, *guild, message);
    else if (op == "CRAFT_ORDER_ADD")
        OnCraftOrderAdd(accountId, *guild, message);
    else if (op == "CRAFT_ORDER_DEL")
        OnCraftOrderDelete(accountId, *guild, message);
    else if (op == "SETTING_SET")
        OnSettingSet(accountId, *guild, message);
    else if (op == "FARM_SEARCH")
        OnFarmSearch(accountId, *guild, message);
    else if (op == "SHOP_SET")
        OnShoppingSet(accountId, *guild, message);
    else if (op == "SHOP_DEL")
        OnShoppingDelete(accountId, message);
    else if (op == "SHOP_SEARCH")
        OnShoppingSearch(accountId, *guild, message);
    // Unknown opcodes are ignored (docs/protocol.md section 4).
}

// ---------------------------------------------------------------------------------------------------------------
// Intents
// ---------------------------------------------------------------------------------------------------------------

void GadvProtocol::OnHello(uint32 accountId, Player* player, AdventureGuild* guild, Message const& message)
{
    Link& link = _links[accountId];
    link.connected = false;
    link.player = player->GetGUID();
    link.sentBots.clear();
    // What the client knows of the dungeons and of their runs is unknown again: the sync sends both. (A block pushed
    // while the client sat in a loading screen never reached it: after the owner's first finished run the Dungeons tab
    // still showed the next dungeon as locked, the list had gone out while he was taken out of the dungeon.)
    link.sentDungeonRuns.clear();
    link.sentDungeonProgress = std::numeric_limits<uint32>::max();

    if (message.GetInt("v").value_or(0) < Gadv::MinClientVersion)
    {
        SendTo(player, link, Message("ERROR").Set("code", "BAD_VERSION")
            .Set("msg", Acore::StringFormat("This server needs GuildAdventure protocol {} or newer.", Gadv::MinClientVersion)));
        return;
    }

    if (!guild)
    {
        // Not the leader (another character of the owner's account): WELCOME so the addon knows the server, then
        // the reason it cannot show a guild.
        SendTo(player, link, Message("WELCOME").Set("v", Gadv::ProtocolVersion).Set("min", Gadv::MinClientVersion)
            .Set("srv", ModuleVersion).Set("now", Now()).Set("zv", sGuildAdventureMgr.GetZoneVersion()));
        std::string leaderName;
        if (AdventureGuild* owned = sGuildAdventureMgr.FindGuild(accountId))
            sCharacterCache->GetCharacterNameByGuid(owned->leader, leaderName);
        SendTo(player, link, Message("ERROR").Set("code", "NOT_OWNER").Set("msg", leaderName.empty() ?
            std::string("This character does not lead an adventure guild.") :
            Acore::StringFormat("Your guild leader is {}; log in with {} to manage the guild.", leaderName, leaderName)));
        return;
    }

    link.connected = true;
    link.helloSettingsVersion = message.GetInt("sv").value_or(0);
    SendSync(accountId, player, *guild, message.GetInt("zv").value_or(0));
}

void GadvProtocol::OnGet(uint32 accountId, Player* player, AdventureGuild& guild, Message const& message)
{
    std::optional<int64> const r = RequestId(message);
    std::string const what = message.GetString("what");

    if (what == "state")
        Send(accountId, BuildState(guild));
    else if (what == "control")
        Send(accountId, BuildControlState(guild, player, sControlSwapService.GetSwapPhase(accountId).has_value()));
    else if (what == "roster")
        SendRoster(accountId, guild);
    else if (what == "leader")
        PushLeader(guild, true);
    else if (what == "bot")
    {
        std::optional<int64> const g = message.GetInt("g");
        ObjectGuid const guid = g ? ObjectGuid::Create<HighGuid::Player>(uint32(*g)) : ObjectGuid::Empty;
        if (!g || !guild.FindBot(guid))
        {
            Error(accountId, r, "UNKNOWN_BOT", "No such guild bot.");
            return;
        }
        PushBot(guild, guid, true);
    }
    else if (what == "zones")
        SendZones(accountId);
    else if (what == "unlocks")
        SendUnlocks(accountId, guild);
    else if (what == "perks")
        SendPerks(accountId, guild);
    else if (what == "milestones")
        SendMilestones(accountId, guild);
    else if (what == "dungeons")
        SendDungeons(accountId);
    else if (what == "rolegear")
        SendRoleGear(accountId, guild, message);
    else if (what == "plan")
    {
        // What a guild character (a bot, or the leader with an order) does now and next: the control bar's Details window.
        std::optional<int64> const g = message.GetInt("g");
        ObjectGuid const guid = g ? ObjectGuid::Create<HighGuid::Player>(uint32(*g)) : ObjectGuid::Empty;
        if (!g || (!guild.FindBot(guid) && guid != guild.leader))
        {
            Error(accountId, r, "UNKNOWN_BOT", "No such guild character.");
            return;
        }
        SendPlan(accountId, guid);
    }
    else if (what == "fightprofiles" || what == "fightprofile" || what == "fightcatalog")
    {
        std::optional<int64> const g = message.GetInt("g");
        ObjectGuid const guid = g ? ObjectGuid::Create<HighGuid::Player>(uint32(*g)) : ObjectGuid::Empty;
        if (!g || !guild.FindBot(guid))
        {
            Error(accountId, r, "UNKNOWN_BOT", "No such guild bot.");
            return;
        }
        if (what == "fightprofiles")
            SendFightProfiles(accountId, guild, guid);
        else if (what == "fightcatalog")
            SendFightCatalog(accountId, guild, guid);
        else
            SendFightProfile(accountId, guild, guid, uint32(std::max<int64>(0, message.GetInt("id").value_or(0))));
    }
    else if (what == "dungeonruns")
        SendDungeonRuns(accountId, guild, true);
    else if (what == "dungeonprep")
        SendDungeonPrep(accountId, guild);
    else if (what == "craftorders")
        SendCraftOrders(accountId, guild);
    else if (what == "craftables")
        SendCraftables(accountId, guild);
    else if (what == "events")
        SendEvents(accountId, guild);
    else if (what == "storage")
        SendStorage(accountId);
    else if (what == "loot")
        SendLoot(accountId);
    else if (what == "lootlog")
        SendLootLog(accountId);
    else if (what == "auctions")
        SendAuctions(accountId);
    else if (what == "shopping")
        SendShopping(accountId);
    else if (what == "settings")
        SendSettings(accountId, guild, -1); // the schema always, then the values
    else if (what == "recipes")
    {
        // The character the owner plays, or one of its guild bots in the world (g).
        Player* crafter = player;
        if (std::optional<int64> const g = message.GetInt("g"))
        {
            ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(uint32(*g));
            crafter = guid == player->GetGUID() ? player : (guild.FindBot(guid) ? ObjectAccessor::FindPlayer(guid) : nullptr);
        }
        if (!crafter || !crafter->IsInWorld())
        {
            Error(accountId, r, "UNKNOWN_BOT", "That guild character is not in the world.");
            return;
        }
        SendRecipes(accountId, crafter);
    }
    else
    {
        Error(accountId, r, "BAD_REQUEST", "Unknown GET target.");
        return;
    }

    Ack(accountId, r);
}

// GET what=plan g: block PLAN, what a guild character does now and what comes next (OrderService::DescribeNext). One
// record PL per line: k the kind (now, task, squad, wait, run, errand, hand, quest, upkeep, aside), t the text.
void GadvProtocol::SendPlan(uint32 accountId, ObjectGuid guid)
{
    std::vector<Message> records;
    for (auto const& [kind, text] : sOrderService.DescribeNext(guid))
        records.push_back(Message("PL").Set("k", kind).Set("t", text));
    SendBlock(accountId, "PLAN", records, Message("PLAN_BEGIN").Set("g", guid.GetCounter()).Set("at", Now()));
}

void GadvProtocol::OnOrderSet(uint32 accountId, AdventureGuild& guild, Message const& message)
{
    std::optional<int64> const r = RequestId(message);
    std::optional<int64> const g = message.GetInt("g");
    ObjectGuid const guid = g ? ObjectGuid::Create<HighGuid::Player>(uint32(*g)) : ObjectGuid::Empty;
    // The leader takes orders too (docs/orders.md "The leader"); its record is leaderPlan, also while it holds.
    bool const isLeader = g && guid == guild.leader;
    GuildBot* bot = isLeader ? &guild.leaderPlan : (g ? guild.FindBot(guid) : nullptr);
    if (!bot)
    {
        Error(accountId, r, g ? "NOT_OWNER" : "BAD_REQUEST", "That character is not one of your guild bots.");
        return;
    }

    if (!sProgressionService.HasFeature(guild, "orders"))
    {
        Error(accountId, r, "LOCKED", "Orders are not unlocked yet.");
        return;
    }

    // HOLD: the leader's default, no order (it holds its position while the AI holds it). Bots always have an order.
    if (message.GetString("o") == "HOLD")
    {
        if (!isLeader)
        {
            Error(accountId, r, "BAD_REQUEST", "HOLD is the leader's order only.");
            return;
        }
        sOrderService.SetLeaderOrder(guild, std::nullopt);
        sControlSwapService.EndWatchOf(guild.leader);
        PushLeader(guild, false);
        RecordEvent(guild, "ORDER", guid, Acore::StringFormat("{} holds its position again (no order).", bot->name));
        Ack(accountId, r);
        return;
    }

    std::optional<OrderType> const type = OrderTypeFromToken(message.GetString("o"));
    if (!type)
    {
        Error(accountId, r, "BAD_REQUEST", "Unknown order.");
        return;
    }

    if (!IsOrderImplemented(*type))
    {
        Error(accountId, r, "NOT_AVAILABLE", Acore::StringFormat("The order {} is not available on this server yet.", ToToken(*type)));
        return;
    }

    BotOrder order = bot->order;
    order.type = *type;
    order.zoneId = 0;
    // A dungeon run: zone = the dungeon's id (Readiness.h); the planner says whether this bot may lead one there.
    if (*type == OrderType::Dungeon)
    {
        std::optional<int64> const dungeonId = message.GetInt("zone");
        std::string why;
        std::string need;
        if (dungeonId && *dungeonId > 0 && !sOrderService.IsDungeonUnlocked(guild, uint32(*dungeonId), &need))
        {
            Error(accountId, r, "DUNGEON_INVALID", "No dungeon run: that dungeon is not open yet, the guild finishes " + need + " first.");
            return;
        }
        if (!dungeonId || *dungeonId <= 0 || !sOrderService.CheckDungeonOrder(guild, *bot, uint32(*dungeonId), why) ||
            !sOrderService.CheckDungeonReady(guild, *bot, uint32(*dungeonId), why))
        {
            Error(accountId, r, "DUNGEON_INVALID", why.empty() ? "Pick a dungeon." : "No dungeon run: " + why + ".");
            return;
        }
        order.zoneId = uint32(*dungeonId);
    }
    // A farm order: zone = the item (Farming.h); it must be something creatures bots can grind drop for this side.
    if (*type == OrderType::Farm)
    {
        std::optional<int64> const item = message.GetInt("zone");
        std::string why;
        if (!item || *item <= 0 || !sOrderService.CheckFarmOrder(guild, uint32(*item), why))
        {
            Error(accountId, r, "FARM_INVALID", why.empty() ? "Pick an item to farm." : why + ".");
            return;
        }
        // n: the target stock (the order ends when the guild storage holds that many), 0 or missing = farm on and on.
        std::optional<int64> const target = message.GetInt("n");
        if (target && (*target < 0 || *target > MaxFarmTarget))
        {
            Error(accountId, r, "BAD_REQUEST", "Invalid target stock.");
            return;
        }
        order.zoneId = uint32(*item);
        order.farmTarget = target ? uint32(*target) : 0;
    }
    else
        order.farmTarget = 0;
    // How often the dungeon is run (1 = once); any other order ends a repeated one.
    // (runs: 0 = run after run until it is stopped.)
    uint32 const dungeonRuns = *type != OrderType::Dungeon ? 1 : message.GetInt("runs").value_or(1) == 0 ?
        OrderService::DungeonRunsEndless :
        uint32(std::clamp<int64>(message.GetInt("runs").value_or(1), 1, int64(GetSettings().dungeonsMaxRuns)));
    if (OrderNeedsZone(*type))
    {
        std::optional<int64> const zoneId = message.GetInt("zone");
        ZoneTemplate const* zone = zoneId ? sGuildAdventureMgr.GetZone(uint32(*zoneId)) : nullptr;
        if (!zone || !zone->AllowsTeam(guild.team) || !zone->AllowsOrder(*type))
        {
            Error(accountId, r, "ZONE_INVALID", "That zone is unknown, of the other faction or does not allow this order.");
            return;
        }
        order.zoneId = zone->zoneId;
    }

    // AUTO limits only with the auto_limits feature; without it the defaults stay (docs/protocol.md section 6).
    if (*type == OrderType::Auto && sProgressionService.HasFeature(guild, "auto_limits"))
    {
        if (std::optional<int64> const lc = message.GetInt("lc"))
        {
            if (*lc < 0 || *lc > int64(sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL)))
            {
                Error(accountId, r, "BAD_REQUEST", "Invalid level cap.");
                return;
            }
            order.levelCap = uint8(*lc);
        }
        if (std::optional<int64> const cont = message.GetInt("cont"))
        {
            if (!Contains(Continents, *cont))
            {
                Error(accountId, r, "BAD_REQUEST", "Invalid continent.");
                return;
            }
            order.continent = int16(*cont);
        }
        if (std::optional<bool> const nd = message.GetBool("nd"))
            order.noDungeons = *nd;
        if (std::optional<int64> const gcap = message.GetInt("gcap"))
        {
            if (*gcap < 0)
            {
                Error(accountId, r, "BAD_REQUEST", "Invalid gold limit.");
                return;
            }
            order.goldCap = uint64(*gcap);
        }
    }

    std::optional<bool> const autoAdvance = message.GetBool("aa");
    if (message.Has("aa") && !autoAdvance)
    {
        Error(accountId, r, "BAD_REQUEST", "Invalid auto-advance flag.");
        return;
    }

    if (isLeader)
    {
        // The leader has no ga_bot row: auto-advance stays on for it.
        sOrderService.SetLeaderOrder(guild, order);
        PushLeader(guild, false);
        RecordEvent(guild, "ORDER", guid, Acore::StringFormat("{}: new order {}.", bot->name, OrderDescription(order)), 0, order.zoneId);
        Ack(accountId, r);
        return;
    }

    order.setAt = Now();
    bot->order = order;
    sGuildAdventureMgr.SaveOrder(*bot);
    sOrderService.SetDungeonRuns(guid, dungeonRuns);
    if (autoAdvance && *autoAdvance != bot->autoAdvance)
    {
        bot->autoAdvance = *autoAdvance;
        sGuildAdventureMgr.SaveBot(guild, *bot);
    }
    sOrderService.OnOrderChanged(guild, *bot);

    PushBot(guild, guid, false);
    RecordEvent(guild, "ORDER", guid, Acore::StringFormat("{}: new order {}.", bot->name, OrderDescription(order)), 0, order.zoneId);
    Ack(accountId, r);
}

void GadvProtocol::OnBotState(uint32 accountId, AdventureGuild& guild, Message const& message)
{
    std::optional<int64> const r = RequestId(message);
    std::optional<int64> const g = message.GetInt("g");
    std::string const s = message.GetString("s");
    if (!g || (s != "active" && s != "benched"))
    {
        Error(accountId, r, "BAD_REQUEST", "BOT_STATE needs g and s = active or benched.");
        return;
    }

    ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(uint32(*g));
    RosterResult const result = sRosterService.SetBotState(guild, guid, s == "benched" ? BotState::Benched : BotState::Active);
    if (!result)
    {
        Error(accountId, r, result.error == RosterError::UnknownBot ? "NOT_OWNER" : ToToken(result.error), result.message);
        return;
    }

    PushBot(guild, guid, false);
    PushState(guild);
    Ack(accountId, r);
}

void GadvProtocol::OnProfSet(uint32 accountId, AdventureGuild& guild, Message const& message)
{
    std::optional<int64> const r = RequestId(message);
    std::optional<int64> const g = message.GetInt("g");
    ObjectGuid const guid = g ? ObjectGuid::Create<HighGuid::Player>(uint32(*g)) : ObjectGuid::Empty;
    GuildBot* bot = g ? guild.FindBot(guid) : nullptr;
    if (!bot)
    {
        Error(accountId, r, g ? "NOT_OWNER" : "BAD_REQUEST", "That character is not one of your guild bots.");
        return;
    }

    if (!sProgressionService.HasFeature(guild, "professions"))
    {
        Error(accountId, r, "LOCKED", "Profession plans are not unlocked yet.");
        return;
    }

    ProfessionPlan plan = bot->professions;
    char const* primaryKeys[2] = { "p1", "p2" };
    char const* targetKeys[2] = { "t1", "t2" };
    for (int i = 0; i < 2; ++i)
    {
        if (std::optional<int64> const p = message.GetInt(primaryKeys[i]))
        {
            if (*p != 0 && !Contains(PrimarySkills, uint16(*p)))
            {
                Error(accountId, r, "BAD_REQUEST", "Unknown primary profession.");
                return;
            }
            plan.primary[i] = uint16(*p);
        }
        if (std::optional<int64> const t = message.GetInt(targetKeys[i]))
        {
            if (!Contains(TargetSkills, uint16(*t)))
            {
                Error(accountId, r, "BAD_REQUEST", "Invalid target skill.");
                return;
            }
            plan.target[i] = uint16(*t);
        }
        if (!plan.primary[i])
            plan.target[i] = 0;
    }

    if (plan.primary[0] && plan.primary[0] == plan.primary[1])
    {
        Error(accountId, r, "BAD_REQUEST", "The two primary professions must differ.");
        return;
    }

    if (message.Has("sec"))
    {
        std::optional<std::vector<int64>> const secondary = message.GetIntList("sec");
        if (!secondary)
        {
            Error(accountId, r, "BAD_REQUEST", "Invalid secondary profession list.");
            return;
        }
        plan.secondary.clear();
        for (int64 skill : *secondary)
        {
            if (!Contains(SecondarySkills, uint16(skill)))
            {
                Error(accountId, r, "BAD_REQUEST", "Unknown secondary profession.");
                return;
            }
            if (!Contains(plan.secondary, uint16(skill)))
                plan.secondary.push_back(uint16(skill));
        }
    }

    bot->professions = plan;
    sGuildAdventureMgr.SaveProfessions(*bot);
    PushBot(guild, guid, false);
    Ack(accountId, r);
}

void GadvProtocol::OnRecruit(uint32 accountId, AdventureGuild& guild, Message const& message)
{
    std::optional<int64> const r = RequestId(message);
    std::optional<int64> const race = message.GetInt("race");
    std::optional<int64> const classId = message.GetInt("class");
    std::optional<int64> const gender = message.GetInt("gender");
    std::string const name = message.GetString("name");
    if (!race || !classId || !gender || name.empty() || *race < 1 || *race > 255 || *classId < 1 || *classId > 255 ||
        *gender < 0 || *gender > 1)
    {
        Error(accountId, r, "BAD_REQUEST", "RECRUIT needs race, class, gender and name.");
        return;
    }

    RecruitRequest request;
    request.race = uint8(*race);
    request.classId = uint8(*classId);
    request.gender = uint8(*gender);
    request.name = name;

    uint32 const owner = guild.ownerAccount;
    RosterResult const started = sRosterService.Recruit(guild, request, [this, accountId, owner, r](RosterResult const& result)
    {
        AdventureGuild* stored = sGuildAdventureMgr.FindGuild(owner);
        if (!result || !stored)
        {
            Error(accountId, r, ToToken(result.error), result.message);
            return;
        }
        // BOT (new) and STATE first, then ACK; the RECRUIT event was pushed by RecordEvent already.
        PushBot(*stored, result.bot, true);
        PushState(*stored);
        Ack(accountId, r);
    });

    if (!started)
        Error(accountId, r, ToToken(started.error), started.message);
}

void GadvProtocol::OnControl(uint32 accountId, Player* player, AdventureGuild& guild, Message const& message, bool take)
{
    std::optional<int64> const r = RequestId(message);

    ObjectGuid target = guild.leader;
    if (take)
    {
        std::optional<int64> const g = message.GetInt("g");
        if (!g)
        {
            Error(accountId, r, "BAD_REQUEST", "CONTROL_TAKE needs g.");
            return;
        }
        target = ObjectGuid::Create<HighGuid::Player>(uint32(*g));
        GuildBot const* bot = guild.FindBot(target);
        if (!bot)
        {
            Error(accountId, r, "SWAP_REFUSED", "That character is not one of your guild bots.", "NOT_OWNER");
            return;
        }
        if (bot->state == BotState::Benched)
        {
            Error(accountId, r, "SWAP_REFUSED", Acore::StringFormat("{} is benched.", bot->name), "BENCHED");
            return;
        }
    }

    SwapRequestResult const result = sControlSwapService.RequestSwap(player->GetSession(), target);
    if (!result)
    {
        Error(accountId, r, "SWAP_REFUSED", result.message, ToString(result.refusal));
        return;
    }

    // docs/protocol.md section 9: busy (with the client path) first, ACK when the swap starts; Path R then sends
    // CONTROL_PENDING right before the client leaves the world (LeavingWorld) and the result is the new
    // character's HELLO sync; Paths W and S push CONTROL_STATE and the BOT deltas on completion.
    Send(accountId, BuildControlState(guild, player, true).Set("path", ToString(result.path))); // relog, reload, switch
    Ack(accountId, r);
}

void GadvProtocol::OnControlWatch(uint32 accountId, Player* player, AdventureGuild& guild, Message const& message)
{
    std::optional<int64> const r = RequestId(message);
    std::optional<bool> const on = message.GetBool("on");
    if (!on)
    {
        Error(accountId, r, "BAD_REQUEST", "CONTROL_WATCH needs on.");
        return;
    }

    // docs/protocol.md section 9 "Watch mode": the WatchChanged event pushes CONTROL_STATE (watch) and the BOT record
    // first, then the ACK follows. A request for the state the character already has is acknowledged with the same record.
    bool const was = sGuildBotHolder.IsWatched(player->GetGUID());
    SwapRequestResult const result = sControlSwapService.RequestWatch(player->GetSession(), *on);
    if (!result)
    {
        Error(accountId, r, "WATCH_REFUSED", result.message, ToString(result.refusal));
        return;
    }

    if (was == *on)
        Send(accountId, BuildControlState(guild, player, sControlSwapService.GetSwapPhase(accountId).has_value()));
    Ack(accountId, r);
}

void GadvProtocol::OnUnlockBuy(uint32 accountId, AdventureGuild& guild, Message const& message)
{
    std::optional<int64> const r = RequestId(message);
    std::optional<int64> const id = message.GetInt("id");
    UnlockTemplate const* unlock = id ? sGuildAdventureMgr.GetUnlock(uint32(*id)) : nullptr;
    if (!unlock)
    {
        Error(accountId, r, "BAD_REQUEST", "Unknown unlock.");
        return;
    }

    // BuyUnlock pushes STATE (slots, bank, ulgold), the UNLOCKS block (covered tiers change too) and the UNLOCK event
    // before the ACK.
    UnlockBuyResult const result = sProgressionService.BuyUnlock(guild, *unlock, "the owner in the Progression tab");
    if (!result.ok)
    {
        Error(accountId, r, result.code, result.message);
        return;
    }
    Ack(accountId, r);
}

// ---------------------------------------------------------------------------------------------------------------
// Swaps
// ---------------------------------------------------------------------------------------------------------------

void GadvProtocol::OnSwapEvent(SwapEvent const& event)
{
    AdventureGuild* guild = sGuildAdventureMgr.FindGuild(event.ownerAccountId);
    if (!guild)
        return;

    auto nameOf = [](ObjectGuid guid)
    {
        std::string name;
        if (guid.IsEmpty() || !sCharacterCache->GetCharacterNameByGuid(guid, name))
            name = "?";
        return name;
    };

    switch (event.type)
    {
        case SwapEventType::Started:
            break;
        case SwapEventType::LeavingWorld:
            // Last message the addon gets before the UI unloads: which character to enter (GlueXML auto-enter).
            Send(event.ownerAccountId, Message("CONTROL_PENDING").Set("g", event.to.GetCounter()).Set("cn", nameOf(event.to)));
            if (Link* link = FindConnectedLink(event.ownerAccountId))
                link->connected = false;
            break;
        case SwapEventType::Completed:
        {
            bool const toLeader = event.to == guild->leader;
            RecordEvent(*guild, "CONTROL", toLeader ? ObjectGuid::Empty : event.to, toLeader ?
                Acore::StringFormat("You lead the guild as {} again.", nameOf(event.to)) :
                Acore::StringFormat("You took control of {}.", nameOf(event.to)));
            // Paths W and S keep the addon loaded: the link follows the new character at once, which learns
            // about it from CONTROL_STATE and the BOT deltas and then sends HELLO again after
            // PLAYER_ENTERING_WORLD. Path R reloaded the UI (the link was closed at LeavingWorld); the new
            // character's HELLO sync carries the result.
            Player* player = GetClientPlayer(event.ownerAccountId);
            Link* link = FindConnectedLink(event.ownerAccountId);
            if (player && link)
            {
                link->player = player->GetGUID();
                SendTo(player, *link, BuildControlState(*guild, player, false));
                for (auto const& [guid, bot] : guild->bots)
                    PushBot(*guild, guid, false);
                PushLeader(*guild, false);
            }
            break;
        }
        case SwapEventType::Failed:
        {
            RecordEvent(*guild, "INFO", ObjectGuid::Empty, event.message.empty() ?
                Acore::StringFormat("Switching to {} failed ({}).", nameOf(event.to), ToString(event.refusal)) :
                Acore::StringFormat("Switching to {} failed: {}", nameOf(event.to), event.message));
            if (Player* player = GetClientPlayer(event.ownerAccountId))
                Send(event.ownerAccountId, BuildControlState(*guild, player, false));
            break;
        }
        case SwapEventType::WatchChanged:
        {
            // Watch mode went on or off: the control bar follows CONTROL_STATE^watch, the roster the BOT record (its
            // why line). No event feed entry: the owner toggles this at will.
            Player* player = GetClientPlayer(event.ownerAccountId);
            if (player && player->GetGUID() == event.from)
                Send(event.ownerAccountId, BuildControlState(*guild, player,
                    sControlSwapService.GetSwapPhase(event.ownerAccountId).has_value()));
            if (guild->FindBot(event.from))
                PushBot(*guild, event.from, false);
            else if (event.from == guild->leader)
                PushLeader(*guild, false);
            break;
        }
        case SwapEventType::ReturnedToAI:
            RecordEvent(*guild, "INFO", event.from, Acore::StringFormat("{} keeps playing as a guild bot.", nameOf(event.from)));
            // The owner logged out or lost the connection while playing that character. The core's logout hooks do not
            // run for a detached player (core patch CP-1), so the link is closed here; otherwise it would stay
            // "connected" and bot deltas would be built for an owner who is gone.
            OnPlayerLogout(event.ownerAccountId, event.from);
            break;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Send path
// ---------------------------------------------------------------------------------------------------------------

Player* GadvProtocol::GetClientPlayer(uint32 accountId) const
{
    WorldSession* session = sWorldSessionMgr->FindSession(accountId);
    Player* player = session ? session->GetPlayer() : nullptr;
    return player && player->IsInWorld() ? player : nullptr;
}

GadvProtocol::Link* GadvProtocol::FindConnectedLink(uint32 accountId)
{
    auto itr = _links.find(accountId);
    if (itr == _links.end() || !itr->second.connected)
        return nullptr;
    return &itr->second;
}

void GadvProtocol::SendTo(Player* player, Link& link, Message const& message)
{
    std::string const encoded = Gadv::Encode(message);
    link.nextOutId = link.nextOutId % Gadv::MaxMessageId + 1;
    std::vector<std::string> const chunks = Gadv::SplitIntoChunks(encoded, 'S', link.nextOutId);
    if (chunks.empty())
    {
        LOG_ERROR("module", "mod-guild-adventure: GADV message {} needs more than {} chunks, not sent", message.Opcode(), Gadv::MaxChunks);
        return;
    }

    if (GetSettings().debug)
        LOG_INFO("module", "mod-guild-adventure: GADV -> {}: {}", player->GetName(), encoded);

    for (std::string const& chunk : chunks)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, PrefixTab + chunk);
        player->SendDirectMessage(&data);
    }
}

void GadvProtocol::Send(uint32 accountId, Message const& message)
{
    Player* player = GetClientPlayer(accountId);
    if (!player)
        return;
    SendTo(player, _links[accountId], message);
}

void GadvProtocol::Ack(uint32 accountId, std::optional<int64> request)
{
    if (request)
        Send(accountId, Message("ACK").Set("r", *request));
}

void GadvProtocol::Error(uint32 accountId, std::optional<int64> request, std::string const& code, std::string const& text,
    std::string const& reason)
{
    Message message("ERROR");
    if (request)
        message.Set("r", *request);
    message.Set("code", code.empty() ? std::string("INTERNAL") : code);
    if (!reason.empty())
        message.Set("reason", reason);
    if (!text.empty())
        message.Set("msg", text);
    Send(accountId, message);
}

void GadvProtocol::SendBlock(uint32 accountId, std::string const& name, std::vector<Message> const& records, Message begin)
{
    begin.Set("n", uint32(records.size()));
    Send(accountId, begin);
    for (Message const& record : records)
        Send(accountId, record);
    Send(accountId, Message(name + "_END"));
}

void GadvProtocol::SendSync(uint32 accountId, Player* player, AdventureGuild& guild, int64 clientZoneVersion)
{
    // docs/protocol.md section 5, in this order.
    Send(accountId, Message("WELCOME").Set("v", Gadv::ProtocolVersion).Set("min", Gadv::MinClientVersion)
        .Set("srv", ModuleVersion).Set("now", Now()).Set("zv", sGuildAdventureMgr.GetZoneVersion()));
    Send(accountId, BuildState(guild));
    Send(accountId, BuildControlState(guild, player, sControlSwapService.GetSwapPhase(accountId).has_value()));
    if (clientZoneVersion != int64(sGuildAdventureMgr.GetZoneVersion()))
        SendZones(accountId);
    SendUnlocks(accountId, guild);
    SendPerks(accountId, guild);
    SendMilestones(accountId, guild);
    SendRoster(accountId, guild);
    SendDungeonRuns(accountId, guild, true);
    PushLeader(guild, true);
    SendEvents(accountId, guild);
    SendAway(accountId);
    SendSpecs(accountId);
    SendSettings(accountId, guild, _links[accountId].helloSettingsVersion);
    // Guild storage and loot filter (docs/protocol.md §5): last, they can be long.
    SendLoot(accountId);
    SendStorage(accountId);
    SendAuctions(accountId);
    SendShopping(accountId);
}

void GadvProtocol::SendRoster(uint32 accountId, AdventureGuild const& guild)
{
    Link& link = _links[accountId];
    std::vector<Message> records;
    for (auto const& [guid, bot] : guild.bots)
    {
        FieldMap fields = BuildBotFields(guild, bot);
        Message record("BOT");
        for (auto const& [key, value] : fields)
            record.Set(key, value);
        records.push_back(std::move(record));
        link.sentBots[guid] = std::move(fields);
    }
    SendBlock(accountId, "ROSTER", records, Message("ROSTER_BEGIN"));
}

void GadvProtocol::SendDungeons(uint32 accountId)
{
    // The dungeons and raids the readiness numbers of the BOT records refer to (docs/protocol.md §7.20).
    // (For the owner's guild: the place of each dungeon in the order, how often the guild has finished it, and
    // whether it is still locked, docs/orders.md "The dungeons in their order".)
    AdventureGuild* guild = sGuildAdventureMgr.FindGuild(accountId);
    std::vector<uint32> const order = guild ? sOrderService.DungeonOrder(*guild) : std::vector<uint32>();
    if (guild)
        _links[accountId].sentDungeonProgress = sOrderService.GetDungeonProgressVersion(*guild);
    std::vector<Message> records;
    for (DungeonInfo const& dungeon : sReadiness.GetDungeons())
    {
        // (Not a dungeon in the other side's capital: Ragefire Chasm for an Alliance guild, the Stockade for a Horde
        // one. No run can be ordered into it, it is in no guild's order, and the run window must not offer it.)
        if (guild && dungeon.team < 2 && dungeon.team != uint8(guild->team))
            continue;
        DungeonPlan const* plan = sDungeons.GetPlan(dungeon.id);
        auto const place = std::find(order.begin(), order.end(), dungeon.id);
        std::string need;
        bool const locked = guild && !sOrderService.IsDungeonUnlocked(*guild, dungeon.id, &need);
        records.push_back(Message("DUNGEON")
            .Set("ord", place != order.end() ? uint32(place - order.begin()) + 1 : 0u)
            .Set("fin", guild ? sOrderService.GetDungeonsFinished(*guild, dungeon.id) : 0u)
            .Set("lock", locked)
            .Set("need", need)
            .Set("id", dungeon.id)
            .Set("map", dungeon.mapId)
            .Set("n", dungeon.name)
            .Set("min", uint32(dungeon.minLevel))
            .Set("max", uint32(dungeon.maxLevel))
            .Set("tgt", uint32(dungeon.targetLevel))
            .Set("size", uint32(dungeon.size))
            .Set("raid", dungeon.raid)
            .Set("gl", uint32(dungeon.gearLevel))
            // Whether a run can be ordered into it (Dungeons.h: entrance and bosses known, a map of its own).
            .Set("run", plan && plan->supported));
    }
    SendBlock(accountId, "DUNGEONS", records, Message("DUNGEONS_BEGIN"));
}

void GadvProtocol::SendZones(uint32 accountId)
{
    std::vector<Message> records;
    for (ZoneTemplate const& zone : sGuildAdventureMgr.GetZones())
    {
        std::string orders;
        for (OrderType type : zone.orders)
        {
            if (!orders.empty())
                orders += ',';
            orders += ToToken(type);
        }
        records.push_back(Message("ZONE").Set("id", zone.zoneId).Set("n", zone.name).Set("min", zone.minLevel)
            .Set("max", zone.maxLevel).Set("fac", std::string(1, zone.faction)).Set("cont", zone.continent).Set("orders", orders));
    }
    SendBlock(accountId, "ZONES", records, Message("ZONES_BEGIN").Set("zv", sGuildAdventureMgr.GetZoneVersion()));
}

void GadvProtocol::SendUnlocks(uint32 accountId, AdventureGuild const& guild)
{
    std::vector<Message> records;
    for (UnlockTemplate const& unlock : sGuildAdventureMgr.GetUnlocks())
        records.push_back(Message("UNLOCK").Set("id", unlock.id).Set("lvl", unlock.renownLevel).Set("kind", ToToken(unlock.kind))
            .Set("val", unlock.value).Set("gold", unlock.goldCost).Set("st", ToToken(sProgressionService.GetUnlockState(guild, unlock)))
            .Set("desc", unlock.description));
    SendBlock(accountId, "UNLOCKS", records, Message("UNLOCKS_BEGIN"));
}

void GadvProtocol::SendEvents(uint32 accountId, AdventureGuild const& guild)
{
    std::vector<Message> records;
    for (GuildEvent const& event : guild.recentEvents)
        records.push_back(BuildEvent(event));
    SendBlock(accountId, "EVENTS", records, Message("EVENTS_BEGIN"));
}

void GadvProtocol::SendAway(uint32 accountId)
{
    AwaySummary const* summary = sOfflineService.GetPendingSummary(accountId);
    if (!summary)
        return;

    std::vector<Message> lines;
    for (auto const& [guid, text] : summary->lines)
        lines.push_back(Message("AWAY_LINE").Set("g", guid.GetCounter()).Set("msg", text));

    SendBlock(accountId, "AWAY", lines, Message("AWAY_BEGIN").Set("from", summary->from).Set("to", summary->to)
        .Set("lv", summary->levels).Set("rp", summary->renown).Set("gold", summary->goldEarned).Set("q", summary->quests)
        .Set("d", summary->deaths));
}

// ---------------------------------------------------------------------------------------------------------------
// Pushes
// ---------------------------------------------------------------------------------------------------------------

void GadvProtocol::PushEvent(AdventureGuild const& guild, GuildEvent const& event)
{
    if (FindConnectedLink(guild.ownerAccount))
        Send(guild.ownerAccount, BuildEvent(event));
}

void GadvProtocol::PushState(AdventureGuild const& guild)
{
    if (FindConnectedLink(guild.ownerAccount))
        Send(guild.ownerAccount, BuildState(guild));
}

void GadvProtocol::PushUnlocks(AdventureGuild const& guild)
{
    if (FindConnectedLink(guild.ownerAccount))
        SendUnlocks(guild.ownerAccount, guild);
}

void GadvProtocol::PushNotice(uint32 ownerAccount, std::string const& message, bool warning)
{
    if (FindConnectedLink(ownerAccount))
        Send(ownerAccount, Message("NOTICE").Set("msg", message).Set("kind", warning ? "warn" : "info"));
}

void GadvProtocol::PushBotRemoved(AdventureGuild const& guild, ObjectGuid bot)
{
    Link* link = FindConnectedLink(guild.ownerAccount);
    if (!link)
        return;
    link->sentBots.erase(bot);
    Send(guild.ownerAccount, Message("BOT_REMOVE").Set("g", bot.GetCounter()));
}

void GadvProtocol::PushBot(AdventureGuild const& guild, ObjectGuid guid, bool full)
{
    if (guid == guild.leader)
    {
        PushLeader(guild, full);
        return;
    }

    Link* link = FindConnectedLink(guild.ownerAccount);
    GuildBot const* bot = guild.FindBot(guid);
    if (!link || !bot)
        return;

    FieldMap fields = BuildBotFields(guild, *bot);
    FieldMap& sent = link->sentBots[guid];
    Message record("BOT");
    record.Set("g", guid.GetCounter());
    for (auto const& [key, value] : fields)
    {
        auto itr = sent.find(key);
        if (full || itr == sent.end() || itr->second != value)
            record.Set(key, value);
    }
    sent = std::move(fields);

    if (record.Fields().size() > 1)
        Send(guild.ownerAccount, record);
}

void GadvProtocol::PushBotDeltas()
{
    for (auto& [account, link] : _links)
    {
        if (!link.connected)
            continue;
        if (AdventureGuild* guild = sGuildAdventureMgr.FindGuild(account))
        {
            for (auto const& [guid, bot] : guild->bots)
                PushBot(*guild, guid, false);
            PushLeader(*guild, false);
            SendDungeonRuns(account, *guild, false);
        }
    }
}

void GadvProtocol::PushLeader(AdventureGuild const& guild, bool full)
{
    Link* link = FindConnectedLink(guild.ownerAccount);
    if (!link || guild.leader.IsEmpty())
        return;

    FieldMap fields = BuildLeaderFields(guild);
    Message record("LEADER");
    record.Set("g", guild.leader.GetCounter());
    for (auto const& [key, value] : fields)
    {
        auto itr = link->sentLeader.find(key);
        if (full || itr == link->sentLeader.end() || itr->second != value)
            record.Set(key, value);
    }
    link->sentLeader = std::move(fields);

    if (record.Fields().size() > 1)
        Send(guild.ownerAccount, record);
}

void GadvProtocol::OnPlayerLogout(uint32 accountId, ObjectGuid guid)
{
    auto itr = _links.find(accountId);
    if (itr != _links.end() && itr->second.player == guid)
    {
        itr->second.connected = false;
        itr->second.partials.clear();
        itr->second.partialOrder.clear();
        itr->second.sentBots.clear();
        itr->second.sentLeader.clear();
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Builders
// ---------------------------------------------------------------------------------------------------------------

Message GadvProtocol::BuildState(AdventureGuild const& guild) const
{
    Settings const& settings = GetSettings();

    std::string guildName;
    uint64 bank = 0;
    if (Guild* wowGuild = guild.guildId ? sGuildMgr->GetGuildById(guild.guildId) : nullptr)
    {
        guildName = wowGuild->GetName();
        bank = wowGuild->GetTotalBankMoney();
    }

    std::string features;
    for (std::string const& feature : sProgressionService.GetFeatures(guild))
    {
        if (!features.empty())
            features += ',';
        features += feature;
    }

    std::string orders;
    if (sProgressionService.HasFeature(guild, "orders"))
        // (The profession mode and the dungeon run were accepted by ORDER_SET but not listed here, so the addon showed
        // them greyed out.)
        for (OrderType type : { OrderType::Auto, OrderType::Idle, OrderType::Follow, OrderType::LevelZone, OrderType::Craft,
                 OrderType::Dungeon, OrderType::Farm })
            if (IsOrderImplemented(type))
            {
                if (!orders.empty())
                    orders += ',';
                orders += ToToken(type);
            }

    return Message("STATE")
        .Set("guild", guildName)
        .Set("fac", guild.team == TEAM_HORDE ? "H" : "A")
        .Set("rl", guild.renownLevel)
        .Set("rp", sProgressionService.GetPointsInLevel(guild))
        .Set("rn", sGuildAdventureMgr.GetRenownToNext(guild.renownLevel))
        .Set("rt", guild.renownTotal)
        .Set("slots", guild.unlockedSlots)
        .Set("used", uint32(guild.UsedSlots() + sRosterService.GetPendingRecruits(guild.ownerAccount)))
        .Set("max", settings.maxSlots)
        .Set("ow", guild.offlineWindowMin)
        .Set("owmax", sProgressionService.GetMaxOfflineWindow())
        .Set("bank", bank)
        .Set("tax", int64(sBotSettings.GetForGuild(guild.ownerAccount, Setting::GuildTaxPercent)))
        .Set("ulgold", sProgressionService.GetUnlockFunds(guild))
        .Set("ulfrom", ToToken(sProgressionService.GetPayFrom(guild)))
        .Set("feat", features)
        .Set("orders", orders)
        .Set("dk", sProgressionService.IsDeathKnightUnlocked(guild))
        .Set("ach", uint32(guild.achievements.size()))
        .Set("achp", sAchievementService.GetPoints(guild))
        .Set("achr", sAchievementService.GetRenown(guild))
        .Set("ms", sMilestoneService.GetEarnedCount(guild))
        .Set("msa", sMilestoneService.GetActiveCount())
        .Set("msp", sMilestoneService.GetPoints(guild))
        .Set("msr", sMilestoneService.GetRenown(guild))
        .Set("stk", sStorageService.GetTotals(guild.ownerAccount).first)
        .Set("sti", sStorageService.GetTotals(guild.ownerAccount).second)
        .Set("gp", sPerkService.GetAvailable(guild))
        .Set("gps", sPerkService.GetSpent(guild.ownerAccount))
        .Set("gpg", sPerkService.GetGranted(guild.ownerAccount));
}

Message GadvProtocol::BuildControlState(AdventureGuild const& guild, Player* current, bool busy) const
{
    std::string leaderName;
    sCharacterCache->GetCharacterNameByGuid(guild.leader, leaderName);
    return Message("CONTROL_STATE")
        .Set("leader", guild.leader.GetCounter())
        .Set("ln", leaderName)
        .Set("g", current ? current->GetGUID().GetCounter() : guild.leader.GetCounter())
        .Set("cn", current ? current->GetName() : leaderName)
        .Set("busy", busy)
        .Set("watch", current && sGuildBotHolder.IsWatched(current->GetGUID()));
}

GadvProtocol::FieldMap GadvProtocol::BuildBotFields(AdventureGuild const& guild, GuildBot const& bot) const
{
    Message record("BOT");
    char const* state = GetBotStateToken(guild, bot);
    Player* player = sRosterService.GetBotPlayer(bot.guid);
    uint32 const maxLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);

    record.Set("g", bot.guid.GetCounter()).Set("n", bot.name).Set("race", bot.race).Set("class", bot.classId)
        .Set("gender", bot.gender).Set("slot", bot.slot).Set("s", state).Set("since", bot.recruitedAt)
        .Set("kills", bot.kills).Set("deaths", bot.deaths).Set("quests", bot.quests);

    uint8 const level = player ? player->GetLevel() : bot.snapshot.level;
    uint32 const zoneId = player ? player->GetZoneId() : bot.snapshot.zoneId;
    record.Set("lvl", level)
        .Set("xp", player ? player->GetUInt32Value(PLAYER_XP) : bot.snapshot.xp)
        .Set("xpmax", level >= maxLevel ? 0u : (player ? player->GetUInt32Value(PLAYER_NEXT_LEVEL_XP) : sObjectMgr->GetXPForLevel(level)))
        .Set("zone", zoneId)
        .Set("zn", GetAreaName(zoneId))
        .Set("map", player ? player->GetMapId() : bot.snapshot.mapId)
        .Set("gold", player ? player->GetMoney() : bot.snapshot.money);

    if (player)
    {
        record.Set("hp", uint32(player->GetHealthPct() + 0.5f)).Set("dead", !player->IsAlive()).Set("combat", player->IsInCombat());

        // Gear level and the readiness for the dungeons in its reach (Readiness.h; "id:percent,...").
        uint16 const gearLevel = Readiness::GearLevel(player);
        record.Set("sk", ProfessionSkillSummary(player)).Set("gl", uint32(gearLevel)).Set("glk", uint32(gearLevel))
            .Set("rdy", sReadiness.Summary(player));
        // A caster without a wand, and who of the guild could make one (CraftOrders.h).
        bool const noWand = CraftOrders::LacksWand(player);
        record.Set("nw", noWand).Set("wm", noWand ? sCraftOrders.WandMaker(guild, player) : std::string());
    }
    else
    {
        // Not in the world: the professions and the gear level it had when it was last seen (the roster's columns;
        // BotSnapshot). gl stays an "in the world" value, glk is the last known one.
        if (!bot.snapshot.skills.empty())
            record.Set("sk", bot.snapshot.skills);
        if (bot.snapshot.gearLevel)
            record.Set("glk", uint32(bot.snapshot.gearLevel));
    }

    BotOrder const& order = bot.order;
    ZoneTemplate const* orderZone = order.zoneId ? sGuildAdventureMgr.GetZone(order.zoneId) : nullptr;
    // (The "zone" of a dungeon order is the dungeon's id.)
    DungeonPlan const* orderDungeon = order.type == OrderType::Dungeon ? sDungeons.GetPlan(order.zoneId) : nullptr;
    // (... and that of a farm order the item.)
    ItemTemplate const* orderItem = order.type == OrderType::Farm ? sObjectMgr->GetItemTemplate(order.zoneId) : nullptr;
    record.Set("o", ToToken(order.type)).Set("oz", order.zoneId)
        .Set("ozn", orderItem ? orderItem->Name1 : orderDungeon ? orderDungeon->name : orderZone ? orderZone->name :
            order.type == OrderType::Farm ? std::string() : GetAreaName(order.zoneId))
        // A farm order: its target stock (0 = none) and what the guild storage holds of the item now.
        .Set("ofn", order.type == OrderType::Farm ? order.farmTarget : 0u)
        .Set("ofs", order.type == OrderType::Farm ? sStorageService.GetStoredOfEntry(guild.ownerAccount, order.zoneId) : 0u)
        .Set("aa", bot.autoAdvance).Set("lc", order.levelCap).Set("cont", order.continent).Set("nd", order.noDungeons)
        .Set("gcap", order.goldCap).Set("why", OrderWhy(guild, bot, state));

    // Talent template (SpecService): role and name of the template the bot plays by, what is assigned (spt: 0 = default
    // of the class, 1 = manual, else a template id) and the talent points per tree while it is in the world.
    {
        static char const* const roles[] = { "dps", "tank", "heal" };
        uint32 const assigned = sSpecService.GetAssigned(bot.guid);
        SpecTemplate const* spec = sSpecService.GetEffective(bot.guid, bot.classId);
        record.Set("role", spec ? roles[std::min<uint8>(spec->role, 2)] : "none")
            .Set("spec", assigned == SpecManual ? std::string("Manual") : spec ? spec->name : std::string())
            .Set("spt", assigned).Set("tal", player ? SpecService::CurrentSummary(player) : std::string());
    }
    // Dungeon role (DungeonRoles.h): the role the owner gave it (dr), the roles its class can fill (drs), the talent
    // template of the role (drt as stored, 0 = the default; drn the one that is used), whether the dungeon set is on
    // (drw), how many pieces of its own the set has (drp) and, in the world, the gear level in the dungeon set (drgl)
    // with the readiness per dungeon it gives (drrdy, as rdy).
    {
        std::string can;
        for (DungeonRole possible : sRoleService.RolesOf(guild.ownerAccount, bot.classId))
            can += (can.empty() ? "" : ",") + std::string(ToToken(possible));
        SpecTemplate const* roleSpec = sRoleService.RoleTemplate(guild.ownerAccount, bot.guid, bot.classId);
        record.Set("dr", ToToken(sRoleService.GetRole(bot.guid))).Set("drs", can).Set("drt", sRoleService.GetTemplateId(bot.guid))
            .Set("drn", roleSpec ? roleSpec->name : std::string()).Set("drw", sRoleService.IsDungeonSetOn(bot.guid))
            .Set("drp", sRoleService.CountDungeonPieces(bot.guid));
        // A dungeon order: the runs it still has (the one that is going included) and those it has done.
        record.Set("druns", order.type == OrderType::Dungeon ? sOrderService.GetDungeonRuns(bot.guid) : 0u)
            .Set("drund", order.type == OrderType::Dungeon ? sOrderService.GetDungeonRunsDone(bot.guid) : 0u);
        if (player)
            record.Set("drgl", uint32(sRoleService.RoleGearLevel(player))).Set("drrdy", sRoleService.ReadinessSummary(player));
    }
    // Fight profiles (FightProfiles.h): the profile of its solo (fp) and its group slot (fpg), 0 = the default; what it
    // plays now in a fight (fpn, empty: the default; fpid its id; fpsl the slot it is from, 0 solo, 1 group; fpd true when
    // it is the default template of its spec, FC12), and why it does not play the profile of its slot (fpw).
    {
        record.Set("fp", sFightProfileStore.Assigned(bot.guid, FightProfiles::Slot::Solo))
            .Set("fpg", sFightProfileStore.Assigned(bot.guid, FightProfiles::Slot::Group));
        FightProfiles::Played const* playing = player ? FightProfiles::Active(player, 0) : nullptr;
        bool const profiled = playing && playing->profileId;
        record.Set("fpn", profiled ? playing->name : std::string()).Set("fpid", profiled ? playing->profileId : 0u)
            .Set("fpd", profiled && playing->byDefault)
            .Set("fpsl", profiled ? uint32(playing->slot) : 0u).Set("fpw", FightProfiles::Refusal(bot.guid));
    }
    // Squad (Squads.h): the leader it follows (0 = none), its state, and for a leader how many members it has.
    record.Set("sq", sSquadService.GetLeader(bot.guid).GetCounter()).Set("sqs", ToToken(sSquadService.GetState(guild, bot.guid)))
        .Set("sqm", uint32(sSquadService.GetMembers(bot.guid).size()));
    // Quest partners (QuestPartners.h): the quest of the partner squad it is in (sqq, its title sqqn; for a member and for
    // its leader), and in the world the quests it keeps for a partner (qw, their titles qwn joined by "; ").
    {
        uint32 partnerQuest = sSquadService.GetPartnerQuest(bot.guid);
        if (!partnerQuest)
            partnerQuest = sSquadService.GetLedPartnerQuest(bot.guid);
        Quest const* partner = partnerQuest ? sObjectMgr->GetQuestTemplate(partnerQuest) : nullptr;
        record.Set("sqq", partnerQuest).Set("sqqn", partner ? partner->GetTitle() : std::string());
        std::string ids;
        std::string titles;
        if (player)
            for (uint32 questId : sQuestPartners.Waiting(bot, player))
            {
                Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                ids += (ids.empty() ? "" : ",") + std::to_string(questId);
                titles += (titles.empty() ? "" : "; ") + (quest ? quest->GetTitle() : std::to_string(questId));
            }
        record.Set("qw", ids).Set("qwn", titles);
    }
    // Rates: rolling hour of the order planner's samples (0 until five minutes in the world).
    record.Set("xph", sOrderService.GetXpPerHour(bot.guid)).Set("gph", sOrderService.GetMoneyPerHour(bot.guid));

    std::string secondary;
    for (uint16 skill : bot.professions.secondary)
    {
        if (!secondary.empty())
            secondary += ',';
        secondary += std::to_string(skill);
    }
    record.Set("p1", bot.professions.primary[0]).Set("p2", bot.professions.primary[1])
        .Set("t1", bot.professions.target[0]).Set("t2", bot.professions.target[1]).Set("sec", secondary);

    return record.Fields();
}

GadvProtocol::FieldMap GadvProtocol::BuildLeaderFields(AdventureGuild const& guild) const
{
    Message record("LEADER");
    GuildBot const& plan = guild.leaderPlan;
    Player* player = ObjectAccessor::FindConnectedPlayer(guild.leader);
    if (player && (!player->IsInWorld() || player->IsDuringRemoveFromWorld()))
        player = nullptr;
    char const* state = !player ? "offline" : sGuildBotHolder.IsAIDriven(guild.leader) ? "active" : "controlled";
    uint32 const maxLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);

    record.Set("g", guild.leader.GetCounter()).Set("n", player ? player->GetName() : plan.name)
        .Set("race", player ? player->getRace() : plan.race).Set("class", player ? player->getClass() : plan.classId)
        .Set("gender", player ? player->getGender() : plan.gender).Set("s", state);

    uint8 const level = player ? player->GetLevel() : plan.snapshot.level;
    uint32 const zoneId = player ? player->GetZoneId() : plan.snapshot.zoneId;
    record.Set("lvl", level)
        .Set("xp", player ? player->GetUInt32Value(PLAYER_XP) : plan.snapshot.xp)
        .Set("xpmax", level >= maxLevel ? 0u : (player ? player->GetUInt32Value(PLAYER_NEXT_LEVEL_XP) : sObjectMgr->GetXPForLevel(level)))
        .Set("zone", zoneId).Set("zn", GetAreaName(zoneId));
    if (player)
        record.Set("hp", uint32(player->GetHealthPct() + 0.5f)).Set("dead", !player->IsAlive()).Set("combat", player->IsInCombat());

    if (guild.leaderPlanned)
    {
        BotOrder const& order = plan.order;
        ZoneTemplate const* orderZone = order.zoneId ? sGuildAdventureMgr.GetZone(order.zoneId) : nullptr;
        record.Set("o", ToToken(order.type)).Set("oz", order.zoneId)
            .Set("ozn", orderZone ? orderZone->name : GetAreaName(order.zoneId)).Set("lc", order.levelCap)
            .Set("cont", order.continent).Set("nd", order.noDungeons).Set("gcap", order.goldCap)
            .Set("why", OrderWhy(guild, plan, state));
    }
    else
    {
        std::string const s = state;
        record.Set("o", "HOLD").Set("oz", 0u).Set("ozn", "").Set("lc", 0u).Set("cont", -1).Set("nd", false).Set("gcap", 0u)
            .Set("why", s == "active" ? "Holding position (no order)" : s == "controlled" ? "Played by you" : "Offline");
    }
    record.Set("xph", sOrderService.GetXpPerHour(guild.leader)).Set("gph", sOrderService.GetMoneyPerHour(guild.leader));
    return record.Fields();
}

Message GadvProtocol::BuildEvent(GuildEvent const& event) const
{
    Message message("EVENT");
    message.Set("id", event.id).Set("t", event.time).Set("type", event.type).Set("g", event.bot.GetCounter()).Set("msg", event.message);
    if (event.level)
        message.Set("lvl", event.level);
    if (event.zoneId)
        message.Set("zone", event.zoneId);
    if (event.points)
        message.Set("pts", event.points);
    return message;
}
