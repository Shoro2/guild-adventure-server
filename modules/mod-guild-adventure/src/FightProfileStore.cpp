/*
 * FightProfileStore (FightProfileStore.h): the catalogue of conditions, loading, validating and saving the guilds'
 * fight profiles, the bots' assignments and their own option values.
 */

#include "FightProfileStore.h"
#include "DatabaseEnv.h"
#include "Field.h"
#include "GameTime.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "QueryResult.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace FP = GuildAdventure::FightProfiles;

namespace
{
    using FP::ConditionType;
    using FP::ParamKind;
    using FP::ValueKind;

    std::string Trim(std::string text)
    {
        auto const notSpace = [](unsigned char c) { return !std::isspace(c); };
        text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
        text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
        return text;
    }

    // A name or note for the database and the addon: printable characters only, no separators of the protocol.
    std::string Clean(std::string const& text, std::size_t maxLength)
    {
        std::string out;
        for (char c : text)
        {
            unsigned char const u = static_cast<unsigned char>(c);
            if (u < 32 || c == '^' || c == '|' || c == '~' || c == ';')
                continue;
            out += c;
        }
        out = Trim(out);
        if (out.size() > maxLength)
            out.resize(maxLength);
        return out;
    }

    bool ValidKey(std::string const& key)
    {
        if (key.empty() || key.size() > 32)
            return false;
        return std::all_of(key.begin(), key.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
    }

    uint32 ClassFamily(uint8 classId)
    {
        switch (classId)
        {
            case CLASS_WARRIOR: return SPELLFAMILY_WARRIOR;
            case CLASS_PALADIN: return SPELLFAMILY_PALADIN;
            case CLASS_HUNTER: return SPELLFAMILY_HUNTER;
            case CLASS_ROGUE: return SPELLFAMILY_ROGUE;
            case CLASS_PRIEST: return SPELLFAMILY_PRIEST;
            case CLASS_DEATH_KNIGHT: return SPELLFAMILY_DEATHKNIGHT;
            case CLASS_SHAMAN: return SPELLFAMILY_SHAMAN;
            case CLASS_MAGE: return SPELLFAMILY_MAGE;
            case CLASS_WARLOCK: return SPELLFAMILY_WARLOCK;
            case CLASS_DRUID: return SPELLFAMILY_DRUID;
            default: return SPELLFAMILY_GENERIC;
        }
    }

    // Item kinds the module's own supply rules use (FC4, docs/fight-profiles.md section 4.11): no rule of a profile.
    bool IsSupplyConsumable(ItemTemplate const* proto)
    {
        return proto->Class == ITEM_CLASS_CONSUMABLE && proto->SubClass >= ITEM_SUBCLASS_POTION &&
            proto->SubClass <= ITEM_SUBCLASS_BANDAGE;
    }

    bool HasUseSpell(ItemTemplate const* proto)
    {
        for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
            if (proto->Spells[i].SpellId > 0 && proto->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
                return true;
        return false;
    }

    char const* OpText(FP::CompareOp op)
    {
        switch (op)
        {
            case FP::CompareOp::Less: return "<";
            case FP::CompareOp::LessEqual: return "<=";
            case FP::CompareOp::Equal: return "=";
            case FP::CompareOp::GreaterEqual: return ">=";
            case FP::CompareOp::Greater: return ">";
            default: return "";
        }
    }

    std::string Number(float value)
    {
        if (std::fabs(value - std::round(value)) < 0.001f)
            return Acore::StringFormat("{}", int32(std::lround(value)));
        return Acore::StringFormat("{:.1f}", value);
    }

    std::string SpellText(std::string const& param)
    {
        uint32 id = 0;
        try
        {
            id = uint32(std::stoul(param));
        }
        catch (...)
        {
            return param;
        }
        SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
        return info ? std::string(info->SpellName[0]) : param;
    }
}

uint32 FP::ClassSpellFamily(uint8 classId)
{
    return ClassFamily(classId);
}

std::vector<ConditionType> const& FP::Conditions()
{
    static std::vector<ConditionType> const types = {
        // me
        { "hp", "my health %", "me", ValueKind::Percent, ParamKind::None, 0, 100, false },
        { "mana", "my mana %", "me", ValueKind::Percent, ParamKind::None, 0, 100, false },
        { "manapts", "my mana", "me", ValueKind::Number, ParamKind::None, 0, 100000, false },
        { "rage", "my rage", "me", ValueKind::Number, ParamKind::None, 0, 100, false },
        { "energy", "my energy", "me", ValueKind::Number, ParamKind::None, 0, 150, false },
        { "runic", "my runic power", "me", ValueKind::Number, ParamKind::None, 0, 130, false },
        { "combo", "my combo points", "me", ValueKind::Number, ParamKind::None, 0, 5, false },
        { "combat", "I am in combat", "me", ValueKind::None, ParamKind::None, 0, 0, false },
        { "aura", "I have the aura", "me", ValueKind::None, ParamKind::Spell, 0, 0, false },
        { "stacks", "stacks of my aura", "me", ValueKind::Number, ParamKind::Spell, 0, 100, false },
        { "moving", "I am moving", "me", ValueKind::None, ParamKind::None, 0, 0, false },
        { "casting", "I am casting", "me", ValueKind::None, ParamKind::None, 0, 0, false },
        { "stunned", "I am stunned", "me", ValueKind::None, ParamKind::None, 0, 0, false },
        { "swimming", "I am swimming", "me", ValueKind::None, ParamKind::None, 0, 0, false },
        { "behind", "I am behind my target", "me", ValueKind::None, ParamKind::None, 0, 0, false },
        { "facing", "I face my target", "me", ValueKind::None, ParamKind::None, 0, 0, false },
        { "level", "my level", "me", ValueKind::Number, ParamKind::None, 1, 80, false },
        // target
        { "thp", "target health %", "target", ValueKind::Percent, ParamKind::None, 0, 100, true },
        { "tdebuff", "target has my aura", "target", ValueKind::None, ParamKind::Spell, 0, 0, true },
        { "taura", "target has the aura", "target", ValueKind::None, ParamKind::Spell, 0, 0, true },
        { "tcasting", "target casts", "target", ValueKind::None, ParamKind::None, 0, 0, true },
        { "tinterrupt", "target casts a spell I can interrupt", "target", ValueKind::None, ParamKind::None, 0, 0, true },
        { "telite", "target is an elite", "target", ValueKind::None, ParamKind::None, 0, 0, true },
        { "tboss", "target is a boss", "target", ValueKind::None, ParamKind::None, 0, 0, true },
        { "tdist", "distance to the target (yd)", "target", ValueKind::Number, ParamKind::None, 0, 100, true },
        { "ttargetsme", "target attacks me", "target", ValueKind::None, ParamKind::None, 0, 0, true },
        { "tenemies", "enemies within N yd of the target", "target", ValueKind::Number, ParamKind::Yards, 0, 40, true },
        { "tdebuffleft", "seconds left of my aura on the target", "target", ValueKind::Number, ParamKind::Spell, 0, 600, true },
        { "ttype", "target is of creature type N", "target", ValueKind::None, ParamKind::Number, 0, 0, true },
        // around
        { "attackers", "attackers of my group", "around", ValueKind::Number, ParamKind::None, 0, 40, false },
        { "myattackers", "attackers on me", "around", ValueKind::Number, ParamKind::None, 0, 40, false },
        { "enemies", "enemies within N yd of me", "around", ValueKind::Number, ParamKind::Yards, 0, 40, false },
        // pet
        { "pet", "I have a pet", "pet", ValueKind::None, ParamKind::None, 0, 0, false },
        { "pethp", "my pet's health %", "pet", ValueKind::Percent, ParamKind::None, 0, 100, false },
        // group
        { "ingroup", "I am in a group", "group", ValueKind::None, ParamKind::None, 0, 0, false },
        { "indungeon", "I am in a dungeon or raid", "group", ValueKind::None, ParamKind::None, 0, 0, false },
        { "membersbelow", "members below N % health", "group", ValueKind::Number, ParamKind::Number, 0, 40, false },
        { "tankhp", "the tank's health %", "group", ValueKind::Percent, ParamKind::None, 0, 100, false },
        // other
        { "known", "I know the spell", "other", ValueKind::None, ParamKind::Spell, 0, 0, false },
        { "ready", "the spell is ready", "other", ValueKind::None, ParamKind::Spell, 0, 0, false },
        { "itemcount", "items in my bags", "other", ValueKind::Number, ParamKind::Item, 0, 1000, false },
        { "itemready", "the item is ready", "other", ValueKind::None, ParamKind::Item, 0, 0, false },
        { "opt", "the profile's option is on", "other", ValueKind::None, ParamKind::Option, 0, 0, false },
        { "trig", "a condition of mod-playerbots", "other", ValueKind::None, ParamKind::Trigger, 0, 0, false } };
    return types;
}

FP::ConditionType const* FP::FindCondition(std::string const& key)
{
    for (ConditionType const& type : Conditions())
        if (type.key == key)
            return &type;
    return nullptr;
}

bool FP::ParseConditions(std::string const& text, StoredProfile const* profile, std::vector<Condition>& out, std::string& why)
{
    out.clear();
    std::stringstream in(text);
    for (std::string part; std::getline(in, part, ';');)
    {
        part = Trim(part);
        if (part.empty())
            continue;
        Condition condition;
        if (part[0] == '!')
        {
            condition.negate = true;
            part.erase(0, 1);
        }
        std::size_t const opAt = part.find_first_of("<>=");
        std::string head = opAt == std::string::npos ? part : part.substr(0, opAt);
        if (opAt != std::string::npos)
        {
            std::string const rest = part.substr(opAt);
            std::size_t width = 1;
            if (rest.rfind("<=", 0) == 0)
                condition.op = CompareOp::LessEqual, width = 2;
            else if (rest.rfind(">=", 0) == 0)
                condition.op = CompareOp::GreaterEqual, width = 2;
            else if (rest[0] == '<')
                condition.op = CompareOp::Less;
            else if (rest[0] == '>')
                condition.op = CompareOp::Greater;
            else
                condition.op = CompareOp::Equal;
            std::string const value = Trim(rest.substr(width));
            if (!value.empty() && value[0] == '$')
                condition.option = value.substr(1);
            else
            {
                try
                {
                    condition.value = std::stof(value);
                }
                catch (...)
                {
                    why = Acore::StringFormat("\"{}\": no number after the comparison", part);
                    return false;
                }
            }
        }
        if (std::size_t const colon = head.find(':'); colon != std::string::npos)
        {
            condition.param = Trim(head.substr(colon + 1));
            head = head.substr(0, colon);
        }
        condition.key = Trim(head);
        ConditionType const* type = FindCondition(condition.key);
        if (!type)
        {
            why = Acore::StringFormat("\"{}\": unknown condition \"{}\"", part, condition.key);
            return false;
        }
        if ((type->value == ValueKind::None) != (condition.op == CompareOp::None))
        {
            why = type->value == ValueKind::None ? Acore::StringFormat("\"{}\": {} takes no value", part, type->key) :
                Acore::StringFormat("\"{}\": {} needs a comparison and a value", part, type->key);
            return false;
        }
        if ((type->param == ParamKind::None) != condition.param.empty())
        {
            why = type->param == ParamKind::None ? Acore::StringFormat("\"{}\": {} takes no parameter", part, type->key) :
                Acore::StringFormat("\"{}\": {} needs a parameter (\"{}:...\")", part, type->key, type->key);
            return false;
        }
        if (type->param == ParamKind::Spell || type->param == ParamKind::Item || type->param == ParamKind::Yards ||
            type->param == ParamKind::Number)
        {
            uint32 ref = 0;
            try
            {
                ref = uint32(std::stoul(condition.param));
            }
            catch (...)
            {
                why = Acore::StringFormat("\"{}\": the parameter is a number", part);
                return false;
            }
            if (type->param == ParamKind::Spell && !sSpellMgr->GetSpellInfo(ref))
            {
                why = Acore::StringFormat("\"{}\": no spell {}", part, ref);
                return false;
            }
            if (type->param == ParamKind::Item && !sObjectMgr->GetItemTemplate(ref))
            {
                why = Acore::StringFormat("\"{}\": no item {}", part, ref);
                return false;
            }
            if ((type->param == ParamKind::Yards || type->param == ParamKind::Number) && ref > 100)
            {
                why = Acore::StringFormat("\"{}\": the parameter is 0 to 100", part);
                return false;
            }
        }
        if (type->param == ParamKind::Option && (!profile || !profile->FindOption(condition.param)))
        {
            why = Acore::StringFormat("\"{}\": the profile has no option \"{}\"", part, condition.param);
            return false;
        }
        if (!condition.option.empty())
        {
            Option const* option = profile ? profile->FindOption(condition.option) : nullptr;
            if (!option)
            {
                why = Acore::StringFormat("\"{}\": the profile has no option \"{}\"", part, condition.option);
                return false;
            }
        }
        else if (type->value != ValueKind::None && (condition.value < type->min || condition.value > type->max))
        {
            why = Acore::StringFormat("\"{}\": {} is {} to {}", part, type->key, Number(type->min), Number(type->max));
            return false;
        }
        out.push_back(condition);
        if (out.size() > MaxConditionsPerRule)
        {
            why = Acore::StringFormat("more than {} conditions", MaxConditionsPerRule);
            return false;
        }
    }
    return true;
}

std::string FP::FormatConditions(std::vector<Condition> const& conditions)
{
    std::string text;
    for (Condition const& condition : conditions)
    {
        if (!text.empty())
            text += ';';
        if (condition.negate)
            text += '!';
        text += condition.key;
        if (!condition.param.empty())
            text += ':' + condition.param;
        if (condition.op != CompareOp::None)
            text += std::string(OpText(condition.op)) + (condition.option.empty() ? Number(condition.value) : "$" + condition.option);
    }
    return text;
}

std::string FP::DescribeCondition(Condition const& condition)
{
    ConditionType const* type = FindCondition(condition.key);
    std::string text = condition.negate ? "not: " : "";
    text += type ? type->label : condition.key.c_str();
    if (!condition.param.empty())
        text += " " + (type && type->param == ParamKind::Spell ? SpellText(condition.param) : condition.param);
    if (condition.op != CompareOp::None)
        text += Acore::StringFormat(" {} {}", OpText(condition.op), condition.option.empty() ? Number(condition.value) :
            "$" + condition.option);
    return text;
}

char const* FP::TargetName(TargetKind target)
{
    switch (target)
    {
        case TargetKind::Self: return "self";
        case TargetKind::Pet: return "pet";
        case TargetKind::Lowest: return "lowest";
        case TargetKind::Tank: return "tank";
        case TargetKind::Enemy: return "enemy";
        default: return "target";
    }
}

bool FP::ParseTarget(std::string const& text, TargetKind& out)
{
    static std::pair<char const*, TargetKind> const names[] = { { "target", TargetKind::Current },
        { "self", TargetKind::Self }, { "pet", TargetKind::Pet }, { "lowest", TargetKind::Lowest },
        { "tank", TargetKind::Tank }, { "enemy", TargetKind::Enemy } };
    for (auto const& [name, kind] : names)
        if (text == name)
        {
            out = kind;
            return true;
        }
    return false;
}

FP::Option const* FP::StoredProfile::FindOption(std::string const& key) const
{
    for (Option const& option : options)
        if (option.key == key)
            return &option;
    return nullptr;
}

uint32 FP::StoredProfile::CountRules(ListKind list) const
{
    return uint32(std::count_if(rules.begin(), rules.end(), [list](Rule const& rule) { return rule.list == list; }));
}

FP::Store& FP::Store::instance()
{
    static Store store;
    return store;
}

void FP::Store::Load()
{
    _profiles.clear();
    _assigned.clear();
    _botOptions.clear();
    _nextId = 1;

    if (QueryResult result = CharacterDatabase.Query("SELECT id, owner_account, class, tab, name, note, template_key, revision "
        "FROM ga_fight_profile"))
    {
        do
        {
            Field* fields = result->Fetch();
            StoredProfile profile;
            profile.id = fields[0].Get<uint32>();
            profile.owner = fields[1].Get<uint32>();
            profile.classId = fields[2].Get<uint8>();
            profile.tab = fields[3].Get<uint8>();
            profile.name = fields[4].Get<std::string>();
            profile.note = fields[5].Get<std::string>();
            profile.templateKey = fields[6].Get<std::string>();
            profile.revision = fields[7].Get<uint32>();
            _nextId = std::max(_nextId, profile.id + 1);
            _profiles[profile.id] = std::move(profile);
        } while (result->NextRow());
    }

    if (QueryResult result = CharacterDatabase.Query("SELECT profile_id, option_key, label, kind, value, min_value, max_value "
        "FROM ga_fight_profile_option"))
    {
        do
        {
            Field* fields = result->Fetch();
            auto it = _profiles.find(fields[0].Get<uint32>());
            if (it == _profiles.end())
                continue;
            Option option;
            option.key = fields[1].Get<std::string>();
            option.label = fields[2].Get<std::string>();
            option.kind = OptionKind(fields[3].Get<uint8>());
            option.value = fields[4].Get<float>();
            option.min = fields[5].Get<float>();
            option.max = fields[6].Get<float>();
            it->second.options.push_back(option);
        } while (result->NextRow());
    }

    uint32 badRules = 0;
    if (QueryResult result = CharacterDatabase.Query("SELECT profile_id, list, enabled, kind, trigger_name, action_name, ref, target, "
        "flags, min_interval, relevance, node, source, conditions FROM ga_fight_profile_rule ORDER BY profile_id, list, position"))
    {
        do
        {
            Field* fields = result->Fetch();
            auto it = _profiles.find(fields[0].Get<uint32>());
            if (it == _profiles.end())
                continue;
            Rule rule;
            rule.list = ListKind(fields[1].Get<uint8>());
            rule.enabled = fields[2].Get<uint8>() != 0;
            rule.kind = ActionKind(fields[3].Get<uint8>());
            rule.trigger = fields[4].Get<std::string>();
            rule.action = fields[5].Get<std::string>();
            rule.ref = fields[6].Get<uint32>();
            rule.target = TargetKind(fields[7].Get<uint8>());
            rule.flags = fields[8].Get<uint8>();
            rule.minInterval = fields[9].Get<uint16>();
            rule.relevance = fields[10].Get<float>();
            rule.node = fields[11].Get<uint16>();
            rule.source = fields[12].Get<std::string>();
            std::string why;
            if (!ParseConditions(fields[13].Get<std::string>(), &it->second, rule.conditions, why))
            {
                // (A condition the catalogue does not know any more: the rule is kept, off.)
                rule.enabled = false;
                ++badRules;
                LOG_ERROR("module", "mod-guild-adventure: fight profile {}: a rule's conditions are switched off: {}", it->first, why);
            }
            it->second.rules.push_back(std::move(rule));
        } while (result->NextRow());
    }

    uint32 const templates = LoadTemplates();

    if (QueryResult result = CharacterDatabase.Query("SELECT bot_guid, slot, profile_id FROM ga_bot_fight_profile"))
    {
        do
        {
            Field* fields = result->Fetch();
            uint8 const slot = fields[1].Get<uint8>();
            uint32 const id = fields[2].Get<uint32>();
            if (slot < 2 && _profiles.count(id))
                _assigned[fields[0].Get<uint32>()][slot] = id;
        } while (result->NextRow());
    }

    if (QueryResult result = CharacterDatabase.Query("SELECT bot_guid, profile_id, option_key, value FROM ga_bot_fight_option"))
    {
        do
        {
            Field* fields = result->Fetch();
            _botOptions[{ fields[0].Get<uint32>(), fields[1].Get<uint32>() }][fields[2].Get<std::string>()] = fields[3].Get<float>();
        } while (result->NextRow());
    }

    for (auto& [bot, version] : _botVersion)
        ++version;

    LOG_INFO("module", "mod-guild-adventure: loaded {} fight profiles, {} templates (solo {}, tank {}, heal {}), {} bots with a fight profile{}",
        _profiles.size() - templates, templates, CountRole(TemplateSolo), CountRole(TemplateTank), CountRole(TemplateHeal), _assigned.size(),
        badRules ? Acore::StringFormat(" ({} rules switched off: unknown conditions)", badRules) : std::string());
}

// The second row of templates (FP-4, FC5): ga_fight_template* of the world database, generated by
// tools/gen_fight_templates.py from the wowbot335 fight classes (with the corrections of the fight class research) and
// from the tank and healer fight classes written from that research (tools/fightclass/). Read-only profiles of no guild (owner 0); spells and spell
// parameters are taken at the first rank of their chain.
uint32 FP::Store::LoadTemplates()
{
    _templateRoles.clear();
    QueryResult result = WorldDatabase.Query("SELECT id, class, tab, name, note, template_key, role FROM ga_fight_template");
    if (!result)
        return 0;
    std::vector<uint32> ids;
    do
    {
        Field* fields = result->Fetch();
        StoredProfile profile;
        profile.id = fields[0].Get<uint32>();
        profile.owner = 0;
        profile.classId = fields[1].Get<uint8>();
        profile.tab = fields[2].Get<uint8>();
        profile.name = fields[3].Get<std::string>();
        profile.note = fields[4].Get<std::string>();
        profile.templateKey = fields[5].Get<std::string>();
        uint8 const role = fields[6].Get<uint8>();
        if (_profiles.count(profile.id))
        {
            LOG_ERROR("module", "mod-guild-adventure: fight profile template {} has the id of a guild's profile: left out", profile.id);
            continue;
        }
        ids.push_back(profile.id);
        _templateRoles[profile.id] = role <= TemplateHeal ? role : TemplateSolo;
        _profiles[profile.id] = std::move(profile);
    } while (result->NextRow());

    if (QueryResult options = WorldDatabase.Query("SELECT template_id, option_key, label, kind, value, min_value, max_value "
        "FROM ga_fight_template_option"))
    {
        do
        {
            Field* fields = options->Fetch();
            auto it = _profiles.find(fields[0].Get<uint32>());
            if (it == _profiles.end() || !IsTemplate(it->second))
                continue;
            Option option;
            option.key = fields[1].Get<std::string>();
            option.label = fields[2].Get<std::string>();
            option.kind = OptionKind(fields[3].Get<uint8>());
            option.value = fields[4].Get<float>();
            option.min = fields[5].Get<float>();
            option.max = fields[6].Get<float>();
            it->second.options.push_back(option);
        } while (options->NextRow());
    }

    uint32 off = 0;
    if (QueryResult rules = WorldDatabase.Query("SELECT template_id, list, kind, ref, target, flags, min_interval, relevance, "
        "conditions FROM ga_fight_template_rule ORDER BY template_id, list, position"))
    {
        do
        {
            Field* fields = rules->Fetch();
            auto it = _profiles.find(fields[0].Get<uint32>());
            if (it == _profiles.end() || !IsTemplate(it->second))
                continue;
            Rule rule;
            rule.list = ListKind(fields[1].Get<uint8>());
            rule.kind = ActionKind(fields[2].Get<uint8>());
            rule.ref = fields[3].Get<uint32>();
            if (rule.kind == ActionKind::Spell)
                if (uint32 const first = sSpellMgr->GetFirstSpellInChain(rule.ref))
                    rule.ref = first;
            rule.target = TargetKind(fields[4].Get<uint8>());
            rule.flags = fields[5].Get<uint8>();
            rule.minInterval = fields[6].Get<uint16>();
            rule.relevance = fields[7].Get<float>();
            std::string why;
            if (!ParseConditions(fields[8].Get<std::string>(), &it->second, rule.conditions, why))
            {
                rule.enabled = false;
                ++off;
                LOG_ERROR("module", "mod-guild-adventure: fight profile template {} ({}): a rule's conditions are switched off: {}",
                    it->first, it->second.name, why);
            }
            for (Condition& condition : rule.conditions)
                if (FindCondition(condition.key) && FindCondition(condition.key)->param == ParamKind::Spell)
                    if (uint32 const spell = uint32(std::strtoul(condition.param.c_str(), nullptr, 10)))
                        if (uint32 const first = sSpellMgr->GetFirstSpellInChain(spell))
                            condition.param = std::to_string(first);
            if (rule.enabled)
                if (std::string const bad = CheckRule(it->second, rule); !bad.empty())
                {
                    rule.enabled = false;
                    ++off;
                    LOG_ERROR("module", "mod-guild-adventure: fight profile template {} ({}): a rule is switched off: {}",
                        it->first, it->second.name, bad);
                }
            it->second.rules.push_back(std::move(rule));
        } while (rules->NextRow());
    }
    if (off)
        LOG_ERROR("module", "mod-guild-adventure: {} rules of the fight profile templates are switched off", off);
    return uint32(ids.size());
}

std::vector<FP::StoredProfile const*> FP::Store::Templates(uint8 classId) const
{
    std::vector<StoredProfile const*> list;
    for (auto const& [id, profile] : _profiles)
        if (IsTemplate(profile) && (!classId || profile.classId == classId))
            list.push_back(&profile);
    std::sort(list.begin(), list.end(), [](StoredProfile const* a, StoredProfile const* b) { return a->id < b->id; });
    return list;
}

uint8 FP::Store::RoleOf(uint32 templateId) const
{
    auto it = _templateRoles.find(templateId);
    return it == _templateRoles.end() ? TemplateSolo : it->second;
}

uint32 FP::Store::CountRole(uint8 role) const
{
    uint32 count = 0;
    for (auto const& [id, r] : _templateRoles)
        count += r == role ? 1 : 0;
    return count;
}

uint32 FP::Store::TemplateFor(uint8 classId, uint8 tab, uint8 role) const
{
    uint32 best = 0;
    for (auto const& [id, profile] : _profiles)
        if (IsTemplate(profile) && profile.classId == classId && profile.tab == tab && RoleOf(id) == role && (!best || id < best))
            best = id;
    return best;
}

FP::StoredProfile const* FP::Store::Get(uint32 id) const
{
    auto it = _profiles.find(id);
    return it == _profiles.end() ? nullptr : &it->second;
}

std::vector<FP::StoredProfile const*> FP::Store::ListFor(uint32 owner, uint8 classId) const
{
    std::vector<StoredProfile const*> list;
    for (auto const& [id, profile] : _profiles)
        if (profile.owner == owner && (!classId || profile.classId == classId))
            list.push_back(&profile);
    std::sort(list.begin(), list.end(), [](StoredProfile const* a, StoredProfile const* b) { return a->id < b->id; });
    return list;
}

std::string FP::Store::CheckRule(StoredProfile const& profile, Rule const& rule)
{
    if (rule.list != ListKind::Combat && rule.list != ListKind::OutOfCombat)
        return "an unknown list";
    if (uint8(rule.target) > uint8(TargetKind::Enemy))
        return "an unknown target";
    if (rule.relevance < 0.0f || rule.relevance > 200.0f)
        return "the relevance is 0 to 200";
    if (rule.conditions.size() > MaxConditionsPerRule)
        return Acore::StringFormat("more than {} conditions", MaxConditionsPerRule);
    for (Condition const& condition : rule.conditions)
    {
        if (!FindCondition(condition.key))
            return Acore::StringFormat("an unknown condition \"{}\"", condition.key);
        if (!condition.option.empty() && !profile.FindOption(condition.option))
            return Acore::StringFormat("the profile has no option \"{}\"", condition.option);
        if (condition.key == "opt" && !profile.FindOption(condition.param))
            return Acore::StringFormat("the profile has no option \"{}\"", condition.param);
    }
    bool const ownCast = rule.kind == ActionKind::Spell || rule.kind == ActionKind::Item || rule.kind == ActionKind::Trinket;
    if (!ownCast && rule.target != TargetKind::Current)
        return "a row or action of mod-playerbots keeps its own target";
    if (!ownCast && (rule.flags || rule.minInterval))
        return "\"once per target\", \"not twice in a row\" and an interval are for spells, items and trinkets";
    if (rule.list == ListKind::OutOfCombat && rule.target == TargetKind::Enemy)
        return "between fights there is no enemy to aim at";
    switch (rule.kind)
    {
        case ActionKind::Row:
            if (rule.action.empty())
                return "a row without an action";
            break;
        case ActionKind::Action:
            if (rule.action.empty())
                return "no action named";
            if (rule.trigger.empty() && rule.conditions.empty())
                return "an action of mod-playerbots needs a trigger or a condition";
            break;
        case ActionKind::Spell:
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(rule.ref);
            if (!info)
                return Acore::StringFormat("no spell {}", rule.ref);
            if (info->IsPassive())
                return Acore::StringFormat("{} is passive", info->SpellName[0]);
            if (info->SpellFamilyName != ClassFamily(profile.classId) && info->SpellFamilyName != SPELLFAMILY_GENERIC)
                return Acore::StringFormat("{} is no spell of the class", info->SpellName[0]);
            break;
        }
        case ActionKind::Item:
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(rule.ref);
            if (!proto)
                return Acore::StringFormat("no item {}", rule.ref);
            if (!HasUseSpell(proto))
                return Acore::StringFormat("{} has no use effect", proto->Name1);
            if (IsSupplyConsumable(proto))
                return Acore::StringFormat("{} is used by the module's own rules (potions, elixirs, food, bandages ...)", proto->Name1);
            break;
        }
        case ActionKind::Trinket:
            if (rule.ref != 1 && rule.ref != 2)
                return "a trinket slot is 1 or 2";
            break;
        default:
            return "an unknown kind of rule";
    }
    return {};
}

