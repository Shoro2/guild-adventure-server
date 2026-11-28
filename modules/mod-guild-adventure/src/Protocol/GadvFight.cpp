/*
 * The GADV part of the fight profiles (docs/protocol.md "Fight profiles", docs/fight-profiles.md, package FP-2: the bot
 * detail's Combat sub-tab). Members of GadvProtocol, kept apart from GadvProtocol.cpp.
 */

#include "GadvProtocol.h"
#include "Bag.h"
#include "DBCStores.h"
#include "FightProfiles.h"
#include "FightTriggerTexts.h"
#include "GuildAdventureMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <optional>
#include <unordered_map>
#include <unordered_set>

using namespace GuildAdventure;
using GuildAdventure::Gadv::Message;
namespace FP = GuildAdventure::FightProfiles;

namespace
{
    std::string Decimal(float value)
    {
        return Acore::StringFormat("{:.1f}", value);
    }

    std::string ConditionsText(std::vector<FP::Condition> const& conditions)
    {
        std::string text;
        for (FP::Condition const& condition : conditions)
            text += (text.empty() ? "" : "; ") + FP::DescribeCondition(condition);
        return text;
    }

    // The spells a character knows by their lowercase names (mod-playerbots names its cast actions so): a row of
    // mod-playerbots gets the spell it casts as its ref, so that the addon shows the spell's icon.
    std::unordered_map<std::string, uint32> KnownSpells(Player* player)
    {
        std::unordered_map<std::string, uint32> spells;
        if (!player)
            return spells;
        for (auto const& [spellId, state] : player->GetSpellMap())
        {
            if (!state || state->State == PLAYERSPELL_REMOVED || !state->Active)
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info || !info->SpellName[0] || !*info->SpellName[0])
                continue;
            std::string name = info->SpellName[0];
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            spells.emplace(std::move(name), spellId);
        }
        return spells;
    }

    std::string Lower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return text;
    }

    // The tab of the spellbook a class spell stands on: the class skill line it is learned under ("Enhancement",
    // "Elemental Combat"), else "General".
    std::string SpellBookTab(uint32 spellId)
    {
        SkillLineAbilityMapBounds const bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        for (auto itr = bounds.first; itr != bounds.second; ++itr)
            if (SkillLineEntry const* line = sSkillLineStore.LookupEntry(itr->second->SkillLine))
                if (line->categoryId == SKILL_CATEGORY_CLASS && line->name[0] && *line->name[0])
                    return line->name[0];
        return "General";
    }

    // The spells a class can learn by their lowercase names (the first rank of each chain on a class skill line), per
    // spell family; made once on the world thread.
    std::unordered_map<std::string, uint32> const& ClassSpells(uint32 family)
    {
        static std::unordered_map<uint32, std::unordered_map<std::string, uint32>> byFamily;
        auto [it, made] = byFamily.try_emplace(family);
        if (!made)
            return it->second;
        for (uint32 id = 1; id < sSpellMgr->GetSpellInfoStoreSize(); ++id)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
            if (!info || info->SpellFamilyName != family || info->IsPassive() || !info->SpellName[0] || !*info->SpellName[0])
                continue;
            if (sSpellMgr->GetFirstSpellInChain(id) != id || SpellBookTab(id) == "General")
                continue;
            it->second.emplace(Lower(info->SpellName[0]), id);
        }
        return it->second;
    }

    // The spell an action of mod-playerbots casts, by its name: "wind shear on enemy healer" casts Wind Shear (the
    // longest spell name its name begins with). The spells the bot knows first, then those its class can learn.
    uint32 SpellOfAction(std::string const& action, std::unordered_map<std::string, uint32> const& known,
                         std::unordered_map<std::string, uint32> const& learnable)
    {
        for (auto const* names : { &known, &learnable })
        {
            if (auto it = names->find(action); it != names->end())
                return it->second;
            uint32 best = 0;
            std::size_t bestLength = 0;
            for (auto const& [name, id] : *names)
                if (name.size() > bestLength && action.size() > name.size() && action.compare(0, name.size(), name) == 0
                    && action[name.size()] == ' ')
                {
                    best = id;
                    bestLength = name.size();
                }
            if (best)
                return best;
        }
        return 0;
    }

    // A trigger's text in plain language (FightTriggerTexts.h), the class's own before the one of every class, with the
    // values of playerbots.conf filled in; empty when there is none.
    std::string TriggerText(uint8 classId, std::string const& name)
    {
        char const* text = nullptr;
        for (FightTriggerTexts::Entry const& entry : FightTriggerTexts::Entries)
            if (name == entry.name && (entry.classId == classId || (!entry.classId && !text)))
            {
                text = entry.text;
                if (entry.classId == classId)
                    break;
            }
        if (!text)
            return {};
        std::string out = text;
        std::pair<char const*, uint32> const values[] = {
            { "{LowHealth}", sPlayerbotAIConfig.LowHealth }, { "{CriticalHealth}", sPlayerbotAIConfig.CriticalHealth },
            { "{MediumHealth}", sPlayerbotAIConfig.MediumHealth }, { "{AlmostFullHealth}", sPlayerbotAIConfig.AlmostFullHealth },
            { "{LowMana}", sPlayerbotAIConfig.LowMana }, { "{MediumMana}", sPlayerbotAIConfig.MediumMana },
            { "{HighMana}", sPlayerbotAIConfig.HighMana } };
        for (auto const& [key, value] : values)
            for (std::size_t at = out.find(key); at != std::string::npos; at = out.find(key))
                out.replace(at, std::strlen(key), std::to_string(value));
        return out;
    }

    // What the addon needs of a rule. For a row of mod-playerbots the spell its action casts, so that the addon shows
    // the spell's icon; "kn" whether the bot knows the spell (only while it is in the world), "lv" from which level.
    Message RuleRecord(ObjectGuid bot, FP::Rule const& rule, std::unordered_map<std::string, uint32> const& spells,
                       uint8 classId, bool live)
    {
        uint32 ref = rule.ref;
        if ((rule.kind == FP::ActionKind::Row || rule.kind == FP::ActionKind::Action) && !ref)
            ref = SpellOfAction(rule.action, spells, ClassSpells(FP::ClassSpellFamily(classId)));
        Message record("FRULE");
        record.Set("g", bot.GetCounter()).Set("l", uint32(rule.list)).Set("k", uint32(rule.kind))
            .Set("ref", ref).Set("a", rule.action).Set("tg", rule.trigger).Set("tgt", uint32(rule.target))
            .Set("rel", Decimal(rule.relevance)).Set("en", rule.enabled).Set("fl", uint32(rule.flags))
            .Set("iv", uint32(rule.minInterval)).Set("c", FP::FormatConditions(rule.conditions))
            .Set("d", ConditionsText(rule.conditions)).Set("src", rule.source).Set("nd", uint32(rule.node));
        bool const spell = rule.kind == FP::ActionKind::Row || rule.kind == FP::ActionKind::Action || rule.kind == FP::ActionKind::Spell;
        if (spell && ref && live)
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(ref))
            {
                bool const known = info->SpellName[0] && spells.count(Lower(info->SpellName[0]));
                record.Set("kn", known);
                if (!known)
                    record.Set("lv", uint32(std::max(info->SpellLevel, info->BaseLevel)));
            }
        return record;
    }

    GuildBot const* BotOf(AdventureGuild& guild, Message const& message, ObjectGuid& guid)
    {
        std::optional<int64> const g = message.GetInt("g");
        guid = g ? ObjectGuid::Create<HighGuid::Player>(uint32(*g)) : ObjectGuid::Empty;
        return g ? guild.FindMember(guid) : nullptr;
    }

    Player* InWorld(ObjectGuid guid)
    {
        Player* player = ObjectAccessor::FindConnectedPlayer(guid);
        return player && player->IsInWorld() ? player : nullptr;
    }
}

