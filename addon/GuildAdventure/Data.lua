-- Static client-side data: DBC ids, names, valid race/class combinations, order and
-- feature metadata. The server stays authoritative; these tables only drive the UI.

local GA = GuildAdventure
local Data = {}
GA.Data = Data

-------------------------------------------------------------------------------
-- Classes and races (3.3.5a DBC ids)
-------------------------------------------------------------------------------

Data.CLASSES = {
    [1]  = { token = "WARRIOR",     name = "Warrior" },
    [2]  = { token = "PALADIN",     name = "Paladin" },
    [3]  = { token = "HUNTER",      name = "Hunter" },
    [4]  = { token = "ROGUE",       name = "Rogue" },
    [5]  = { token = "PRIEST",      name = "Priest" },
    [6]  = { token = "DEATHKNIGHT", name = "Death Knight" },
    [7]  = { token = "SHAMAN",      name = "Shaman" },
    [8]  = { token = "MAGE",        name = "Mage" },
    [9]  = { token = "WARLOCK",     name = "Warlock" },
    [11] = { token = "DRUID",       name = "Druid" },
}
Data.CLASS_ORDER = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 11 }
Data.DEATH_KNIGHT = 6

Data.RACES = {
    [1]  = { token = "Human",    name = "Human",     faction = "A", startZone = 12 },
    [3]  = { token = "Dwarf",    name = "Dwarf",     faction = "A", startZone = 1 },
    [4]  = { token = "NightElf", name = "Night Elf", faction = "A", startZone = 141 },
    [7]  = { token = "Gnome",    name = "Gnome",     faction = "A", startZone = 1 },
    [11] = { token = "Draenei",  name = "Draenei",   faction = "A", startZone = 3524 },
    [2]  = { token = "Orc",      name = "Orc",       faction = "H", startZone = 14 },
    [5]  = { token = "Scourge",  name = "Undead",    faction = "H", startZone = 85 },
    [6]  = { token = "Tauren",   name = "Tauren",    faction = "H", startZone = 215 },
    [8]  = { token = "Troll",    name = "Troll",     faction = "H", startZone = 14 },
    [10] = { token = "BloodElf", name = "Blood Elf", faction = "H", startZone = 3430 },
}
Data.RACE_ORDER = { 1, 3, 4, 7, 11, 2, 5, 6, 8, 10 }

-- Race -> allowed classes in 3.3.5a (CharBaseInfo). Death Knights are allowed for every race.
Data.RACE_CLASSES = {
    [1]  = { 1, 2, 4, 5, 6, 8, 9 },
    [3]  = { 1, 2, 3, 4, 5, 6 },
    [4]  = { 1, 3, 4, 5, 6, 11 },
    [7]  = { 1, 4, 6, 8, 9 },
    [11] = { 1, 2, 3, 5, 6, 7, 8 },
    [2]  = { 1, 3, 4, 6, 7, 9 },
    [5]  = { 1, 4, 5, 6, 8, 9 },
    [6]  = { 1, 3, 6, 7, 11 },
    [8]  = { 1, 3, 4, 5, 6, 7, 8 },
    [10] = { 2, 3, 4, 5, 6, 8, 9 },
}

Data.DK_START_LEVEL = 55
Data.DK_START_ZONE_NAME = "Acherus: The Ebon Hold"

local FALLBACK_CLASS_COLORS = {
    WARRIOR = { r = 0.78, g = 0.61, b = 0.43 },
    PALADIN = { r = 0.96, g = 0.55, b = 0.73 },
    HUNTER = { r = 0.67, g = 0.83, b = 0.45 },
    ROGUE = { r = 1.00, g = 0.96, b = 0.41 },
    PRIEST = { r = 1.00, g = 1.00, b = 1.00 },
    DEATHKNIGHT = { r = 0.77, g = 0.12, b = 0.23 },
    SHAMAN = { r = 0.00, g = 0.44, b = 0.87 },
    MAGE = { r = 0.41, g = 0.80, b = 0.94 },
    WARLOCK = { r = 0.58, g = 0.51, b = 0.79 },
    DRUID = { r = 1.00, g = 0.49, b = 0.04 },
}