bool FP::Store::Save(StoredProfile profile, std::vector<std::string>& errors, uint32* savedId)
{
    errors.clear();
    StoredProfile const* old = profile.id ? Get(profile.id) : nullptr;
    if (profile.id && (!old || old->owner != profile.owner))
    {
        errors.push_back("not a profile of this guild");
        return false;
    }
    if (old && old->classId != profile.classId)
        errors.push_back("a profile keeps its class");
    if (!profile.classId || profile.classId >= MAX_CLASSES || profile.tab > 2)
        errors.push_back("an unknown class or talent tree");
    profile.name = Clean(profile.name, MaxProfileName);
    profile.note = Clean(profile.note, MaxProfileNote);
    if (profile.name.empty())
        errors.push_back("the profile needs a name");
    if (!old && ListFor(profile.owner, profile.classId).size() >= MaxProfilesPerClass)
        errors.push_back(Acore::StringFormat("the guild has {} profiles of this class already", MaxProfilesPerClass));
    if (profile.rules.size() > MaxRulesPerProfile)
        errors.push_back(Acore::StringFormat("more than {} rules", MaxRulesPerProfile));
    if (profile.options.size() > MaxOptionsPerProfile)
        errors.push_back(Acore::StringFormat("more than {} options", MaxOptionsPerProfile));
    for (Option& option : profile.options)
    {
        option.label = Clean(option.label, 64);
        if (!ValidKey(option.key))
            errors.push_back(Acore::StringFormat("option \"{}\": a key is 1 to 32 letters, digits or _", option.key));
        else if (uint8(option.kind) > uint8(OptionKind::Percent) || option.min > option.max || option.value < option.min ||
            option.value > option.max)
            errors.push_back(Acore::StringFormat("option \"{}\": its value lies outside {} to {}", option.key, Number(option.min),
                Number(option.max)));
    }
    std::stable_sort(profile.rules.begin(), profile.rules.end(), [](Rule const& a, Rule const& b) { return a.list < b.list; });
    for (std::size_t i = 0; i < profile.rules.size(); ++i)
        if (std::string const why = CheckRule(profile, profile.rules[i]); !why.empty())
            errors.push_back(Acore::StringFormat("rule {}: {}", i, why));
    if (!errors.empty())
        return false;

    if (!profile.id)
        profile.id = _nextId++;
    profile.revision = old ? old->revision + 1 : 1;
    WriteProfile(profile);
    uint32 const id = profile.id;
    _profiles[id] = std::move(profile);
    for (ObjectGuid const& bot : UsedBy(id))
        ++_botVersion[bot.GetCounter()];
    if (savedId)
        *savedId = id;
    return true;
}