// Block FPROFILES: the guild's fight profiles of a bot's class, with the bot's two slots.
void GadvProtocol::SendFightProfiles(uint32 accountId, AdventureGuild& guild, ObjectGuid bot)
{
    GuildBot const* member = guild.FindMember(bot);
    if (!member)
        return;
    Player* player = InWorld(bot);
    std::vector<Message> records;
    for (FP::StoredProfile const* profile : sFightProfileStore.ListFor(guild.ownerAccount, member->classId))
        records.push_back(Message("FPROF").Set("g", bot.GetCounter()).Set("id", profile->id).Set("n", profile->name)
            .Set("tab", uint32(profile->tab)).Set("rev", profile->revision)
            .Set("rc", profile->CountRules(FP::ListKind::Combat)).Set("rn", profile->CountRules(FP::ListKind::OutOfCombat))
            .Set("used", uint32(sFightProfileStore.UsedBy(profile->id).size())).Set("opts", uint32(profile->options.size())));
    // The templates of no guild (FP-4): read-only, a bot can be given one as it is; tr = its role (0 solo, 1 tank, 2 heal).
    for (FP::StoredProfile const* profile : sFightProfileStore.Templates(member->classId))
        records.push_back(Message("FPROF").Set("g", bot.GetCounter()).Set("id", profile->id).Set("n", profile->name)
            .Set("tab", uint32(profile->tab)).Set("rev", profile->revision)
            .Set("rc", profile->CountRules(FP::ListKind::Combat)).Set("rn", profile->CountRules(FP::ListKind::OutOfCombat))
            .Set("used", uint32(sFightProfileStore.UsedBy(profile->id).size())).Set("opts", uint32(profile->options.size()))
            .Set("tpl", true).Set("tr", uint32(sFightProfileStore.RoleOf(profile->id))));
    Message begin("FPROFILES_BEGIN");
    begin.Set("g", bot.GetCounter()).Set("cls", uint32(member->classId))
        .Set("solo", sFightProfileStore.Assigned(bot, FP::Slot::Solo)).Set("grp", sFightProfileStore.Assigned(bot, FP::Slot::Group))
        .Set("max", FP::MaxProfilesPerClass);
    // The template it plays by default with an empty slot (FC12): alone (dflt) and in a dungeon or raid group (dfltg).
    if (player)
        begin.Set("tab", uint32(FP::TalentTree(player))).Set("dflt", FP::DefaultFor(player, false))
            .Set("dfltg", FP::DefaultFor(player, true));
    SendBlock(accountId, "FPROFILES", records, begin);
}

