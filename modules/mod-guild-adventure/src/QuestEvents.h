/*
 * QuestEvents: quests whose credit comes from a script event, and what a bot has to do for it
 * (docs/orders.md "Quest events").
 *
 * Killing, looting and walking into an area trigger do not complete these quests; a script gives the credit when
 * something happens. Three kinds of "something" can be read from the world database, and the order planner plays them:
 *   - Talk: a gossip choice (or just greeting the NPC or game object) gives the credit, for the quest's event
 *           ("Hidden Enemies": tell Neeru Fireblade the password) or for a creature credit of an objective ("Lost in
 *           Battle": look at the corpse). The way through the menus to the choice is found on the menus themselves.
 *   - Stay: the quest giver's script starts an event when the quest is taken (a defence, a brawl, a speech) and
 *           credits the quest when it is over. The bot stays with the giver and fights what attacks either of them.
 *   - Lead: the quest giver follows whoever took the quest until it stands next to another NPC ("Mist"). The bot walks
 *           there and waits for it on the way.
 *   - Enter: walking into an area trigger starts the event ("Resupplying the Excavation": an ambush on the dwarves at
 *           the road). The bot walks into it, stays and fights.
 *   - Clear: the creature of an objective shows itself only when the place around its spawn is empty of enemies
 *           ("WANTED: Murkdeep!": no Greymist Warrior or Hunter alive near the camp fire, then three waves, then he
 *           comes). The bot kills what is hostile around the spawn, stays and fights what comes. Hand list only.
 * Escorts (the NPC walks a path, the bot follows) are QuestSupport's and the planner's "escort" task.
 *
 * How credit is traced (Load): every SmartAI row that credits a quest (actions "area explored or event happens",
 * "group event happens", "killed monster") is followed back to the events that start it: through links and timed
 * action lists, through "set data" (to the rows that set it on that creature) and through summons (the death of a
 * summoned creature goes back to whoever summons it). A gossip event at the root makes a Talk plan; an "accepted quest" event in the giver's own script makes a
 * Stay plan for a quest that has nothing else to do; a "follow" action with an end creature and an event credit makes a
 * Lead plan. C++ scripts cannot be read: their quests are listed by hand in ga_quest_event_template.
 *
 * World thread only (Load at startup and on .ga reload; the planner and its talk run outside map updates).
 */

#ifndef MOD_GUILD_ADVENTURE_QUEST_EVENTS_H
#define MOD_GUILD_ADVENTURE_QUEST_EVENTS_H

#include "Define.h"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Player;
class WorldObject;

namespace GuildAdventure
{
    enum class QuestEventKind : uint8
    {
        None,
        Talk,
        Stay,
        Lead,
        Enter,
        Clear
    };

    char const* ToString(QuestEventKind kind);

    struct QuestEventPlan
    {
        QuestEventKind kind = QuestEventKind::None;
        int32 objective = -1;      // -1: the quest's event ("exploration or event"); 0-3: the creature credit of that objective
        uint32 entry = 0;          // Talk: whom to talk to; Stay: the NPC of the event (the giver); Lead: the NPC that follows;
                                   // Enter: the area trigger; Clear: the creature whose spawn is the place to clear
        bool gameObject = false;   // Talk: `entry` is a game object
        bool hello = false;        // Talk: greeting it is enough
        uint32 menuId = 0;         // Talk: the gossip menu ...
        uint32 optionId = 0;       // ... and the option on it that gives the credit
        uint32 endEntry = 0;       // Lead: the creature to bring it to
    };

    enum class TalkResult : uint8
    {
        Chosen,     // the choice was made (or the greeting sent)
        NoMenu,     // the NPC did not open a menu for the bot
        NoWay       // a menu opened, but nothing on it leads to the choice (a condition the bot does not meet)
    };
}

class QuestEvents
{
public:
    static QuestEvents& Instance();

    void Load();

    // The plans of a quest: at most one for its event, and one per objective a talk credits. Empty for most quests.
    [[nodiscard]] std::vector<GuildAdventure::QuestEventPlan> const& GetPlans(uint32 questId) const;
    // The plan for the quest's event (objective -1), nullptr if there is none.
    [[nodiscard]] GuildAdventure::QuestEventPlan const* GetEventPlan(uint32 questId) const;

    // Talks to the creature or game object like a client: greets it (uses the object), then picks options until the
    // plan's choice is made. `detail` gets what happened, for the activity log.
    GuildAdventure::TalkResult Talk(Player* player, WorldObject* target, GuildAdventure::QuestEventPlan const& plan,
        std::string& detail) const;

    // For .ga admin quests: the counts, and with a quest id its plans.
    [[nodiscard]] std::vector<std::string> Describe(uint32 questId = 0) const;

private:
    QuestEvents() = default;

    // True when gossip menu `to` can be reached from menu `from` through the action menus of gossip options.
    [[nodiscard]] bool Reaches(uint32 from, uint32 to) const;

    std::unordered_map<uint32, std::vector<GuildAdventure::QuestEventPlan>> _plans;
    uint32 _talks = 0, _stays = 0, _leads = 0, _enters = 0, _clears = 0;
};

#define sQuestEvents QuestEvents::Instance()

#endif
