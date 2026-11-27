/*
 * Auto loot for guild characters (LootService::OnCharacterUpdate, docs/storage.md "Auto loot").
 *
 * A port of ForgottenLand's mod-auto-loot (the owner's own module): every few hundred milliseconds a guild character
 * empties its own kills within a few yards, skins them when it can, and gathers the mining and herb nodes in reach,
 * without walking to each corpse or opening a loot window. Differences to the original:
 *   - items are taken through the core's Player::StoreLootItem, so quest items, roll winners, loot achievements and
 *     OnPlayerLootItem behave exactly as for a click in the loot window (the original built the items itself and lost
 *     their random suffix);
 *   - in a dungeon or a raid the group's loot method counts: with group loot or need before greed the rolls start
 *     when the first member reaches the corpse (as when a loot window opens), and an item a roll is running for
 *     stays in the corpse until it is decided. In the open world nothing is rolled for: whoever of the group stands
 *     there takes it, as before;
 *   - what does not fit stays in the corpse (nothing is mailed: the bags of guild characters stay empty because the
 *     loot filter moves their loot into the guild storage);
 *   - a group is fine as long as every member in it is a character of the same guild (the leader with its following
 *     bots); the corpse must belong to the character or to its group;
 *   - the switch is per guild (the addon's loot settings) instead of per character.
 *
 * Runs in the character's own update (PlayerScript::OnPlayerUpdate, its map thread): everything it touches is on the
 * same map. The loot filter then sees the items through the loot hook, like any other loot.
 */

#include "CellImpl.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "GuildAdventureConfig.h"
#include "GuildAdventureSettings.h"
#include "Bag.h"
#include "BotSettings.h"
#include "GuildAdventureMgr.h"
#include "GuildBank.h"
#include "GuildBotHolder.h"
#include "LootItemStorage.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include <map>
#include "LootMgr.h"
#include "Map.h"
#include "Log.h"
#include "LootService.h"
#include "Player.h"
#include "Professions.h"
#include "QuestInteraction.h"
#include "StorageService.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "ScriptMgr.h"

using namespace GuildAdventure;

namespace
{
    constexpr uint32 SpellMining = 2575;
    constexpr uint32 SpellHerbGathering = 2366;
    // The skinning ranks, highest first (the corpse's required skill is checked by the spell).
    constexpr uint32 SkinningSpells[] = { 50305, 32678, 10768, 8618, 8617, 8613 };
    constexpr uint32 MinFreeSlots = 4;

    // Until when the corpse scan of auto loot runs for a character (getMSTime): armed by a kill it or its group has the
    // loot of (PF-3). Kept in the character's own data, written and read on its map's thread.
    struct AutoLootArmed : public DataMap::Base
    {
        uint32 until = 0;
    };
    constexpr char const* AutoLootArmedKey = "ga-auto-loot-armed";
    // How long after such a kill corpses are looked for (the loot of a fight, the skinning after it, bags that had no
    // room at first).
    constexpr uint32 ArmedMs = 30 * IN_MILLISECONDS;
    constexpr float ArmGroupYards = 100.0f;

    void Arm(Player* player, uint32 now)
    {
        player->CustomData.GetDefault<AutoLootArmed>(AutoLootArmedKey)->until = std::max<uint32>(now + ArmedMs, 1);
    }

    bool IsArmed(Player* player, uint32 now)
    {
        AutoLootArmed const* armed = player->CustomData.Get<AutoLootArmed>(AutoLootArmedKey);
        return armed && armed->until && int32(armed->until - now) > 0;
    }

    // The gathering spell the character can open this node with, 0 for none (the lock's skill case decides; the
    // node's required skill rank is checked by the spell itself).
    uint32 GatherSpellFor(Player* player, GameObject* go)
    {
        LockEntry const* lock = sLockStore.LookupEntry(go->GetGOInfo()->GetLockId());
        if (!lock)
            return 0;
        for (uint8 i = 0; i < MAX_LOCK_CASE; ++i)
        {
            if (lock->Type[i] != LOCK_KEY_SKILL)
                continue;
            if (lock->Index[i] == LOCKTYPE_MINING && player->HasSkill(SKILL_MINING))
                return SpellMining;
            if (lock->Index[i] == LOCKTYPE_HERBALISM && player->HasSkill(SKILL_HERBALISM))
                return SpellHerbGathering;
        }
        return 0;
    }