// Blocks FPROFILE (its rules) and FOPTS (its options with the bot's values) of one profile; id 0 is the template of
// the bot (read from its strategies: only while it is in the world).
void GadvProtocol::SendFightProfile(uint32 accountId, AdventureGuild& guild, ObjectGuid bot, uint32 id)
{
    GuildBot const* member = guild.FindMember(bot);
    if (!member)
        return;
    Player* player = InWorld(bot);
    std::vector<Message> rules;
    std::vector<Message> options;
    Message begin("FPROFILE_BEGIN");
    begin.Set("g", bot.GetCounter()).Set("id", id).Set("live", player != nullptr);
    auto const spells = KnownSpells(player);
    if (!id)
    {
        begin.Set("pn", "Default (mod-playerbots)");
        if (player)
        {
            begin.Set("tab", uint32(FP::TalentTree(player)));
            uint32 hidden = 0;
            std::string replaced;
            for (uint8 engine = 0; engine < 2; ++engine)
            {
                FP::Played played;
                std::string why;
                if (!FP::Template(player, engine, played, why))
                    continue;
                for (FP::PlayedRule const& row : played.rules)
                {
                    if (row.hidden)
                        ++hidden;
                    else
                        rules.push_back(RuleRecord(bot, row.rule, spells, member->classId, player != nullptr));
                }
                if (!engine)
                    for (std::string const& name : played.replaced)
                        replaced += (replaced.empty() ? "" : ", ") + name;
            }
            begin.Set("rep", replaced).Set("hid", hidden);
        }
    }
    else
    {
        FP::StoredProfile const* profile = sFightProfileStore.Get(id);
        if (!profile || (profile->owner != guild.ownerAccount && !FP::Store::IsTemplate(*profile)))
            return;
        if (FP::Store::IsTemplate(*profile))
            begin.Set("tpl", true);
        begin.Set("pn", profile->name).Set("tab", uint32(profile->tab)).Set("rev", profile->revision).Set("note", profile->note)
            .Set("rep", profile->templateKey);
        for (FP::Rule const& rule : profile->rules)
            rules.push_back(RuleRecord(bot, rule, spells, member->classId, player != nullptr));
        for (FP::Option const& option : profile->options)
        {
            float own = 0.0f;
            bool const has = sFightProfileStore.BotOption(bot, id, option.key, own);
            options.push_back(Message("FOPT").Set("g", bot.GetCounter()).Set("id", id).Set("k", option.key).Set("n", option.label)
                .Set("kind", uint32(option.kind)).Set("v", Decimal(option.value)).Set("min", Decimal(option.min))
                .Set("max", Decimal(option.max)).Set("own", has).Set("bv", Decimal(has ? own : option.value)));
        }
    }
    SendBlock(accountId, "FPROFILE", rules, begin);
    SendBlock(accountId, "FOPTS", options, Message("FOPTS_BEGIN").Set("g", bot.GetCounter()).Set("id", id));
}