void FP::Store::WriteProfile(StoredProfile const& profile)
{
    uint32 const now = uint32(GameTime::GetGameTime().count());
    std::string name = profile.name;
    std::string note = profile.note;
    std::string key = profile.templateKey;
    CharacterDatabase.EscapeString(name);
    CharacterDatabase.EscapeString(note);
    CharacterDatabase.EscapeString(key);
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    trans->Append("REPLACE INTO ga_fight_profile (id, owner_account, class, tab, name, note, template_key, revision, created_at, "
        "updated_at) VALUES ({}, {}, {}, {}, '{}', '{}', '{}', {}, {}, {})", profile.id, profile.owner, uint32(profile.classId),
        uint32(profile.tab), name, note, key, profile.revision, now, now);
    trans->Append("DELETE FROM ga_fight_profile_rule WHERE profile_id = {}", profile.id);
    trans->Append("DELETE FROM ga_fight_profile_option WHERE profile_id = {}", profile.id);
    uint32 position[2] = { 0, 0 };
    for (Rule const& rule : profile.rules)
    {
        std::string trigger = rule.trigger;
        std::string action = rule.action;
        std::string source = rule.source;
        std::string conditions = FormatConditions(rule.conditions);
        CharacterDatabase.EscapeString(trigger);
        CharacterDatabase.EscapeString(action);
        CharacterDatabase.EscapeString(source);
        CharacterDatabase.EscapeString(conditions);
        trans->Append("INSERT INTO ga_fight_profile_rule (profile_id, list, position, enabled, kind, trigger_name, action_name, ref, "
            "target, flags, min_interval, relevance, node, source, conditions) VALUES ({}, {}, {}, {}, {}, '{}', '{}', {}, {}, {}, {}, "
            "{}, {}, '{}', '{}')", profile.id, uint32(rule.list), position[uint8(rule.list) ? 1 : 0]++, rule.enabled ? 1 : 0,
            uint32(rule.kind), trigger, action, rule.ref, uint32(rule.target), uint32(rule.flags), uint32(rule.minInterval),
            rule.relevance, uint32(rule.node), source, conditions);
    }
    for (Option const& option : profile.options)
    {
        std::string optionKey = option.key;
        std::string label = option.label;
        CharacterDatabase.EscapeString(optionKey);
        CharacterDatabase.EscapeString(label);
        trans->Append("INSERT INTO ga_fight_profile_option (profile_id, option_key, label, kind, value, min_value, max_value) VALUES "
            "({}, '{}', '{}', {}, {}, {}, {})", profile.id, optionKey, label, uint32(option.kind), option.value, option.min,
            option.max);
    }
    CharacterDatabase.CommitTransaction(trans);
}

