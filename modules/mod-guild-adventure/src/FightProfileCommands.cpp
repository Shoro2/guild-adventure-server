/*
 * Commands of the fight profiles (FightProfiles.h, FightProfileStore.h, docs/fight-profiles.md section 4.9). Until the
 * addon's Combat sub-tab and editor (FP-2, FP-3) these are how a profile is made, changed and assigned.
 *
 * .ga fight <character>                      (GM, console) its slots, option values and what it plays now
 * .ga admin fight <character> <what> [...]   (administrator, console, SOAP)
 *   dump | nc                       every strategy of its combat / non-combat engine with its rows
 *   template [buffs]                the template of its rotation (or buff list): the rows a profile owns and the rest
 *   copy [<name>]                   a new profile of the guild from its two templates (its class and talent tree)
 *   list                            the guild's profiles of its class
 *   assign <solo|group> <id|default>
 *   option <profile id> <key> <value|default>   its own value of an option (FC11)
 *   copyto <character> [...]        its two assignments and its own option values for other bots of its class (FC3)
 *   forget                          takes what it plays out now (it goes in again at its next quiet planner step)
 *   stats [reset]                   what it cast in its fights since "stats reset"
 * .ga admin fightprofile <id> <what> [...]   (administrator, console, SOAP)
 *   show | check | delete | rename <name> | note <text>
 *   rule add <c|n> <spell|item|trinket|action|row> <relevance> <spell id | item entry | 1|2 | action name> [<setting> ...]
 *   rule set <c|n> <index> [<setting> ...]       rule del <c|n> <index>       rule move <c|n> <from> <to>
 *   option set <key> <switch|number|percent> <value> [<min> <max>] [<label>]       option del <key>
 * Settings of a rule: rel=<relevance> target=<target|self|pet|lowest|tank|enemy> cond=<conditions | -> trigger=<name | ->
 * interval=<s> once !once notwice !notwice on off. Conditions: [!]key[:param][op value] joined by ";" (catalogue:
 * FightProfileStore.cpp); "_" stands for a space in action and trigger names.
 */

#include "FightProfiles.h"
#include "AiFactory.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "CommandScript.h"
#include "GuildAdventureMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include <algorithm>
#include <sstream>

using namespace Acore::ChatCommands;
namespace FP = GuildAdventure::FightProfiles;

namespace
{
    std::vector<std::string> Words(std::string const& text)
    {
        std::istringstream in{ text };
        std::vector<std::string> words;
        for (std::string word; in >> word;)
            words.push_back(word);
        return words;
    }

    std::string Spaces(std::string text)
    {
        std::replace(text.begin(), text.end(), '_', ' ');
        return text;
    }

    std::string JoinFrom(std::vector<std::string> const& words, std::size_t from)
    {
        std::string text;
        for (std::size_t i = from; i < words.size(); ++i)
            text += (text.empty() ? "" : " ") + words[i];
        return text;
    }

    bool Number(std::string const& text, float& out)
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

    bool Unsigned(std::string const& text, uint32& out)
    {
        float value = 0.0f;
        if (!Number(text, value) || value < 0.0f)
            return false;
        out = uint32(value);
        return true;
    }

    void Send(ChatHandler* handler, std::vector<std::string> const& lines)
    {
        for (std::string const& line : lines)
            handler->SendSysMessage(line);
    }

    bool Fail(ChatHandler* handler, std::string const& message)
    {
        handler->SendErrorMessage(message, false);
        return false;
    }

    bool ParseList(std::string const& text, FP::ListKind& out)
    {
        if (text == "c" || text == "combat" || text == "fight")
            out = FP::ListKind::Combat;
        else if (text == "n" || text == "buffs" || text == "nc")
            out = FP::ListKind::OutOfCombat;
        else
            return false;
        return true;
    }

    // The position in the profile's rules of the index-th rule of a list (rules.size(): none).
    std::size_t RuleAt(FP::StoredProfile const& profile, FP::ListKind list, uint32 index)
    {
        uint32 seen = 0;
        for (std::size_t i = 0; i < profile.rules.size(); ++i)
            if (profile.rules[i].list == list && seen++ == index)
                return i;
        return profile.rules.size();
    }

