/*
 * PerkService implementation. See PerkService.h.
 */

#include "PerkService.h"
#include "Random.h"
#include "AchievementService.h"
#include "MilestoneService.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "GuildAdventureMgr.h"
#include "GuildAdventureSettings.h"
#include "GuildAdventureUtil.h"
#include "GuildEvents.h"
#include "GadvProtocol.h"
#include "Item.h"
#include "Log.h"
#include "LootService.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "OrderService.h"
#include "Player.h"
#include "PlayerTaxi.h"
#include "ProgressionService.h"
#include "GuildBank.h"
#include "GuildBudget.h"
#include "WorldPacket.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "StorageService.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Timer.h"
#include "Tokenize.h"
#include <algorithm>
#include <cmath>

using namespace GuildAdventure;

namespace
{
    constexpr uint32 SnapshotIntervalMs = 10 * IN_MILLISECONDS; // new recruits, swaps of the leader
    constexpr uint32 TickIntervalMs = 500;                       // instant flights, reagent fields
    constexpr uint32 RegenTickMs = IN_MILLISECONDS;
    constexpr uint8 RegenTicks = 10;                             // over 10 s
    constexpr uint32 RegenCooldownMs = 10 * IN_MILLISECONDS;

    struct KindKey
    {
        PerkKind kind;
        char const* key;
    };

    constexpr KindKey Keys[] =
    {
        { PerkKind::MoveSpeed,        "move_speed" },
        { PerkKind::Experience,       "experience" },
        { PerkKind::Gold,             "gold" },
        { PerkKind::ExtraMaterial,    "extra_material" },
        { PerkKind::KillRegen,        "kill_regen" },
        { PerkKind::NoReagents,       "no_reagents" },
        { PerkKind::InstantFlight,    "instant_flight" },
        { PerkKind::AllFlightPaths,   "all_flight_paths" },
        { PerkKind::MaterialsStorage, "materials_storage" },
        { PerkKind::DropChance,       "drop_chance" },
        { PerkKind::CraftSpeed,       "craft_speed" },
        { PerkKind::GatherSpeed,      "gather_speed" },
        { PerkKind::WaterBreathing,   "water_breathing" },
        { PerkKind::NoFallDamage,     "no_fall_damage" },
        { PerkKind::SharedFlightPaths, "shared_flight_paths" },
    };

    PerkKind KindOf(std::string const& key)
    {
        for (KindKey const& entry : Keys)
            if (key == entry.key)
                return entry.kind;
        return PerkKind::Count;
    }

    std::string FormatValue(float value)
    {
        if (std::fabs(value - std::round(value)) < 0.001f)
            return std::to_string(int32(std::lround(value)));
        return Acore::StringFormat("{:.1f}", value);
    }

    constexpr UnitMoveType SpeedTypes[] = { MOVE_RUN, MOVE_RUN_BACK, MOVE_SWIM, MOVE_SWIM_BACK, MOVE_FLIGHT, MOVE_FLIGHT_BACK };
}

char const* GuildAdventure::ToKey(PerkKind kind)
{
    for (KindKey const& entry : Keys)
        if (entry.kind == kind)
            return entry.key;
    return "";
}

float PerkTemplate::ValueAt(uint8 rank) const
{
    if (!rank || values.empty())
        return 0.0f;
    return values[std::min<std::size_t>(rank, values.size()) - 1];
}

std::string PerkTemplate::Describe(uint8 rank) const
{
    std::string text = description;
    std::size_t const at = text.find("{}");
    if (at != std::string::npos)
        text.replace(at, 2, FormatValue(ValueAt(rank)));
    return text;
}

PerkService& PerkService::Instance()
{
    static PerkService instance;
    return instance;
}

