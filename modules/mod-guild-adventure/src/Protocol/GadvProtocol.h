/*
 * GadvProtocol: the server side of the GADV addon protocol (docs/protocol.md, version 1).
 *
 * Transport: the addon whispers its own character with LANG_ADDON and the prefix "GADV". The chat hook hands
 * those whispers to OnAddonWhisper, which only queues them; Update (world thread, WorldScript::OnUpdate)
 * reassembles the chunks, rate-limits, decodes and dispatches them. Replies and pushes go to the character the
 * owner's client controls right now, as addon whispers from that character to itself.
 *
 * One "link" per owner account: the reassembly buffers, the outgoing message ids, the rate limit and the last
 * BOT values sent (pushes are deltas against them). A link is connected after a valid HELLO from the character
 * the client plays; after a swap the addon sends HELLO again from the new character (Path R reloads the UI).
 */

#ifndef MOD_GUILD_ADVENTURE_GADV_PROTOCOL_H
#define MOD_GUILD_ADVENTURE_GADV_PROTOCOL_H

#include "Define.h"
#include "GadvCodec.h"
#include "ObjectGuid.h"
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class Player;
class WorldSession;

namespace GuildAdventure
{
    struct AdventureGuild;
    struct GuildBot;
    struct GuildEvent;
    struct SwapEvent;
}

class GadvProtocol
{
public:
    static GadvProtocol& Instance();

    // Startup: registers the swap event listener.
    void Init();

    // Chat hook (PlayerScript::OnPlayerCanUseChat). Returns true when the whisper is a GADV message the module
    // consumes; the core must then not deliver it.
    bool OnAddonWhisper(Player* player, std::string const& text);
    // True for the text of a GADV transport message ("GADV\t" + chunk), whoever sent it to whom.
    [[nodiscard]] static bool IsTransportText(std::string const& text);

    void Update(uint32 diff);
    void OnPlayerLogout(uint32 accountId, ObjectGuid guid);

    // Pushes to the owner's addon (no-ops while it is not connected).
    void PushEvent(GuildAdventure::AdventureGuild const& guild, GuildAdventure::GuildEvent const& event);
    void PushState(GuildAdventure::AdventureGuild const& guild);
    void PushBot(GuildAdventure::AdventureGuild const& guild, ObjectGuid bot, bool full);
    void PushBotRemoved(GuildAdventure::AdventureGuild const& guild, ObjectGuid bot);
    void PushUnlocks(GuildAdventure::AdventureGuild const& guild);
    void PushNotice(uint32 ownerAccount, std::string const& message, bool warning);
    // Guild perks (GadvPerks.cpp): the PERKS block after a purchase.
    void PushPerks(GuildAdventure::AdventureGuild const& guild);
    // Craft orders of the guild (GadvCraftOrders.cpp): the CRAFT_ORDERS block after a change.
    void PushCraftOrders(GuildAdventure::AdventureGuild const& guild);
    // Tiered guild achievements (GadvMilestones.cpp): the MILESTONES block after the guild reached a tier.
    void PushMilestones(GuildAdventure::AdventureGuild const& guild);
    // Guild storage and loot filter (docs/storage.md, docs/protocol.md §7.10-§7.13): storage deltas and new loot log
    // lines every second (world update), the loot settings and rules after a change from outside the addon.
    void PushStorageChanges(uint32 diff);
    void PushLoot(uint32 ownerAccount);
    // The guild storage's auction rules and what is listed (docs/protocol.md §7.14).
    void PushAuctions(uint32 ownerAccount);
    // The guild storage's shopping list (GadvShopping.cpp, docs/protocol.md section 7.25): after a purchase or a change
    // from outside the addon.
    void PushShopping(uint32 ownerAccount);
    // Bot behaviour settings (GadvSettings.cpp, docs/protocol.md section 7.19): the values of one scope after a change from
    // outside the addon (character empty = the guild's), and the schema to every connected addon after a config reload.
    void PushSettingValues(uint32 ownerAccount, ObjectGuid character);
    void PushSettingDefs();

private:
    GadvProtocol() = default;