    // The nearest node in range that is ready, that this character can gather and that it stands close enough to (the
    // gathering spell has its own reach, the node's interact distance: a cast from farther away fails).
    // Nodes only: a quest object with a herb or mining lock (a Plant Bundle) is the quest's, not a herb.
    class NearestGatherableNodeCheck
    {
    public:
        NearestGatherableNodeCheck(Player* player, float range) : _player(player), _range(range) { }

        bool operator()(GameObject* go)
        {
            if (go->GetGoType() != GAMEOBJECT_TYPE_CHEST || go->getLootState() != GO_READY || go->IsInvisibleDueToDespawn()
                || !_player->IsWithinDistInMap(go, _range) || !sProfessions.IsNode(go->GetEntry()) || !GatherSpellFor(_player, go)
                || !go->IsWithinDistInMap(_player))
                return false;
            _range = _player->GetDistance(go);
            return true;
        }

    private:
        Player* _player;
        float _range;
    };

    // In a group, every member in the world must be a character of the same guild (same WoW guild); battleground and
    // dungeon-finder groups never.
    bool GroupAllowsAutoLoot(Player* player)
    {
        Group* group = player->GetGroup();
        if (!group)
            return true;
        if (group->isBGGroup() || group->isLFGGroup())
            return false;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource())
                if (member->GetGuildId() != player->GetGuildId())
                    return false;
        return true;
    }

    bool IsOwnKill(Player* player, Creature* creature)
    {
        if (creature->GetLootRecipient() == player)
            return true;
        Group* group = player->GetGroup();
        return group && creature->GetLootRecipientGroup() == group;
    }

    // In a dungeon or a raid the group's loot method counts as for a loot window: what Player::SendLoot does for the
    // first member that opens the corpse (the rolls of group loot and need before greed, the master looter's share).
    // The core tells a corpse nobody has opened by loot_type LOOT_NONE and sets the type with the window.
    void StartGroupRolls(Creature* creature, Loot* loot)
    {
        if (loot->loot_type != LOOT_NONE || !creature->GetMap()->IsDungeon())
            return;
        Group* group = creature->GetLootRecipientGroup();
        if (!group)
            return;
        switch (group->GetLootMethod())
        {
            case GROUP_LOOT:
                group->GroupLoot(loot, creature);
                break;
            case NEED_BEFORE_GREED:
                group->NeedBeforeGreed(loot, creature);
                break;
            case MASTER_LOOT:
                group->MasterLoot(loot, creature);
                break;
            default:
                return;
        }
        loot->loot_type = LOOT_CORPSE;
    }

    // Takes every item of 'loot' the character may have; true if nothing it may take is left.
    bool TakeLootItems(Player* player, Loot* loot)
    {
        bool allTaken = true;
        uint32 const maxSlot = loot->GetMaxSlotInLootFor(player);
        for (uint32 i = 0; i < maxSlot; ++i)
        {
            LootItem* item = loot->LootItemInSlot(i, player);
            if (!item) // looted already (LootItemInSlot answers nullptr then)
                continue;
            // An item a roll is running for, the master looter's, or one another member won: it stays (the core
            // would refuse it and close the character's loot window for it). Quest items use the field otherwise.
            if (i < loot->items.size() && (item->is_blocked || (item->rollWinnerGUID && item->rollWinnerGUID != player->GetGUID())))
            {
                allTaken = false;
                continue;
            }
            InventoryResult msg = EQUIP_ERR_OK;
            player->StoreLootItem(uint8(i), loot, msg);
            if (msg != EQUIP_ERR_OK)
                allTaken = false;
        }
        return allTaken;
    }

    // A corpse whose loot is gone, as WorldSession::DoLootRelease leaves it.
    void FinishCorpse(Creature* creature)
    {
        Loot* loot = &creature->loot;
        if (!loot->isLooted())
            return;
        creature->RemoveDynamicFlag(UNIT_DYNFLAG_LOOTABLE);
        if (!creature->IsAlive())
            creature->AllLootRemovedFromCorpse();
        loot->clear();
    }

    // A spell that opened a loot window for the character (skinning, gathering): close it again.
    void ReleaseLootWindow(Player* player, ObjectGuid guid, Loot* loot)
    {
        if (player->GetLootGUID() == guid)
            player->SetLootGUID(ObjectGuid::Empty);
        player->SendLootRelease(guid);
        player->RemoveUnitFlag(UNIT_FLAG_LOOTING);
        loot->RemoveLooter(player->GetGUID());
    }

    void SkinCorpse(Player* player, Creature* creature)
    {
        uint32 spellId = 0;
        for (uint32 rank : SkinningSpells)
            if (player->HasSpell(rank))
            {
                spellId = rank;
                break;
            }
        if (!spellId)
            return;

        player->CastSpell(creature, spellId, true);
        if (creature->HasUnitFlag(UNIT_FLAG_SKINNABLE))
            return; // the spell refused (skill too low)

        Loot* loot = &creature->loot;
        TakeLootItems(player, loot);
        ReleaseLootWindow(player, creature->GetGUID(), loot);
        FinishCorpse(creature);
    }

    void GatherNode(Player* player, float range)
    {
        GameObject* node = nullptr;
        NearestGatherableNodeCheck check(player, range);
        Acore::GameObjectLastSearcher<NearestGatherableNodeCheck> searcher(player, node, check);
        Cell::VisitObjects(player, searcher, range);
        if (!node)
            return;

        // Only a cast that opened the node counts: the spell fails out of reach, without the tool, and at every other
        // try at a node as hard as the skill allows. (An unopened node has no loot, and "everything taken" of nothing
        // despawned it ungathered.)
        if (player->CastSpell(node, GatherSpellFor(player, node), true) != SPELL_CAST_OK || player->GetLootGUID() != node->GetGUID())
            return;
        Loot* loot = &node->loot;
        bool const allTaken = TakeLootItems(player, loot);
        ReleaseLootWindow(player, node->GetGUID(), loot);
        if (allTaken)
            node->DespawnOrUnsummon(); // as mod-auto-loot: the node respawns on its timer
    }
}