void PerkService::LoadTemplates()
{
    _templates.clear();
    if (QueryResult result = WorldDatabase.Query("SELECT id, perk_key, name, costs, vals, unit, description FROM ga_perk_template ORDER BY id"))
    {
        do
        {
            Field* fields = result->Fetch();
            PerkTemplate perk;
            perk.id = fields[0].Get<uint32>();
            perk.key = fields[1].Get<std::string>();
            perk.kind = KindOf(perk.key);
            perk.name = fields[2].Get<std::string>();
            std::string const costs = fields[3].Get<std::string>();
            std::string const values = fields[4].Get<std::string>();
            for (std::string_view token : Acore::Tokenize(costs, ',', false))
                if (Optional<uint32> cost = Acore::StringTo<uint32>(token))
                    perk.costs.push_back(*cost);
            for (std::string_view token : Acore::Tokenize(values, ',', false))
                if (Optional<float> value = Acore::StringTo<float>(token))
                    perk.values.push_back(*value);
            perk.unit = fields[5].Get<std::string>();
            perk.description = fields[6].Get<std::string>();

            if (perk.kind == PerkKind::Count)
            {
                LOG_ERROR("module", "mod-guild-adventure: ga_perk_template {} has the unknown key '{}', skipped", perk.id, perk.key);
                continue;
            }
            if (perk.costs.empty() || perk.costs.size() != perk.values.size())
            {
                LOG_ERROR("module", "mod-guild-adventure: ga_perk_template {} ({}) needs as many costs as values, skipped", perk.id, perk.key);
                continue;
            }
            _templates.push_back(std::move(perk));
        } while (result->NextRow());
    }
    _dirty = true;
    LOG_INFO("module", "mod-guild-adventure: loaded {} guild perks", _templates.size());
}

void PerkService::Load()
{
    _ranks.clear();
    _spent.clear();
    _granted.clear();
    uint32 count = 0;
    if (QueryResult result = CharacterDatabase.Query("SELECT owner_account, perk_id, perk_rank, spent FROM ga_guild_perk"))
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 const owner = fields[0].Get<uint32>();
            uint32 const perkId = fields[1].Get<uint32>();
            uint8 const rank = fields[2].Get<uint8>();
            if (!perkId)
            {
                _granted[owner] += fields[3].Get<uint32>(); // perk id 0: points a GM granted
                continue;
            }
            if (rank)
                _ranks[owner][perkId] = rank;
            _spent[owner] += fields[3].Get<uint32>();
            ++count;
        } while (result->NextRow());
    }
    _dirty = true;
    RebuildSnapshot();
    LOG_INFO("module", "mod-guild-adventure: loaded {} guild perk ranks", count);
}

PerkTemplate const* PerkService::FindTemplate(uint32 id) const
{
    for (PerkTemplate const& perk : _templates)
        if (perk.id == id)
            return &perk;
    return nullptr;
}

PerkTemplate const* PerkService::FindTemplate(std::string const& key) const
{
    for (PerkTemplate const& perk : _templates)
        if (perk.key == key)
            return &perk;
    return nullptr;
}

bool PerkService::IsAvailable(PerkKind kind)
{
    switch (kind)
    {
        case PerkKind::MoveSpeed:
        case PerkKind::CraftSpeed:
        case PerkKind::GatherSpeed:
        case PerkKind::WaterBreathing:
        case PerkKind::NoFallDamage:
#ifdef GUILDADVENTURE_CP8_PERK_HOOKS
            return true;
#else
            return false;
#endif
        case PerkKind::Count:
            return false;
        default:
            return true;
    }
}

uint8 PerkService::GetRank(uint32 owner, uint32 perkId) const
{
    auto itr = _ranks.find(owner);
    if (itr == _ranks.end())
        return 0;
    auto rank = itr->second.find(perkId);
    return rank != itr->second.end() ? rank->second : 0;
}

uint32 PerkService::GetEarned(AdventureGuild const& guild) const
{
    // The guild's WoW achievements and its tiered guild achievements (MilestoneService).
    return sAchievementService.GetPoints(guild) + sMilestoneService.GetPoints(guild);
}

uint32 PerkService::GetSpent(uint32 owner) const
{
    auto itr = _spent.find(owner);
    return itr != _spent.end() ? itr->second : 0;
}

uint32 PerkService::GetGranted(uint32 owner) const
{
    auto itr = _granted.find(owner);
    return itr != _granted.end() ? itr->second : 0;
}

uint32 PerkService::GetAvailable(AdventureGuild const& guild) const
{
    uint32 const earned = GetEarned(guild) + GetGranted(guild.ownerAccount);
    uint32 const spent = GetSpent(guild.ownerAccount);
    return earned > spent ? earned - spent : 0;
}