bool FP::Store::Delete(uint32 owner, uint32 id, std::string& why)
{
    StoredProfile const* profile = Get(id);
    if (!profile || profile->owner != owner)
    {
        why = "not a profile of this guild";
        return false;
    }
    for (auto& [bot, slots] : _assigned)
        for (uint32& assigned : slots)
            if (assigned == id)
            {
                assigned = 0;
                ++_botVersion[bot];
            }
    for (auto it = _botOptions.begin(); it != _botOptions.end();)
        it = it->first.second == id ? _botOptions.erase(it) : std::next(it);
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    trans->Append("DELETE FROM ga_fight_profile WHERE id = {}", id);
    trans->Append("DELETE FROM ga_fight_profile_rule WHERE profile_id = {}", id);
    trans->Append("DELETE FROM ga_fight_profile_option WHERE profile_id = {}", id);
    trans->Append("DELETE FROM ga_bot_fight_profile WHERE profile_id = {}", id);
    trans->Append("DELETE FROM ga_bot_fight_option WHERE profile_id = {}", id);
    CharacterDatabase.CommitTransaction(trans);
    _profiles.erase(id);
    return true;
}

uint32 FP::Store::Assigned(ObjectGuid bot, Slot slot) const
{
    auto it = _assigned.find(bot.GetCounter());
    return it == _assigned.end() ? 0 : it->second[uint8(slot) ? 1 : 0];
}