    using Message = GuildAdventure::Gadv::Message;
    using FieldMap = std::map<std::string, std::string>;

    struct Partial
    {
        uint32 total = 0;
        uint32 received = 0;
        uint32 firstAtMs = 0;
        std::vector<std::optional<std::string>> parts;
    };

    struct Link
    {
        bool connected = false;
        ObjectGuid player;                          // the character that sent HELLO
        uint32 nextOutId = 0;
        std::map<uint32, Partial> partials;         // by incoming message id
        std::deque<uint32> partialOrder;            // oldest first
        uint32 rateWindowStartMs = 0;
        uint32 rateCount = 0;
        uint32 lastRateErrorMs = 0;
        std::unordered_map<ObjectGuid, FieldMap> sentBots;
        FieldMap sentLeader;                        // the last LEADER record (docs/protocol.md section 7.15)
        int64 helloSettingsVersion = 0;             // HELLO^sv: the settings schema the addon has cached
        std::string sentDungeonRuns;                // the last DRUNS block, as text (sent again when it changes)
        uint32 sentDungeonProgress = 0;             // OrderService::GetDungeonProgressVersion when DUNGEONS last went out
    };

    struct Inbound
    {
        uint32 accountId = 0;
        ObjectGuid player;
        std::string chunk;
    };

    // Receive path
    void HandleChunk(uint32 accountId, ObjectGuid playerGuid, std::string const& text);
    void HandleMessage(uint32 accountId, Player* player, Message const& message);
    bool CheckRate(Link& link, uint32 accountId);

