/*
 * RosterService implementation. See RosterService.h.
 */

#include "RosterService.h"
#include "BotSettings.h"
#include "BotDriver.h"
#include "Bag.h"
#include "DungeonRoles.h"
#include "AccountMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "CryptoRandom.h"
#include "DBCStores.h"
#include "DeathKnights.h"
#include "DatabaseEnv.h"
#include "GuildAdventureMgr.h"
#include "GuildAdventureSettings.h"
#include "GuildAdventureUtil.h"
#include "GuildBank.h"
#include "GuildBudget.h"
#include "GuildBotHolder.h"
#include "GuildEvents.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "OfflineService.h"
#include "OrderService.h"
#include "Player.h"
#include "ProgressionService.h"
#include "Readiness.h"
#include "Realm.h"
#include "Squads.h"
#include "StorageService.h"
#include "SRP6.h"
#include "Timer.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "Util.h"
#include "World.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"
#include <limits>
#include <map>
#include <memory>
#include <vector>

using namespace GuildAdventure;

namespace
{
    constexpr uint32 RefreshIntervalMs = 2000;

    void Tell(Player* player, std::string const& text)
    {
        if (player && player->GetSession())
            ChatHandler(player->GetSession()).SendSysMessage("|cff3fbf3fGuild Adventure:|r " + text);
    }

    RosterResult Fail(RosterError error, std::string message)
    {
        RosterResult result;
        result.error = error;
        result.message = std::move(message);
        return result;
    }

    // Headless session on a bot account, the same kind GuildBotHolder uses for home sessions.
    WorldSession* CreateCreationSession(uint32 accountId)
    {
        return new WorldSession(accountId, "", 0x0, nullptr, SEC_PLAYER, EXPANSION_WRATH_OF_THE_LICH_KING, time_t(0),
            sWorld->GetDefaultDbcLocale(), 0, false, false, 0);
    }

    // A random appearance from CharSections.dbc, as mod-playerbots does for its bots
    // (RandomPlayerbotFactory::CreateRandomBot).
    std::unique_ptr<CharacterCreateInfo> MakeCreateInfo(RecruitRequest const& request)
    {
        std::vector<uint8> facialHairs;
        std::vector<std::pair<uint8, uint8>> faces;
        std::vector<std::pair<uint8, uint8>> hairs;
        for (CharSectionsEntry const* section : sCharSectionsStore)
        {
            if (section->RaceID != request.race || section->SexID != request.gender)
                continue;

            switch (section->BaseSection)
            {
                case SECTION_TYPE_FACE:
                    faces.emplace_back(section->VariationIndex, section->ColorIndex);
                    break;
                case SECTION_TYPE_FACIAL_HAIR:
                    facialHairs.push_back(section->VariationIndex);
                    break;
                case SECTION_TYPE_HAIR:
                    hairs.emplace_back(section->VariationIndex, section->ColorIndex);
                    break;
                default:
                    break;
            }
        }

        std::pair<uint8, uint8> const face = faces.empty() ? std::pair<uint8, uint8>(0, 0) : faces[urand(0, faces.size() - 1)];
        std::pair<uint8, uint8> const hair = hairs.empty() ? std::pair<uint8, uint8>(0, 0) : hairs[urand(0, hairs.size() - 1)];
        bool const noFacialHair = request.race == RACE_TAUREN || request.race == RACE_DRAENEI ||
            (request.gender == GENDER_FEMALE && request.race != RACE_NIGHTELF && request.race != RACE_UNDEAD_PLAYER);
        uint8 const facialHair = (noFacialHair || facialHairs.empty()) ? 0 : facialHairs[urand(0, facialHairs.size() - 1)];

        return std::make_unique<CharacterCreateInfo>(request.name, request.race, request.classId, request.gender,
            face.second, face.first, hair.first, hair.second, facialHair);
    }

    std::string RandomPassword()
    {
        static char const alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        std::array<uint8, 16> bytes = Acore::Crypto::GetRandomBytes<16>();
        std::string password;
        for (uint8 byte : bytes)
            password += alphabet[byte % (sizeof(alphabet) - 1)];
        return password;
    }
}

char const* GuildAdventure::ToToken(RosterError error)
{
    switch (error)
    {
        case RosterError::None:         return "";
        case RosterError::BadRequest:   return "BAD_REQUEST";
        case RosterError::Locked:       return "LOCKED";
        case RosterError::NoSlot:       return "NO_SLOT";
        case RosterError::NameInvalid:  return "NAME_INVALID";
        case RosterError::NameTaken:    return "NAME_TAKEN";
        case RosterError::InvalidCombo: return "INVALID_COMBO";
        case RosterError::UnknownBot:   return "UNKNOWN_BOT";
        case RosterError::Busy:         return "BUSY";
        case RosterError::Internal:     return "INTERNAL";
    }
    return "INTERNAL";
}

char const* GuildAdventure::GetBotStateToken(AdventureGuild const& /*guild*/, GuildBot const& bot)
{
    if (sGuildBotHolder.IsAIDriven(bot.guid))
        return "active";

    if (sGuildBotHolder.IsManaged(bot.guid))
    {
        for (ManagedInfo const& info : sGuildBotHolder.ListControlled())
            if (info.guid == bot.guid)
                return "controlled";
        return "offline"; // loading
    }

    return bot.state == BotState::Benched ? "benched" : "offline";
}