uint64 PerkService::GoldPerPoint() const
{
    return uint64(GetSettings().perksGoldPerPoint) * GOLD;
}

bool PerkService::Buy(AdventureGuild& guild, uint32 perkId, std::string& code, std::string& text, bool withGold)
{
    PerkTemplate const* perk = FindTemplate(perkId);
    if (!perk)
    {
        code = "UNKNOWN_PERK";
        text = "Unknown perk.";
        return false;
    }
    if (!IsAvailable(perk->kind))
    {
        code = "NOT_AVAILABLE";
        text = Acore::StringFormat("{} needs a newer server core (core patch CP-8).", perk->name);
        return false;
    }

    uint32 const owner = guild.ownerAccount;
    uint8 const rank = GetRank(owner, perk->id);
    if (rank >= perk->MaxRank())
    {
        code = "MAX_RANK";
        text = Acore::StringFormat("{} is at its highest rank.", perk->name);
        return false;
    }
    uint32 const cost = perk->costs[rank];
    uint32 const available = GetAvailable(guild);
    // Bought with gold (the owner, 2026-11-28: [quote left out]): the guild points are not touched, the gold is paid as an unlock is.
    std::string paid;
    if (withGold)
    {
        uint64 const price = uint64(cost) * GoldPerPoint();
        if (!price)
        {
            code = "NOT_AVAILABLE";
            text = "Perks cannot be bought with gold on this server.";
            return false;
        }
        UnlockPayFrom const from = sProgressionService.GetPayFrom(guild);
        uint64 const bank = from != UnlockPayFrom::Leader ? GuildBank::Money(guild.ownerAccount) : 0;
        Player* leader = from != UnlockPayFrom::Bank ? ObjectAccessor::FindPlayer(guild.leader) : nullptr;
        uint64 const purse = leader ? leader->GetMoney() : 0;
        uint64 const fromBank = std::min(bank, price);
        uint64 const fromLeader = price - fromBank;
        auto money = [](uint64 copper) { return GuildBudget::MoneyText(uint32(std::min<uint64>(copper, UINT32_MAX))); };
        if (fromLeader > purse || fromLeader > uint64(INT32_MAX))
        {
            code = "NOT_ENOUGH_GOLD";
            text = Acore::StringFormat("{} rank {} costs {}; {}.", perk->name, rank + 1, money(price), from == UnlockPayFrom::Leader
                ? (leader ? "the guild leader has " + money(purse) : std::string("the guild leader is not in the world"))
                : "the guild bank has " + money(bank) + (from == UnlockPayFrom::Bank ? std::string() : leader ? " and the guild leader " + money(purse) : std::string()));
            return false;
        }
        if (fromBank && !GuildBank::Withdraw(guild.ownerAccount, fromBank, Acore::StringFormat("bought the guild perk {} rank {}", perk->name, rank + 1)))
        {
            code = "NOT_ENOUGH_GOLD";
            text = "The guild bank could not pay (the adventure guild has no WoW guild bank).";
            return false;
        }
        if (fromLeader)
        {
            leader->ModifyMoney(-int32(fromLeader));
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            leader->SaveGoldToDB(trans);
            CharacterDatabase.CommitTransaction(trans);
        }
        paid = fromBank ? money(fromBank) + " from the guild bank" : std::string();
        if (fromLeader)
            paid += (paid.empty() ? "" : " and ") + money(fromLeader) + " from " + leader->GetName() + "'s purse";
    }
    else if (available < cost)
    {
        code = "NO_POINTS";
        text = Acore::StringFormat("{} rank {} costs {} guild points; the guild has {}.", perk->name, rank + 1, cost, available);
        return false;
    }

    uint8 const newRank = rank + 1;
    _ranks[owner][perk->id] = newRank;
    // (A rank bought with gold spends no guild points: ga_guild_perk.spent counts the points the perk took.)
    uint32 const pointsNow = withGold ? 0 : cost;
    _spent[owner] += pointsNow;
    uint32 spentOnPerk = pointsNow;
    if (QueryResult result = CharacterDatabase.Query("SELECT spent FROM ga_guild_perk WHERE owner_account = {} AND perk_id = {}", owner, perk->id))
        spentOnPerk += result->Fetch()[0].Get<uint32>();
    CharacterDatabase.Execute("REPLACE INTO ga_guild_perk (owner_account, perk_id, perk_rank, spent, updated_at) VALUES ({}, {}, {}, {}, {})",
        owner, perk->id, uint32(newRank), spentOnPerk, Now());

    RecordEvent(guild, "PERK", ObjectGuid::Empty, perk->MaxRank() > 1 ?
        Acore::StringFormat("The guild bought {} rank {}: {}.", perk->name, newRank, perk->Describe(newRank)) :
        Acore::StringFormat("The guild bought {}: {}.", perk->name, perk->Describe(newRank)), newRank, 0, 0);
    if (withGold)
        LOG_INFO("module.ga.activity", "activity: adventure guild of account {} bought the guild perk {} rank {} with gold ({})", owner,
            perk->name, newRank, paid);
    else
        LOG_INFO("module", "mod-guild-adventure: account {} bought guild perk {} rank {} for {} points", owner, perk->key, newRank, cost);

    _dirty = true;
    RebuildSnapshot();
    ApplyToGuild(guild);
    PushGuildState(guild);
    sGadvProtocol.PushPerks(guild);
    return true;
}

