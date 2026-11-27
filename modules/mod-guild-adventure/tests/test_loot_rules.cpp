// Offline tests for src/Storage/LootRules.h, the rule logic of the guild loot filter (docs/storage.md "Loot filter";
// a port of mod-loot-filter's LootFilterRules.h without the Paragon "cursed" condition).
//
// Header-only, compiled against tests/stubs/Define.h. From the module folder modules/mod-guild-adventure:
//
//     g++ -std=c++20 -Wall -Wextra -I tests/stubs tests/test_loot_rules.cpp -o test_lr && ./test_lr
//     cl /std:c++20 /EHsc /W4 /I tests\stubs tests\test_loot_rules.cpp /Fe:test_lr.exe && test_lr.exe

#include "../src/Storage/LootRules.h"
#include <cstdio>
#include <string>

using namespace GuildAdventure::LootRules;

namespace
{
    int failures = 0;
    int checks = 0;

    void Check(bool ok, char const* what)
    {
        ++checks;
        if (!ok)
        {
            ++failures;
            std::printf("FAIL: %s\n", what);
        }
    }

    Condition Cond(uint8 type, uint8 op, uint32 value, uint32 value2 = 0, std::string text = {}, uint8 group = 0)
    {
        Condition c;
        c.type = type;
        c.op = op;
        c.value = value;
        c.value2 = value2;
        c.text = std::move(text);
        c.group = group;
        return c;
    }

    Rule MakeRule(uint32 id, uint8 action, std::vector<Condition> conditions, uint8 logic = 0)
    {
        Rule r;
        r.id = id;
        r.action = action;
        r.conditions = std::move(conditions);
        r.logic = logic;
        return r;
    }

    ItemFacts Gray()
    {
        ItemFacts f;
        f.entry = 4865;          // Ruined Pelt
        f.quality = 0;
        f.itemLevel = 5;
        f.sellPrice = 5;
        f.itemClass = 15;
        f.subClass = 0;
        f.name = "Ruined Pelt";
        return f;
    }

    ItemFacts LinenCloth()
    {
        ItemFacts f;
        f.entry = 2589;
        f.quality = 1;
        f.itemLevel = 5;
        f.sellPrice = 13;
        f.itemClass = 7;
        f.subClass = 5;
        f.name = "Linen Cloth";
        return f;
    }
}