RosterService& RosterService::Instance()
{
    static RosterService instance;
    return instance;
}

// ---------------------------------------------------------------------------------------------------------------
// Owner login, the guild and the WoW guild
// ---------------------------------------------------------------------------------------------------------------

void RosterService::OnOwnerCharacterLogin(Player* player)
{
    WorldSession* session = player ? player->GetSession() : nullptr;
    if (!session || session->IsHeadless())
        return;

    uint32 const owner = session->GetAccountId();
    // A character of another account on the owner's session is a controlled guild bot, not the leader.
    if (player->GetCharacterAccountId() != owner || sGuildAdventureMgr.FindGuildOfBot(player->GetGUID()))
        return;

    AdventureGuild* guild = sGuildAdventureMgr.FindGuild(owner);
    if (!guild)
    {
        Settings const& settings = GetSettings();
        guild = &sGuildAdventureMgr.CreateGuild(owner, player, settings.startSlots, settings.offlineWindowStartMinutes);
        sProgressionService.ApplyUnlocks(*guild);
        RecordEvent(*guild, "INFO", ObjectGuid::Empty, Acore::StringFormat("{} founded the adventure guild.", player->GetName()));
        LOG_INFO("module", "mod-guild-adventure: account {} founded an adventure guild, leader {}", owner, player->GetName());
        Tell(player, Acore::StringFormat("you lead an adventure guild now: {} bot slots. Recruit your first adventurers with /ga.",
            guild->unlockedSlots));
    }

    if (guild->leader != player->GetGUID())
    {
        std::string leaderName;
        sCharacterCache->GetCharacterNameByGuid(guild->leader, leaderName);
        Tell(player, Acore::StringFormat("your guild leader is {}. Log in with {} to lead your guild.", leaderName, leaderName));
        return;
    }

    EnsureWowGuild(*guild, player);
    sOfflineService.OnOwnerBack(*guild);
    LoginActiveBots(*guild, player);
}

bool RosterService::EnsureWowGuild(AdventureGuild& guild, Player* leader)
{
    if (guild.guildId)
    {
        if (sGuildMgr->GetGuildById(guild.guildId))
            return true;

        LOG_WARN("module", "mod-guild-adventure: WoW guild {} of owner account {} is gone, founding a new one", guild.guildId, guild.ownerAccount);
        guild.guildId = 0;
        sGuildAdventureMgr.SaveGuild(guild);
    }

    if (uint32 const current = leader->GetGuildId())
    {
        Guild* wowGuild = sGuildMgr->GetGuildById(current);
        if (wowGuild && wowGuild->GetLeaderGUID() == leader->GetGUID())
        {
            guild.guildId = current;
            sGuildAdventureMgr.SaveGuild(guild);
            RecordEvent(guild, "INFO", ObjectGuid::Empty, Acore::StringFormat("The adventure guild is <{}>.", wowGuild->GetName()));
            return true;
        }

        Tell(leader, "you are a member of a guild you do not lead. Leave it so the adventure guild can be founded.");
        return false;
    }

    std::string name = GetSettings().guildNameFormat;
    std::size_t const placeholder = name.find("{}");
    name.replace(placeholder, 2, leader->GetName());
    if (name.size() > 24)
        name.resize(24);
    while (!name.empty() && name.back() == ' ')
        name.pop_back();

    if (!ObjectMgr::IsValidCharterName(name) || sGuildMgr->GetGuildByName(name))
    {
        Tell(leader, Acore::StringFormat("the guild name <{}> is invalid or taken. Found a guild yourself; it becomes the adventure guild at your next login.", name));
        return false;
    }

    Guild* wowGuild = new Guild();
    if (!wowGuild->Create(leader, name))
    {
        delete wowGuild;
        Tell(leader, Acore::StringFormat("the guild <{}> could not be created.", name));
        return false;
    }

    sGuildMgr->AddGuild(wowGuild);
    guild.guildId = wowGuild->GetId();
    sGuildAdventureMgr.SaveGuild(guild);
    RecordEvent(guild, "INFO", ObjectGuid::Empty, Acore::StringFormat("The guild <{}> was founded.", name));
    LOG_INFO("module", "mod-guild-adventure: founded WoW guild '{}' ({}) for owner account {}", name, guild.guildId, guild.ownerAccount);

    // Bots recruited before the WoW guild existed join it now.
    for (auto const& [guid, bot] : guild.bots)
        if (!sCharacterCache->GetCharacterGuildIdByGuid(guid) && !ObjectAccessor::FindConnectedPlayer(guid))
            wowGuild->AddMember(guid);
    _guildChecked.clear();
    return true;
}

void RosterService::LoginActiveBots(AdventureGuild& guild, Player* master)
{
    ObjectGuid const masterGuid = master ? master->GetGUID() : ObjectGuid::Empty;
    for (auto const& [guid, bot] : guild.bots)
    {
        if (bot.state != BotState::Active || sGuildBotHolder.IsManaged(guid) || _pendingRecruits.count(guid))
            continue;

        LoginResult const result = sGuildBotHolder.Login(guid, guild.ownerAccount, masterGuid);
        if (result != LoginResult::Started && result != LoginResult::AlreadyManaged)
            LOG_WARN("module", "mod-guild-adventure: guild bot {} did not log in: {}", bot.name, ToString(result));
    }
}