void PerkService::Reset(AdventureGuild& guild)
{
    uint32 const owner = guild.ownerAccount;
    _ranks.erase(owner);
    _spent.erase(owner);
    _granted.erase(owner);
    CharacterDatabase.Execute("DELETE FROM ga_guild_perk WHERE owner_account = {}", owner);
    RecordEvent(guild, "PERK", ObjectGuid::Empty, "The guild's perks were reset; every point is back.");
    _dirty = true;
    RebuildSnapshot();
    ApplyToGuild(guild);
    PushGuildState(guild);
    sGadvProtocol.PushPerks(guild);
}

void PerkService::Grant(AdventureGuild& guild, uint32 points)
{
    uint32 const owner = guild.ownerAccount;
    _granted[owner] += points;
    CharacterDatabase.Execute("REPLACE INTO ga_guild_perk (owner_account, perk_id, perk_rank, spent, updated_at) VALUES ({}, 0, 0, {}, {})",
        owner, _granted[owner], Now());
    PushGuildState(guild);
    sGadvProtocol.PushPerks(guild);
}

std::vector<ObjectGuid> PerkService::Members(AdventureGuild const& guild) const
{
    std::vector<ObjectGuid> members;
    members.reserve(guild.bots.size() + 1);
    if (guild.leader)
        members.push_back(guild.leader);
    for (auto const& [guid, bot] : guild.bots)
        members.push_back(guid);
    return members;
}

void PerkService::RebuildSnapshot()
{
    std::unordered_map<ObjectGuid, PerkEffects> snapshot;
    for (auto& [owner, guild] : sGuildAdventureMgr.GetGuilds())
    {
        PerkEffects effects;
        auto ranks = _ranks.find(owner);
        if (ranks != _ranks.end())
            for (auto const& [perkId, rank] : ranks->second)
                if (PerkTemplate const* perk = FindTemplate(perkId))
                    if (IsAvailable(perk->kind))
                        effects.value[std::size_t(perk->kind)] = perk->ValueAt(rank);
        for (ObjectGuid member : Members(guild))
            snapshot[member] = effects;
    }

    std::unique_lock<std::shared_mutex> lock(_snapshotLock);
    _snapshot.swap(snapshot);
    _dirty = false;
}

PerkEffects PerkService::GetEffects(ObjectGuid character) const
{
    std::shared_lock<std::shared_mutex> lock(_snapshotLock);
    auto itr = _snapshot.find(character);
    return itr != _snapshot.end() ? itr->second : PerkEffects();
}

float PerkService::GetValue(ObjectGuid character, PerkKind kind) const
{
    std::shared_lock<std::shared_mutex> lock(_snapshotLock);
    auto itr = _snapshot.find(character);
    return itr != _snapshot.end() ? itr->second.Get(kind) : 0.0f;
}