bool FP::Store::Assign(uint32 owner, ObjectGuid bot, uint8 classId, Slot slot, uint32 profileId, std::string& why)
{
    if (profileId)
    {
        StoredProfile const* profile = Get(profileId);
        if (!profile || (profile->owner != owner && !IsTemplate(*profile)))
        {
            why = "not a profile of this guild";
            return false;
        }
        if (profile->classId != classId)
        {
            why = "a profile of another class";
            return false;
        }
    }
    uint8 const index = uint8(slot) ? 1 : 0;
    std::array<uint32, 2>& slots = _assigned[bot.GetCounter()];
    slots[index] = profileId;
    ++_botVersion[bot.GetCounter()];
    if (profileId)
        CharacterDatabase.Execute("REPLACE INTO ga_bot_fight_profile (bot_guid, slot, profile_id, set_at) VALUES ({}, {}, {}, {})",
            bot.GetCounter(), uint32(index), profileId, uint32(GameTime::GetGameTime().count()));
    else
        CharacterDatabase.Execute("DELETE FROM ga_bot_fight_profile WHERE bot_guid = {} AND slot = {}", bot.GetCounter(), uint32(index));
    if (!slots[0] && !slots[1])
        _assigned.erase(bot.GetCounter());
    return true;
}

std::vector<ObjectGuid> FP::Store::UsedBy(uint32 profileId) const
{
    std::vector<ObjectGuid> bots;
    for (auto const& [bot, slots] : _assigned)
        if (slots[0] == profileId || slots[1] == profileId)
            bots.push_back(ObjectGuid::Create<HighGuid::Player>(bot));
    return bots;
}