bool RosterService::LoginPlannedLeader(AdventureGuild& guild)
{
    if (!GetSettings().leaderOfflinePlay || !guild.leaderPlanned || guild.leader.IsEmpty())
        return false;

    // Any session of the owner's account means the owner is there (in the world, at the character screen, or on the
    // offline session of a closed connection): its own login decides about the leader. Checked on the world thread,
    // where the core adds sessions, so no core login of the leader can be in flight past this check.
    uint32 const owner = guild.ownerAccount;
    if (sWorldSessionMgr->FindSession(owner) || sWorldSessionMgr->FindOfflineSession(owner))
        return false;

    if (sGuildBotHolder.IsManaged(guild.leader) || ObjectAccessor::FindConnectedPlayer(guild.leader))
        return false;

    LoginResult const result = sGuildBotHolder.Login(guild.leader, owner, ObjectGuid::Empty);
    if (result != LoginResult::Started)
    {
        LOG_WARN("module", "mod-guild-adventure: leader {} did not log in: {}", guild.leaderPlan.name, ToString(result));
        return false;
    }

    LOG_INFO("module", "mod-guild-adventure: leader {} of owner account {} has the order {}: it logs in to play on "
        "without its owner", guild.leaderPlan.name, owner, ToToken(guild.leaderPlan.order.type));
    return true;
}

Player* RosterService::GetBotPlayer(ObjectGuid bot) const
{
    Player* player = ObjectAccessor::FindConnectedPlayer(bot);
    return player && player->IsInWorld() ? player : nullptr;
}

// ---------------------------------------------------------------------------------------------------------------
// Bench / activate
// ---------------------------------------------------------------------------------------------------------------

RosterResult RosterService::SetBotState(AdventureGuild& guild, ObjectGuid guid, BotState state)
{
    GuildBot* bot = guild.FindBot(guid);
    if (!bot)
        return Fail(RosterError::UnknownBot, "That character is not one of your guild bots.");

    if (bot->state == state)
    {
        RosterResult ok;
        ok.bot = guid;
        return ok;
    }

    if (state == BotState::Benched)
    {
        if (sGuildBotHolder.IsManaged(guid) && !sGuildBotHolder.IsAIDriven(guid))
            return Fail(RosterError::Busy, Acore::StringFormat("{} is logging in or played by you right now.", bot->name));

        if (sGuildBotHolder.IsAIDriven(guid) && !sGuildBotHolder.Logout(guid))
            return Fail(RosterError::Internal, Acore::StringFormat("{} could not be logged out.", bot->name));
    }

    bot->state = state;
    sGuildAdventureMgr.SaveBot(guild, *bot);
    RecordEvent(guild, "INFO", guid, state == BotState::Benched ?
        Acore::StringFormat("{} was benched.", bot->name) : Acore::StringFormat("{} is back in the roster.", bot->name));

    if (state == BotState::Active)
    {
        // Log in at once if the owner plays the leader or a guild bot right now.
        if (WorldSession* session = sWorldSessionMgr->FindSession(guild.ownerAccount))
            if (Player* player = session->GetPlayer())
                if (player->IsInWorld())
                {
                    LoginResult const result = sGuildBotHolder.Login(guid, guild.ownerAccount, player->GetGUID());
                    if (result != LoginResult::Started && result != LoginResult::AlreadyManaged)
                        LOG_WARN("module", "mod-guild-adventure: guild bot {} did not log in: {}", bot->name, ToString(result));
                }
    }

    RosterResult ok;
    ok.bot = guid;
    return ok;
}

// ---------------------------------------------------------------------------------------------------------------
// Dismiss
// ---------------------------------------------------------------------------------------------------------------

namespace
{
    constexpr uint32 DismissLoginTimeoutMs = 120 * IN_MILLISECONDS;

    // Every item of the character, the contents of its bags (also those in its bank) before the bags: a bag goes into
    // the guild storage only when it is empty.
    std::vector<Item*> AllItemsOf(Player* player)
    {
        std::vector<Item*> items;
        std::vector<Item*> bags;
        auto addBag = [&](uint8 bagSlot)
        {
            Bag* bag = player->GetBagByPos(bagSlot);
            if (!bag)
                return;
            for (uint32 i = 0; i < bag->GetBagSize(); ++i)
                if (Item* item = bag->GetItemByPos(uint8(i)))
                    items.push_back(item);
            bags.push_back(bag);
        };
        auto addSlots = [&](uint8 first, uint8 end)
        {
            for (uint8 slot = first; slot < end; ++slot)
                if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                    items.push_back(item);
        };
        for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
            addBag(slot);
        for (uint8 slot = BANK_SLOT_BAG_START; slot < BANK_SLOT_BAG_END; ++slot)
            addBag(slot);
        addSlots(INVENTORY_SLOT_ITEM_START, INVENTORY_SLOT_ITEM_END);
        addSlots(BANK_SLOT_ITEM_START, BANK_SLOT_ITEM_END);
        addSlots(KEYRING_SLOT_START, CURRENCYTOKEN_SLOT_END);
        addSlots(EQUIPMENT_SLOT_START, EQUIPMENT_SLOT_END);
        items.insert(items.end(), bags.begin(), bags.end());
        return items;
    }

