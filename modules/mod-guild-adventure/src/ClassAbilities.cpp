/*
 * ClassAbilities implementation. See ClassAbilities.h.
 */

#include "ClassAbilities.h"
#include "BotSettings.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "GuildAdventureSettings.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "SpellInfo.h"
#include "GameTime.h"
#include "SpecService.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "World.h"
#include <algorithm>
#include <map>
#include <unordered_set>
#include <vector>

using namespace GuildAdventure;

namespace
{
    struct Row
    {
        uint8 classId = 0;
        uint8 race = 0;        // 0 = every race
        uint8 minLevel = 0;
        uint32 spell = 0;
        uint32 item = 0;
        uint32 creature = 0;   // hunter: the beast a pet is made from
        uint8 petKind = 0;     // 0 first pet, 1 good pet at minLevel, 2 exotic pet (Beast Mastery talent)
    };

    std::vector<Row> rows;
    // Hunters that got their good or exotic pet (ga_bot_pet_upgrade): once per hunter.
    std::unordered_set<ObjectGuid::LowType> upgraded;

    constexpr uint8 PetFirst = 0;
    constexpr uint8 PetGood = 1;
    constexpr uint8 PetExotic = 2;
    constexpr uint32 SpellBeastMastery = 53270; // the hunter talent that allows exotic pets
    constexpr uint8 HunterTabBeastMastery = 0;
    constexpr uint8 BeastMasteryRow = 10;       // its row in the tree (the only talent there)

    // True if the hunter's talent template takes the Beast Mastery talent with the points a character has at the
    // server's level cap (so the exotic pet is worth waiting for).
    bool ReachesBeastMastery(SpecTemplate const* spec)
    {
        if (!spec)
            return false;
        uint32 const cap = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);
        std::size_t const points = cap >= 10 ? cap - 9 : 0;
        for (std::size_t i = 0; i < spec->points.size() && i < points; ++i)
            if (spec->points[i].tab == HunterTabBeastMastery && spec->points[i].row == BeastMasteryRow)
                return true;
        return false;
    }

    bool HasHunterPet(Player* player)
    {
        if (player->GetPet())
            return true;
        PetStable const* stable = player->GetPetStable();
        if (!stable)
            return false;
        if (stable->CurrentPet || stable->GetUnslottedHunterPet())
            return true;
        for (auto const& stabled : stable->StabledPets)
            if (stabled)
                return true;
        return false;
    }

    // The hunter's first pet: a beast of its own level, as if it had tamed it (the same steps the core takes after
    // Tame Beast: Unit::CreateTamedPetFrom, then into the world as the hunter's minion).
    bool GivePet(Player* player, uint32 creatureEntry)
    {
        if (!player->IsAlive() || !player->IsInWorld() || player->IsBeingTeleported() || player->IsInFlight() ||
            player->GetTransport() || player->IsMounted())
            return false; // next time

        Pet* pet = player->CreateTamedPetFrom(creatureEntry, 0);
        if (!pet)
            return false;

        uint8 const level = player->GetLevel();
        // One level lower before it enters the world, so the client shows the level-up effect.
        pet->SetUInt32Value(UNIT_FIELD_LEVEL, level - 1);
        pet->GetMap()->AddToMap(pet->ToCreature());
        pet->SetUInt32Value(UNIT_FIELD_LEVEL, level);
        player->SetMinion(pet, true);
        pet->InitTalentForLevel();
        pet->SetPower(POWER_HAPPINESS, pet->GetMaxPower(POWER_HAPPINESS));
        pet->SetFullHealth();
        pet->SavePetToDB(PET_SAVE_AS_CURRENT);
        player->PetSpellInitialize();
        return true;
    }

    // The good pet replaces the hunter's pet (which is abandoned), wherever that one is: at its side, put away while the
    // hunter rides (a level-60 bot rides nearly all the time), dismissed or dead. Pets in the stable stay.
    bool ReplacePet(Player* player, uint32 creatureEntry)
    {
        if (player->IsInCombat() || !player->IsAlive() || !player->IsInWorld() || player->IsBeingTeleported() || player->IsInFlight() ||
            player->GetTransport())
        {
            LOG_DEBUG("module", "mod-guild-adventure: {} gets its new pet later (in a fight, dead or travelling)", player->GetName());
            return false;
        }
        Pet* const old = player->GetPet();
        if (old && old->IsInCombat())
        {
            LOG_DEBUG("module", "mod-guild-adventure: {} gets its new pet later (its pet is in a fight)", player->GetName());
            return false;
        }

        // Off the mount, without the core calling the old pet back.
        player->SetTemporaryUnsummonedPetNumber(0);
        if (player->IsMounted())
        {
            player->RemoveAurasByType(SPELL_AURA_MOUNTED);
            if (player->IsMounted())
                return false;
        }
        if (old)
            player->RemovePet(old, PET_SAVE_AS_DELETED);
        if (PetStable* stable = player->GetPetStable())
        {
            if (stable->CurrentPet)
            {
                Pet::DeleteFromDB(stable->CurrentPet->PetNumber);
                stable->CurrentPet.reset();
            }
            for (PetStable::PetInfo const& info : stable->UnslottedPets)
                if (info.Type == HUNTER_PET)
                    Pet::DeleteFromDB(info.PetNumber);
            std::erase_if(stable->UnslottedPets, [](PetStable::PetInfo const& info) { return info.Type == HUNTER_PET; });
        }

        bool const given = GivePet(player, creatureEntry);
        if (!given)
            LOG_ERROR("module", "mod-guild-adventure: {} could not be given a pet made from creature {}", player->GetName(), creatureEntry);
        return given;
    }
}