bool FP::Store::BotOption(ObjectGuid bot, uint32 profileId, std::string const& key, float& value) const
{
    auto it = _botOptions.find({ bot.GetCounter(), profileId });
    if (it == _botOptions.end())
        return false;
    auto found = it->second.find(key);
    if (found == it->second.end())
        return false;
    value = found->second;
    return true;
}

bool FP::Store::SetBotOption(ObjectGuid bot, uint32 profileId, std::string const& key, bool reset, float value, std::string& why)
{
    StoredProfile const* profile = Get(profileId);
    Option const* option = profile ? profile->FindOption(key) : nullptr;
    if (!option)
    {
        why = Acore::StringFormat("the profile has no option \"{}\"", key);
        return false;
    }
    std::string escaped = key;
    CharacterDatabase.EscapeString(escaped);
    if (reset)
    {
        auto it = _botOptions.find({ bot.GetCounter(), profileId });
        if (it != _botOptions.end())
            it->second.erase(key);
        CharacterDatabase.Execute("DELETE FROM ga_bot_fight_option WHERE bot_guid = {} AND profile_id = {} AND option_key = '{}'",
            bot.GetCounter(), profileId, escaped);
    }
    else
    {
        if (value < option->min || value > option->max)
        {
            why = Acore::StringFormat("\"{}\" is {} to {}", key, Number(option->min), Number(option->max));
            return false;
        }
        _botOptions[{ bot.GetCounter(), profileId }][key] = value;
        CharacterDatabase.Execute("REPLACE INTO ga_bot_fight_option (bot_guid, profile_id, option_key, value) VALUES ({}, {}, '{}', {})",
            bot.GetCounter(), profileId, escaped, value);
    }
    ++_botVersion[bot.GetCounter()];
    return true;
}

std::map<std::string, float> FP::Store::BotOptions(ObjectGuid bot, uint32 profileId) const
{
    auto it = _botOptions.find({ bot.GetCounter(), profileId });
    return it == _botOptions.end() ? std::map<std::string, float>() : it->second;
}

uint32 FP::Store::BotVersion(ObjectGuid bot) const
{
    auto it = _botVersion.find(bot.GetCounter());
    return it == _botVersion.end() ? 0 : it->second;
}
