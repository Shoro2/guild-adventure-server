/*
 * GuildBank implementation. See GuildBank.h.
 */

#include "GuildBank.h"
#include "AuctionHouseMgr.h"
#include "BotSettings.h"
#include "CharacterCache.h"
#include "DatabaseEnv.h"
#include "GadvProtocol.h"
#include "GameTime.h"
#include "Guild.h"
#include "GuildAdventureConfig.h"
#include "GuildAdventureMgr.h"
#include "GuildMgr.h"
#include "Log.h"
#include "Mail.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "StorageService.h"
#include "StringFormat.h"
#include "Timer.h"
#include "TradeData.h"
#include "WorldSession.h"
#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

using namespace GuildAdventure;

namespace
{
    // A hint from a hook that names the next gain of the same character on the same thread holds this long.
    constexpr uint32 HintMs = 1000;

    thread_local int t_noTax = 0;
    thread_local char const* t_source = nullptr;

    // What a hook said about the next gain of one character (quest reward, vendor sale, guild bank withdrawal).
    struct Hint
    {
        ObjectGuid::LowType guid = 0;
        uint32 at = 0;
        std::string what;      // the gain's name in the line
        uint32 amount = 0;     // a guild bank withdrawal: its amount (the gain must be this much)
        bool guildMoney = false;
    };
    thread_local Hint t_hint;

    // One gain of a guild character whose tax waits for its booking on the world thread.
    struct Booking
    {
        uint32 owner = 0;
        std::string who;
        uint32 gross = 0;
        uint32 tax = 0;
        uint32 percent = 0;
        std::string source;
    };

    std::mutex taxLock;
    std::unordered_map<ObjectGuid::LowType, uint32> taxRemainder;  // income x percent below 100, per character
    std::vector<Booking> taxQueue;

    std::string Text(uint64 copper)
    {
        uint64 const gold = copper / GOLD;
        uint64 const silver = (copper % GOLD) / SILVER;
        uint64 const rest = copper % SILVER;
        std::string text;
        if (gold)
            text += Acore::StringFormat("{}g", gold);
        if (silver)
            text += Acore::StringFormat("{}{}s", text.empty() ? "" : " ", silver);
        if (rest || text.empty())
            text += Acore::StringFormat("{}{}c", text.empty() ? "" : " ", rest);
        return text;
    }

    void PushBank(uint32 owner)
    {
        if (AdventureGuild const* guild = sGuildAdventureMgr.FindGuild(owner))
            sGadvProtocol.PushState(*guild);
    }

    bool Move(uint32 owner, uint64 copper, bool add, std::string const& why, bool push = true)
    {
        Guild* guild = GuildBank::WowGuildOf(owner);
        if (!guild || !copper)
            return false;
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        if (!guild->ModifyBankMoney(trans, copper, add))
            return false;
        CharacterDatabase.CommitTransaction(trans);
        LOG_INFO("module.ga.activity", "activity: guild bank of {}: {}{} ({}), now {}", guild->GetName(), add ? "+" : "-", Text(copper),
            why, Text(guild->GetTotalBankMoney()));
        if (push)
            PushBank(owner);
        return true;
    }

    // A character of the owner's own account, or a member of the same WoW guild (the leader, a roster bot).
    bool IsOwnSide(uint32 owner, uint32 guildId, ObjectGuid guid)
    {
        if (!guid)
            return false;
        if (Player* online = ObjectAccessor::FindConnectedPlayer(guid))
        {
            if (online->GetSession() && online->GetSession()->GetAccountId() == owner)
                return true;
            return guildId && online->GetGuildId() == guildId;
        }
        if (sCharacterCache->GetCharacterAccountIdByGuid(guid) == owner)
            return true;
        return guildId && sCharacterCache->GetCharacterGuildIdByGuid(guid) == guildId;
    }

    std::string NameOf(ObjectGuid guid)
    {
        std::string name;
        if (!guid || !sCharacterCache->GetCharacterNameByGuid(guid, name))
            name = "someone";
        return name;
    }

    // What the gain of `amount` is, read from where the character is: a trade it is completing, a mail whose money it
    // is taking. `handover` = it comes from the owner's own characters or the guild; `refund` = the auction house
    // gives back what the character bid.
    struct Kind
    {
        std::string what;
        bool handover = false;
        bool refund = false;
    };