    // Intents (docs/protocol.md section 6)
    void OnHello(uint32 accountId, Player* player, GuildAdventure::AdventureGuild* guild, Message const& message);
    void OnGet(uint32 accountId, Player* player, GuildAdventure::AdventureGuild& guild, Message const& message);
    void OnOrderSet(uint32 accountId, GuildAdventure::AdventureGuild& guild, Message const& message);
    void OnBotState(uint32 accountId, GuildAdventure::AdventureGuild& guild, Message const& message);
    void OnBotDismiss(uint32 accountId, GuildAdventure::AdventureGuild& guild, Message const& message);
    void OnProfSet(uint32 accountId, GuildAdventure::AdventureGuild& guild, Message const& message);
    void OnRecruit(uint32 accountId, GuildAdventure::AdventureGuild& guild, Message const& message);
    void OnControl(uint32 accountId, Player* player, GuildAdventure::AdventureGuild& guild, Message const& message, bool take);
    // CONTROL_WATCH^on: watch mode of the owner's current character (docs/take-control.md §5.12)
    void OnControlWatch(uint32 accountId, Player* player, GuildAdventure::AdventureGuild& guild, Message const& message);
    void OnUnlockBuy(uint32 accountId, GuildAdventure::AdventureGuild& guild, Message const& message);
    // Guild storage, loot filter, crafting (GadvStorage.cpp)
    // Talent templates (GadvSpecs.cpp)
    void SendSpecs(uint32 accountId);
    void SendDungeons(uint32 accountId);
    void OnSpecSet(uint32 accountId, GuildAdventure::AdventureGuild& guild, Message const& message);
    // Dungeon roles (GadvRoles.cpp, docs/protocol.md "Dungeon roles").
    void OnRoleSet(uint32 accountId, GuildAdventure::AdventureGuild& guild, Message const& message);
    // The dungeon run window (GadvRoles.cpp, docs/protocol.md "Dungeon runs").
    void OnDungeonStart(uint32 accountId, GuildAdventure::AdventureGuild& guild, Message const& message);
    void OnDungeonStop(uint32 accountId, GuildAdventure::AdventureGuild& guild, Message const& message);
    // Block DRUNS: the guild's dungeon runs. `force`: also when nothing changed since the last one.
    void SendDungeonRuns(uint32 accountId, GuildAdventure::AdventureGuild& guild, bool force);
    // Block DPREP: what the run window's preparation step offers (the storage's items a run can use, the fight rules).
    void SendDungeonPrep(uint32 accountId, GuildAdventure::AdventureGuild& guild);
    void SendRoleGear(uint32 accountId, GuildAdventure::AdventureGuild& guild, Message const& message);
    void SendPlan(uint32 accountId, ObjectGuid guid);
    // Fight profiles (GadvFight.cpp, docs/protocol.md "Fight profiles").
    void SendFightProfiles(uint32 accountId, GuildAdventure::AdventureGuild& guild, ObjectGuid bot);
    void SendFightProfile(uint32 accountId, GuildAdventure::AdventureGuild& guild, ObjectGuid bot, uint32 id);
    void OnFightAssign(uint32 accountId, GuildAdventure::AdventureGuild& guild, Message const& message);
    void OnFightOption(uint32 accountId, GuildAdventure::AdventureGuild& guild, Message const& message);
    void OnFightCopy(uint32 accountId, GuildAdventure::AdventureGuild& guild, Message const& message);
    void OnFightSave(uint32 accountId, GuildAdventure::AdventureGuild& guild, Message const& message);
    void OnFightDelete(uint32 accountId, GuildAdventure::AdventureGuild& guild, Message const& message);
    void SendFightCatalog(uint32 accountId, GuildAdventure::AdventureGuild& guild, ObjectGuid bot);
    // Squads (GadvSquads.cpp, docs/protocol.md "Squads").
    void OnSquadSet(uint32 accountId, GuildAdventure::AdventureGuild& guild, Message const& message);
    void OnSpecSave(uint32 accountId, Player* player, GuildAdventure::AdventureGuild& guild, Message const& message);
    void OnSpecDelete(uint32 accountId, GuildAdventure::AdventureGuild& guild, Message const& message);
    void OnStorageTake(uint32 accountId, Player* player, Message const& message);
    void OnStorageDeposit(uint32 accountId, Player* player, Message const& message);
    void OnStorageSellJunk(uint32 accountId, Player* player, Message const& message);
    void OnStorageSell(uint32 accountId, Player* player, Message const& message);
    void OnStorageDelete(uint32 accountId, Player* player, Message const& message);
    void OnStorageReserve(uint32 accountId, Player* player, Message const& message);
    void OnStorageDisenchant(uint32 accountId, Player* player, Message const& message);
    // STORAGE_ENCH when the guild's best enchanter in the world changed (part of PushStorageChanges).
    void PushEnchanters();
    void OnLootRuleSet(uint32 accountId, Message const& message);
    void OnLootRuleDelete(uint32 accountId, Message const& message);
    void OnLootRuleMove(uint32 accountId, Message const& message);
    void OnLootRuleEnable(uint32 accountId, Message const& message);
    void OnLootOptions(uint32 accountId, Message const& message);
    void OnCraft(uint32 accountId, Player* player, GuildAdventure::AdventureGuild& guild, Message const& message);
    void OnAuctionRuleSet(uint32 accountId, Message const& message);
    void OnAuctionRuleDelete(uint32 accountId, Message const& message);
    void OnAuctionCategorySet(uint32 accountId, Message const& message);
    void OnAuctionCategoryDelete(uint32 accountId, Message const& message);
    void OnPerkBuy(uint32 accountId, GuildAdventure::AdventureGuild& guild, Message const& message);
    void OnCraftOrderAdd(uint32 accountId, GuildAdventure::AdventureGuild& guild, Message const& message);
    void OnCraftOrderDelete(uint32 accountId, GuildAdventure::AdventureGuild& guild, Message const& message);
    // Bot behaviour settings (GadvSettings.cpp)
    void OnSettingSet(uint32 accountId, GuildAdventure::AdventureGuild& guild, Message const& message);
    // The item picker of the farm order (GadvFarm.cpp)
    void OnFarmSearch(uint32 accountId, GuildAdventure::AdventureGuild& guild, Message const& message);
    // The shopping list (GadvShopping.cpp)
    void OnShoppingSet(uint32 accountId, GuildAdventure::AdventureGuild& guild, Message const& message);
    void OnShoppingDelete(uint32 accountId, Message const& message);
    void OnShoppingSearch(uint32 accountId, GuildAdventure::AdventureGuild& guild, Message const& message);