int main()
{
    uint8 const all = AllowedMask(true, true, true);

    // Matching
    Check(Matches(Cond(CondQuality, OpIs, 0), Gray()), "quality is poor");
    Check(!Matches(Cond(CondQuality, OpAtLeast, 2), LinenCloth()), "linen is not uncommon or better");
    Check(Matches(Cond(CondItemType, OpIs, 7, AnySubclass), LinenCloth()), "trade goods, any subclass");
    Check(Matches(Cond(CondItemType, OpIs, 7, 5), LinenCloth()), "trade goods / cloth");
    Check(!Matches(Cond(CondItemType, OpIs, 7, 6), LinenCloth()), "not trade goods / leather");
    Check(Matches(Cond(CondName, OpIs, 0, 0, "linen"), LinenCloth()), "name contains, case-insensitive");
    Check(Matches(Cond(CondItem, OpIs, 2589), LinenCloth()), "item id");
    Check(Matches(Cond(CondSellPrice, OpAtMost, 13), LinenCloth()), "sell price at most");

    // Evaluation: first matching rule whose action fits; quest items are protected; no match = no rule
    std::vector<Rule> rules;
    rules.push_back(MakeRule(1, ActionSell, { Cond(CondQuality, OpIs, 0) }));
    rules.push_back(MakeRule(2, ActionStore, { Cond(CondItemType, OpIs, 7, AnySubclass) }));
    Renumber(rules);
    Check(Evaluate(rules, Gray(), all).result == ActionSell, "gray -> sell");
    Check(Evaluate(rules, LinenCloth(), all).result == ActionStore, "linen -> store");
    ItemFacts quest = Gray();
    quest.questItem = true;
    Check(Evaluate(rules, quest, all).result == ResultProtected, "quest item protected");
    ItemFacts other = LinenCloth();
    other.itemClass = 2;
    Check(Evaluate(rules, other, all).result == ResultNoRule, "no rule matches");

    // An action that cannot handle the item passes it on; a disallowed action is skipped
    ItemFacts unsellable = Gray();
    unsellable.sellable = false;
    std::vector<Rule> chain;
    chain.push_back(MakeRule(1, ActionSell, { Cond(CondQuality, OpIs, 0) }));
    chain.push_back(MakeRule(2, ActionDelete, { Cond(CondQuality, OpAtMost, 1) }));
    Renumber(chain);
    Verdict v = Evaluate(chain, unsellable, all);
    Check(v.result == ActionDelete && v.position == 2, "unsellable gray falls to the delete rule");
    v = Evaluate(chain, unsellable, AllowedMask(true, true, false));
    Check(v.result == ActionSell && v.position == 1, "delete not allowed: first match decides (fallback)");

    // Groups: (quality is poor OR name contains pelt) AND item level at most 10
    Rule grouped = MakeRule(7, ActionDelete, { Cond(CondQuality, OpIs, 0, 0, {}, 0), Cond(CondName, OpIs, 0, 0, "pelt", 0),
        Cond(CondItemLevel, OpAtMost, 10, 0, {}, 1) }, 0x1);
    Check(ValidRule(grouped), "grouped rule valid");
    Check(RuleMatches(grouped, Gray()), "grouped rule matches the pelt");
    Check(!RuleMatches(grouped, LinenCloth()), "grouped rule does not match linen");

    // Validation
    Check(!ValidRule(MakeRule(1, ActionKeep, {})), "a rule needs a condition");
    Check(!ValidRule(MakeRule(1, 9, { Cond(CondQuality, OpIs, 0) })), "unknown action");
    Check(!ValidCondition(Cond(5, OpIs, 1)), "the Paragon cursed condition (5) is gone");
    Check(!ValidCondition(Cond(CondQuality, OpIs, 8)), "quality above 7");
    Check(!ValidCondition(Cond(CondName, OpIs, 0, 0, "a:b")), "':' is not allowed in a name");
    Check(!ValidRule(MakeRule(1, ActionKeep, { Cond(CondQuality, OpIs, 0, 0, {}, 1) })), "groups start at 0");

    // Codec round trip
    std::string const encoded = EncodeConditions(grouped.conditions);
    Check(encoded == "0:0:0:0:;7:0:0:0:pelt;1:2:10:0::1", encoded.c_str());
    std::vector<Condition> decoded;
    Check(DecodeConditions(encoded, decoded) && decoded.size() == 3 && decoded[2].group == 1 && decoded[1].text == "pelt",
        "decode what was encoded");
    Check(!DecodeConditions("", decoded), "empty is not a condition list");
    Check(!DecodeConditions("0:0:9:0:", decoded), "quality 9 refused by the decoder");
    Check(!DecodeConditions("5:0:1:0:", decoded), "cursed refused by the decoder");

    // Ordering
    std::vector<Rule> order;
    for (uint32 id = 1; id <= 3; ++id)
        order.push_back(MakeRule(id, ActionKeep, { Cond(CondItem, OpIs, id) }));
    Renumber(order);
    Check(Move(order, 3, 1) && order[0].id == 3 && order[0].position == 1 && order[2].id == 2, "move to the top");
    Check(Remove(order, 1) && order.size() == 2 && order[1].position == 2, "remove renumbers");
    Insert(order, MakeRule(9, ActionKeep, { Cond(CondItem, OpIs, 9) }), 0);
    Check(order.back().id == 9 && order.back().position == 3, "insert at 0 appends");

    std::printf("%d of %d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