// FIGHT_ASSIGN g, slot (0 solo, 1 group), id (0 = the default): the profile of one of a bot's slots. With `to` (a list
// of guild bots of its class) instead: those bots get its two assignments and its own option values ("Copy to bots").
void GadvProtocol::OnFightAssign(uint32 accountId, AdventureGuild& guild, Message const& message)
{
    std::optional<int64> const r = message.GetInt("r");
    ObjectGuid guid;
    GuildBot const* bot = BotOf(guild, message, guid);
    if (!bot)
    {
        Error(accountId, r, "NOT_OWNER", "That character is not one of your guild bots.");
        return;
    }
    std::string why;
    if (std::optional<std::vector<int64>> const to = message.GetIntList("to"))
    {
        uint32 copied = 0;
        for (int64 low : *to)
        {
            ObjectGuid const target = ObjectGuid::Create<HighGuid::Player>(uint32(low));
            GuildBot const* other = guild.FindMember(target);
            if (!other || other->classId != bot->classId || target == guid)
                continue;
            for (FP::Slot slot : { FP::Slot::Solo, FP::Slot::Group })
            {
                uint32 const id = sFightProfileStore.Assigned(guid, slot);
                if (!sFightProfileStore.Assign(guild.ownerAccount, target, other->classId, slot, id, why))
                    continue;
                FP::StoredProfile const* profile = id ? sFightProfileStore.Get(id) : nullptr;
                if (!profile)
                    continue;
                auto const own = sFightProfileStore.BotOptions(guid, id);
                for (FP::Option const& option : profile->options)
                {
                    auto found = own.find(option.key);
                    sFightProfileStore.SetBotOption(target, id, option.key, found == own.end(),
                        found == own.end() ? 0.0f : found->second, why);
                }
            }
            PushBot(guild, target, false);
            ++copied;
        }
        if (!copied)
        {
            Error(accountId, r, "FIGHT_INVALID", "No guild bot of its class to copy to.");
            return;
        }
        SendFightProfiles(accountId, guild, guid);
        Ack(accountId, r);
        return;
    }
    std::optional<int64> const slot = message.GetInt("slot");
    std::optional<int64> const id = message.GetInt("id");
    if (!slot || *slot < 0 || *slot > 1 || !id || *id < 0)
    {
        Error(accountId, r, "BAD_REQUEST", "A slot and a profile (0 = the default) are needed.");
        return;
    }
    if (!sFightProfileStore.Assign(guild.ownerAccount, guid, bot->classId, *slot ? FP::Slot::Group : FP::Slot::Solo,
        uint32(*id), why))
    {
        Error(accountId, r, "FIGHT_INVALID", Acore::StringFormat("Not assigned: {}.", why));
        return;
    }
    PushBot(guild, guid, false);
    SendFightProfiles(accountId, guild, guid);
    Ack(accountId, r);
}

