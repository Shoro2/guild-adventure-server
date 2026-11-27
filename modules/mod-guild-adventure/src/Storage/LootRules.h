/*
 * LootRules: the pure rule logic of the guild loot filter (docs/storage.md "Loot filter").
 *
 * A port of mod-loot-filter's LootFilterRules.h (ForgottenLand, the owner's own module) without its Paragon part: the
 * "cursed" condition (mod-paragon-itemgen's slot 11 enchant) and the one-tick wait for that enchant are gone, and so
 * are the migration of the old one-condition table and the bag addressing of its window. What stays is the model:
 * a rule is one action plus up to eight conditions in up to three groups; rules are checked top to bottom; the first
 * matching rule whose action can handle the item decides; quest items are never touched.
 *
 * Nothing here touches the core (only Define.h for the integer types), so tests/test_loot_rules.cpp compiles it on
 * its own.
 */

#ifndef MOD_GUILD_ADVENTURE_LOOT_RULES_H
#define MOD_GUILD_ADVENTURE_LOOT_RULES_H

#include "Define.h"
#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace GuildAdventure::LootRules
{
    constexpr std::size_t MaxConditions = 8;   // per rule, all groups
    constexpr std::size_t MaxGroups = 3;
    constexpr std::size_t MaxText = 40;
    // Rule logic bits: bit g = group g matches when ANY of its conditions does (else all); LogicJoinAny = the rule
    // matches when any group does (else all groups).
    constexpr uint32 LogicJoinAny = 0x10;
    constexpr uint32 AnySubclass = 255;
    constexpr uint32 MaxQuality = 7;
    constexpr uint32 MaxItemLevel = 65535;
    constexpr uint32 MaxItemClass = 16;
    constexpr uint32 MaxSubclass = 31;

    enum Action : uint8
    {
        ActionKeep       = 0,  // stays in the looter's bags
        ActionSell       = 1,  // vendor price to the looter
        ActionDisenchant = 2,  // materials to the guild storage
        ActionDelete     = 3,  // destroyed
        ActionStore      = 4,  // to the guild storage
        ActionCount      = 5
    };

    // Evaluation results: 0-4 are the actions above.
    enum Result : uint8
    {
        ResultNoRule    = 5,
        ResultProtected = 6
    };

    // The numbers are mod-loot-filter's; 4 (lone subclass) and 5 (Paragon "cursed") are not used here.
    enum CondType : uint8
    {
        CondQuality   = 0,
        CondItemLevel = 1,
        CondSellPrice = 2,
        CondItemType  = 3,  // value = class, value2 = subclass or AnySubclass
        CondItem      = 6,  // value = item entry
        CondName      = 7   // text = case-insensitive substring of the item's name
    };

    enum Op : uint8
    {
        OpIs      = 0,
        OpAtLeast = 1,
        OpAtMost  = 2
    };

    struct Condition
    {
        uint8 type = CondQuality;
        uint8 op = OpIs;
        uint32 value = 0;
        uint32 value2 = 0;
        std::string text;
        uint8 group = 0;        // 0..MaxGroups-1, conditions sorted by it
    };

    struct Rule
    {
        uint32 id = 0;
        uint8 position = 0;     // 1-based evaluation order
        uint8 action = ActionKeep;
        bool enabled = true;
        uint8 logic = 0;        // Logic* bits; 0 = every condition must match
        std::vector<Condition> conditions;
    };

    // What the evaluator needs to know about one item.
    struct ItemFacts
    {
        uint32 entry = 0;
        uint32 quality = 0;
        uint32 itemLevel = 0;
        uint32 sellPrice = 0;
        uint32 itemClass = 0;
        uint32 subClass = 0;
        bool questItem = false;
        std::string name;
        // Whether an action can do its job on this item; a rule whose action cannot passes the item on to the next
        // matching rule.
        bool sellable = true;       // vendor price, and the gold cap allows it
        bool disenchantable = true; // has a disenchant loot id
        bool storable = true;       // the guild storage takes it
    };

    struct Verdict
    {
        uint8 result = ResultNoRule;
        uint8 position = 0;     // position of the deciding rule, 0 if none
    };

    // ---------------------------------------------------------------------------------------------------------------
    // Matching and evaluation
    // ---------------------------------------------------------------------------------------------------------------

    inline uint8 AllowedMask(bool sell, bool disenchant, bool del)
    {
        uint32 mask = (1u << ActionKeep) | (1u << ActionStore);
        if (sell)
            mask |= 1u << ActionSell;
        if (disenchant)
            mask |= 1u << ActionDisenchant;
        if (del)
            mask |= 1u << ActionDelete;
        return static_cast<uint8>(mask);
    }

    inline bool Compare(uint32 actual, uint8 op, uint32 expected)
    {
        switch (op)
        {
            case OpIs:      return actual == expected;
            case OpAtLeast: return actual >= expected;
            case OpAtMost:  return actual <= expected;
            default:        return false;
        }
    }

    inline char LowerAscii(char c)
    {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }

    inline std::string Lower(std::string s)
    {
        for (char& c : s)
            c = LowerAscii(c);
        return s;
    }

    inline bool Matches(Condition const& c, ItemFacts const& item)
    {
        switch (c.type)
        {
            case CondQuality:
                return Compare(item.quality, c.op, c.value);
            case CondItemLevel:
                return Compare(item.itemLevel, c.op, c.value);
            case CondSellPrice:
                return Compare(item.sellPrice, c.op, c.value);
            case CondItemType:
                return item.itemClass == c.value && (c.value2 == AnySubclass || item.subClass == c.value2);
            case CondItem:
                return item.entry == c.value;
            case CondName:
                return !c.text.empty() && Lower(item.name).find(Lower(c.text)) != std::string::npos;
            default:
                return false;
        }
    }

    inline std::size_t GroupCount(Rule const& rule)
    {
        return rule.conditions.empty() ? 0 : std::size_t(rule.conditions.back().group) + 1;
    }

    // Each group is all (AND) or any (OR) of its conditions; the groups are joined with all or any. One group in "all"
    // mode is the plain AND list.
    inline bool RuleMatches(Rule const& rule, ItemFacts const& item)
    {
        std::size_t const groups = GroupCount(rule);
        if (!groups)
            return false;
        bool const joinAny = (rule.logic & LogicJoinAny) != 0;
        for (std::size_t g = 0; g < groups; ++g)
        {
            bool const any = (rule.logic & (1u << g)) != 0;
            bool hit = !any;
            for (Condition const& c : rule.conditions)
            {
                if (c.group != g)
                    continue;
                bool const m = Matches(c, item);
                if (any && m)
                {
                    hit = true;
                    break;
                }
                if (!any && !m)
                {
                    hit = false;
                    break;
                }
            }
            if (joinAny && hit)
                return true;
            if (!joinAny && !hit)
                return false;
        }
        return !joinAny;
    }

    inline bool Applies(uint8 action, ItemFacts const& item)
    {
        switch (action)
        {
            case ActionSell:       return item.sellable;
            case ActionDisenchant: return item.disenchantable;
            case ActionStore:      return item.storable;
            default:               return true;
        }
    }

    // 'rules' must be in position order. Quest items are never touched; a rule is skipped when it is off, empty, or
    // its action is not allowed. The first matching rule whose action can handle the item decides. When no matching
    // rule can, the first matching one decides and its action's fallback applies (the caller: to the storage if
    // possible, else kept).
    inline Verdict Evaluate(std::vector<Rule> const& rules, ItemFacts const& item, uint8 allowedMask)
    {
        Verdict verdict;
        if (item.questItem)
        {
            verdict.result = ResultProtected;
            return verdict;
        }

        Verdict firstMatch;
        for (Rule const& rule : rules)
        {
            if (!rule.enabled || rule.conditions.empty() || rule.action >= ActionCount
                || !(allowedMask & (1u << rule.action)))
                continue;

            if (!RuleMatches(rule, item))
                continue;

            if (Applies(rule.action, item))
            {
                verdict.result = rule.action;
                verdict.position = rule.position;
                return verdict;
            }
            if (firstMatch.result == ResultNoRule)
            {
                firstMatch.result = rule.action;
                firstMatch.position = rule.position;
            }
        }

        return firstMatch;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Validation
    // ---------------------------------------------------------------------------------------------------------------

    inline bool IsTextChar(char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '\''
            || c == '-' || c == '.' || c == ',';
    }

    inline bool ValidText(std::string const& text)
    {
        if (text.empty() || text.size() > MaxText)
            return false;
        return std::all_of(text.begin(), text.end(), IsTextChar);
    }

    inline bool ValidCondition(Condition const& c)
    {
        bool const plain = c.value2 == 0 && c.text.empty();
        switch (c.type)
        {
            case CondQuality:
                return plain && c.op <= OpAtMost && c.value <= MaxQuality;
            case CondItemLevel:
                return plain && c.op <= OpAtMost && c.value <= MaxItemLevel;
            case CondSellPrice:
                return plain && c.op <= OpAtMost;
            case CondItemType:
                return c.text.empty() && c.op == OpIs && c.value <= MaxItemClass
                    && (c.value2 <= MaxSubclass || c.value2 == AnySubclass);
            case CondItem:
                return plain && c.op == OpIs && c.value != 0;
            case CondName:
                return c.op == OpIs && c.value == 0 && c.value2 == 0 && ValidText(c.text);
            default:
                return false;
        }
    }

    // Groups are numbered 0, 1, 2 in order without gaps and none is empty; logic bits exist only for the groups in use.
    inline bool ValidRule(Rule const& rule)
    {
        if (rule.action >= ActionCount || rule.conditions.empty() || rule.conditions.size() > MaxConditions)
            return false;
        if (!std::all_of(rule.conditions.begin(), rule.conditions.end(), ValidCondition))
            return false;
        uint8 expected = 0;
        for (Condition const& c : rule.conditions)
        {
            if (c.group == expected + 1)
                ++expected;
            else if (c.group != expected)
                return false;
        }
        if (rule.conditions.front().group != 0 || std::size_t(expected) + 1 > MaxGroups)
            return false;
        uint32 const allowed = ((1u << (expected + 1)) - 1) | LogicJoinAny;
        return (rule.logic & ~allowed) == 0;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Condition codec (the value of the GADV key "c", docs/protocol.md §7.11): conditions split by ';', condition
    // fields by ':' ("type:op:value:value2:text[:group]"). The text is restricted to IsTextChar, which never contains
    // ':' or ';'.
    // ---------------------------------------------------------------------------------------------------------------

    inline bool ParseUInt(std::string const& s, uint32 max, uint32& out)
    {
        if (s.empty() || s.size() > 10)
            return false;
        uint64 v = 0;
        for (char c : s)
        {
            if (c < '0' || c > '9')
                return false;
            v = v * 10 + static_cast<uint64>(c - '0');
        }
        if (v > max)
            return false;
        out = static_cast<uint32>(v);
        return true;
    }

    inline std::vector<std::string> Split(std::string const& s, char sep)
    {
        std::vector<std::string> parts;
        std::size_t start = 0;
        while (true)
        {
            std::size_t const at = s.find(sep, start);
            if (at == std::string::npos)
            {
                parts.push_back(s.substr(start));
                return parts;
            }
            parts.push_back(s.substr(start, at - start));
            start = at + 1;
        }
    }

    // "type:op:value:value2:text:group"; a group 0 condition leaves out ":0".
    inline std::string EncodeCondition(Condition const& c)
    {
        std::string s = std::to_string(c.type) + ":" + std::to_string(c.op) + ":" + std::to_string(c.value) + ":"
            + std::to_string(c.value2) + ":" + c.text;
        if (c.group)
            s += ":" + std::to_string(c.group);
        return s;
    }

    inline bool DecodeCondition(std::string const& s, Condition& out)
    {
        std::vector<std::string> const f = Split(s, ':');
        if (f.size() != 5 && f.size() != 6)
            return false;
        uint32 type = 0;
        uint32 op = 0;
        uint32 group = 0;
        Condition c;
        if (!ParseUInt(f[0], 255, type) || !ParseUInt(f[1], 255, op) || !ParseUInt(f[2], 0xFFFFFFFFu, c.value)
            || !ParseUInt(f[3], 0xFFFFFFFFu, c.value2) || (f.size() == 6 && !ParseUInt(f[5], uint32(MaxGroups - 1), group)))
            return false;
        c.type = static_cast<uint8>(type);
        c.op = static_cast<uint8>(op);
        c.text = f[4];
        c.group = static_cast<uint8>(group);
        if (!ValidCondition(c))
            return false;
        out = std::move(c);
        return true;
    }

    inline std::string EncodeConditions(std::vector<Condition> const& conds)
    {
        std::string s;
        for (std::size_t i = 0; i < conds.size(); ++i)
        {
            if (i)
                s += ';';
            s += EncodeCondition(conds[i]);
        }
        return s;
    }

    inline bool DecodeConditions(std::string const& s, std::vector<Condition>& out)
    {
        if (s.empty())
            return false;
        std::vector<std::string> const parts = Split(s, ';');
        if (parts.size() > MaxConditions)
            return false;
        std::vector<Condition> conds;
        for (std::string const& p : parts)
        {
            Condition c;
            if (!DecodeCondition(p, c))
                return false;
            conds.push_back(std::move(c));
        }
        out = std::move(conds);
        return true;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Ordering: the vector order is the evaluation order; positions are always 1..n after a change.
    // ---------------------------------------------------------------------------------------------------------------

    inline void Renumber(std::vector<Rule>& rules)
    {
        for (std::size_t i = 0; i < rules.size(); ++i)
            rules[i].position = static_cast<uint8>(i + 1);
    }

    inline void SortByPosition(std::vector<Rule>& rules)
    {
        std::stable_sort(rules.begin(), rules.end(), [](Rule const& a, Rule const& b)
        {
            if (a.position != b.position)
                return a.position < b.position;
            return a.id < b.id;
        });
        Renumber(rules);
    }

    // pos 0 or past the end appends.
    inline void Insert(std::vector<Rule>& rules, Rule rule, uint32 pos)
    {
        if (pos == 0 || pos > rules.size())
            rules.push_back(std::move(rule));
        else
            rules.insert(rules.begin() + (pos - 1), std::move(rule));
        Renumber(rules);
    }

    inline bool Move(std::vector<Rule>& rules, uint32 id, uint32 pos)
    {
        auto it = std::find_if(rules.begin(), rules.end(), [id](Rule const& r) { return r.id == id; });
        if (it == rules.end())
            return false;
        Rule rule = std::move(*it);
        rules.erase(it);
        Insert(rules, std::move(rule), pos);
        return true;
    }

    inline bool Remove(std::vector<Rule>& rules, uint32 id)
    {
        auto it = std::find_if(rules.begin(), rules.end(), [id](Rule const& r) { return r.id == id; });
        if (it == rules.end())
            return false;
        rules.erase(it);
        Renumber(rules);
        return true;
    }
}

#endif