void LootService::OnCharacterUpdate(Player* player, uint32 diff)
{
    Settings const& settings = GetSettings();
    if (!settings.autoLootEnable || !IsModuleEnabled())
        return;

    // Discovery is throttled; the update runs every world tick per character. A scan once every ScanIntervalMs, at a
    // phase of the character's own: when a multiple of the interval (shifted by its guid) passed in the last update.
    // Nothing is looked up for the other updates (PF-3: a string-keyed timer per character per tick cost more than the
    // scans).
    uint32 const now = getMSTime();
    if (settings.autoLootScanMs)
    {
        uint32 const phase = uint32((uint64(player->GetGUID().GetCounter()) * 2654435761ull) % settings.autoLootScanMs);
        if (diff < settings.autoLootScanMs && (now + phase) % settings.autoLootScanMs >= diff)
            return;
    }

    if (!player->IsInWorld() || !player->IsAlive() || player->IsInFlight() || player->IsBeingTeleported())
        return;
    // Corpses only after a kill it has the loot of (GuildAdventure.AutoLoot.ArmByKill); nodes for a gatherer always.
    bool const corpsesDue = !settings.autoLootArmByKill || IsArmed(player, now);
    bool const gatherer = player->HasSkill(SKILL_MINING) || player->HasSkill(SKILL_HERBALISM);
    if (!corpsesDue && !gatherer)
        return;
    if (player->GetFreeInventorySpace() < MinFreeSlots)
        return;
    uint32 const owner = sStorageService.GetOwnerOf(player);
    if (!owner || !IsAutoLootOn(owner) || !GroupAllowsAutoLoot(player))
        return;

    float const range = settings.autoLootRange;
    std::list<Creature*> corpses;
    if (corpsesDue)
        player->GetDeadCreatureListInGrid(corpses, range);
    uint32 gold = 0;
    for (Creature* creature : corpses)
    {
        // Only the character's own kills (the core's tap), never a corpse whose loot window someone has open.
        if (!IsOwnKill(player, creature) || player->GetLootGUID() == creature->GetGUID())
            continue;

        Loot* loot = &creature->loot;
        if (creature->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE) && loot->loot_type != LOOT_SKINNING)
        {
            StartGroupRolls(creature, loot);
            // As the core's loot window does (CMSG_LOOT_MONEY): scripts may change the gold first (guild perks).
            sScriptMgr->OnPlayerBeforeLootMoney(player, loot);
            uint32 const coins = loot->gold;
            gold += coins;
            loot->gold = 0;
            bool const allTaken = TakeLootItems(player, loot);
            FinishCorpse(creature);
            if (settings.debug)
                LOG_INFO("module", "mod-guild-adventure: auto loot: {} emptied {} ({} copper{})", player->GetName(),
                    creature->GetName(), coins, allTaken ? "" : ", some items left in it");
        }

        // Skinning in passing, as mining and herbs.
        // (Not a corpse a quest interaction wants: a skinned corpse is gone at once. QuestInteraction::KeepsCorpse.)
        if (!creature->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE) && creature->HasUnitFlag(UNIT_FLAG_SKINNABLE)
            && player->HasSkill(SKILL_SKINNING) && !QuestInteraction::KeepsCorpse(player->GetGUID(), creature->GetEntry()))
            SkinCorpse(player, creature);
    }

    if (gold)
    {
        {
            GuildBank::IncomeSource source("looted coin (auto loot)");
            player->ModifyMoney(gold);
        }
        player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_MONEY, gold);
        WorldPacket data(SMSG_LOOT_MONEY_NOTIFY, 4 + 1);
        data << uint32(gold);
        data << uint8(1); // "you loot ..." (alone)
        player->SendDirectMessage(&data);
    }

    if (gatherer)
        GatherNode(player, range);
}