    // The module's own rows of a character that is gone. Each on its own, so that a table one install lacks does not
    // keep the others (the event log keeps its lines: they are the guild's history).
    void DeleteModuleRows(ObjectGuid guid)
    {
        uint32 const low = guid.GetCounter();
        static char const* const byBot[] = { "ga_bot", "ga_bot_order", "ga_bot_profession_plan", "ga_bot_quest_block",
            "ga_bot_spec", "ga_bot_role", "ga_bot_role_gear", "ga_bot_dungeon_runs", "ga_bot_farm", "ga_bot_squad",
            "ga_bot_pet_upgrade", "ga_bot_wait", "ga_bot_death_place", "ga_bot_fight_stats", "ga_bot_foe_cost",
            "ga_bot_budget", "ga_bot_fight_profile", "ga_bot_fight_option", "ga_bot_pair_share" };
        for (char const* table : byBot)
            CharacterDatabase.Execute("DELETE FROM {} WHERE bot_guid = {}", table, low);
        CharacterDatabase.Execute("DELETE FROM ga_bot_squad WHERE leader_guid = {}", low);
        CharacterDatabase.Execute("DELETE FROM ga_character_setting WHERE character_guid = {}", low);
        CharacterDatabase.Execute("DELETE FROM ga_storage_origin WHERE character_guid = {}", low);
        CharacterDatabase.Execute("DELETE FROM ga_unworn_loot WHERE character_guid = {}", low);
        CharacterDatabase.Execute("DELETE FROM ga_dungeon_group_member WHERE member_guid = {}", low);
    }

    std::string DescribeMoved(std::map<std::string, uint32> const& moved)
    {
        std::string text;
        uint32 shown = 0;
        for (auto const& [name, count] : moved)
        {
            if (shown == 8)
            {
                text += Acore::StringFormat(" and {} more kinds", moved.size() - shown);
                break;
            }
            text += (shown ? ", " : "") + Acore::StringFormat("{} x {}", count, name);
            ++shown;
        }
        return text;
    }
}

RosterResult RosterService::Dismiss(AdventureGuild& guild, ObjectGuid guid, std::function<void(RosterResult const&)> done)
{
    GuildBot* bot = guild.FindBot(guid);
    if (!bot)
        return Fail(RosterError::UnknownBot, "That character is not one of your guild bots.");
    std::string const name = bot->name;
    if (_pendingDismiss.count(guid))
        return Fail(RosterError::Busy, Acore::StringFormat("{} is being dismissed already.", name));
    if (sGuildBotHolder.IsLoggingIn(guid))
        return Fail(RosterError::Busy, Acore::StringFormat("{} is logging in: try again in a moment.", name));
    if (sGuildBotHolder.IsWatched(guid) || (sGuildBotHolder.IsManaged(guid) && !sGuildBotHolder.IsAIDriven(guid)))
        return Fail(RosterError::Busy, Acore::StringFormat("You play {} right now: take another character first.", name));
    // On a dungeon run: its own (it leads it) or its squad leader's (OrderService::IsOnDungeonRun, which is private).
    ObjectGuid const runLeader = bot->order.type == OrderType::Dungeon ? guid : sSquadService.GetLeader(guid);
    GuildBot const* runLead = runLeader.IsEmpty() ? nullptr : guild.FindBot(runLeader);
    if (runLead && runLead->order.type == OrderType::Dungeon)
        return Fail(RosterError::Busy, Acore::StringFormat("{} is on a dungeon run: stop the run first.", name));

    if (Player* player = GetBotPlayer(guid); player && sGuildBotHolder.IsAIDriven(guid))
    {
        RosterResult const result = FinishDismiss(guild, guid, player);
        if (done)
            done(result);
        return result;
    }

    // Not in the world: it logs in to leave its things to the guild (a benched bot too).
    LoginResult const login = sGuildBotHolder.Login(guid, guild.ownerAccount, ObjectGuid::Empty);
    if (login != LoginResult::Started)
        return Fail(RosterError::Internal, Acore::StringFormat("{} could not log in to leave its things to the guild ({}).", name,
            ToString(login)));
    PendingDismiss& pending = _pendingDismiss[guid];
    pending.ownerAccount = guild.ownerAccount;
    pending.since = getMSTime();
    pending.done = std::move(done);
    LOG_INFO("module.ga.activity", "activity: {} logs in to be dismissed: it leaves its things to the guild first", name);
    RosterResult started;
    started.bot = guid;
    started.message = Acore::StringFormat("{} logs in to leave its things to the guild, then it is dismissed.", name);
    return started;
}

void RosterService::UpdateDismissals()
{
    if (_pendingDismiss.empty())
        return;
    std::vector<ObjectGuid> guids;
    for (auto const& [guid, pending] : _pendingDismiss)
        guids.push_back(guid);
    for (ObjectGuid const& guid : guids)
    {
        auto itr = _pendingDismiss.find(guid);
        if (itr == _pendingDismiss.end())
            continue;
        PendingDismiss const pending = itr->second;
        AdventureGuild* guild = sGuildAdventureMgr.FindGuild(pending.ownerAccount);
        GuildBot const* bot = guild ? guild->FindBot(guid) : nullptr;
        RosterResult result;
        bool finished = true;
        if (!bot)
            result = Fail(RosterError::UnknownBot, "That character is not one of your guild bots any more.");
        else if (Player* player = GetBotPlayer(guid); player && sGuildBotHolder.IsAIDriven(guid))
            result = FinishDismiss(*guild, guid, player);
        else if (!sGuildBotHolder.IsManaged(guid))
            result = Fail(RosterError::Internal, Acore::StringFormat("{} could not log in to leave its things to the guild: "
                "it is not dismissed.", bot->name));
        else if (GetMSTimeDiffToNow(pending.since) > DismissLoginTimeoutMs)
            result = Fail(RosterError::Internal, Acore::StringFormat("{} did not come into the world to leave its things to the "
                "guild: it is not dismissed.", bot->name));
        else
            finished = false;
        if (!finished)
            continue;
        _pendingDismiss.erase(guid);
        if (!result)
        {
            LOG_INFO("module.ga.activity", "activity: a dismissal failed: {}", result.message);
            if (guild)
                PushNotice(guild->ownerAccount, result.message, true);
        }
        if (pending.done)
            pending.done(result);
    }
}