    // Applies "rel=", "target=", "cond=", "trigger=", "interval=" and the flags to a rule.
    bool ApplySettings(FP::StoredProfile const& profile, FP::Rule& rule, std::vector<std::string> const& words, std::size_t from,
        std::string& why)
    {
        for (std::size_t i = from; i < words.size(); ++i)
        {
            std::string const& word = words[i];
            std::size_t const eq = word.find('=');
            std::string const key = eq == std::string::npos ? word : word.substr(0, eq);
            std::string const value = eq == std::string::npos ? std::string() : word.substr(eq + 1);
            if (word == "on" || word == "off")
                rule.enabled = word == "on";
            else if (word == "once" || word == "!once")
                rule.flags = uint8(word == "once" ? (rule.flags | FP::RuleOncePerTarget) : (rule.flags & ~FP::RuleOncePerTarget));
            else if (word == "notwice" || word == "!notwice")
                rule.flags = uint8(word == "notwice" ? (rule.flags | FP::RuleNotTwiceInARow) : (rule.flags & ~FP::RuleNotTwiceInARow));
            else if (key == "rel")
            {
                if (!Number(value, rule.relevance))
                {
                    why = "rel= takes a number";
                    return false;
                }
            }
            else if (key == "target")
            {
                if (!FP::ParseTarget(value, rule.target))
                {
                    why = "target= is target, self, pet, lowest, tank or enemy";
                    return false;
                }
            }
            else if (key == "interval")
            {
                uint32 seconds = 0;
                if (!Unsigned(value, seconds) || seconds > 600)
                {
                    why = "interval= takes 0 to 600 seconds";
                    return false;
                }
                rule.minInterval = uint16(seconds);
            }
            else if (key == "trigger")
                rule.trigger = value == "-" ? std::string() : Spaces(value);
            else if (key == "cond")
            {
                if (value == "-")
                    rule.conditions.clear();
                else
                {
                    std::vector<FP::Condition> conditions;
                    if (!FP::ParseConditions(value, &profile, conditions, why))
                        return false;
                    for (FP::Condition& condition : conditions)
                        if (condition.key == "trig")
                            condition.param = Spaces(condition.param);
                    rule.conditions = std::move(conditions);
                }
            }
            else
            {
                why = Acore::StringFormat("unknown setting \"{}\"", word);
                return false;
            }
        }
        return true;
    }

    bool SaveAndReport(ChatHandler* handler, FP::StoredProfile profile)
    {
        std::vector<std::string> errors;
        uint32 id = 0;
        if (!sFightProfileStore.Save(std::move(profile), errors, &id))
        {
            for (std::string const& error : errors)
                handler->SendErrorMessage(error, false);
            return false;
        }
        if (FP::StoredProfile const* saved = sFightProfileStore.Get(id))
            Send(handler, FP::DescribeStored(*saved));
        return true;
    }

    struct Member
    {
        ObjectGuid guid;
        std::string name;
        uint8 classId = 0;
        uint32 owner = 0;
    };

    bool FindMember(PlayerIdentifier const& target, Member& out, std::string& why)
    {
        out.guid = target.GetGUID();
        out.name = target.GetName();
        GuildAdventure::AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfMember(out.guid);
        if (!guild)
        {
            why = Acore::StringFormat("{} is no character of an adventure guild.", out.name);
            return false;
        }
        out.owner = guild->ownerAccount;
        if (CharacterCacheEntry const* entry = sCharacterCache->GetCharacterCacheByGuid(out.guid))
            out.classId = entry->Class;
        if (!out.classId)
        {
            why = Acore::StringFormat("The class of {} is not known.", out.name);
            return false;
        }
        return true;
    }

    class GuildAdventureFightProfileCommandScript : public CommandScript
    {
    public:
        GuildAdventureFightProfileCommandScript() : CommandScript("GuildAdventureFightProfileCommandScript") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable adminTable =
            {
                { "fight", HandleFight, SEC_ADMINISTRATOR, Console::Yes },
                { "fightprofile", HandleProfile, SEC_ADMINISTRATOR, Console::Yes }
            };
            static ChatCommandTable gaTable =
            {
                { "fight", HandleInfo, SEC_GAMEMASTER, Console::Yes },
                { "admin", adminTable }
            };
            static ChatCommandTable commandTable =
            {
                { "ga", gaTable }
            };
            return commandTable;
        }