void ClassAbilities::Load()
{
    rows.clear();
    upgraded.clear();
    if (QueryResult done = CharacterDatabase.Query("SELECT bot_guid FROM ga_bot_pet_upgrade"))
    {
        do
        {
            upgraded.insert(done->Fetch()[0].Get<uint32>());
        } while (done->NextRow());
    }
    QueryResult result = WorldDatabase.Query("SELECT class, race, min_level, spell_id, item_id, creature_id, pet_kind FROM ga_class_ability "
        "ORDER BY class, min_level, creature_id, spell_id, item_id");
    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            Row row;
            row.classId = fields[0].Get<uint8>();
            row.race = fields[1].Get<uint8>();
            row.minLevel = fields[2].Get<uint8>();
            row.spell = fields[3].Get<uint32>();
            row.item = fields[4].Get<uint32>();
            row.creature = fields[5].Get<uint32>();
            row.petKind = fields[6].Get<uint8>();

            if (row.spell && !sSpellMgr->GetSpellInfo(row.spell))
            {
                LOG_ERROR("module", "mod-guild-adventure: ga_class_ability names the unknown spell {}, skipped", row.spell);
                continue;
            }
            if (row.item && !sObjectMgr->GetItemTemplate(row.item))
            {
                LOG_ERROR("module", "mod-guild-adventure: ga_class_ability names the unknown item {}, skipped", row.item);
                continue;
            }
            if (row.creature)
            {
                CreatureTemplate const* beast = sObjectMgr->GetCreatureTemplate(row.creature);
                if (!beast || !beast->IsTameable(row.petKind == PetExotic))
                {
                    LOG_ERROR("module", "mod-guild-adventure: ga_class_ability names creature {}, which is no tameable beast, skipped",
                        row.creature);
                    continue;
                }
            }
            if (!row.spell && !row.item && !row.creature)
                continue;
            rows.push_back(row);
        } while (result->NextRow());
    }
    LOG_INFO("module", "mod-guild-adventure: loaded {} class abilities (ga_class_ability)", rows.size());
}

bool ClassAbilities::IsPetPending(Player* player)
{
    if (!player || !sBotSettings.IsOn(player, Setting::ClassAbilitiesEnable) || !sBotSettings.IsOn(player, Setting::ClassAbilitiesHunterPet) || player->getClass() != CLASS_HUNTER ||
        upgraded.count(player->GetGUID().GetCounter()))
        return false;
    for (Row const& row : rows)
        if (row.classId == CLASS_HUNTER && row.creature && row.petKind != PetFirst && row.minLevel <= player->GetLevel())
            return true;
    return false;
}