RosterResult RosterService::FinishDismiss(AdventureGuild& guild, ObjectGuid guid, Player* player)
{
    GuildBot* bot = guild.FindBot(guid);
    if (!bot || !player)
        return Fail(RosterError::UnknownBot, "That character is not one of your guild bots.");
    std::string const name = bot->name;
    if (!sGuildBotHolder.IsAIDriven(guid))
        return Fail(RosterError::Busy, Acore::StringFormat("{} is not played by the AI right now: it is not dismissed.", name));
    uint8 const level = player->GetLevel();
    uint32 const owner = guild.ownerAccount;
    uint32 botAccount = sCharacterCache->GetCharacterAccountIdByGuid(guid);
    if (!botAccount)
        botAccount = bot->botAccount;

    // What the guild storage takes: the pieces of its role store first (no items in the world: by kind), then every
    // item it has that is not soulbound.
    std::map<std::string, uint32> moved;
    uint32 items = 0;
    if (std::vector<RolePiece> const* store = sRoleService.GetStore(guid))
        for (RolePiece const& piece : *store)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(piece.entry);
            if (!proto || (piece.flags & ITEM_FIELD_FLAG_SOULBOUND) || StorageService::RefuseReason(proto) ||
                piece.enchantments.find_first_of("123456789") != std::string::npos)
                continue;
            if (sStorageService.Add(owner, proto, piece.randomProperty, 1))
            {
                moved[proto->Name1] += 1;
                ++items;
            }
        }
    uint32 kept = 0;
    for (Item* item : AllItemsOf(player))
    {
        std::string const itemName = item->GetTemplate()->Name1;
        std::string error;
        if (uint32 const count = sStorageService.DepositDismissed(player, item, error))
        {
            moved[itemName] += count;
            items += count;
        }
        else
            ++kept;
    }

    // Its gold into the guild bank (with no WoW guild it stays with the character and goes with it).
    uint32 const money = player->GetMoney();
    bool moneyMoved = false;
    if (money && GuildBank::Deposit(owner, money, Acore::StringFormat("the gold of {}, who was dismissed", name)))
    {
        player->ModifyMoney(-int32(std::min<uint32>(money, uint32(std::numeric_limits<int32>::max()))));
        moneyMoved = true;
    }
    // The storage's rows now, before the character goes: a server killed in between keeps what went to the guild.
    sStorageService.FlushNow();

    std::string message = Acore::StringFormat("{} (level {}) was dismissed: ", name, level);
    message += items ? Acore::StringFormat("{} items into the guild storage", items) : std::string("nothing for the guild storage");
    if (moneyMoved)
        message += ", " + GuildBudget::MoneyText(money) + " into the guild bank";
    message += ".";
    LOG_INFO("module.ga.activity", "activity: {} was dismissed (level {}): into the guild storage {}; {} into the guild bank; "
        "{} items (soulbound, quest items and the like) go with the character", name, level,
        moved.empty() ? std::string("nothing") : DescribeMoved(moved), moneyMoved ? GuildBudget::MoneyText(money) : std::string("no gold"), kept);

    // Gone from the module, then from the world, then for good (the slot is free once its row is gone; renown and
    // unlocks are the guild's and stay).
    if (!sGuildBotHolder.Logout(guid)) // saves the character without what went to the guild
    {
        LOG_ERROR("module", "mod-guild-adventure: {} could not be logged out to be dismissed: the character stays", name);
        return Fail(RosterError::Internal, Acore::StringFormat("{} could not be logged out: it is not dismissed (what went to the "
            "guild stays there).", name));
    }
    sOrderService.Forget(guid);
    sSquadService.Forget(guid);
    sRoleService.Forget(guid);
    sBotSettings.ClearAll(owner, guid);
    guild.bots.erase(guid);
    sScriptMgr->OnPlayerDelete(guid, botAccount);
    Player::DeleteFromDB(guid.GetCounter(), botAccount, true, true);
    DeleteModuleRows(guid);

    PushBotRemoved(guild, guid);
    PushGuildState(guild);
    RecordEvent(guild, "DISMISS", ObjectGuid::Empty, message, level);

    RosterResult ok;
    ok.bot = guid;
    ok.message = message;
    return ok;
}

// ---------------------------------------------------------------------------------------------------------------
// Recruitment
// ---------------------------------------------------------------------------------------------------------------

uint32 RosterService::GetPendingRecruits(uint32 ownerAccount) const
{
    uint32 count = 0;
    for (auto const& [guid, pending] : _pendingRecruits)
        if (pending.ownerAccount == ownerAccount)
            ++count;
    return count;
}