bool PerkService::IsCraftSpell(SpellInfo const* spellInfo)
{
    return spellInfo && spellInfo->HasAttribute(SPELL_ATTR0_IS_TRADESKILL);
}

bool PerkService::IsGatherSpell(SpellInfo const* spellInfo)
{
    if (!spellInfo)
        return false;
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        SpellEffectInfo const& effect = spellInfo->Effects[i];
        if (effect.Effect == SPELL_EFFECT_SKINNING)
            return true;
        if (effect.Effect == SPELL_EFFECT_OPEN_LOCK && (effect.MiscValue == LOCKTYPE_MINING || effect.MiscValue == LOCKTYPE_HERBALISM))
            return true;
    }
    return false;
}

bool PerkService::IsMaterial(ItemTemplate const* proto)
{
    return proto && proto->Class == ITEM_CLASS_TRADE_GOODS;
}

void PerkService::Apply(Player* player, PerkEffects const& effects)
{
    if (!player || !player->IsInWorld())
        return;

    // The speed hook (CP-8) reads the snapshot; the rates are recalculated now, not at the next aura change.
    for (UnitMoveType type : SpeedTypes)
        player->UpdateSpeed(type, true);

    if (effects.Has(PerkKind::AllFlightPaths))
    {
        TeamId const team = player->GetTeamId();
        uint32 learned = 0;
        for (uint32 node = 1; node < sTaxiNodesStore.GetNumRows(); ++node)
            if (sTaxiNodesStore.LookupEntry(node) && sObjectMgr->GetTaxiMountDisplayId(node, team) &&
                !player->m_taxi.IsTaximaskNodeKnown(node) && player->m_taxi.SetTaximaskNode(node))
                ++learned;
        if (learned && GetSettings().debug)
            LOG_INFO("module", "mod-guild-adventure: {} knows {} more flight paths (guild perk)", player->GetName(), learned);
    }
    if (effects.Has(PerkKind::SharedFlightPaths))
        if (AdventureGuild* guild = sAchievementService.FindGuildOf(player->GetGUID()))
            if (uint32 const learned = ShareFlightPaths(*guild, player))
                LOG_INFO("module.ga.activity", "activity: {} learned {} flight paths its guild knows (guild perk Shared Maps)",
                    player->GetName(), learned);
}

// Guild perk Shared Maps (the owner, 2026-11-28: [quote left out]): a flight path one character of the guild knows, every
// character of the guild of that side knows. Only with the perk (the callers check it).
std::set<uint32>& PerkService::GuildNodes(AdventureGuild const& guild)
{
    auto [itr, inserted] = _guildNodes.try_emplace(guild.ownerAccount);
    if (inserted)
    {
        std::string list;
        for (ObjectGuid member : Members(guild))
            list += (list.empty() ? "" : ",") + std::to_string(member.GetCounter());
        if (!list.empty())
            if (QueryResult result = CharacterDatabase.Query("SELECT taximask FROM characters WHERE guid IN ({})", list))
                do
                {
                    PlayerTaxi taxi;
                    taxi.LoadTaxiMask(result->Fetch()[0].Get<std::string>());
                    for (uint32 node = 1; node < sTaxiNodesStore.GetNumRows(); ++node)
                        if (taxi.IsTaximaskNodeKnown(node))
                            itr->second.insert(node);
                } while (result->NextRow());
    }
    return itr->second;
}

uint32 PerkService::ShareFlightPaths(AdventureGuild const& guild, Player* player)
{
    std::set<uint32>& nodes = GuildNodes(guild);
    for (uint32 node = 1; node < sTaxiNodesStore.GetNumRows(); ++node)
        if (player->m_taxi.IsTaximaskNodeKnown(node))
            nodes.insert(node);
    TeamId const team = player->GetTeamId();
    uint32 learned = 0;
    for (uint32 node : nodes)
        if (sObjectMgr->GetTaxiMountDisplayId(node, team) && !player->m_taxi.IsTaximaskNodeKnown(node) &&
            player->m_taxi.SetTaximaskNode(node))
            ++learned;
    return learned;
}