function Data.ClassColor(classId)
    local class = Data.CLASSES[classId]
    if not class then
        return 1, 1, 1
    end
    local c = (RAID_CLASS_COLORS and RAID_CLASS_COLORS[class.token]) or FALLBACK_CLASS_COLORS[class.token]
    return c.r, c.g, c.b
end

function Data.ClassColorCode(classId)
    local r, g, b = Data.ClassColor(classId)
    return string.format("|cff%02x%02x%02x", r * 255, g * 255, b * 255)
end

function Data.ClassName(classId)
    local class = Data.CLASSES[classId]
    return class and class.name or ("Class " .. tostring(classId))
end

function Data.RaceName(raceId)
    local race = Data.RACES[raceId]
    return race and race.name or ("Race " .. tostring(raceId))
end

function Data.IsValidCombo(raceId, classId)
    local list = Data.RACE_CLASSES[raceId]
    if not list then
        return false
    end
    for i = 1, #list do
        if list[i] == classId then
            return true
        end
    end
    return false
end

-- Class icon atlas used by the 3.3.5a battleground score board (same layout as CLASS_ICON_TCOORDS).
Data.CLASS_ICON_TEXTURE = "Interface\\WorldStateFrame\\Icons-Classes"
local FALLBACK_CLASS_TCOORDS = {
    WARRIOR = { 0, 0.25, 0, 0.25 },
    MAGE = { 0.25, 0.49609375, 0, 0.25 },
    ROGUE = { 0.49609375, 0.7421875, 0, 0.25 },
    DRUID = { 0.7421875, 0.98828125, 0, 0.25 },
    HUNTER = { 0, 0.25, 0.25, 0.5 },
    SHAMAN = { 0.25, 0.49609375, 0.25, 0.5 },
    PRIEST = { 0.49609375, 0.7421875, 0.25, 0.5 },
    WARLOCK = { 0.7421875, 0.98828125, 0.25, 0.5 },
    PALADIN = { 0, 0.25, 0.5, 0.75 },
    DEATHKNIGHT = { 0.25, 0.49609375, 0.5, 0.75 },
}

function Data.ClassTexCoords(classId)
    local class = Data.CLASSES[classId]
    if not class then
        return 0, 1, 0, 1
    end
    local c = (CLASS_ICON_TCOORDS and CLASS_ICON_TCOORDS[class.token]) or FALLBACK_CLASS_TCOORDS[class.token]
    return c[1], c[2], c[3], c[4]
end

-------------------------------------------------------------------------------
-- Professions (SkillLine ids)
-------------------------------------------------------------------------------

Data.PROFESSIONS = {
    [171] = { name = "Alchemy",        icon = "Trade_Alchemy",                 primary = true },
    [164] = { name = "Blacksmithing",  icon = "Trade_BlackSmithing",           primary = true },
    [333] = { name = "Enchanting",     icon = "Trade_Engraving",               primary = true },
    [202] = { name = "Engineering",    icon = "Trade_Engineering",             primary = true },
    [182] = { name = "Herbalism",      icon = "Spell_Nature_NatureTouchGrow",  primary = true, gathering = true },
    [773] = { name = "Inscription",    icon = "INV_Inscription_Tradeskill01",  primary = true },
    [755] = { name = "Jewelcrafting",  icon = "INV_Misc_Gem_01",               primary = true },
    [165] = { name = "Leatherworking", icon = "INV_Misc_ArmorKit_17",          primary = true },
    [186] = { name = "Mining",         icon = "Trade_Mining",                  primary = true, gathering = true },
    [393] = { name = "Skinning",       icon = "INV_Misc_Pelt_Wolf_01",         primary = true, gathering = true },
    [197] = { name = "Tailoring",      icon = "Trade_Tailoring",               primary = true },
    [185] = { name = "Cooking",        icon = "INV_Misc_Food_15",              primary = false },
    [129] = { name = "First Aid",      icon = "Spell_Holy_SealOfSacrifice",    primary = false },
    [356] = { name = "Fishing",        icon = "Trade_Fishing",                 primary = false },
}
Data.PRIMARY_ORDER = { 171, 164, 333, 202, 182, 773, 755, 165, 186, 393, 197 }
Data.SECONDARY_ORDER = { 185, 129, 356 }