    bool KindOfTransfer(Player* player, uint32 owner, uint32 amount, Kind& kind)
    {
        uint32 const guildId = player->GetGuildId();
        // Trade: the money moves in HandleAcceptTradeOpcode while both trade windows still exist.
        if (TradeData* mine = player->GetTradeData())
            if (Player* other = mine->GetTrader())
                if (TradeData* theirs = other->GetTradeData(); theirs && theirs->GetMoney() == amount)
                {
                    kind.handover = IsOwnSide(owner, guildId, other->GetGUID());
                    kind.what = Acore::StringFormat("by trade from {}", other->GetName());
                    return true;
                }
        // Mail: HandleMailTakeMoney sets the mail's money to 0 only after the character has it.
        time_t const now = GameTime::GetGameTime().count();
        for (Mail const* mail : player->GetMails())
        {
            if (!mail || mail->state == MAIL_STATE_DELETED || mail->money != amount || mail->deliver_time > now)
                continue;
            if (mail->messageType == MAIL_AUCTION)
            {
                // Subject "<item>:0:<response>:<auction>:<count>": 0 outbid and 4 cancelled to the bidder give the bid back.
                std::string const& subject = mail->subject;
                size_t const first = subject.find(':');
                size_t const second = first == std::string::npos ? first : subject.find(':', first + 1);
                int const response = second == std::string::npos ? -1 : std::atoi(subject.c_str() + second + 1);
                kind.refund = response == AUCTION_OUTBIDDED || response == AUCTION_CANCELLED_TO_BIDDER;
                kind.what = kind.refund ? "the auction house giving its bid back" : "the auction house (a sale)";
                return true;
            }
            if (mail->messageType == MAIL_NORMAL)
            {
                ObjectGuid const sender = ObjectGuid::Create<HighGuid::Player>(mail->sender);
                kind.handover = IsOwnSide(owner, guildId, sender);
                kind.what = Acore::StringFormat("by mail from {}", NameOf(sender));
                return true;
            }
            kind.what = "by mail";
            return true;
        }
        return false;
    }
}

Guild* GuildBank::WowGuildOf(uint32 owner)
{
    AdventureGuild const* guild = sGuildAdventureMgr.FindGuild(owner);
    return guild && guild->guildId ? sGuildMgr->GetGuildById(guild->guildId) : nullptr;
}

uint64 GuildBank::Money(uint32 owner)
{
    Guild const* guild = WowGuildOf(owner);
    return guild ? guild->GetTotalBankMoney() : 0;
}

bool GuildBank::Deposit(uint32 owner, uint64 copper, std::string const& why)
{
    return Move(owner, copper, true, why);
}

bool GuildBank::Withdraw(uint32 owner, uint64 copper, std::string const& why)
{
    return Move(owner, copper, false, why);
}

GuildBank::NoTax::NoTax()
{
    ++t_noTax;
}

GuildBank::NoTax::~NoTax()
{
    --t_noTax;
}

GuildBank::IncomeSource::IncomeSource(char const* what) : _previous(t_source)
{
    t_source = what;
}

GuildBank::IncomeSource::~IncomeSource()
{
    t_source = _previous;
}

void GuildBank::OnQuestReward(Player* player, uint32 questId)
{
    if (!player)
        return;
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    t_hint = Hint();
    t_hint.guid = player->GetGUID().GetCounter();
    t_hint.at = getMSTime();
    t_hint.what = quest ? Acore::StringFormat("the reward of quest {} \"{}\"", questId, quest->GetTitle())
                        : Acore::StringFormat("the reward of quest {}", questId);
}

void GuildBank::OnVendorSale(Player* player, uint32 itemEntry)
{
    if (!player)
        return;
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemEntry);
    t_hint = Hint();
    t_hint.guid = player->GetGUID().GetCounter();
    t_hint.at = getMSTime();
    t_hint.what = Acore::StringFormat("selling {} to a vendor", proto ? proto->Name1 : std::to_string(itemEntry));
}

void GuildBank::OnGuildBankWithdrawal(Player* player, uint32 amount, bool repair)
{
    if (!player || repair || !amount)
        return;
    t_hint = Hint();
    t_hint.guid = player->GetGUID().GetCounter();
    t_hint.at = getMSTime();
    t_hint.what = "a withdrawal from the guild bank";
    t_hint.amount = amount;
    t_hint.guildMoney = true;
}