void PerkService::OnTaxiNodeLearned(ObjectGuid character, uint32 node)
{
    if (!Has(character, PerkKind::SharedFlightPaths))
        return;
    AdventureGuild* guild = sAchievementService.FindGuildOf(character);
    if (!guild)
        return;
    GuildNodes(*guild).insert(node);
    uint32 told = 0;
    for (ObjectGuid member : Members(*guild))
    {
        if (member == character)
            continue;
        Player* other = ObjectAccessor::FindPlayer(member);
        if (!other || !other->IsInWorld() || !sObjectMgr->GetTaxiMountDisplayId(node, other->GetTeamId()) ||
            other->m_taxi.IsTaximaskNodeKnown(node) || !other->m_taxi.SetTaximaskNode(node))
            continue;
        ++told;
        if (WorldSession* session = other->GetSession())
        {
            WorldPacket msg(SMSG_NEW_TAXI_PATH, 0);
            session->SendPacket(&msg);
        }
    }
    Player const* finder = ObjectAccessor::FindPlayer(character);
    TaxiNodesEntry const* entry = sTaxiNodesStore.LookupEntry(node);
    LOG_INFO("module.ga.activity", "activity: {} discovered the flight path {}: {} more characters of its guild in the world know it "
        "now (guild perk Shared Maps)", finder ? finder->GetName() : character.ToString(), entry ? entry->name[0] : "?", told);
}

void PerkService::ApplyToGuild(AdventureGuild const& guild)
{
    for (ObjectGuid member : Members(guild))
        if (Player* player = ObjectAccessor::FindPlayer(member))
            Apply(player, GetEffects(member));
    TickReagentFields();
}

void PerkService::OnCharacterInWorld(ObjectGuid character)
{
    if (!sAchievementService.FindGuildOf(character))
        return;
    if (_dirty)
        RebuildSnapshot();
    Apply(ObjectAccessor::FindPlayer(character), GetEffects(character));
}

void PerkService::OnKill(ObjectGuid character)
{
    float const pct = GetValue(character, PerkKind::KillRegen);
    if (pct <= 0.0f)
        return;
    uint32 const now = getMSTime();
    Regen& regen = _regen[character];
    if (now < regen.cooldownUntil)
        return; // 10 s cooldown
    regen.cooldownUntil = now + RegenCooldownMs;
    regen.ticksLeft = RegenTicks;
    regen.pctPerTick = pct / RegenTicks;
    if (GetSettings().debug)
        if (Player* player = ObjectAccessor::FindPlayer(character))
            LOG_INFO("module", "mod-guild-adventure: {} regenerates {}% health and mana over 10 s after a kill (guild perk; "
                "health {}/{})", player->GetName(), pct, player->GetHealth(), player->GetMaxHealth());
}

void PerkService::OnLooted(ObjectGuid character, uint32 itemEntry)
{
    float const pct = GetValue(character, PerkKind::ExtraMaterial);
    if (pct <= 0.0f)
        return;
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemEntry);
    if (!IsMaterial(proto) || !roll_chance_f(pct))
        return;
    Player* player = ObjectAccessor::FindPlayer(character);
    if (!player || !player->IsInWorld())
        return;

    // One more of what was looted; it goes through the loot filter like the loot itself.
    ItemPosCountVec dest;
    if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemEntry, 1) == EQUIP_ERR_OK)
    {
        if (Item* item = player->StoreNewItem(dest, itemEntry, true, Item::GenerateItemRandomPropertyId(itemEntry)))
        {
            player->SendNewItem(item, 1, true, false);
            sLootService.OnLooted(player, item);
        }
    }
    else if (uint32 const owner = sStorageService.GetOwnerOf(player))
        sStorageService.Add(owner, proto, 0, 1);

    if (GetSettings().debug)
        LOG_INFO("module", "mod-guild-adventure: {} found one more {} (guild perk)", player->GetName(), proto->Name1);
}