std::string ClassAbilities::LearnWeaponSkills(Player* player)
{
    // The weapon skill and the spell that teaches it (the weapon master's spell).
    struct Weapon
    {
        uint16 skill;
        uint32 spell;
        char const* name;
    };
    static Weapon const weapons[] =
    {
        { 44, 196, "One-Handed Axes" }, { 172, 197, "Two-Handed Axes" }, { 54, 198, "One-Handed Maces" },
        { 160, 199, "Two-Handed Maces" }, { 229, 200, "Polearms" }, { 43, 201, "One-Handed Swords" },
        { 55, 202, "Two-Handed Swords" }, { 136, 227, "Staves" }, { 45, 264, "Bows" }, { 46, 266, "Guns" },
        { 173, 1180, "Daggers" }, { 176, 2567, "Thrown" }, { 228, 5009, "Wands" }, { 226, 5011, "Crossbows" },
        { 473, 15590, "Fist Weapons" },
    };
    if (!player)
        return "";
    std::string learned;
    uint16 const max = player->GetMaxSkillValueForLevel();
    for (Weapon const& weapon : weapons)
    {
        if (player->HasSkill(weapon.skill) || !sSpellMgr->GetSpellInfo(weapon.spell) || !player->IsSpellFitByClassAndRace(weapon.spell))
            continue;
        player->learnSpell(weapon.spell);
        if (!player->HasSkill(weapon.skill))
            continue;
        player->SetSkill(weapon.skill, std::max<uint16>(player->GetSkillStep(weapon.skill), 1), max, max);
        learned += (learned.empty() ? "" : ", ") + std::string(weapon.name);
    }
    return learned;
}

std::string ClassAbilities::RaiseLockpicking(Player* player)
{
    constexpr uint32 PickLock = 1804;
    constexpr uint8 PickLockLevel = 16;
    if (!player || player->getClass() != CLASS_ROGUE || player->GetLevel() < PickLockLevel)
        return "";
    std::string changed;
    if (!player->HasSpell(PickLock) && sSpellMgr->GetSpellInfo(PickLock))
    {
        player->learnSpell(PickLock);
        changed = "Pick Lock";
    }
    if (!player->HasSkill(SKILL_LOCKPICKING))
        return changed;
    uint16 const target = player->GetMaxSkillValueForLevel();
    uint16 const value = player->GetPureSkillValue(SKILL_LOCKPICKING);
    uint16 const max = std::max<uint16>(player->GetPureMaxSkillValue(SKILL_LOCKPICKING), target);
    if (value < target)
    {
        player->SetSkill(SKILL_LOCKPICKING, std::max<uint16>(player->GetSkillStep(SKILL_LOCKPICKING), 1), target, max);
        changed += Acore::StringFormat("{}Lockpicking {} -> {}", changed.empty() ? "" : ", ", value, target);
    }
    return changed;
}