void LootService::OnKillCredit(Player* player, Creature* killed)
{
    if (!player || !killed || !GetSettings().autoLootEnable || !GetSettings().autoLootArmByKill || !IsModuleEnabled())
        return;
    // Map thread of the creature's map: the credited character and the members of its group on that map (the group's
    // tap: whoever of them scans first loots, as before).
    uint32 const now = getMSTime();
    Arm(player, now);
    if (Group* group = player->GetGroup())
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource(); member && member != player && member->IsInWorld() &&
                member->GetMap() == killed->GetMap() && member->GetExactDist2d(killed) <= ArmGroupYards)
                Arm(member, now);
}

// ---------------------------------------------------------------------------------------------------------------
// Containers (the owner, 2026-11-13: [quote left out]; and [quote left out] for those already in the guild storage). docs/storage.md "Containers".
// ---------------------------------------------------------------------------------------------------------------

namespace
{
    constexpr uint32 PickLockSpell = 1804;         // the rogue's Pick Lock
    constexpr uint32 OpenMinFreeSlots = 2;         // room for what comes out
    constexpr uint32 OpenPerCharacter = 5;         // containers of its bags a character opens per look
    constexpr uint32 OpenFromStoragePerGuild = 5;  // containers of the guild storage opened per look
    constexpr uint32 StorageOpenerFreeSlots = 6;   // a character takes containers out only with this much room

    // Every item in the backpack and the bags (not the bank, not what is worn).
    template <typename Callback>
    void ForBagItems(Player* player, Callback&& callback)
    {
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (!callback(item))
                    return;
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = player->GetBagByPos(bagSlot))
                for (uint32 i = 0; i < bag->GetBagSize(); ++i)
                    if (Item* item = bag->GetItemByPos(uint8(i)))
                        if (!callback(item))
                            return;
    }

    std::string CopperText(uint32 copper)
    {
        uint32 const gold = copper / GOLD;
        uint32 const silver = (copper % GOLD) / SILVER;
        uint32 const rest = copper % SILVER;
        std::string text;
        if (gold)
            text += Acore::StringFormat("{}g", gold);
        if (silver)
            text += Acore::StringFormat("{}{}s", text.empty() ? "" : " ", silver);
        if (rest || text.empty())
            text += Acore::StringFormat("{}{}c", text.empty() ? "" : " ", rest);
        return text;
    }
}