void PerkService::TickRegen(uint32 /*now*/)
{
    for (auto itr = _regen.begin(); itr != _regen.end();)
    {
        Regen& regen = itr->second;
        if (regen.ticksLeft)
        {
            Player* player = ObjectAccessor::FindPlayer(itr->first);
            if (!player || !player->IsInWorld() || !player->IsAlive())
                regen.ticksLeft = 0;
            else
            {
                --regen.ticksLeft;
                if (player->GetHealth() < player->GetMaxHealth())
                    player->ModifyHealth(std::max<int32>(1, int32(player->GetMaxHealth() * regen.pctPerTick / 100.0f)));
                uint32 const maxMana = player->GetMaxPower(POWER_MANA);
                if (maxMana && player->GetPower(POWER_MANA) < maxMana)
                    player->ModifyPower(POWER_MANA, std::max<int32>(1, int32(maxMana * regen.pctPerTick / 100.0f)));
            }
        }
        // Forget characters whose cooldown ran out long ago.
        if (!regen.ticksLeft && getMSTime() > regen.cooldownUntil + RegenCooldownMs)
            itr = _regen.erase(itr);
        else
            ++itr;
    }
}

void PerkService::TickFlights()
{
    std::vector<ObjectGuid> flyers;
    {
        std::shared_lock<std::shared_mutex> lock(_snapshotLock);
        for (auto const& [guid, effects] : _snapshot)
            if (effects.Has(PerkKind::InstantFlight))
                flyers.push_back(guid);
    }

    for (ObjectGuid guid : flyers)
    {
        Player* player = ObjectAccessor::FindPlayer(guid);
        if (!player || !player->IsInWorld() || !player->IsInFlight() || player->IsBeingTeleported() || player->m_taxi.empty())
            continue;
        // Flights a spell started (quest rides with events on the way) keep flying: only flight masters have a faction.
        if (!player->m_taxi.GetFlightMasterFactionTemplate())
            continue;
        TaxiNodesEntry const* landing = sTaxiNodesStore.LookupEntry(player->m_taxi.GetPath().back());
        if (!landing)
            continue;
        uint32 const from = player->m_taxi.GetTaxiSource();
        if (player->TeleportTo(landing->map_id, landing->x, landing->y, landing->z, player->GetOrientation()))
        {
            sOrderService.OnModuleTeleport(guid);
            if (GetSettings().debug)
                LOG_INFO("module", "mod-guild-adventure: {} flew from node {} to node {} at once (guild perk)", player->GetName(), from,
                    landing->ID);
        }
    }
}

void PerkService::TickReagentFields()
{
    std::vector<std::pair<ObjectGuid, bool>> members;
    {
        std::shared_lock<std::shared_mutex> lock(_snapshotLock);
        for (auto const& [guid, effects] : _snapshot)
            members.emplace_back(guid, effects.Has(PerkKind::NoReagents));
    }

    for (auto const& [guid, freeReagents] : members)
    {
        bool const wasSet = _reagentFieldsSet.count(guid) != 0;
        if (!freeReagents && !wasSet)
            continue;
        Player* player = ObjectAccessor::FindPlayer(guid);
        if (!player || !player->IsInWorld())
            continue;
        // The spell families' "no reagent" masks (an aura sets them for some spells; the core and the client both read
        // them): all bits set = every class spell, never a tradeskill recipe (family 0, no flags).
        uint32 const wanted = freeReagents ? 0xFFFFFFFF : 0;
        for (uint16 field = PLAYER_NO_REAGENT_COST_1; field < PLAYER_NO_REAGENT_COST_1 + 3; ++field)
            if (player->GetUInt32Value(field) != wanted)
                player->SetUInt32Value(field, wanted);
        if (freeReagents)
            _reagentFieldsSet[guid] = true;
        else
            _reagentFieldsSet.erase(guid);
    }
}

void PerkService::Update(uint32 diff)
{
    _snapshotTimer += diff;
    if (_dirty || _snapshotTimer >= SnapshotIntervalMs)
    {
        _snapshotTimer = 0;
        RebuildSnapshot();
    }

    _regenTimer += diff;
    if (_regenTimer >= RegenTickMs)
    {
        _regenTimer = 0;
        TickRegen(getMSTime());
    }

    _tickTimer += diff;
    if (_tickTimer >= TickIntervalMs)
    {
        _tickTimer = 0;
        TickFlights();
        TickReagentFields();
    }
}