    // Swaps (ControlSwapService listener)
    void OnSwapEvent(GuildAdventure::SwapEvent const& event);

    // Send path
    void Send(uint32 accountId, Message const& message);
    void SendTo(Player* player, Link& link, Message const& message);
    void Ack(uint32 accountId, std::optional<int64> request);
    void Error(uint32 accountId, std::optional<int64> request, std::string const& code, std::string const& text,
        std::string const& reason = {});
    void SendSync(uint32 accountId, Player* player, GuildAdventure::AdventureGuild& guild, int64 clientZoneVersion);
    void SendRoster(uint32 accountId, GuildAdventure::AdventureGuild const& guild);
    void SendZones(uint32 accountId);
    void SendUnlocks(uint32 accountId, GuildAdventure::AdventureGuild const& guild);
    void SendEvents(uint32 accountId, GuildAdventure::AdventureGuild const& guild);
    void SendAway(uint32 accountId);
    void SendStorage(uint32 accountId);
    void SendLoot(uint32 accountId);
    void SendLootLog(uint32 accountId);
    void SendRecipes(uint32 accountId, Player* crafter);
    void SendAuctions(uint32 accountId);
    void SendShopping(uint32 accountId);
    void SendPerks(uint32 accountId, GuildAdventure::AdventureGuild const& guild);
    void SendCraftOrders(uint32 accountId, GuildAdventure::AdventureGuild const& guild);
    void SendCraftables(uint32 accountId, GuildAdventure::AdventureGuild const& guild);
    void SendSettings(uint32 accountId, GuildAdventure::AdventureGuild const& guild, int64 clientSchemaVersion);
    void SendSettingDefs(uint32 accountId);
    void SendSettingValues(uint32 accountId, GuildAdventure::AdventureGuild const& guild);
    Message BuildSettingValues(uint32 ownerAccount, ObjectGuid character) const;
    void SendMilestones(uint32 accountId, GuildAdventure::AdventureGuild const& guild);
    void SendBlock(uint32 accountId, std::string const& name, std::vector<Message> const& records, Message begin);
    void PushBotDeltas();

    // Builders
    Message BuildState(GuildAdventure::AdventureGuild const& guild) const;
    Message BuildControlState(GuildAdventure::AdventureGuild const& guild, Player* current, bool busy) const;
    FieldMap BuildBotFields(GuildAdventure::AdventureGuild const& guild, GuildAdventure::GuildBot const& bot) const;
    // The leader record (docs/protocol.md section 7.15): its order (HOLD without one), what it does, where it is.
    FieldMap BuildLeaderFields(GuildAdventure::AdventureGuild const& guild) const;
    void PushLeader(GuildAdventure::AdventureGuild const& guild, bool full);
    Message BuildEvent(GuildAdventure::GuildEvent const& event) const;

    Link* FindConnectedLink(uint32 accountId);
    Player* GetClientPlayer(uint32 accountId) const;

    std::unordered_map<uint32, Link> _links; // by owner account
    std::unordered_map<uint32, std::pair<uint32, std::string>> _enchanters; // owner -> the best enchanter last sent
    std::mutex _inboxLock;
    std::vector<Inbound> _inbox;
    uint32 _deltaTimer = 0;
    uint32 _storageTimer = 0;
};

#define sGadvProtocol GadvProtocol::Instance()

#endif