        static bool HandleInfo(ChatHandler* handler, PlayerIdentifier target)
        {
            Player* player = target.GetConnectedPlayer();
            if (!player || !player->IsInWorld())
                return Fail(handler, Acore::StringFormat("{} is not in the world.", target.GetName()));
            Send(handler, FP::DescribeBot(player));
            return true;
        }

        static bool HandleFight(ChatHandler* handler, PlayerIdentifier target, std::string what, Tail rest)
        {
            std::vector<std::string> const words = Words(std::string(rest));
            std::string why;
            Player* player = target.GetConnectedPlayer();
            bool const inWorld = player && player->IsInWorld();
            auto needWorld = [&]() -> bool
            {
                if (!inWorld)
                    handler->SendErrorMessage(Acore::StringFormat("{} is not in the world.", target.GetName()), false);
                return inWorld;
            };

            if (what == "dump" || what == "nc")
            {
                if (!needWorld())
                    return false;
                Send(handler, FP::DumpEngine(player, what == "dump" ? 0 : 1));
                return true;
            }
            if (what == "template")
            {
                if (!needWorld())
                    return false;
                FP::Played played;
                if (!FP::Template(player, !words.empty() && words[0] == "buffs" ? 1 : 0, played, why))
                    return Fail(handler, why);
                Send(handler, FP::DescribePlayed(played));
                return true;
            }
            if (what == "stats")
            {
                if (!needWorld())
                    return false;
                if (!words.empty() && words[0] == "reset")
                {
                    FP::StartCounting(player);
                    handler->SendSysMessage(Acore::StringFormat("Counting what {} casts in its fights from now on.", player->GetName()));
                    return true;
                }
                Send(handler, FP::DescribeCounts(player));
                return true;
            }
            if (what == "forget")
            {
                if (!needWorld())
                    return false;
                FP::Forget(player);
                handler->SendSysMessage(Acore::StringFormat("{} plays the default for now; its profile goes in again at its next quiet "
                    "planner step.", player->GetName()));
                return true;
            }

            Member member;
            if (!FindMember(target, member, why))
                return Fail(handler, why);

            if (what == "copy")
            {
                if (!needWorld())
                    return false;
                FP::StoredProfile profile;
                if (!FP::CopyTemplate(player, profile, why))
                    return Fail(handler, why);
                profile.owner = member.owner;
                profile.name = words.empty() ? Acore::StringFormat("{} {}", player->GetName(), uint32(profile.tab)) : JoinFrom(words, 0);
                return SaveAndReport(handler, std::move(profile));
            }
            if (what == "list")
            {
                auto const profiles = sFightProfileStore.ListFor(member.owner, member.classId);
                if (profiles.empty())
                    handler->SendSysMessage("The guild has no fight profile of that class.");
                for (FP::StoredProfile const* profile : profiles)
                    handler->SendSysMessage(Acore::StringFormat("#{} \"{}\": talent tree {}, {} rules in the fight, {} between fights, "
                        "{} options, used by {} characters", profile->id, profile->name, uint32(profile->tab),
                        profile->CountRules(FP::ListKind::Combat), profile->CountRules(FP::ListKind::OutOfCombat),
                        profile->options.size(), sFightProfileStore.UsedBy(profile->id).size()));
                for (FP::StoredProfile const* profile : sFightProfileStore.Templates(member.classId))
                    handler->SendSysMessage(Acore::StringFormat("#{} \"{}\" (a template, read-only): talent tree {}, {} rules in the fight, "
                        "{} between fights, {} options, used by {} characters", profile->id, profile->name, uint32(profile->tab),
                        profile->CountRules(FP::ListKind::Combat), profile->CountRules(FP::ListKind::OutOfCombat),
                        profile->options.size(), sFightProfileStore.UsedBy(profile->id).size()));
                return true;
            }
            if (what == "assign")
            {
                if (words.size() < 2 || (words[0] != "solo" && words[0] != "group"))
                    return Fail(handler, "Usage: assign <solo|group> <profile id|default>");
                uint32 id = 0;
                if (words[1] != "default" && !Unsigned(words[1], id))
                    return Fail(handler, "The profile is its id or \"default\".");
                FP::Slot const slot = words[0] == "group" ? FP::Slot::Group : FP::Slot::Solo;
                if (!sFightProfileStore.Assign(member.owner, member.guid, member.classId, slot, id, why))
                    return Fail(handler, why);
                handler->SendSysMessage(Acore::StringFormat("{} has {} in its {} slot (it goes in at its next quiet planner step).",
                    member.name, id ? Acore::StringFormat("fight profile #{}", id) : std::string("nothing of its own (the template of its class and talent tree, or mod-playerbots' rotation where none fits or "
                    "Fight.TemplatesByDefault is off)"), words[0]));
                if (id && inWorld)
                    if (FP::StoredProfile const* profile = sFightProfileStore.Get(id);
                        profile && profile->tab != AiFactory::GetPlayerSpecTab(player))
                        handler->SendSysMessage(Acore::StringFormat("Its talents are of tree {}, the profile is made for tree {}: it plays "
                            "the template of its tree by default (or mod-playerbots' rotation) until they match.",
                            uint32(AiFactory::GetPlayerSpecTab(player)), uint32(profile->tab)));
                return true;
            }
            if (what == "option")
            {
                uint32 id = 0;
                if (words.size() < 3 || !Unsigned(words[0], id))
                    return Fail(handler, "Usage: option <profile id> <key> <value|default>");
                float value = 0.0f;
                bool const reset = words[2] == "default";
                if (!reset && !Number(words[2], value))
                    return Fail(handler, "The value is a number or \"default\".");
                FP::StoredProfile const* profile = sFightProfileStore.Get(id);
                if (!profile || (profile->owner != member.owner && !FP::Store::IsTemplate(*profile)))
                    return Fail(handler, "Not a profile of its guild.");
                if (!sFightProfileStore.SetBotOption(member.guid, id, words[1], reset, value, why))
                    return Fail(handler, why);
                handler->SendSysMessage(Acore::StringFormat("{}: option {} of profile #{} is {}.", member.name, words[1], id,
                    reset ? std::string("the profile's value again") : Acore::StringFormat("{:.1f} for it", value)));
                return true;
            }
            if (what == "copyto")
            {
                if (words.empty())
                    return Fail(handler, "Usage: copyto <character> [<character> ...]");
                for (std::string const& name : words)
                {
                    ObjectGuid const guid = sCharacterCache->GetCharacterGuidByName(name);
                    CharacterCacheEntry const* entry = guid ? sCharacterCache->GetCharacterCacheByGuid(guid) : nullptr;
                    GuildAdventure::AdventureGuild* guild = guid ? sGuildAdventureMgr.FindGuildOfMember(guid) : nullptr;
                    if (!entry || !guild || guild->ownerAccount != member.owner)
                    {
                        handler->SendSysMessage(Acore::StringFormat("{}: no character of the same guild.", name));
                        continue;
                    }
                    if (entry->Class != member.classId)
                    {
                        handler->SendSysMessage(Acore::StringFormat("{}: another class.", name));
                        continue;
                    }
                    for (FP::Slot slot : { FP::Slot::Solo, FP::Slot::Group })
                    {
                        uint32 const id = sFightProfileStore.Assigned(member.guid, slot);
                        if (!sFightProfileStore.Assign(member.owner, guid, entry->Class, slot, id, why))
                            handler->SendSysMessage(Acore::StringFormat("{}: {}", name, why));
                        FP::StoredProfile const* profile = id ? sFightProfileStore.Get(id) : nullptr;
                        if (!profile)
                            continue;
                        auto const own = sFightProfileStore.BotOptions(member.guid, id);
                        for (FP::Option const& option : profile->options)
                        {
                            auto found = own.find(option.key);
                            sFightProfileStore.SetBotOption(guid, id, option.key, found == own.end(),
                                found == own.end() ? 0.0f : found->second, why);
                        }
                    }
                    handler->SendSysMessage(Acore::StringFormat("{} has the fight profiles and option values of {} now.", name, member.name));
                }
                return true;
            }
            return Fail(handler, "Usage: .ga admin fight <character> dump|nc|template [buffs]|copy [name]|list|assign <solo|group> "
                "<id|default>|option <id> <key> <value|default>|copyto <character> ...|forget|stats [reset]");
        }