// FIGHT_OPTION g, id, k, v (a number) or def (back to the profile's value): a bot's own value of an option (FC11).
void GadvProtocol::OnFightOption(uint32 accountId, AdventureGuild& guild, Message const& message)
{
    std::optional<int64> const r = message.GetInt("r");
    ObjectGuid guid;
    GuildBot const* bot = BotOf(guild, message, guid);
    std::optional<int64> const id = message.GetInt("id");
    FP::StoredProfile const* profile = id ? sFightProfileStore.Get(uint32(*id)) : nullptr;
    if (!bot || !profile || (profile->owner != guild.ownerAccount && !FP::Store::IsTemplate(*profile)))
    {
        Error(accountId, r, "FIGHT_INVALID", "Not a bot and a fight profile of your guild.");
        return;
    }
    bool const reset = message.GetBool("def").value_or(false);
    float value = 0.0f;
    if (!reset)
    {
        try
        {
            value = std::stof(message.GetString("v"));
        }
        catch (...)
        {
            Error(accountId, r, "BAD_REQUEST", "The value is a number.");
            return;
        }
    }
    std::string why;
    if (!sFightProfileStore.SetBotOption(guid, profile->id, message.GetString("k"), reset, value, why))
    {
        Error(accountId, r, "FIGHT_INVALID", Acore::StringFormat("Not set: {}.", why));
        return;
    }
    SendFightProfile(accountId, guild, guid, profile->id);
    Ack(accountId, r);
}

// FIGHT_COPY g, n: a new profile of the guild from the bot's template (its class and talent tree), named n; with
// `from` a copy of that profile of the guild instead (the bot need not be in the world then).
void GadvProtocol::OnFightCopy(uint32 accountId, AdventureGuild& guild, Message const& message)
{
    std::optional<int64> const r = message.GetInt("r");
    ObjectGuid guid;
    GuildBot const* bot = BotOf(guild, message, guid);
    if (std::optional<int64> const from = message.GetInt("from"); bot && from && *from > 0)
    {
        FP::StoredProfile const* source = sFightProfileStore.Get(uint32(*from));
        if (!source || (source->owner != guild.ownerAccount && !FP::Store::IsTemplate(*source)) || source->classId != bot->classId)
        {
            Error(accountId, r, "FIGHT_INVALID", "Not copied: no fight profile of your guild for its class with that id.");
            return;
        }
        FP::StoredProfile copy = *source;
        copy.id = 0;
        copy.owner = guild.ownerAccount;
        copy.name = message.GetString("n");
        if (copy.name.empty())
            copy.name = source->name + " copy";
        std::vector<std::string> errors;
        if (!sFightProfileStore.Save(std::move(copy), errors))
        {
            Error(accountId, r, "FIGHT_INVALID", Acore::StringFormat("Not copied: {}.", errors.empty() ? std::string("?") : errors.front()));
            return;
        }
        SendFightProfiles(accountId, guild, guid);
        Ack(accountId, r);
        return;
    }
    Player* player = bot ? InWorld(guid) : nullptr;
    if (!player)
    {
        Error(accountId, r, bot ? "FIGHT_INVALID" : "NOT_OWNER", bot ? "The bot must be in the world: its template is read from it." :
            "That character is not one of your guild bots.");
        return;
    }
    FP::StoredProfile profile;
    std::string why;
    if (!FP::CopyTemplate(player, profile, why))
    {
        Error(accountId, r, "FIGHT_INVALID", Acore::StringFormat("Not copied: {}.", why));
        return;
    }
    profile.owner = guild.ownerAccount;
    profile.name = message.GetString("n");
    if (profile.name.empty())
        profile.name = Acore::StringFormat("{} copy", player->GetName());
    std::vector<std::string> errors;
    if (!sFightProfileStore.Save(std::move(profile), errors))
    {
        Error(accountId, r, "FIGHT_INVALID", Acore::StringFormat("Not copied: {}.", errors.empty() ? std::string("?") : errors.front()));
        return;
    }
    SendFightProfiles(accountId, guild, guid);
    Ack(accountId, r);
}