-- Profession ranks: skill cap, rank name, minimum character level to learn the rank.
Data.PROFESSION_RANKS = {
    { cap = 75,  name = "Apprentice",   level = 5 },
    { cap = 150, name = "Journeyman",   level = 10 },
    { cap = 225, name = "Expert",       level = 20 },
    { cap = 300, name = "Artisan",      level = 35 },
    { cap = 375, name = "Master",       level = 50 },
    { cap = 450, name = "Grand Master", level = 65 },
}

function Data.ProfessionName(skillId)
    local p = Data.PROFESSIONS[skillId]
    return p and p.name or ("Skill " .. tostring(skillId))
end

function Data.ProfessionIcon(skillId)
    local p = Data.PROFESSIONS[skillId]
    return "Interface\\Icons\\" .. (p and p.icon or "INV_Misc_QuestionMark")
end

function Data.RankForCap(cap)
    for i = 1, #Data.PROFESSION_RANKS do
        if Data.PROFESSION_RANKS[i].cap == cap then
            return Data.PROFESSION_RANKS[i]
        end
    end
    return nil
end

-------------------------------------------------------------------------------
-- Orders (docs/protocol.md §8.1)
-------------------------------------------------------------------------------

Data.ORDERS = {
    AUTO = {
        label = "Autopilot",
        desc = "The bot plans its own next step: it picks the best zone for its level, quests, "
            .. "grinds to fill gaps, visits trainers and vendors and follows its profession plan.",
    },
    IDLE = {
        label = "Idle",
        desc = "Stay in a safe spot (city or guild hall) and do nothing.",
    },
    FOLLOW = {
        label = "Follow",
        desc = "Follow the character you are currently playing, like a normal party member.",
    },
    LEVEL_ZONE = {
        label = "Level in zone",
        desc = "Travel to the chosen zone and level there with quests and grinding inside the zone's level band.",
        needsZone = true,
    },
    GRIND = { label = "Grind", desc = "Kill mobs in a zone, no quests.", needsZone = true },
    QUEST = { label = "Quest", desc = "Quests only in a zone, grind only to fill gaps.", needsZone = true },
    GATHER = { label = "Gather", desc = "Run a mining, herbalism or skinning route in a zone.", needsZone = true },
    CRAFT = {
        label = "Professions",
        desc = "Work on the professions of the bot's plan until they reach their targets: craft what gives skill "
            .. "from the guild storage, gather on a round over the nodes its skill opens.",
    },
    DUNGEON = {
        label = "Dungeon",
        desc = "Lead its squad into a dungeon and from boss to boss. Give it to the squad's leader; afterwards it is on "
            .. "Autopilot again.",
        needsDungeon = true,
    },
    FARM = {
        label = "Farm item",
        desc = "Farm one item: the bot hunts groups of creatures that drop it and are far enough below its level, on "
            .. "another continent if it must. The item goes into the guild storage, until the target stock is reached "
            .. "(or on and on). It tells you when there is nothing it can farm it from.",
        needsItem = true,
    },
    TRAIN = { label = "Train", desc = "Visit trainers for class spells, profession ranks and riding." },
    VENDOR = { label = "Vendor", desc = "Sell junk, repair and restock." },
}
Data.ORDER_ORDER = { "AUTO", "IDLE", "FOLLOW", "LEVEL_ZONE", "GRIND", "QUEST", "GATHER", "CRAFT",
    "DUNGEON", "FARM", "TRAIN", "VENDOR" }

function Data.OrderLabel(order)
    local o = Data.ORDERS[order]
    return o and o.label or tostring(order or "?")
end

-------------------------------------------------------------------------------
-- Features (docs/protocol.md §8.4)
-------------------------------------------------------------------------------

Data.FEATURES = {
    orders = { label = "Orders", desc = "Give bots orders: Autopilot, Idle, Follow, Level in zone." },
    recruit = { label = "Recruitment", desc = "Recruit new bots into free slots." },
    auto_limits = { label = "Autopilot limits", desc = "Level cap, continent, dungeon and gold limits for Autopilot." },
    professions = { label = "Professions", desc = "Plan professions per bot; bots train and level them." },
    talents = { label = "Talents", desc = "Choose a talent template per bot." },
    gear = { label = "Gear", desc = "Gear view and equipment management per bot." },
    groups = { label = "Groups", desc = "Party builder: form bot groups with roles." },
    dungeons = { label = "Dungeons", desc = "Send bot parties into dungeons." },
    raids = { label = "Raids", desc = "Raid orders for 10 and 25 bots." },
    map = { label = "Map", desc = "World map overlay with bot positions and assigned zones." },
    stats = { label = "Statistics", desc = "Per-bot statistics: XP/hour, gold/hour, deaths, quests." },
    guild_bank = { label = "Guild bank rules", desc = "Rules for depositing gathered materials and gold." },
}