LootService::ContainerKind LootService::ContainerKindOf(ItemTemplate const* proto)
{
    if (!proto)
        return ContainerKind::None;
    if (proto->HasFlag(ITEM_FLAG_HAS_LOOT))
        return proto->LockID ? ContainerKind::Locked : ContainerKind::Loot;
    // The clams: no loot of their own, their use spell hands it out (spell_loot_template) and uses one up.
    _Spell const& use = proto->Spells[0];
    if (use.SpellId > 0 && use.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE && use.SpellCharges < 0
        && LootTemplates_Spell.HaveLootFor(uint32(use.SpellId)))
        return ContainerKind::Spell;
    return ContainerKind::None;
}

int32 LootService::PickLockSkill(ItemTemplate const* proto)
{
    LockEntry const* lock = proto && proto->LockID ? sLockStore.LookupEntry(proto->LockID) : nullptr;
    if (!lock)
        return -1;
    for (uint8 j = 0; j < MAX_LOCK_CASE; ++j)
        if (lock->Type[j] == LOCK_KEY_SKILL && lock->Index[j] == LOCKTYPE_PICKLOCK)
            return int32(lock->Skill[j]);
    return -1;
}

int32 LootService::KeySkill(ItemTemplate const* proto)
{
    // Only a blacksmith's skeleton key (it asks for Blacksmithing): an engineer's Seaforium Charge opens a lock the
    // same way, and round 235 used all 69 of the guild's Small and Large Seaforium Charges on Iron Lockboxes (the Large
    // ones are for the inner door of the Dire Maul tribute run).
    if (!proto || proto->Spells[0].SpellId <= 0 || proto->RequiredSkill != SKILL_BLACKSMITHING)
        return -1;
    SpellInfo const* info = sSpellMgr->GetSpellInfo(uint32(proto->Spells[0].SpellId));
    if (!info || info->IsAbilityOfSkillType(SKILL_LOCKPICKING))
        return -1;
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        if (info->Effects[i].Effect == SPELL_EFFECT_OPEN_LOCK && uint32(info->Effects[i].MiscValue) == LOCKTYPE_PICKLOCK)
            return info->Effects[i].CalcValue();
    return -1;
}

Item* LootService::KeyFor(Player* player, int32 need)
{
    Item* best = nullptr;
    int32 bestSkill = 0;
    ForBagItems(player, [&](Item* candidate)
    {
        ItemTemplate const* proto = candidate->GetTemplate();
        int32 const skill = KeySkill(proto);
        if (skill >= need && (!best || skill < bestSkill) && player->CanUseItem(proto) == EQUIP_ERR_OK)
        {
            best = candidate;
            bestSkill = skill;
        }
        return true;
    });
    return best;
}

bool LootService::IsLastingReason(char const* reason)
{
    if (!reason)
        return false;
    std::string const text(reason);
    return text == "a key" || text == "no lockpicking" || text == "lockpicking too low" || text == "wrapped" || text == "not a container";
}

char const* LootService::CannotOpen(Player* player, Item* item) const
{
    ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
    ContainerKind const kind = ContainerKindOf(proto);
    if (kind == ContainerKind::None)
        return "not a container";
    if (item->IsWrapped())
        return "wrapped";
    if (kind == ContainerKind::Locked && item->IsLocked())
    {
        int32 const need = PickLockSkill(proto);
        if (need < 0)
            return "a key";
        // (A blacksmith's skeleton key in its bags opens it too.)
        if (!KeyFor(player, need))
        {
            if (!player->HasSpell(PickLockSpell))
                return "no lockpicking";
            if (int32(player->GetSkillValue(SKILL_LOCKPICKING)) < need)
                return "lockpicking too low";
        }
    }
    if (!player->IsInWorld() || !player->IsAlive() || player->IsInCombat() || player->IsInFlight() || player->IsBeingTeleported())
        return "busy";
    if (!Player::IsInventoryPos(item->GetPos()) || item->GetOwnerGUID() != player->GetGUID())
        return "not in the bags";
    if (player->GetFreeInventorySpace() < OpenMinFreeSlots)
        return "no room";
    return nullptr;
}