std::string ClassAbilities::Grant(Player* player)
{
    Settings const settings = sBotSettings.For(player);
    if (!settings.classAbilitiesEnable || !player)
        return "";

    std::string given;
    auto add = [&given](std::string const& what)
    {
        given += given.empty() ? what : ", " + what;
    };

    uint8 const level = player->GetLevel();
    // The pet row of the hunter's own race wins over the row for every race.
    Row const* petRow = nullptr;
    Row const* goodPet = nullptr;
    Row const* exoticPet = nullptr;
    for (Row const& row : rows)
    {
        if (row.classId != player->getClass() || row.minLevel > level || (row.race && row.race != player->getRace()))
            continue;

        if (row.spell && !player->HasSpell(row.spell))
        {
            player->learnSpell(row.spell, false);
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(row.spell))
                add(info->SpellName[0]);
        }
        if (row.item && !player->HasItemCount(row.item, 1, true))
        {
            // No room in the bags: it comes with the next call.
            if (player->StoreNewItemInBestSlots(row.item, 1))
                if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(row.item))
                    add(proto->Name1);
        }
        if (row.creature && row.petKind == PetGood)
            goodPet = &row;
        else if (row.creature && row.petKind == PetExotic)
            exoticPet = &row;
        else if (row.creature && (!petRow || (row.race && !petRow->race)))
            petRow = &row;
    }

    if (petRow && settings.classAbilitiesHunterPet && player->getClass() == CLASS_HUNTER && !HasHunterPet(player) &&
        GivePet(player, petRow->creature))
        if (CreatureTemplate const* beast = sObjectMgr->GetCreatureTemplate(petRow->creature))
            add(Acore::StringFormat("a pet ({})", beast->Name));

    // The good pet (DQ2), once per hunter: an exotic one as soon as the hunter has the Beast Mastery talent; a hunter
    // whose talent template gets that talent (at this server's level cap) waits for it, every other hunter gets the good
    // pet of its level.
    if (settings.classAbilitiesHunterPet && player->getClass() == CLASS_HUNTER && (goodPet || exoticPet) &&
        !upgraded.count(player->GetGUID().GetCounter()))
    {
        SpecTemplate const* spec = sSpecService.GetEffective(player->GetGUID(), CLASS_HUNTER);
        Row const* wanted = nullptr;
        if (exoticPet && player->HasTalent(SpellBeastMastery, player->GetActiveSpec())) // a talent is no entry of the spell book
            wanted = exoticPet;
        else if (goodPet && !(exoticPet && ReachesBeastMastery(spec)))
            wanted = goodPet;
        if (wanted && ReplacePet(player, wanted->creature))
        {
            upgraded.insert(player->GetGUID().GetCounter());
            CharacterDatabase.Execute("REPLACE INTO ga_bot_pet_upgrade (bot_guid, creature_id, given_at) VALUES ({}, {}, {})",
                player->GetGUID().GetCounter(), wanted->creature, uint32(GameTime::GetGameTime().count()));
            if (CreatureTemplate const* beast = sObjectMgr->GetCreatureTemplate(wanted->creature))
                add(Acore::StringFormat("a new pet ({}{})", beast->Name, wanted == exoticPet ? ", exotic" : ""));
        }
    }

    return given;
}

namespace
{
    // One step of a pet talent build: the talent (Talent.dbc id) and the rank to reach. Diving pets (bats, birds, wind
    // serpents, moths, wasps ...) have Dive, Swoop and their Mobility in place of Dash, Charge and the other Mobility:
    // the same place in the tree, another talent id.
    struct PetTalentStep
    {
        uint32 talent;
        uint32 diveTalent; // 0 = the same for every pet of the tree
        uint8 rank;        // 1-based
    };

    // The builds for levelling, in the order the points are spent. A pet has 1 point at level 20 and one more every
    // four levels, 11 at level 60, 16 at 80; the hunter's Beast Mastery talent gives 4 more (20). Each list keeps the
    // tree's rules at every step: a row needs 3 points per row above it in the tree, and a talent that depends on
    // another comes after it at the rank it asks for. The steps after the twentieth point are never reached by a pet
    // of this build; they name what mod-playerbots' own premade pet builds (playerbots.conf PremadeHunterPetLink)
    // take, so a pet a bot tamed through mod-playerbots counts as one of the build and is not the owner's choice.
    // Ferocity (damage): Cobra Reflexes 2, Dash 1, Spiked Collar 3, Culling the Herd 3, Spider's Bite 3, Call of the
    // Wild, Rabid, Bloodthirsty 1, Wild Hunt 1 (16 points); Bloodthirsty 2, Heart of the Phoenix, Shark Attack 2 (20).
    PetTalentStep const FerocityBuild[] =
    {
        { 2107, 0, 2 }, { 2109, 2203, 1 }, { 2125, 0, 3 }, { 2106, 0, 3 }, { 2129, 0, 3 }, { 2157, 0, 1 },
        { 2155, 0, 1 }, { 2128, 0, 1 }, { 2253, 0, 1 }, { 2128, 0, 2 }, { 2156, 0, 1 }, { 2254, 0, 2 },
        { 2151, 0, 1 }, { 2253, 0, 2 },
    };
    // Tenacity (tanking): Great Stamina 3, Natural Armor 2, Blood of the Rhino 2, Guard Dog 2, Thunderstomp, Cobra
    // Reflexes 2 (12), Taunt, Last Stand, Pet Barding 1 (15), Silverback 2, Pet Barding 2, Great Resistance 2 (20).
    PetTalentStep const TenacityBuild[] =
    {
        { 2116, 0, 3 }, { 2117, 0, 2 }, { 2173, 0, 2 }, { 2123, 0, 2 }, { 2277, 0, 1 }, { 2114, 0, 2 },
        { 2170, 0, 1 }, { 2171, 0, 1 }, { 2122, 0, 1 }, { 2258, 0, 2 }, { 2122, 0, 2 }, { 2161, 0, 2 },
        { 2237, 0, 1 }, { 2126, 0, 3 }, { 2110, 0, 3 }, { 2163, 0, 2 }, { 2172, 0, 1 }, { 2255, 0, 1 },
        { 2161, 0, 3 }, { 2169, 0, 1 },
    };
    // Cunning (damage): Cobra Reflexes 2, Dash 1, Spiked Collar 3, Culling the Herd 3, Feeding Frenzy 2 (11),
    // Mobility 1, Wolverine Bite, Roar of Recovery, Mobility 2, Wild Hunt 1 (16), Wild Hunt 2, Cornered 2 (19),
    // Bullheaded (20).
    PetTalentStep const CunningBuild[] =
    {
        { 2118, 0, 2 }, { 2119, 2201, 1 }, { 2127, 0, 3 }, { 2166, 0, 3 }, { 2183, 0, 2 }, { 2207, 2208, 1 },
        { 2181, 0, 1 }, { 2184, 0, 1 }, { 2207, 2208, 2 }, { 2256, 0, 1 }, { 2256, 0, 2 }, { 2177, 0, 2 },
        { 2175, 0, 1 },
    };