uint8 RosterService::FindFreeSlot(AdventureGuild const& guild) const
{
    for (uint8 slot = 1; slot <= guild.unlockedSlots; ++slot)
    {
        bool used = false;
        for (auto const& [guid, bot] : guild.bots)
            if (bot.slot == slot)
                used = true;
        for (auto const& [guid, pending] : _pendingRecruits)
            if (pending.ownerAccount == guild.ownerAccount && pending.slot == slot)
                used = true;
        if (!used)
            return slot;
    }
    return 0;
}

RosterResult RosterService::CheckRecruit(AdventureGuild const& guild, RecruitRequest& request) const
{
    if (!sProgressionService.HasFeature(guild, "recruit"))
        return Fail(RosterError::Locked, "Recruiting is not unlocked yet.");

    if (guild.UsedSlots() + GetPendingRecruits(guild.ownerAccount) >= guild.unlockedSlots || !FindFreeSlot(guild))
        return Fail(RosterError::NoSlot, Acore::StringFormat("All {} bot slots are taken.", guild.unlockedSlots));

    if (request.gender != GENDER_MALE && request.gender != GENDER_FEMALE)
        return Fail(RosterError::BadRequest, "Unknown gender.");

    ChrClassesEntry const* classEntry = sChrClassesStore.LookupEntry(request.classId);
    ChrRacesEntry const* raceEntry = sChrRacesStore.LookupEntry(request.race);
    if (!classEntry || !raceEntry || !sObjectMgr->GetPlayerInfo(request.race, request.classId))
        return Fail(RosterError::InvalidCombo, "That race and class cannot be combined.");

    uint32 const expansion = sWorld->getIntConfig(CONFIG_EXPANSION);
    if (raceEntry->expansion > expansion || classEntry->expansion > expansion)
        return Fail(RosterError::InvalidCombo, "That race or class needs a newer expansion.");

    if (((1 << (request.race - 1)) & sWorld->getIntConfig(CONFIG_CHARACTER_CREATING_DISABLED_RACEMASK)) ||
        ((1 << (request.classId - 1)) & sWorld->getIntConfig(CONFIG_CHARACTER_CREATING_DISABLED_CLASSMASK)))
        return Fail(RosterError::InvalidCombo, "That race or class is disabled on this server.");

    // D9: recruits are of the leader's faction.
    if (Player::TeamIdForRace(request.race) != guild.team)
        return Fail(RosterError::InvalidCombo, "Recruits must be of your faction.");

    if (request.classId == CLASS_DEATH_KNIGHT && !sProgressionService.IsDeathKnightUnlocked(guild))
        return Fail(RosterError::Locked, "Death Knights join once a guild member reached level 55.");

    if (!normalizePlayerName(request.name) || ObjectMgr::CheckPlayerName(request.name, true) != CHAR_NAME_SUCCESS)
        return Fail(RosterError::NameInvalid, "That name is not allowed.");

    if (sCharacterCache->GetCharacterGuidByName(request.name))
        return Fail(RosterError::NameTaken, Acore::StringFormat("The name {} is taken.", request.name));

    for (auto const& [guid, pending] : _pendingRecruits)
        if (pending.name == request.name)
            return Fail(RosterError::NameTaken, Acore::StringFormat("{} is being recruited already.", request.name));

    RosterResult ok;
    return ok;
}

RosterResult RosterService::Recruit(AdventureGuild& guild, RecruitRequest request, std::function<void(RosterResult const&)> done)
{
    if (RosterResult check = CheckRecruit(guild, request); !check)
        return check;

    std::string error;
    uint32 const botAccount = GetOrCreateBotAccount(guild.ownerAccount, error);
    if (!botAccount)
        return Fail(RosterError::Internal, "The bot account could not be created: " + error);

    uint8 const slot = FindFreeSlot(guild);

    WorldSession* session = CreateCreationSession(botAccount);
    std::shared_ptr<Player> player(new Player(session), [session](Player* ptr)
    {
        // As the core's character creation: only a fully created character needs the cleanups.
        if (ptr->HasAtLoginFlag(AT_LOGIN_FIRST))
            ptr->CleanupsBeforeDelete();
        delete ptr;
        delete session;
    });

    std::unique_ptr<CharacterCreateInfo> info = MakeCreateInfo(request);
    player->GetMotionMaster()->Initialize();
    if (!player->Create(sObjectMgr->GetGenerator<HighGuid::Player>().Generate(), info.get()))
        return Fail(RosterError::Internal, "The character could not be created.");

    player->setCinematic(1); // no intro cinematic
    player->SetAtLoginFlag(AT_LOGIN_FIRST);

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    player->SaveToDB(trans, true, false);
    // A death knight of a guild that skips Acherus is saved in its side's capital (DeathKnights.h).
    bool const placed = DeathKnights::PlaceRecruit(player.get(), guild.ownerAccount, trans);

    LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_REP_REALM_CHARACTERS);
    // The bots of that account, the recruits still being created on it, and this one.
    uint32 charactersOnAccount = sGuildAdventureMgr.CountBotsOnAccount(botAccount) + 1;
    for (auto const& [pendingGuid, other] : _pendingRecruits)
        if (other.botAccount == botAccount)
            ++charactersOnAccount;
    stmt->SetData(0, uint8(charactersOnAccount));
    stmt->SetData(1, botAccount);
    stmt->SetData(2, realm.Id.Realm);
    LoginDatabase.Execute(stmt);

    ObjectGuid const guid = player->GetGUID();
    PendingRecruit pending;
    pending.ownerAccount = guild.ownerAccount;
    pending.botAccount = botAccount;
    pending.slot = slot;
    pending.name = request.name;
    pending.done = std::move(done);
    _pendingRecruits[guid] = std::move(pending);

    _transactions.AddCallback(CharacterDatabase.AsyncCommitTransaction(trans)).AfterComplete(
        [this, player, guid, botAccount, slot, owner = guild.ownerAccount](bool success)
        {
            if (success)
            {
                sScriptMgr->OnPlayerCreate(player.get());
                sCharacterCache->AddCharacterCacheEntry(guid, botAccount, player->GetName(), player->getGender(), player->getRace(),
                    player->getClass(), player->GetLevel());
            }
            FinishRecruit(owner, guid, botAccount, slot, success);
        });

    LOG_INFO("module", "mod-guild-adventure: owner account {} recruits {} (race {}, class {}) on bot account {}, slot {}{}",
        guild.ownerAccount, request.name, request.race, request.classId, botAccount, slot,
        placed ? " (a death knight that skips Acherus: it begins in its side's capital)" : "");

    RosterResult started;
    started.bot = guid;
    return started;
}