        static bool HandleProfile(ChatHandler* handler, uint32 id, std::string what, Tail rest)
        {
            FP::StoredProfile const* stored = sFightProfileStore.Get(id);
            if (!stored)
                return Fail(handler, Acore::StringFormat("No fight profile #{}.", id));
            std::vector<std::string> const words = Words(std::string(rest));
            std::string why;
            FP::StoredProfile profile = *stored;

            if (what == "show")
            {
                Send(handler, FP::DescribeStored(profile));
                return true;
            }
            if (what == "check")
            {
                Send(handler, FP::DescribeStored(profile));
                for (std::size_t i = 0; i < profile.rules.size(); ++i)
                    if (std::string const problem = FP::Store::CheckRule(profile, profile.rules[i]); !problem.empty())
                        handler->SendSysMessage(Acore::StringFormat("rule {}: {}", i, problem));
                for (ObjectGuid const& guid : sFightProfileStore.UsedBy(id))
                {
                    Player* player = ObjectAccessor::FindConnectedPlayer(guid);
                    if (!player || !player->IsInWorld())
                        continue;
                    std::string unknown;
                    for (FP::Rule const& rule : profile.rules)
                        if (rule.kind == FP::ActionKind::Spell)
                        {
                            bool known = false;
                            for (uint32 rank = sSpellMgr->GetFirstSpellInChain(rule.ref); rank && !known; rank = sSpellMgr->GetNextSpellInChain(rank))
                                known = player->HasSpell(rank);
                            if (!known)
                                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(rule.ref))
                                    unknown += (unknown.empty() ? "" : ", ") + std::string(info->SpellName[0]);
                        }
                    FP::Played const* active = FP::Active(player, 0);
                    handler->SendSysMessage(Acore::StringFormat("{}: {}{}", player->GetName(), active && active->profileId == id ?
                        std::string("plays it") : std::string("does not play it now"), unknown.empty() ? std::string() :
                        "; spells it does not know yet (skipped): " + unknown));
                }
                return true;
            }
            if (FP::Store::IsTemplate(profile))
                return Fail(handler, "A template is read-only: make a copy of it in the fight profile editor (New copy) to change it.");
            if (what == "delete")
            {
                if (!sFightProfileStore.Delete(profile.owner, id, why))
                    return Fail(handler, why);
                handler->SendSysMessage(Acore::StringFormat("Fight profile #{} is deleted; the bots that had it play the default.", id));
                return true;
            }
            if (what == "rename" || what == "note")
            {
                if (what == "rename" && words.empty())
                    return Fail(handler, "Usage: rename <name>");
                (what == "rename" ? profile.name : profile.note) = JoinFrom(words, 0);
                return SaveAndReport(handler, std::move(profile));
            }
            if (what == "rule")
            {
                FP::ListKind list = FP::ListKind::Combat;
                if (words.size() < 2 || !ParseList(words[1], list))
                    return Fail(handler, "Usage: rule add|set|del|move <c|n> ...");
                std::string const& verb = words[0];
                if (verb == "add")
                {
                    if (words.size() < 5)
                        return Fail(handler, "Usage: rule add <c|n> <spell|item|trinket|action|row> <relevance> <ref> [<setting> ...]");
                    FP::Rule rule;
                    rule.list = list;
                    std::string const& kind = words[2];
                    if (!Number(words[3], rule.relevance))
                        return Fail(handler, "The relevance is a number.");
                    if (kind == "spell" || kind == "item" || kind == "trinket")
                    {
                        rule.kind = kind == "spell" ? FP::ActionKind::Spell : kind == "item" ? FP::ActionKind::Item : FP::ActionKind::Trinket;
                        if (!Unsigned(words[4], rule.ref))
                            return Fail(handler, "A spell, item or trinket rule takes a number (spell id, item entry, trinket slot 1 or 2).");
                        if (rule.kind == FP::ActionKind::Spell)
                            rule.ref = sSpellMgr->GetFirstSpellInChain(rule.ref);
                    }
                    else if (kind == "action" || kind == "row")
                    {
                        rule.kind = kind == "action" ? FP::ActionKind::Action : FP::ActionKind::Row;
                        rule.action = Spaces(words[4]);
                    }
                    else
                        return Fail(handler, "The kind is spell, item, trinket, action or row.");
                    if (!ApplySettings(profile, rule, words, 5, why))
                        return Fail(handler, why);
                    profile.rules.push_back(rule);
                    return SaveAndReport(handler, std::move(profile));
                }
                uint32 index = 0;
                if (words.size() < 3 || !Unsigned(words[2], index))
                    return Fail(handler, "Name the rule by its index in the list.");
                std::size_t const at = RuleAt(profile, list, index);
                if (at >= profile.rules.size())
                    return Fail(handler, Acore::StringFormat("The list has no rule {}.", index));
                if (verb == "set")
                {
                    if (!ApplySettings(profile, profile.rules[at], words, 3, why))
                        return Fail(handler, why);
                }
                else if (verb == "del")
                    profile.rules.erase(profile.rules.begin() + at);
                else if (verb == "move")
                {
                    uint32 to = 0;
                    if (words.size() < 4 || !Unsigned(words[3], to))
                        return Fail(handler, "Usage: rule move <c|n> <from> <to>");
                    FP::Rule const moved = profile.rules[at];
                    profile.rules.erase(profile.rules.begin() + at);
                    std::size_t const into = RuleAt(profile, list, to);
                    profile.rules.insert(profile.rules.begin() + std::min(into, profile.rules.size()), moved);
                }
                else
                    return Fail(handler, "Usage: rule add|set|del|move <c|n> ...");
                return SaveAndReport(handler, std::move(profile));
            }
            if (what == "option")
            {
                if (words.size() >= 2 && words[0] == "del")
                {
                    auto it = std::find_if(profile.options.begin(), profile.options.end(),
                        [&](FP::Option const& option) { return option.key == words[1]; });
                    if (it == profile.options.end())
                        return Fail(handler, Acore::StringFormat("The profile has no option \"{}\".", words[1]));
                    profile.options.erase(it);
                    return SaveAndReport(handler, std::move(profile));
                }
                if (words.size() < 4 || words[0] != "set")
                    return Fail(handler, "Usage: option set <key> <switch|number|percent> <value> [<min> <max>] [<label>] | option del <key>");
                FP::Option option;
                option.key = words[1];
                if (words[2] == "switch")
                    option.kind = FP::OptionKind::Switch, option.max = 1.0f;
                else if (words[2] == "percent")
                    option.kind = FP::OptionKind::Percent;
                else if (words[2] != "number")
                    return Fail(handler, "The kind of an option is switch, number or percent.");
                if (!Number(words[3], option.value))
                    return Fail(handler, "The value is a number.");
                std::size_t labelFrom = 4;
                float min = 0.0f, max = 0.0f;
                if (words.size() >= 6 && Number(words[4], min) && Number(words[5], max))
                {
                    option.min = min;
                    option.max = max;
                    labelFrom = 6;
                }
                option.label = JoinFrom(words, labelFrom);
                auto it = std::find_if(profile.options.begin(), profile.options.end(),
                    [&](FP::Option const& existing) { return existing.key == option.key; });
                if (it != profile.options.end())
                    *it = option;
                else
                    profile.options.push_back(option);
                return SaveAndReport(handler, std::move(profile));
            }
            return Fail(handler, "Usage: .ga admin fightprofile <id> show|check|delete|rename <name>|note <text>|rule ...|option ...");
        }
    };
}

void AddGuildAdventureFightProfileCommandScripts()
{
    new GuildAdventureFightProfileCommandScript();
}