    // Creature families (CreatureFamily.dbc) whose pets dive: the families Talent.dbc's pet mask allows Dive (talents
    // 2201 and 2203) for. The core does not read that mask.
    bool IsDivingFamily(uint32 family)
    {
        switch (family)
        {
            case 7: case 24: case 26: case 27: case 30: case 33: case 34: case 37: case 38: case 44:
                return true;
            default:
                return false;
        }
    }

    char const* TreeName(int32 talentType)
    {
        switch (talentType)
        {
            case 0: return "Ferocity";
            case 1: return "Tenacity";
            case 2: return "Cunning";
            default: return "?";
        }
    }

    // The rank of a talent the pet has (0 = none).
    uint8 PetTalentRank(Pet* pet, TalentEntry const* talent)
    {
        for (int8 rank = MAX_TALENT_RANK - 1; rank >= 0; --rank)
            if (talent->RankID[rank] && pet->HasSpell(talent->RankID[rank]))
                return uint8(rank + 1);
        return 0;
    }

    std::string TalentName(TalentEntry const* talent)
    {
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(talent->RankID[0]))
            return info->SpellName[0];
        return Acore::StringFormat("talent {}", talent->TalentID);
    }

    // Pets whose talents are the owner's own choice, and pets whose tree refused a step, by pet number: said once.
    std::unordered_set<uint32> ownChoiceTold;
    std::unordered_set<uint32> refusedTold;
}