bool LootService::OpenContainer(Player* player, Item* item, char const* how)
{
    if (CannotOpen(player, item))
        return false;
    ItemTemplate const* proto = item->GetTemplate();
    ContainerKind const kind = ContainerKindOf(proto);
    uint32 const owner = sStorageService.GetOwnerOf(player);
    uint32 const entry = proto->ItemId;
    int32 const randomProperty = item->GetItemRandomPropertyId();

    // A lockbox: picked as the rogue's Pick Lock does it (Spell::EffectOpenLock for an item: unlocked, one skill-up of
    // Lockpicking against the lock's level). Not through a cast: the spell's effect comes a moment later, and the
    // playerbots AI opens an unlocked box in its bags by itself in the meantime, without the guild's log line.
    std::string picked;
    if (kind == ContainerKind::Locked && item->IsLocked())
    {
        int32 const need = PickLockSkill(proto);
        uint32 const before = player->GetPureSkillValue(SKILL_LOCKPICKING);
        if (need < 0)
            return false;
        if (player->HasSpell(PickLockSpell) && int32(player->GetSkillValue(SKILL_LOCKPICKING)) >= need)
        {
            item->SetFlag(ITEM_FIELD_FLAGS, ITEM_FIELD_FLAG_UNLOCKED);
            item->SetState(ITEM_CHANGED, player);
            if (before)
                player->UpdateGatherSkill(SKILL_LOCKPICKING, before, uint32(need));
            picked = Acore::StringFormat("picked its lock (Lockpicking {} -> {}); ", before, player->GetPureSkillValue(SKILL_LOCKPICKING));
        }
        else if (Item* key = KeyFor(player, need))
        {
            // A blacksmith's skeleton key, used up as its use spell does (Spell::EffectOpenLock with the key as the cast
            // item): the lock is open, the key gone.
            std::string const keyName = key->GetTemplate()->Name1;
            uint32 one = 1;
            player->DestroyItemCount(key, one, true);
            item->SetFlag(ITEM_FIELD_FLAGS, ITEM_FIELD_FLAG_UNLOCKED);
            item->SetState(ITEM_CHANGED, player);
            picked = Acore::StringFormat("opened its lock with a {}; ", keyName);
        }
        else
            return false;
    }

    // The loot: the item's own (with money; stored with the item until it is empty, as the core keeps it), or what the
    // use spell hands out.
    Loot spellLoot;
    Loot* loot = nullptr;
    if (kind == ContainerKind::Spell)
    {
        spellLoot.FillLoot(uint32(proto->Spells[0].SpellId), LootTemplates_Spell, player, true);
        if (spellLoot.GetMaxSlotInLootFor(player) + 1 > player->GetFreeInventorySpace())
            return false; // what does not fit would be lost: later
        loot = &spellLoot;
    }
    else
    {
        loot = &item->loot;
        loot->containerGUID = item->GetGUID();
        if (!item->m_lootGenerated && !sLootItemStorage->LoadStoredLoot(item, player))
        {
            item->m_lootGenerated = true;
            loot->clear();
            loot->generateMoneyLoot(proto->MinMoneyLoot, proto->MaxMoneyLoot);
            loot->FillLoot(entry, LootTemplates_Item, player, true, loot->gold != 0);
            if (loot->gold > 0 || loot->unlootedCount > 0)
                sLootItemStorage->AddNewStoredLoot(loot, player);
        }
        player->SetLootGUID(item->GetGUID());
    }

    // What comes out, for the log, before it is taken.
    std::map<uint32, uint32> contents;
    uint32 const maxSlot = loot->GetMaxSlotInLootFor(player);
    for (uint32 i = 0; i < maxSlot; ++i)
        if (LootItem* lootItem = loot->LootItemInSlot(i, player))
            contents[lootItem->itemid] += lootItem->count;

    // The money, as the core's CMSG_LOOT_MONEY handler gives it (scripts first: the guild perk on looted gold).
    uint32 gold = 0;
    if (loot->gold)
    {
        sScriptMgr->OnPlayerBeforeLootMoney(player, loot);
        gold = loot->gold;
        loot->NotifyMoneyRemoved();
        {
            GuildBank::IncomeSource source("coin from a container it opened");
            player->ModifyMoney(int32(gold));
        }
        player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_MONEY, gold);
        sScriptMgr->OnLootMoney(player, gold);
        loot->gold = 0;
        if (loot->containerGUID)
            sLootItemStorage->RemoveStoredLootMoney(loot->containerGUID, loot);
    }
    // Each item through Player::StoreLootItem: the loot hook, so the loot filter decides about it next tick.
    bool const allTaken = TakeLootItems(player, loot);

    // Used up: one of the stack for a use spell; the whole container once its loot is empty (WorldSession::DoLootRelease).
    if (kind == ContainerKind::Spell)
    {
        uint32 one = 1;
        player->DestroyItemCount(item, one, true);
    }
    else
    {
        player->SetLootGUID(ObjectGuid::Empty);
        if (loot->isLooted())
            player->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
    }

    std::string what;
    for (auto const& [itemId, count] : contents)
    {
        ItemTemplate const* got = sObjectMgr->GetItemTemplate(itemId);
        what += Acore::StringFormat("{}{} x {}", what.empty() ? "" : ", ", count, got ? got->Name1 : std::to_string(itemId));
    }
    if (gold)
        what += (what.empty() ? "" : ", ") + CopperText(gold);
    if (what.empty())
        what = "nothing";
    if (!allTaken)
        what += " (some of it stays in it: no room)";
    LOG_INFO("module.ga.activity", "activity: {} opened {} ({}): {}{}", player->GetName(), proto->Name1, how, picked, what);
    if (owner)
        AddStorageLog(owner, player->GetGUID(), entry, randomProperty, 1, LogOpened, gold, picked + what);
    return true;
}