void RosterService::FinishRecruit(uint32 ownerAccount, ObjectGuid guid, uint32 botAccount, uint8 slot, bool success)
{
    auto itr = _pendingRecruits.find(guid);
    if (itr == _pendingRecruits.end())
        return;

    PendingRecruit pending = std::move(itr->second);
    _pendingRecruits.erase(itr);

    AdventureGuild* guild = sGuildAdventureMgr.FindGuild(ownerAccount);
    CharacterCacheEntry const* cache = success ? sCharacterCache->GetCharacterCacheByGuid(guid) : nullptr;
    if (!guild || !cache)
    {
        LOG_ERROR("module", "mod-guild-adventure: recruiting {} for owner account {} failed (character not saved)", guid.ToString(), ownerAccount);
        if (pending.done)
            pending.done(Fail(RosterError::Internal, "The character could not be saved."));
        return;
    }

    GuildBot bot;
    bot.guid = guid;
    bot.botAccount = botAccount;
    bot.slot = slot;
    bot.state = BotState::Active;
    bot.autoAdvance = sBotSettings.GetForGuild(ownerAccount, Setting::AutoAdvanceDefault) != 0;
    bot.recruitedAt = Now();
    bot.order.type = OrderType::Auto; // D16: new recruits start on the autopilot
    bot.order.setAt = bot.recruitedAt;
    bot.name = cache->Name;
    bot.race = cache->Race;
    bot.classId = cache->Class;
    bot.gender = cache->Sex;
    bot.snapshot.level = cache->Level;
    sGuildAdventureMgr.AddBot(*guild, bot);

    if (guild->guildId)
        if (Guild* wowGuild = sGuildMgr->GetGuildById(guild->guildId))
            if (!wowGuild->AddMember(guid))
                LOG_WARN("module", "mod-guild-adventure: {} could not join the guild <{}>", bot.name, wowGuild->GetName());

    RecordEvent(*guild, "RECRUIT", guid, Acore::StringFormat("{} joined the guild.", bot.name), cache->Level);

    // The new bot starts playing at once while the owner is in the world.
    if (WorldSession* session = sWorldSessionMgr->FindSession(ownerAccount))
        if (Player* player = session->GetPlayer())
            if (player->IsInWorld())
                sGuildBotHolder.Login(guid, ownerAccount, player->GetGUID());

    RosterResult result;
    result.bot = guid;
    if (pending.done)
        pending.done(result);
}

uint32 RosterService::GetOrCreateBotAccount(uint32 ownerAccount, std::string& error)
{
    uint8 const maxCharacters = GetSettings().botAccountMaxCharacters;
    uint8 nextIndex = 1;
    for (BotAccount const& account : sGuildAdventureMgr.GetBotAccounts(ownerAccount))
    {
        uint32 pendingOnAccount = 0;
        for (auto const& [guid, pending] : _pendingRecruits)
            if (pending.botAccount == account.accountId)
                ++pendingOnAccount;

        if (sGuildAdventureMgr.CountBotsOnAccount(account.accountId) + pendingOnAccount < maxCharacters)
            return account.accountId;
        nextIndex = std::max<uint8>(nextIndex, account.poolIndex + 1);
    }

    return CreateBotAccount(ownerAccount, nextIndex, error);
}