std::string ClassAbilities::SpendPetTalents(Player* player)
{
    if (!player || player->getClass() != CLASS_HUNTER || !sBotSettings.IsOn(player, Setting::ClassAbilitiesEnable) ||
        !sBotSettings.IsOn(player, Setting::ClassAbilitiesPetTalents))
        return "";
    Pet* pet = player->GetPet();
    if (!pet || pet->getPetType() != HUNTER_PET || !pet->IsInWorld() || pet->GetFreeTalentPoints() == 0)
        return "";
    CreatureTemplate const* beast = pet->GetCreatureTemplate();
    CreatureFamilyEntry const* family = beast ? sCreatureFamilyStore.LookupEntry(beast->family) : nullptr;
    if (!family || family->petTalentType < 0)
        return "";

    PetTalentStep const* build = nullptr;
    std::size_t steps = 0;
    switch (family->petTalentType)
    {
        case 0: build = FerocityBuild; steps = std::size(FerocityBuild); break;
        case 1: build = TenacityBuild; steps = std::size(TenacityBuild); break;
        case 2: build = CunningBuild; steps = std::size(CunningBuild); break;
        default: return "";
    }
    bool const dives = IsDivingFamily(beast->family);
    auto stepTalent = [dives](PetTalentStep const& step) { return dives && step.diveTalent ? step.diveTalent : step.talent; };

    // The highest rank the build gives each of its talents.
    std::map<uint32, uint8> wanted;
    for (std::size_t i = 0; i < steps; ++i)
        wanted[stepTalent(build[i])] = std::max(wanted[stepTalent(build[i])], build[i].rank);

    // A pet with a talent outside its build was given it by the owner: its talents stay as they are.
    uint32 const treeMask = 1u << family->petTalentType;
    for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
    {
        TalentEntry const* talent = sTalentStore.LookupEntry(i);
        if (!talent)
            continue;
        TalentTabEntry const* tab = sTalentTabStore.LookupEntry(talent->TalentTab);
        if (!tab || !(tab->petTalentMask & treeMask))
            continue;
        uint8 const rank = PetTalentRank(pet, talent);
        if (!rank)
            continue;
        auto const itr = wanted.find(talent->TalentID);
        if (itr == wanted.end() || rank > itr->second)
        {
            uint32 const petNumber = pet->GetCharmInfo() ? pet->GetCharmInfo()->GetPetNumber() : pet->GetGUID().GetCounter();
            if (ownChoiceTold.insert(petNumber).second)
                return Acore::StringFormat("leaves the talents of its pet {} as they are: {} {} is not in the {} build, so "
                    "they are the owner's choice; {} talent point{} free", pet->GetName(), TalentName(talent), uint32(rank),
                    TreeName(family->petTalentType), uint32(pet->GetFreeTalentPoints()),
                    pet->GetFreeTalentPoints() == 1 ? " stays" : "s stay");
            return "";
        }
    }

    uint32 const freeBefore = pet->GetFreeTalentPoints();
    std::vector<std::pair<std::string, uint8>> learned; // talent name, rank it has now
    std::string refused;
    for (std::size_t i = 0; i < steps && pet->GetFreeTalentPoints() > 0; ++i)
    {
        TalentEntry const* talent = sTalentStore.LookupEntry(stepTalent(build[i]));
        if (!talent)
            continue;
        uint8 const had = PetTalentRank(pet, talent);
        uint8 rank = had;
        while (rank < build[i].rank && pet->GetFreeTalentPoints() > 0)
        {
            uint32 const before = pet->GetFreeTalentPoints();
            player->LearnPetTalent(pet->GetGUID(), talent->TalentID, rank); // the rank index to learn = the next rank
            if (pet->GetFreeTalentPoints() >= before)
                break;
            ++rank;
        }
        if (rank > had)
        {
            std::string const name = TalentName(talent);
            auto found = std::find_if(learned.begin(), learned.end(), [&name](auto const& entry) { return entry.first == name; });
            if (found != learned.end())
                found->second = rank;
            else
                learned.emplace_back(name, rank);
        }
        if (rank < build[i].rank && pet->GetFreeTalentPoints() > 0)
        {
            // The tree refused the step (a row or a dependency not met): the build stops here rather than skip ahead.
            refused = TalentName(talent);
            break;
        }
    }

    uint32 const spent = freeBefore - pet->GetFreeTalentPoints();
    uint32 const number = pet->GetCharmInfo() ? pet->GetCharmInfo()->GetPetNumber() : pet->GetGUID().GetCounter();
    if (!spent)
        return refused.empty() || !refusedTold.insert(number).second ? "" : Acore::StringFormat("could not spend the talent points of its pet {}: the {} tree "
            "refused {}", pet->GetName(), TreeName(family->petTalentType), refused);

    player->SendTalentsInfoData(true);
    std::string list;
    for (auto const& [name, rank] : learned)
        list += (list.empty() ? "" : ", ") + Acore::StringFormat("{} {}", name, uint32(rank));
    return Acore::StringFormat("spent {} talent point{} of its pet {} (level {}, {}): {}; {} left{}", spent,
        spent == 1 ? "" : "s", pet->GetName(), uint32(pet->GetLevel()), TreeName(family->petTalentType), list,
        uint32(pet->GetFreeTalentPoints()), refused.empty() ? "" : Acore::StringFormat(" (the tree refused {})", refused));
}