uint32 LootService::OpenStoredContainers(uint32 owner, Player* player, uint32 limit)
{
    uint32 opened = 0;
    for (StorageItem const& stored : sStorageService.List(owner))
    {
        if (opened >= limit)
            break;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(stored.entry);
        ContainerKind const kind = ContainerKindOf(proto);
        if (kind == ContainerKind::None || !stored.amount)
            continue;
        // A lockbox only for a character that can pick it or has a key for it, and regularly (the owner, 2026-11-13: [quote left out]; 2026-11-26: [quote left out]): one per rogue or blacksmith every Storage.LockboxMinutes. A blacksmith's
        // skeleton key comes out of its bags or the guild storage (made by the guild's blacksmiths: KeysForGuild).
        uint32 want = std::min(limit - opened, stored.amount);
        bool paced = false;
        StorageItem storedKey;
        if (kind == ContainerKind::Locked)
        {
            int32 const need = PickLockSkill(proto);
            if (need < 0)
                continue;
            bool const picks = player->HasSpell(PickLockSpell) && int32(player->GetSkillValue(SKILL_LOCKPICKING)) >= need;
            if (!picks && !KeyFor(player, need))
            {
                // The weakest key of the storage it may use that opens it.
                int32 bestSkill = 0;
                for (StorageItem const& key : sStorageService.List(owner))
                {
                    ItemTemplate const* keyProto = key.amount ? sObjectMgr->GetItemTemplate(key.entry) : nullptr;
                    int32 const skill = KeySkill(keyProto);
                    if (skill >= need && (!storedKey.entry || skill < bestSkill) && player->CanUseItem(keyProto) == EQUIP_ERR_OK)
                    {
                        storedKey = key;
                        bestSkill = skill;
                    }
                }
                if (!storedKey.entry)
                    continue;
            }
            uint32 const minutes = uint32(std::max<int32>(sBotSettings.GetForGuild(owner, Setting::StorageLockboxMinutes), 0));
            if (minutes)
            {
                uint32 const now = getMSTime();
                auto last = _lastStoredLockbox.find(player->GetGUID());
                if (last != _lastStoredLockbox.end() && getMSTimeDiff(last->second, now) < minutes * MINUTE * IN_MILLISECONDS)
                    continue;
                paced = true;
                want = 1;
            }
        }
        std::string error;
        if (storedKey.entry)
        {
            if (!sStorageService.Withdraw(player, storedKey.entry, storedKey.randomProperty, 1, error))
                continue;
            LogStorageMove(owner, player->GetGUID(), player->GetName(), storedKey.entry, storedKey.randomProperty, 1, false,
                "taken out to open a lockbox of the guild storage");
            want = 1;
        }
        uint32 const taken = sStorageService.Withdraw(player, stored.entry, stored.randomProperty, want, error);
        if (!taken)
            continue;
        if (paced)
            _lastStoredLockbox[player->GetGUID()] = getMSTime();
        LogStorageMove(owner, player->GetGUID(), player->GetName(), stored.entry, stored.randomProperty, taken, false,
            "taken out to be opened (Storage.OpenContainers)");
        for (uint32 i = 0; i < taken; ++i)
        {
            Item* container = nullptr;
            ForBagItems(player, [&](Item* candidate)
            {
                if (candidate->GetEntry() != stored.entry)
                    return true;
                container = candidate;
                return false;
            });
            if (!container || !OpenContainer(player, container, "out of the guild storage"))
                break; // what is left of it waits in its bags for the next look
            ++opened;
        }
    }
    return opened;
}