uint32 RosterService::CreateBotAccount(uint32 ownerAccount, uint8 poolIndex, std::string& error)
{
    std::string const username = Acore::StringFormat("{}{:05}{:02}", GetSettings().botAccountPrefix, ownerAccount, poolIndex);
    if (username.size() > MAX_ACCOUNT_STR)
    {
        error = "account name too long";
        return 0;
    }

    if (AccountMgr::GetId(username))
    {
        // Not in ga_bot_account (that was checked by the caller), so it is not ours to use.
        error = Acore::StringFormat("an account named {} exists already", username);
        LOG_ERROR("module", "mod-guild-adventure: bot account name {} is taken by an account the module does not own", username);
        return 0;
    }

    // Synchronous on purpose (world thread, only when a recruit needs a new account): the id is needed at once.
    // Same rows as AccountMgr::CreateAccount writes. Plain SQL, because LOGIN_INS_ACCOUNT and
    // LOGIN_INS_REALM_CHARACTERS_INIT are prepared for the asynchronous connections only (a DirectExecute of them
    // asserts). The name is [A-Z0-9] by construction; salt and verifier go in as hex literals. The password is
    // random and not kept: nobody logs into a bot account (D11).
    std::string const password = RandomPassword();
    auto [salt, verifier] = Acore::Crypto::SRP6::MakeRegistrationData(username, password);
    LoginDatabase.DirectExecute(Acore::StringFormat("INSERT INTO account (username, salt, verifier, expansion, reg_mail, email, joindate) "
        "VALUES ('{}', X'{}', X'{}', {}, '', '', NOW())", username, ByteArrayToHexStr(salt), ByteArrayToHexStr(verifier),
        sWorld->getIntConfig(CONFIG_EXPANSION)));
    LoginDatabase.DirectExecute("INSERT INTO realmcharacters (realmid, acctid, numchars) SELECT realmlist.id, account.id, 0 "
        "FROM realmlist, account LEFT JOIN realmcharacters ON acctid = account.id WHERE acctid IS NULL");

    uint32 const accountId = AccountMgr::GetId(username);
    if (!accountId)
    {
        error = "account insert failed";
        return 0;
    }

    BotAccount account;
    account.accountId = accountId;
    account.ownerAccount = ownerAccount;
    account.poolIndex = poolIndex;
    account.createdAt = Now();
    sGuildAdventureMgr.AddBotAccount(account);

    LOG_INFO("module", "mod-guild-adventure: created bot account {} (id {}) for owner account {}", username, accountId, ownerAccount);
    return accountId;
}

// ---------------------------------------------------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------------------------------------------------

void RosterService::Update(uint32 diff)
{
    _transactions.ProcessReadyCallbacks();
    UpdateDismissals();

    _refreshTimer += diff;
    if (_refreshTimer >= RefreshIntervalMs)
    {
        _refreshTimer = 0;
        RefreshBotsInWorld();
    }

    constexpr uint32 RealGuildCheckMs = 30 * IN_MILLISECONDS;
    _realGuildTimer += diff;
    if (_realGuildTimer >= RealGuildCheckMs)
    {
        _realGuildTimer = 0;
        CheckRealGuilds();
    }
}

// playerbots keeps a list of the guilds a person leads and lets their bots run their whole AI; every other bot alone gets
// the share Playerbots.BotActiveAlone gives (10 %: thirty seconds of AI now and then) unless a real player stands near.
// It reads that list at its startup and once an hour, so the WoW guild the module founds for a new owner is not on it
// for up to an hour: the three recruits of a new guild stood still for minutes at a time, took no quest at givers they
// stood beside and did not fight what stood next to them, and played normally the moment their owner's client watched
// one of them (2026-11-24, a guild founded 40 minutes after a restart). The list is read again while one of the
// adventure guilds is missing from it (the DB row of a guild just founded needs a moment; a few tries at most).
void RosterService::CheckRealGuilds()
{
    constexpr uint8 MaxReloads = 5;
    bool reload = false;
    for (auto const& [owner, guild] : sGuildAdventureMgr.GetGuilds())
    {
        if (!guild.guildId || BotDriver::IsRealGuild(guild.guildId))
            continue;
        uint8& tries = _realGuildReloads[guild.guildId];
        if (tries >= MaxReloads)
            continue;
        if (!tries)
            LOG_INFO("module", "mod-guild-adventure: playerbots does not count the WoW guild {} of owner account {} as a "
                "player's guild yet (its list is read at startup and once an hour): its bots alone get only the AI share of "
                "Playerbots.BotActiveAlone; the list is read again", guild.guildId, owner);
        else if (tries + 1 == MaxReloads)
            LOG_WARN("module", "mod-guild-adventure: playerbots still does not count the WoW guild {} of owner account {} as a "
                "player's guild after {} reads of its list", guild.guildId, owner, MaxReloads);
        ++tries;
        reload = true;
    }
    if (reload)
        BotDriver::ReloadRealGuilds();
}

void RosterService::RefreshBotsInWorld()
{
    for (auto& [owner, guild] : sGuildAdventureMgr.GetGuilds())
    {
        Guild* wowGuild = guild.guildId ? sGuildMgr->GetGuildById(guild.guildId) : nullptr;
        for (auto& [guid, bot] : guild.bots)
        {
            Player* player = GetBotPlayer(guid);
            if (!player)
            {
                _guildChecked.erase(guid);
                continue;
            }

            bot.snapshot.level = player->GetLevel();
            bot.snapshot.xp = player->GetUInt32Value(PLAYER_XP);
            bot.snapshot.zoneId = player->GetZoneId();
            bot.snapshot.mapId = player->GetMapId();
            bot.snapshot.money = player->GetMoney();
            bot.snapshot.gearLevel = Readiness::GearLevel(player);
            bot.snapshot.skills = ProfessionSkillSummary(player);

            if (!wowGuild || !_guildChecked.insert(guid).second)
                continue;

            if (player->GetGuildId() == 0)
            {
                if (!wowGuild->AddMember(guid))
                    LOG_WARN("module", "mod-guild-adventure: {} could not join the guild <{}>", bot.name, wowGuild->GetName());
            }
            else if (player->GetGuildId() != guild.guildId)
                LOG_WARN("module", "mod-guild-adventure: guild bot {} is in another guild ({})", bot.name, player->GetGuildId());
        }
    }
}