void GuildBank::OnMoneyChanged(Player* player, int32& amount)
{
    if (amount <= 0 || !player)
        return;
    // A hook's hint about this gain: taken now, whatever follows (it names one gain only).
    Hint hint;
    if (t_hint.guid == player->GetGUID().GetCounter() && getMSTimeDiff(t_hint.at, getMSTime()) <= HintMs
        && (!t_hint.amount || t_hint.amount == uint32(amount)))
    {
        hint = t_hint;
        t_hint = Hint();
    }
    if (t_noTax || !IsModuleEnabled() || !player->GetGuildId())
        return;
    uint32 const owner = sStorageService.GetOwnerOf(player);
    if (!owner)
        return;

    // The guild leader (the guild master of the WoW guild) pays no tax, whoever plays it (the owner, 2026-11-14: [quote left out]).
    if (player->GetRank() == GR_GUILDMASTER)
        return;

    // What the gain is. The guild's own gold and the auction house's refunds are no income.
    Kind kind;
    bool const transfer = KindOfTransfer(player, owner, uint32(amount), kind);
    if (hint.guildMoney || kind.refund || (kind.handover && !sBotSettings.GetForGuild(owner, Setting::GuildTaxHandovers)))
    {
        LOG_INFO("module.ga.activity", "activity: {} got {} ({}): no guild tax on it ({})", player->GetName(), Text(uint32(amount)),
            hint.guildMoney ? hint.what : kind.what, hint.guildMoney ? "the guild's own gold" : kind.refund ? "a refund, no income"
            : "money handed over by the guild's own characters");
        return;
    }
    uint32 const percent = uint32(std::clamp<int32>(sBotSettings.GetForGuild(owner, Setting::GuildTaxPercent), 0, 100));
    if (!percent)
        return;

    std::string source;
    if (t_source)
        source = t_source;
    else if (transfer)
        source = kind.what;
    else if (!hint.what.empty())
        source = hint.what;
    else if (player->GetLootGUID())
        source = "looted coin";
    else
        source = "income";

    std::lock_guard<std::mutex> lock(taxLock);
    uint32& remainder = taxRemainder[player->GetGUID().GetCounter()];
    uint64 const product = uint64(amount) * percent + remainder;
    uint32 const tax = uint32(std::min<uint64>(product / 100, uint64(amount)));
    remainder = uint32(product % 100);
    if (!tax)
        return;
    Booking booking;
    booking.owner = owner;
    booking.who = player->GetName();
    booking.gross = uint32(amount);
    booking.tax = tax;
    booking.percent = percent;
    booking.source = std::move(source);
    taxQueue.push_back(std::move(booking));
    amount -= int32(tax);
}

uint64 GuildBank::PendingTax(uint32 owner)
{
    std::lock_guard<std::mutex> lock(taxLock);
    uint64 sum = 0;
    for (Booking const& booking : taxQueue)
        if (booking.owner == owner)
            sum += booking.tax;
    return sum;
}

void GuildBank::Flush()
{
    std::vector<Booking> queue;
    {
        std::lock_guard<std::mutex> lock(taxLock);
        if (taxQueue.empty())
            return;
        queue.swap(taxQueue);
    }
    // Each gain is booked by itself, a line each; the addon gets the new bank money once per guild.
    std::set<uint32> booked;
    std::vector<Booking> later;
    for (Booking& booking : queue)
    {
        if (Move(booking.owner, booking.tax, true, Acore::StringFormat("guild tax {}% of {} {} got: {}", booking.percent,
            Text(booking.gross), booking.who, booking.source), false))
        {
            booked.insert(booking.owner);
            continue;
        }
        // No WoW guild right now: it stays held back for the next try.
        later.push_back(std::move(booking));
    }
    for (uint32 owner : booked)
        PushBank(owner);
    if (!later.empty())
    {
        std::lock_guard<std::mutex> lock(taxLock);
        taxQueue.insert(taxQueue.begin(), std::make_move_iterator(later.begin()), std::make_move_iterator(later.end()));
    }
}

void GuildBank::Update(uint32 /*diff*/)
{
    // Every world tick: the tax of a gain is in the guild bank a moment after the gain.
    Flush();
}