void LootService::OpenContainersStep()
{
    if (!GetSettings().lootFilterEnable || !IsModuleEnabled())
        return;
    for (auto& [owner, guild] : sGuildAdventureMgr.GetGuilds())
    {
        auto itr = _guilds.find(owner);
        if (itr != _guilds.end() && !itr->second.settings.filterEnabled)
            continue;
        // The bots first, the leader last (while the AI plays it): the leader is the owner's own character.
        std::vector<ObjectGuid> characters;
        for (auto const& [guid, bot] : guild.bots)
            characters.push_back(guid);
        characters.push_back(guild.leader);

        std::vector<Player*> openers;
        for (ObjectGuid const& guid : characters)
        {
            Player* player = sGuildBotHolder.GetPlannedPlayer(guid);
            if (!player)
                player = sGuildBotHolder.GetAIDrivenPlayer(guid);
            if (!player || !player->IsInWorld() || player->IsDuringRemoveFromWorld() || !IsAIPlayed(player)
                || !sBotSettings.IsOn(player, Setting::LootOpenContainers))
                continue;

            // What it carries: containers it looted while it could not open them (a fight, full bags).
            std::vector<Item*> containers;
            ForBagItems(player, [&](Item* candidate)
            {
                if (ContainerKindOf(candidate->GetTemplate()) != ContainerKind::None)
                    containers.push_back(candidate);
                return true;
            });
            uint32 done = 0;
            for (Item* container : containers)
            {
                if (done >= OpenPerCharacter)
                    break;
                GuildAdventure::LootRules::Verdict const verdict = GuildAdventure::LootRules::Evaluate(Guild(owner).rules,
                    Facts(player, container), AllowedMask());
                if (verdict.result != GuildAdventure::LootRules::ResultNoRule && verdict.result != GuildAdventure::LootRules::ActionStore)
                    continue; // a rule of the guild decided about it
                while (done < OpenPerCharacter && !CannotOpen(player, container))
                {
                    uint32 const left = container->GetCount();
                    if (!OpenContainer(player, container, "from its bags"))
                        break;
                    ++done;
                    if (left <= 1)
                        break; // used up (the pointer is gone)
                }
            }

            if (!player->IsInCombat() && player->IsAlive() && player->GetFreeInventorySpace() >= StorageOpenerFreeSlots)
                openers.push_back(player);
        }

        // The guild storage's containers (the owner, 2026-11-13: [quote left out]), a few per look, by characters with room: each
        // takes what it can open (a lockbox only a rogue that can pick it), the next one what is left.
        if (openers.empty() || !sBotSettings.GetForGuild(owner, Setting::StorageOpenContainers))
            continue;
        uint32 opened = 0;
        for (Player* opener : openers)
        {
            opened += OpenStoredContainers(owner, opener, OpenFromStoragePerGuild - opened);
            if (opened >= OpenFromStoragePerGuild)
                break;
        }
    }
}