function Data.FeatureLabel(key)
    local f = Data.FEATURES[key]
    return f and f.label or tostring(key)
end

-------------------------------------------------------------------------------
-- States, events, continents
-------------------------------------------------------------------------------

Data.STATES = {
    active = { label = "Active", color = "|cff40ff40" },
    benched = { label = "Benched", color = "|cff9d9d9d" },
    controlled = { label = "Played by you", color = "|cff40c0ff" },
    offline = { label = "Offline", color = "|cffff8040" },
}

function Data.StateLabel(state, colored)
    local s = Data.STATES[state]
    if not s then
        return tostring(state or "?")
    end
    return colored and (s.color .. s.label .. "|r") or s.label
end

Data.EVENT_COLORS = {
    LEVEL_UP = "|cffffd100",
    DEATH = "|cffff4040",
    QUEST_DONE = "|cff40ff40",
    ZONE_CHANGE = "|cff80c0ff",
    ZONE_DONE = "|cff80c0ff",
    PROF_UP = "|cffc080ff",
    ACHIEVEMENT = "|cffffff00",
    MILESTONE = "|cffffe060",
    PERK = "|cff40ffc0",
    GEAR = "|cffc0a060",
    FARM = "|cffff8040",
    RENOWN = "|cffffa040",
    RENOWN_LEVEL = "|cffff8000",
    UNLOCK = "|cffff8000",
    RECRUIT = "|cff40ff40",
    ORDER = "|cffcccccc",
    CONTROL = "|cff40c0ff",
    LOGIN = "|cffcccccc",
    LOGOUT = "|cffcccccc",
    INFO = "|cffcccccc",
}

Data.CONTINENTS = {
    { id = -1, name = "Anywhere" },
    { id = 0, name = "Eastern Kingdoms" },
    { id = 1, name = "Kalimdor" },
    { id = 530, name = "Outland" },
    { id = 571, name = "Northrend" },
}

function Data.ContinentName(id)
    for i = 1, #Data.CONTINENTS do
        if Data.CONTINENTS[i].id == id then
            return Data.CONTINENTS[i].name
        end
    end
    return "Map " .. tostring(id)
end

-------------------------------------------------------------------------------
-- Formatting helpers
-------------------------------------------------------------------------------

-- Copper -> "12g 34s 56c" with coin colours (plain text, no textures).
function Data.FormatMoney(copper)
    copper = math.floor(tonumber(copper) or 0)
    local negative = copper < 0
    if negative then
        copper = -copper
    end
    local gold = math.floor(copper / 10000)
    local silver = math.floor((copper % 10000) / 100)
    local cop = copper % 100
    local parts = {}
    if gold > 0 then
        parts[#parts + 1] = "|cffffd700" .. gold .. "g|r"
    end
    if silver > 0 or gold > 0 then
        parts[#parts + 1] = "|cffc7c7cf" .. silver .. "s|r"
    end
    parts[#parts + 1] = "|cffeda55f" .. cop .. "c|r"
    return (negative and "-" or "") .. table.concat(parts, " ")
end

-- Minutes -> "1 h", "1 h 30 min", "45 min".
function Data.FormatMinutes(minutes)
    minutes = math.floor(tonumber(minutes) or 0)
    local h = math.floor(minutes / 60)
    local m = minutes % 60
    if h > 0 and m > 0 then
        return h .. " h " .. m .. " min"
    elseif h > 0 then
        return h .. " h"
    end
    return m .. " min"
end

-- Seconds -> "just now", "5 min ago", "3 h ago", "2 d ago".
function Data.FormatAgo(seconds)
    seconds = math.max(0, math.floor(tonumber(seconds) or 0))
    if seconds < 60 then
        return "just now"
    elseif seconds < 3600 then
        return math.floor(seconds / 60) .. " min ago"
    elseif seconds < 86400 then
        return math.floor(seconds / 3600) .. " h ago"
    end
    return math.floor(seconds / 86400) .. " d ago"
end