namespace
{
    std::vector<std::string> SplitTabs(std::string const& text)
    {
        std::vector<std::string> parts;
        std::size_t start = 0;
        while (true)
        {
            std::size_t const tab = text.find('\t', start);
            parts.push_back(text.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
            if (tab == std::string::npos)
                return parts;
            start = tab + 1;
        }
    }

    bool ParseNumber(std::string const& text, float& out)
    {
        try
        {
            std::size_t used = 0;
            out = std::stof(text, &used);
            return used == text.size();
        }
        catch (...)
        {
            return false;
        }
    }

    bool ParseUnsigned(std::string const& text, uint32& out)
    {
        if (text.empty() || text.size() > 10 || text.find_first_not_of("0123456789") != std::string::npos)
            return false;
        uint64 const value = std::stoull(text);
        if (value > 0xFFFFFFFFull)
            return false;
        out = uint32(value);
        return true;
    }

    // An option of FIGHT_SAVE: key, label, kind, value, min, max (tab separated).
    bool ParseOptionField(std::string const& text, FP::Option& option, std::string& why)
    {
        std::vector<std::string> const parts = SplitTabs(text);
        uint32 kind = 0;
        if (parts.size() != 6 || !ParseUnsigned(parts[2], kind) || kind > uint32(FP::OptionKind::Percent) ||
            !ParseNumber(parts[3], option.value) || !ParseNumber(parts[4], option.min) || !ParseNumber(parts[5], option.max))
        {
            why = "an option is: key, label, kind, value, min, max";
            return false;
        }
        option.key = parts[0];
        option.label = parts[1];
        option.kind = FP::OptionKind(kind);
        return true;
    }

    // A rule of FIGHT_SAVE: list, kind, ref, action, trigger, target, relevance, enabled, flags, interval, node, source,
    // conditions (tab separated, the fields of FRULE). The conditions are parsed against the profile's options.
    bool ParseRuleField(std::string const& text, FP::StoredProfile const& profile, FP::Rule& rule, std::string& why)
    {
        std::vector<std::string> const parts = SplitTabs(text);
        uint32 list = 0, kind = 0, target = 0, flags = 0, interval = 0, node = 0;
        if (parts.size() != 13 || !ParseUnsigned(parts[0], list) || list > 1 || !ParseUnsigned(parts[1], kind) ||
            kind > uint32(FP::ActionKind::Trinket) || !ParseUnsigned(parts[2], rule.ref) || !ParseUnsigned(parts[5], target) ||
            !ParseNumber(parts[6], rule.relevance) || (parts[7] != "0" && parts[7] != "1") || !ParseUnsigned(parts[8], flags) ||
            flags > 0xFF || !ParseUnsigned(parts[9], interval) || interval > 3600 || !ParseUnsigned(parts[10], node) || node > 0xFFFF)
        {
            why = "a rule is: list, kind, ref, action, trigger, target, relevance, on, flags, interval, node, source, conditions";
            return false;
        }
        rule.list = FP::ListKind(list);
        rule.kind = FP::ActionKind(kind);
        rule.action = parts[3];
        rule.trigger = parts[4];
        rule.target = FP::TargetKind(std::min<uint32>(target, 255));
        rule.enabled = parts[7] == "1";
        rule.flags = uint8(flags);
        rule.minInterval = uint16(interval);
        rule.node = uint16(node);
        rule.source = parts[11];
        // A row of mod-playerbots names its spell as ref for the addon's icon (FRULE); it stores none.
        if (rule.kind == FP::ActionKind::Row || rule.kind == FP::ActionKind::Action)
            rule.ref = 0;
        return FP::ParseConditions(parts[12], &profile, rule.conditions, why);
    }
}

// FIGHT_SAVE g, id, n, note, oc + o1..oN, rc + r1..rN: a profile of the guild as the editor has it (FP-3). The rules
// come list 0 first; the store checks them (errors name "rule i", counted over all rules in that order).
void GadvProtocol::OnFightSave(uint32 accountId, AdventureGuild& guild, Message const& message)
{
    std::optional<int64> const r = message.GetInt("r");
    ObjectGuid guid;
    GuildBot const* bot = BotOf(guild, message, guid);
    std::optional<int64> const id = message.GetInt("id");
    FP::StoredProfile const* old = id && *id > 0 ? sFightProfileStore.Get(uint32(*id)) : nullptr;
    if (bot && old && FP::Store::IsTemplate(*old))
    {
        Error(accountId, r, "FIGHT_INVALID", "Not saved: a template is read-only; make a copy of it to edit it.");
        return;
    }
    if (!bot || !old || old->owner != guild.ownerAccount || old->classId != bot->classId)
    {
        Error(accountId, r, "FIGHT_INVALID", "Not a bot and a fight profile of your guild for its class.");
        return;
    }
    FP::StoredProfile profile = *old;
    profile.name = message.GetString("n", old->name);
    profile.note = message.GetString("note", old->note);
    profile.options.clear();
    profile.rules.clear();
    std::string why;
    int64 const options = message.GetInt("oc").value_or(0);
    int64 const rules = message.GetInt("rc").value_or(0);
    if (options < 0 || options > int64(FP::MaxOptionsPerProfile) || rules < 0 || rules > int64(FP::MaxRulesPerProfile))
    {
        Error(accountId, r, "FIGHT_INVALID", Acore::StringFormat("Not saved: at most {} rules and {} options.",
            FP::MaxRulesPerProfile, FP::MaxOptionsPerProfile));
        return;
    }
    for (int64 i = 1; i <= options; ++i)
    {
        FP::Option option;
        if (!ParseOptionField(message.GetString(Acore::StringFormat("o{}", i)), option, why))
        {
            Error(accountId, r, "FIGHT_INVALID", Acore::StringFormat("Not saved: option {}: {}.", i, why));
            return;
        }
        profile.options.push_back(std::move(option));
    }
    for (int64 i = 1; i <= rules; ++i)
    {
        FP::Rule rule;
        if (!ParseRuleField(message.GetString(Acore::StringFormat("r{}", i)), profile, rule, why))
        {
            Error(accountId, r, "FIGHT_INVALID", Acore::StringFormat("Not saved: rule {}: {}.", i - 1, why));
            return;
        }
        profile.rules.push_back(std::move(rule));
    }
    std::vector<std::string> errors;
    if (!sFightProfileStore.Save(std::move(profile), errors))
    {
        std::string text;
        for (std::size_t i = 0; i < errors.size() && i < 4; ++i)
            text += (text.empty() ? "" : "; ") + errors[i];
        if (errors.size() > 4)
            text += Acore::StringFormat("; and {} more", errors.size() - 4);
        Error(accountId, r, "FIGHT_INVALID", "Not saved: " + text + ".");
        return;
    }
    for (ObjectGuid const& user : sFightProfileStore.UsedBy(uint32(*id)))
        PushBot(guild, user, false);
    SendFightProfiles(accountId, guild, guid);
    SendFightProfile(accountId, guild, guid, uint32(*id));
    Ack(accountId, r);
}

// FIGHT_DEL g, id: a profile of the guild goes; the bots that had it play the default again.
void GadvProtocol::OnFightDelete(uint32 accountId, AdventureGuild& guild, Message const& message)
{
    std::optional<int64> const r = message.GetInt("r");
    ObjectGuid guid;
    GuildBot const* bot = BotOf(guild, message, guid);
    std::optional<int64> const id = message.GetInt("id");
    if (!bot || !id || *id <= 0)
    {
        Error(accountId, r, "BAD_REQUEST", "A bot and a profile are needed.");
        return;
    }
    std::vector<ObjectGuid> const users = sFightProfileStore.UsedBy(uint32(*id));
    std::string why;
    if (!sFightProfileStore.Delete(guild.ownerAccount, uint32(*id), why))
    {
        Error(accountId, r, "FIGHT_INVALID", Acore::StringFormat("Not deleted: {}.", why));
        return;
    }
    for (ObjectGuid const& user : users)
        PushBot(guild, user, false);
    SendFightProfiles(accountId, guild, guid);
    Ack(accountId, r);
}

// Blocks FCONDS (the condition catalogue), FSPELLS (the spells of its class the bot knows, by the first rank of each
// chain, with their spellbook tab), FITEMS (the usable items it carries that a rule may use) and FTRIGS (the triggers of
// mod-playerbots in plain language): what the editor offers (FP-3).
void GadvProtocol::SendFightCatalog(uint32 accountId, AdventureGuild& guild, ObjectGuid bot)
{
    GuildBot const* member = guild.FindMember(bot);
    if (!member)
        return;
    std::vector<Message> conditions;
    for (FP::ConditionType const& type : FP::Conditions())
        conditions.push_back(Message("FCOND").Set("k", type.key).Set("n", std::string(type.label)).Set("grp", std::string(type.group))
            .Set("vk", uint32(type.value)).Set("pk", uint32(type.param)).Set("min", Decimal(type.min)).Set("max", Decimal(type.max))
            .Set("tg", type.onTarget));
    SendBlock(accountId, "FCONDS", conditions, Message("FCONDS_BEGIN"));

    Player* player = InWorld(bot);
    std::vector<Message> spells;
    std::vector<Message> items;
    if (player)
    {
        FP::StoredProfile probe;
        probe.classId = member->classId;
        uint32 const family = FP::ClassSpellFamily(member->classId);
        std::unordered_map<uint32, uint32> best;  // first rank -> the highest rank it knows
        for (auto const& [spellId, state] : player->GetSpellMap())
        {
            if (!state || state->State == PLAYERSPELL_REMOVED || !state->Active)
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info || info->IsPassive() || info->SpellFamilyName != family)
                continue;
            uint32 const first = sSpellMgr->GetFirstSpellInChain(spellId);
            uint32& known = best[first];
            if (!known || sSpellMgr->GetSpellRank(spellId) > sSpellMgr->GetSpellRank(known))
                known = spellId;
        }
        for (auto const& [first, known] : best)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(first);
            if (!info || spells.size() >= 250)
                continue;
            spells.push_back(Message("FSP").Set("id", first).Set("hi", known).Set("n", std::string(info->SpellName[0]))
                .Set("tab", SpellBookTab(first)));
        }
        std::unordered_set<uint32> seen;
        auto addItem = [&](Item* item)
        {
            if (!item || items.size() >= 60 || !seen.insert(item->GetEntry()).second)
                return;
            FP::Rule rule;
            rule.kind = FP::ActionKind::Item;
            rule.ref = item->GetEntry();
            if (!FP::Store::CheckRule(probe, rule).empty())
                return;
            items.push_back(Message("FIT").Set("e", item->GetEntry()).Set("n", item->GetTemplate()->Name1));
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            addItem(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
            if (Bag* container = player->GetBagByPos(bag))
                for (uint32 slot = 0; slot < container->GetBagSize(); ++slot)
                    addItem(container->GetItemByPos(uint8(slot)));
    }
    SendBlock(accountId, "FSPELLS", spells, Message("FSPELLS_BEGIN").Set("g", bot.GetCounter()).Set("live", player != nullptr));
    SendBlock(accountId, "FITEMS", items, Message("FITEMS_BEGIN").Set("g", bot.GetCounter()));

    // Block FTRIGS: the triggers of mod-playerbots in plain language for its class (a row's trigger, the condition
    // "a trigger of mod-playerbots").
    std::vector<Message> triggers;
    std::unordered_set<std::string> named;
    for (FightTriggerTexts::Entry const& entry : FightTriggerTexts::Entries)
        if ((entry.classId == member->classId || !entry.classId) && named.insert(entry.name).second)
            triggers.push_back(Message("FTRIG").Set("n", std::string(entry.name)).Set("t", TriggerText(member->classId, entry.name)));
    SendBlock(accountId, "FTRIGS", triggers, Message("FTRIGS_BEGIN").Set("g", bot.GetCounter()));
}
