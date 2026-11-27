-- MockServer: a simulated mod-guild-adventure inside the client (docs/protocol.md §12).
-- It speaks the real wire format: it reassembles the client's "C" chunks, answers with "S"
-- chunks and feeds them through GA.Transport.OnAddonMessage, the same entry point the live
-- CHAT_MSG_ADDON path uses. Its world (guild, bots, renown) lives in GuildAdventureDB.mock.
-- This file is a development and demo tool; the real server never depends on it.

local GA = GuildAdventure
local Mock = {}
GA.MockServer = Mock

local Codec, Transport, Data = GA.Codec, GA.Transport, GA.Data

Mock.TICK = 4            -- seconds between simulation steps
Mock.LATENCY = 0.1       -- simulated round trip
Mock.ZONE_VERSION = 1
Mock.SERVER_VERSION = "mock-0.1.0"
Mock.LEADER_GUID = 1
Mock.MAX_SLOTS = 39
Mock.MAX_LEVEL = 80
Mock.MAX_EVENTS = 60

local LEVEL_ORDERS = { "LEVEL_ZONE", "GRIND", "QUEST", "GATHER" }

-- Sample of what ga_zone_template should hold: id, name, level band, faction, continent.
Mock.ZONES = {
    { id = 12, n = "Elwynn Forest", min = 1, max = 10, fac = "A", cont = 0 },
    { id = 1, n = "Dun Morogh", min = 1, max = 10, fac = "A", cont = 0 },
    { id = 141, n = "Teldrassil", min = 1, max = 10, fac = "A", cont = 1 },
    { id = 3524, n = "Azuremyst Isle", min = 1, max = 10, fac = "A", cont = 530 },
    { id = 14, n = "Durotar", min = 1, max = 10, fac = "H", cont = 1 },
    { id = 215, n = "Mulgore", min = 1, max = 10, fac = "H", cont = 1 },
    { id = 85, n = "Tirisfal Glades", min = 1, max = 10, fac = "H", cont = 0 },
    { id = 3430, n = "Eversong Woods", min = 1, max = 10, fac = "H", cont = 530 },
    { id = 40, n = "Westfall", min = 10, max = 20, fac = "A", cont = 0 },
    { id = 38, n = "Loch Modan", min = 10, max = 20, fac = "A", cont = 0 },
    { id = 148, n = "Darkshore", min = 10, max = 20, fac = "A", cont = 1 },
    { id = 3525, n = "Bloodmyst Isle", min = 10, max = 20, fac = "A", cont = 530 },
    { id = 17, n = "The Barrens", min = 10, max = 25, fac = "H", cont = 1 },
    { id = 130, n = "Silverpine Forest", min = 10, max = 20, fac = "H", cont = 0 },
    { id = 3433, n = "Ghostlands", min = 10, max = 20, fac = "H", cont = 530 },
    { id = 44, n = "Redridge Mountains", min = 15, max = 25, fac = "A", cont = 0 },
    { id = 406, n = "Stonetalon Mountains", min = 15, max = 27, fac = "C", cont = 1 },
    { id = 331, n = "Ashenvale", min = 18, max = 30, fac = "C", cont = 1 },
    { id = 10, n = "Duskwood", min = 18, max = 30, fac = "A", cont = 0 },
    { id = 11, n = "Wetlands", min = 20, max = 30, fac = "A", cont = 0 },
    { id = 267, n = "Hillsbrad Foothills", min = 20, max = 30, fac = "C", cont = 0 },
    { id = 400, n = "Thousand Needles", min = 25, max = 35, fac = "C", cont = 1 },
    { id = 36, n = "Alterac Mountains", min = 30, max = 40, fac = "C", cont = 0 },
    { id = 45, n = "Arathi Highlands", min = 30, max = 40, fac = "C", cont = 0 },
    { id = 405, n = "Desolace", min = 30, max = 40, fac = "C", cont = 1 },
    { id = 33, n = "Stranglethorn Vale", min = 30, max = 45, fac = "C", cont = 0 },
    { id = 15, n = "Dustwallow Marsh", min = 35, max = 45, fac = "C", cont = 1 },
    { id = 3, n = "Badlands", min = 35, max = 45, fac = "C", cont = 0 },
    { id = 8, n = "Swamp of Sorrows", min = 35, max = 45, fac = "C", cont = 0 },
    { id = 357, n = "Feralas", min = 40, max = 50, fac = "C", cont = 1 },
    { id = 440, n = "Tanaris", min = 40, max = 50, fac = "C", cont = 1 },
    { id = 47, n = "The Hinterlands", min = 40, max = 50, fac = "C", cont = 0 },
    { id = 51, n = "Searing Gorge", min = 43, max = 50, fac = "C", cont = 0 },
    { id = 16, n = "Azshara", min = 45, max = 55, fac = "C", cont = 1 },
    { id = 4, n = "Blasted Lands", min = 45, max = 55, fac = "C", cont = 0 },
    { id = 490, n = "Un'Goro Crater", min = 48, max = 55, fac = "C", cont = 1 },
    { id = 361, n = "Felwood", min = 48, max = 55, fac = "C", cont = 1 },
    { id = 46, n = "Burning Steppes", min = 50, max = 58, fac = "C", cont = 0 },
    { id = 28, n = "Western Plaguelands", min = 51, max = 58, fac = "C", cont = 0 },
    { id = 139, n = "Eastern Plaguelands", min = 53, max = 60, fac = "C", cont = 0 },
    { id = 618, n = "Winterspring", min = 53, max = 60, fac = "C", cont = 1 },
    { id = 1377, n = "Silithus", min = 55, max = 60, fac = "C", cont = 1 },
    { id = 3483, n = "Hellfire Peninsula", min = 58, max = 63, fac = "C", cont = 530 },
    { id = 3521, n = "Zangarmarsh", min = 60, max = 64, fac = "C", cont = 530 },
    { id = 3519, n = "Terokkar Forest", min = 62, max = 65, fac = "C", cont = 530 },
    { id = 3518, n = "Nagrand", min = 64, max = 67, fac = "C", cont = 530 },
    { id = 3522, n = "Blade's Edge Mountains", min = 65, max = 68, fac = "C", cont = 530 },
    { id = 3523, n = "Netherstorm", min = 67, max = 70, fac = "C", cont = 530 },
    { id = 3520, n = "Shadowmoon Valley", min = 67, max = 70, fac = "C", cont = 530 },
    { id = 3537, n = "Borean Tundra", min = 68, max = 72, fac = "C", cont = 571 },
    { id = 495, n = "Howling Fjord", min = 68, max = 72, fac = "C", cont = 571 },
    { id = 65, n = "Dragonblight", min = 71, max = 75, fac = "C", cont = 571 },
    { id = 394, n = "Grizzly Hills", min = 73, max = 75, fac = "C", cont = 571 },
    { id = 66, n = "Zul'Drak", min = 74, max = 77, fac = "C", cont = 571 },
    { id = 3711, n = "Sholazar Basin", min = 76, max = 78, fac = "C", cont = 571 },
    { id = 67, n = "The Storm Peaks", min = 77, max = 80, fac = "C", cont = 571 },
    { id = 210, n = "Icecrown", min = 77, max = 80, fac = "C", cont = 571 },
}
local ZONE_BY_ID = {}
for _, z in ipairs(Mock.ZONES) do
    ZONE_BY_ID[z.id] = z
end

local CITIES = {
    A = { id = 1519, n = "Stormwind City", map = 0 },
    H = { id = 1637, n = "Orgrimmar", map = 1 },
}
local DK_START = { id = 4298, n = "Plaguelands: The Scarlet Enclave", map = 609 }

-- Example progression curve (the design log §2.2): renown level -> unlocks.
Mock.UNLOCKS = {
    { id = 1, lvl = 1, kind = "slots", val = "3", gold = 0, desc = "3 bot slots" },
    { id = 2, lvl = 1, kind = "offline", val = "60", gold = 0, desc = "Bots keep playing 1 h after you log out" },
    { id = 3, lvl = 1, kind = "feature", val = "orders", gold = 0, desc = "Orders: Autopilot, Idle, Follow, Level in zone" },
    { id = 4, lvl = 1, kind = "feature", val = "recruit", gold = 0, desc = "Recruit bots into free slots" },
    { id = 5, lvl = 2, kind = "slots", val = "5", gold = 0, desc = "5 bot slots" },
    { id = 6, lvl = 2, kind = "feature", val = "professions", gold = 0, desc = "Profession plans" },
    { id = 7, lvl = 3, kind = "slots", val = "8", gold = 0, desc = "8 bot slots" },
    { id = 8, lvl = 3, kind = "offline", val = "120", gold = 0, desc = "Offline window 2 h" },
    { id = 9, lvl = 3, kind = "feature", val = "auto_limits", gold = 0, desc = "Autopilot limits (level cap, continent, dungeons, gold)" },
    { id = 10, lvl = 4, kind = "slots", val = "10", gold = 1000000, desc = "10 bot slots" },
    { id = 11, lvl = 1, kind = "feature", val = "talents", gold = 0, desc = "Talent templates" },
    { id = 12, lvl = 5, kind = "slots", val = "15", gold = 2000000, desc = "15 bot slots" },
    { id = 13, lvl = 5, kind = "offline", val = "240", gold = 0, desc = "Offline window 4 h" },
    { id = 14, lvl = 1, kind = "feature", val = "gear", gold = 0, desc = "Gear management" },
    { id = 15, lvl = 6, kind = "slots", val = "20", gold = 0, desc = "20 bot slots" },
    { id = 16, lvl = 6, kind = "feature", val = "groups", gold = 0, desc = "Party builder" },
    { id = 17, lvl = 7, kind = "slots", val = "25", gold = 5000000, desc = "25 bot slots" },
    { id = 18, lvl = 7, kind = "offline", val = "360", gold = 0, desc = "Offline window 6 h" },
    { id = 19, lvl = 7, kind = "feature", val = "dungeons", gold = 0, desc = "Dungeon orders" },
    { id = 20, lvl = 7, kind = "feature", val = "map", gold = 0, desc = "World map overlay" },
    { id = 21, lvl = 8, kind = "slots", val = "30", gold = 0, desc = "30 bot slots" },
    { id = 22, lvl = 8, kind = "feature", val = "stats", gold = 0, desc = "Bot statistics" },
    { id = 23, lvl = 9, kind = "slots", val = "39", gold = 10000000, desc = "39 bot slots" },
    { id = 24, lvl = 9, kind = "offline", val = "480", gold = 0, desc = "Offline window 8 h" },
    { id = 25, lvl = 9, kind = "feature", val = "raids", gold = 0, desc = "Raid orders" },
}
local UNLOCK_BY_ID = {}
for _, u in ipairs(Mock.UNLOCKS) do
    UNLOCK_BY_ID[u.id] = u
end

-- Renown needed to go from level n to n + 1.
Mock.RENOWN_NEEDED = { 500, 1250, 2500, 4500, 7000, 10000, 14000, 19000 }
Mock.RENOWN = { LEVEL_UP = 10, QUEST_DONE = 3, PROF_UP = 5 }

local S                  -- persisted mock world (GA.db.mock.state)
local enabled = false
local tickTimer = nil
local outId = 0
local outQueue = {}
local flushScheduled = false
local dirtyBots = {}
local dirtyState = false
local silent = false     -- true while simulating an offline window: no pushes

-------------------------------------------------------------------------------
-- Wire
-------------------------------------------------------------------------------

local function FlushOut()
    flushScheduled = false
    local me = UnitName("player")
    local chunks = outQueue
    outQueue = {}
    for i = 1, #chunks do
        Transport.OnAddonMessage(GA.PREFIX, chunks[i], "WHISPER", me)
    end
end

local function Reply(op, fields)
    if silent then
        return
    end
    outId = outId % Transport.MAX_ID + 1
    local chunks = Transport.Split(Codec.Encode(op, fields), "S", outId)
    for i = 1, #chunks do
        outQueue[#outQueue + 1] = chunks[i]
    end
    if not flushScheduled then
        flushScheduled = true
        GA.After(Mock.LATENCY, FlushOut)
    end
end
Mock.Reply = Reply

local function ReplyError(r, code, msg, reason)
    Reply("ERROR", { r = r, code = code, msg = msg, reason = reason })
end

local function Ack(r)
    if r then
        Reply("ACK", { r = r })
    end
end

-------------------------------------------------------------------------------
-- World helpers
-------------------------------------------------------------------------------

local function XpFor(level)
    if level >= Mock.MAX_LEVEL then
        return 0
    end
    return math.floor((40 * level * level + 360 * level) / 100 + 0.5) * 100
end

local function RenownNeeded(level)
    return Mock.RENOWN_NEEDED[level] or 0
end

local function IsOwned(id)
    return S.owned[id] == true
end

local function OwnedValue(kind)
    local best = 0
    for _, u in ipairs(Mock.UNLOCKS) do
        if u.kind == kind and IsOwned(u.id) then
            best = math.max(best, tonumber(u.val) or 0)
        end
    end
    return best
end

-- A slots / offline tier no larger than what the guild has already is "covered" (nothing to buy), as on the server.
local function UnlockStatus(u)
    if IsOwned(u.id) then
        return "owned"
    elseif (u.kind == "slots" or u.kind == "offline") and (tonumber(u.val) or 0) <= OwnedValue(u.kind) then
        return "covered"
    elseif u.lvl <= S.renown.level then
        return "available"
    end
    return "locked"
end

local function HasFeature(key)
    for _, u in ipairs(Mock.UNLOCKS) do
        if u.kind == "feature" and u.val == key and IsOwned(u.id) then
            return true
        end
    end
    return false
end

local function BotCount()
    local n = 0
    for _ in pairs(S.bots) do
        n = n + 1
    end
    return n
end

local function SortedGuids()
    local list = {}
    for g in pairs(S.bots) do
        list[#list + 1] = g
    end
    table.sort(list)
    return list
end

local function DkUnlocked()
    if (UnitLevel("player") or 0) >= Data.DK_START_LEVEL then
        return true
    end
    for _, bot in pairs(S.bots) do
        if bot.lvl >= Data.DK_START_LEVEL then
            return true
        end
    end
    return false
end

local function ControlledName()
    if S.controlled == Mock.LEADER_GUID then
        return S.leader.n
    end
    local bot = S.bots[S.controlled]
    return bot and bot.n or S.leader.n
end

local function ZoneOf(bot)
    return ZONE_BY_ID[bot.zone]
end

local function ZoneAllowedForFaction(zone)
    return zone.fac == "C" or zone.fac == S.faction
end

-- Best zone for a bot: matches faction and AUTO continent limit, contains the bot's level,
-- prefers the highest band (most XP). Falls back to the lowest zone above the bot's level.
local function BestZone(bot)
    local best, fallback
    for _, z in ipairs(Mock.ZONES) do
        if ZoneAllowedForFaction(z) and (bot.cont == nil or bot.cont < 0 or z.cont == bot.cont) then
            if z.min <= bot.lvl and z.max >= bot.lvl then
                if not best or z.min > best.min then
                    best = z
                end
            elseif z.min > bot.lvl and (not fallback or z.min < fallback.min) then
                fallback = z
            end
        end
    end
    return best or fallback
end

local function MoveTo(bot, zone)
    bot.zone = zone.id
    bot.zn = zone.n
    bot.map = zone.cont
end

local MOCK_ROLE_TOKENS = { [1] = "tank", [2] = "heal" }
local MOCK_SPEC_ROLES_OF

local function BotFields(bot)
    local f = {}
    for k, v in pairs(bot) do
        if string.sub(k, 1, 1) ~= "_" then
            f[k] = v
        end
    end
    -- Gear level and dungeon readiness of a bot in the world (docs/protocol.md §7.20): a little below its level.
    if bot.s ~= "benched" and Mock.Readiness then
        bot.gl = bot.gl or math.max(1, (bot.lvl or 1) - 3)
        f.gl = bot.gl
        f.rdy = Mock.Readiness(bot)
    end
    -- The last known gear level, also for a benched bot (the roster's iLvl column).
    f.glk = bot.gl or math.max(1, (bot.lvl or 1) - 3)
    -- Dungeon role (docs/protocol.md "Dungeon roles"): what the class can be, and the readiness in the dungeon set.
    local can = MOCK_ROLE_TOKENS[MOCK_SPEC_ROLES_OF and MOCK_SPEC_ROLES_OF(bot.class) or 0]
    f.drs = can or ""
    if bot.dr and bot.dr ~= "none" and f.rdy then
        f.drgl = math.max(1, (bot.gl or 1) - 2 + (bot.drp or 0))
        f.drrdy = f.rdy
    end
    return f
end

local function MarkBot(bot)
    dirtyBots[bot.g] = true
end

local MilestoneTotals

local function StateFields()
    local msGot, msAct, msPoints = 0, 0, 0
    if MilestoneTotals then
        msGot, msAct, msPoints = MilestoneTotals()
    end
    local orders = {}
    if HasFeature("orders") then
        orders = { "AUTO", "IDLE", "FOLLOW", "LEVEL_ZONE", "CRAFT", "DUNGEON", "FARM" }
    end
    local feat = {}
    for _, u in ipairs(Mock.UNLOCKS) do
        if u.kind == "feature" and IsOwned(u.id) then
            feat[#feat + 1] = u.val
        end
    end
    return {
        guild = S.guildName, fac = S.faction,
        rl = S.renown.level, rp = S.renown.points, rn = RenownNeeded(S.renown.level),
        rt = S.renown.total, slots = OwnedValue("slots"), used = BotCount(), max = Mock.MAX_SLOTS,
        ow = OwnedValue("offline"), owmax = 480, bank = S.bank, ulgold = S.bank, ulfrom = "bank", feat = feat, orders = orders,
        dk = DkUnlocked(), ms = msGot, msa = msAct, msp = msPoints, msr = msPoints,
    }
end

local function ControlFields(busy)
    return {
        leader = Mock.LEADER_GUID, ln = S.leader.n, g = S.controlled, cn = ControlledName(),
        busy = busy and true or false, path = busy and "switch" or nil,
        watch = S.watching and true or false,
    }
end

-- The leader record (docs/protocol.md §7.15): its order (HOLD = none, the default) and what it does.
local LEADER_WHY = {
    HOLD = "Holding position (no order)",
    AUTO = "Questing near you: Autopilot",
    IDLE = "Idle: waiting at the inn",
    FOLLOW = "Following you",
}

local function LeaderFields()
    local l = S.leader
    l.o = l.o or "HOLD"
    local playing = S.controlled == Mock.LEADER_GUID
    local why
    if playing and not S.watching then
        why = "Played by you"
    elseif l.o == "LEVEL_ZONE" then
        why = "Levelling in " .. (l.ozn or "its zone")
    else
        why = LEADER_WHY[l.o] or Data.OrderLabel(l.o)
    end
    if playing and S.watching then
        why = "Watch mode: " .. why
    end
    local classToken = type(UnitClass) == "function" and select(2, UnitClass("player")) or nil
    local classId = 1
    for id, class in pairs(Data.CLASSES) do
        if class.token == classToken then
            classId = id
        end
    end
    return {
        g = Mock.LEADER_GUID, n = l.n, race = 1, class = classId,
        gender = 0, s = playing and "controlled" or "active",
        lvl = type(UnitLevel) == "function" and UnitLevel("player") or 1,
        zn = type(GetZoneText) == "function" and GetZoneText() or "",
        o = l.o, oz = l.oz or 0, ozn = l.ozn or "", lc = 0, cont = -1, nd = false, gcap = 0, why = why,
    }
end

local function UnlockFields(u)
    return { id = u.id, lvl = u.lvl, kind = u.kind, val = u.val, gold = u.gold, st = UnlockStatus(u), desc = u.desc }
end

local function AddEvent(etype, g, msg, extras)
    local e = { id = S.nextEventId, t = time(), type = etype, g = g or 0, msg = msg }
    if extras then
        for k, v in pairs(extras) do
            e[k] = v
        end
    end
    S.nextEventId = S.nextEventId + 1
    S.events[#S.events + 1] = e
    while #S.events > Mock.MAX_EVENTS do
        table.remove(S.events, 1)
    end
    Reply("EVENT", e)
    if S.awayLog then
        S.awayLog[#S.awayLog + 1] = e
    end
end

-- Pushes pending BOT / STATE changes (one message per changed record).
local function FlushDirty()
    for _, g in ipairs(SortedGuids()) do
        if dirtyBots[g] then
            Reply("BOT", BotFields(S.bots[g]))
        end
    end
    dirtyBots = {}
    if dirtyState then
        Reply("STATE", StateFields())
        dirtyState = false
    end
end

local function GainRenown(points)
    S.renown.points = S.renown.points + points
    S.renown.total = S.renown.total + points
    dirtyState = true
    while RenownNeeded(S.renown.level) > 0 and S.renown.points >= RenownNeeded(S.renown.level) do
        S.renown.points = S.renown.points - RenownNeeded(S.renown.level)
        S.renown.level = S.renown.level + 1
        AddEvent("RENOWN_LEVEL", 0, "The guild reached Renown level " .. S.renown.level .. ".", { lvl = S.renown.level })
        for _, u in ipairs(Mock.UNLOCKS) do
            if u.lvl == S.renown.level then
                if u.gold == 0 then
                    S.owned[u.id] = true
                    AddEvent("UNLOCK", 0, "Unlocked: " .. u.desc .. ".")
                else
                    AddEvent("UNLOCK", 0, "Available for purchase: " .. u.desc .. " ("
                        .. math.floor(u.gold / 10000) .. "g).")
                end
                Reply("UNLOCK", UnlockFields(u))
            end
        end
    end
    -- Tiers the new level's free unlocks cover change too.
    for _, u in ipairs(Mock.UNLOCKS) do
        if u.kind == "slots" or u.kind == "offline" then
            Reply("UNLOCK", UnlockFields(u))
        end
    end
    if RenownNeeded(S.renown.level) == 0 then
        S.renown.points = 0
    end
end

-------------------------------------------------------------------------------
-- New world
-------------------------------------------------------------------------------

local function NewBot(g, slot, name, race, class, gender, level)
    local bot = {
        g = g, n = name, race = race, class = class, gender = gender, slot = slot,
        lvl = level, xp = 0, xpmax = XpFor(level), s = "active",
        zone = 0, zn = "", map = 0, o = "AUTO", oz = 0, ozn = "", aa = true,
        lc = 0, cont = -1, nd = false, gcap = 0, why = "", hp = 100, dead = false, combat = false,
        gold = 0, role = "none", spec = "", spt = 0, tal = "", p1 = 0, p2 = 0, t1 = 0, t2 = 0, sec = {}, sk = {},
        dr = "none", drt = 0, drn = "", drw = false, drp = 0,
        fp = 0, fpg = 0, fpn = "", fpsl = 0, fpw = "",
        kills = 0, deaths = 0, quests = 0, xph = 0, gph = 0, since = time(),
    }
    return bot
end

local SEEDS = {
    A = {
        { "Aldric", 1, 1, 0, 11, 40, "AUTO", { 186, 164 }, { 225, 225 }, { 185 }, { { 186, 68, 150 }, { 164, 45, 75 }, { 185, 30, 75 } } },
        { "Brenna", 3, 5, 1, 8, 1, "LEVEL_ZONE", { 182, 171 }, { 150, 150 }, { 129 }, { { 182, 40, 75 }, { 171, 25, 75 }, { 129, 20, 75 } } },
        { "Faelyn", 4, 11, 1, 13, 148, "AUTO", { 393, 165 }, { 150, 150 }, { 356 }, { { 393, 70, 75 }, { 165, 58, 75 }, { 356, 12, 75 } } },
    },
    H = {
        { "Grukash", 2, 1, 0, 11, 17, "AUTO", { 186, 164 }, { 225, 225 }, { 185 }, { { 186, 68, 150 }, { 164, 45, 75 }, { 185, 30, 75 } } },
        { "Zalaya", 8, 5, 1, 8, 14, "LEVEL_ZONE", { 182, 171 }, { 150, 150 }, { 129 }, { { 182, 40, 75 }, { 171, 25, 75 }, { 129, 20, 75 } } },
        { "Morwen", 5, 8, 1, 13, 130, "AUTO", { 197, 333 }, { 150, 150 }, { 356 }, { { 197, 70, 75 }, { 333, 58, 75 }, { 356, 12, 75 } } },
    },
}

local function NewWorld()
    local factionToken = UnitFactionGroup("player")
    local faction = factionToken == "Horde" and "H" or "A"
    local guildName = GetGuildInfo("player") or "The Wandering Banners"
    local w = {
        schema = 1,
        faction = faction,
        guildName = guildName,
        leader = { n = UnitName("player") },
        controlled = Mock.LEADER_GUID,
        renown = { level = 2, points = 200, total = 700 },
        bank = 2400000,
        owned = {},
        bots = {},
        nextGuid = 101,
        events = {},
        nextEventId = 1,
        away = nil,
    }
    for _, u in ipairs(Mock.UNLOCKS) do
        if u.lvl <= w.renown.level and u.gold == 0 then
            w.owned[u.id] = true
        end
    end
    for i, seed in ipairs(SEEDS[faction]) do
        local bot = NewBot(w.nextGuid, i, seed[1], seed[2], seed[3], seed[4], seed[5])
        w.nextGuid = w.nextGuid + 1
        local zone = ZONE_BY_ID[seed[6]]
        bot.zone, bot.zn, bot.map = zone.id, zone.n, zone.cont
        bot.o = seed[7]
        if bot.o == "LEVEL_ZONE" then
            bot.oz, bot.ozn = zone.id, zone.n
        end
        bot.p1, bot.p2 = seed[8][1], seed[8][2]
        bot.t1, bot.t2 = seed[9][1], seed[9][2]
        bot.sec = seed[10]
        bot.sk = seed[11]
        bot.xp = math.floor(bot.xpmax * 0.25 * i)
        bot.gold = 2500 * i * bot.lvl
        bot.kills = 40 * bot.lvl
        bot.quests = 3 * bot.lvl
        bot.since = time() - 86400 * (4 - i)
        w.bots[bot.g] = bot
    end
    return w
end

-------------------------------------------------------------------------------
-- "Why" lines (what the bot is doing right now)
-------------------------------------------------------------------------------

local function UpdateWhy(bot, note)
    local zone = ZoneOf(bot)
    local zn = bot.zn ~= "" and bot.zn or "the world"
    if bot.s == "benched" then
        bot.why = "Benched: offline, keeps its slot"
    elseif bot.s == "controlled" and not S.watching then
        bot.why = "Played by you"
    elseif bot.dead then
        bot.why = "Dead: running back to the corpse in " .. zn
    elseif note then
        bot.why = note
    elseif bot.o == "AUTO" then
        if bot.lc and bot.lc > 0 and bot.lvl >= bot.lc then
            bot.why = "Reached the level cap " .. bot.lc .. ": grinding gold in " .. zn
        elseif math.random() < 0.15 then
            bot.why = "Grinding to fill a gap: no eligible quests nearby"
        elseif math.random() < 0.1 then
            bot.why = "Selling junk and repairing in the nearest town"
        else
            local left = zone and math.max(1, (zone.max - bot.lvl) * 2 + math.random(0, 3)) or 3
            bot.why = "Questing in " .. zn .. ": " .. left .. " eligible quests left"
        end
    elseif bot.o == "LEVEL_ZONE" then
        if zone and bot.lvl > zone.max then
            bot.why = "Out-levelled " .. zn .. ": waiting for a new order"
        elseif zone then
            bot.why = "Levelling in " .. zn .. " (quests and grinding, band " .. zone.min .. "-" .. zone.max .. ")"
        else
            bot.why = "Travelling to " .. (bot.ozn ~= "" and bot.ozn or "its zone")
        end
    elseif bot.o == "IDLE" then
        bot.why = "Waiting in " .. zn
    elseif bot.o == "FARM" then
        -- Only creatures far enough below the bot: with none it has nothing to farm.
        local item
        for _, entry in ipairs(Mock.FARM_ITEMS) do
            if entry.item == bot.oz then
                item = entry
            end
        end
        local name = (bot.ozn and bot.ozn ~= "") and bot.ozn or "an item"
        if item and (bot.lvl or 1) < item.lv + Mock.FARM_LEVELS_BELOW then
            bot.why = "Farm order for " .. name .. ": nothing to farm: every creature that drops " .. name .. " is above level "
                .. math.max((bot.lvl or 1) - Mock.FARM_LEVELS_BELOW, 0)
        else
            bot.why = "Farming " .. name .. ": hunting in " .. zn
        end
    elseif bot.o == "FOLLOW" then
        bot.why = bot.s == "controlled" and "Holding position: you watch this character, so there is no one to follow"
            or ("Following " .. ControlledName())
    else
        bot.why = GA.Data.OrderLabel(bot.o)
    end
    if bot.s == "controlled" and S.watching then
        bot.why = "Watch mode: " .. bot.why
    end
end

-------------------------------------------------------------------------------
-- Simulation
-------------------------------------------------------------------------------

local function FindSkill(bot, skillId)
    for _, entry in ipairs(bot.sk) do
        if entry[1] == skillId then
            return entry
        end
    end
    return nil
end

local function NextCap(cap, level)
    for _, rank in ipairs(Data.PROFESSION_RANKS) do
        if rank.cap > cap then
            if level >= rank.level then
                return rank.cap
            end
            return nil
        end
    end
    return nil
end

local function ProgressProfession(bot, skillId, target)
    if skillId == 0 then
        return
    end
    local entry = FindSkill(bot, skillId)
    if not entry then
        if bot.lvl >= 5 then
            entry = { skillId, 1, 75 }
            bot.sk[#bot.sk + 1] = entry
            AddEvent("PROF_UP", bot.g, bot.n .. " learned " .. Data.ProfessionName(skillId) .. ".")
        end
        return
    end
    local goal = target > 0 and math.min(target, 450) or entry[3]
    if entry[2] >= goal then
        return
    end
    if entry[2] >= entry[3] then
        local cap = NextCap(entry[3], bot.lvl)
        if cap and cap <= goal then
            entry[3] = cap
            local rank = Data.RankForCap(cap)
            AddEvent("PROF_UP", bot.g, bot.n .. " trained " .. rank.name .. " " .. Data.ProfessionName(skillId) .. ".")
        end
        return
    end
    local gain = Data.PROFESSIONS[skillId] and Data.PROFESSIONS[skillId].gathering and math.random(1, 3) or math.random(0, 2)
    local before = entry[2]
    entry[2] = math.min(entry[3], entry[2] + gain)
    if entry[2] == entry[3] and before < entry[3] then
        AddEvent("PROF_UP", bot.g, bot.n .. " reached " .. Data.ProfessionName(skillId) .. " " .. entry[2] .. ".", { pts = Mock.RENOWN.PROF_UP })
        GainRenown(Mock.RENOWN.PROF_UP)
    end
end

local function LevelUp(bot)
    bot.xp = bot.xp - bot.xpmax
    bot.lvl = bot.lvl + 1
    bot.xpmax = XpFor(bot.lvl)
    if bot.xpmax == 0 then
        bot.xp = 0
    end
    AddEvent("LEVEL_UP", bot.g, bot.n .. " reached level " .. bot.lvl .. ".", { lvl = bot.lvl, pts = Mock.RENOWN.LEVEL_UP })
    GainRenown(Mock.RENOWN.LEVEL_UP)
    if bot.lvl == Data.DK_START_LEVEL then
        AddEvent("UNLOCK", 0, bot.n .. " reached level 55: Death Knight recruits are now available.")
        dirtyState = true
    end
    local zone = ZoneOf(bot)
    if zone and bot.lvl > zone.max then
        if bot.o == "AUTO" or (bot.o == "LEVEL_ZONE" and bot.aa) then
            local nextZone = BestZone(bot)
            if nextZone and nextZone.id ~= zone.id then
                MoveTo(bot, nextZone)
                if bot.o == "LEVEL_ZONE" then
                    bot.oz, bot.ozn = nextZone.id, nextZone.n
                end
                AddEvent("ZONE_CHANGE", bot.g, bot.n .. " moved on from " .. zone.n .. " to " .. nextZone.n .. ".", { zone = nextZone.id })
            end
        elseif bot.o == "LEVEL_ZONE" then
            AddEvent("ZONE_DONE", bot.g, bot.n .. " has out-levelled " .. zone.n .. " (auto-advance is off).", { zone = zone.id })
        end
    end
    return true
end

local function TickBot(bot)
    if bot.s ~= "active" then
        UpdateWhy(bot)
        return
    end
    if bot.dead then
        bot.dead = false
        bot.hp = 50
        UpdateWhy(bot)
        MarkBot(bot)
        return
    end
    local working = bot.o == "AUTO" or bot.o == "LEVEL_ZONE"
    local capped = bot.o == "AUTO" and bot.lc > 0 and bot.lvl >= bot.lc
    local outlevelled = bot.o == "LEVEL_ZONE" and ZoneOf(bot) and bot.lvl > ZoneOf(bot).max
    local rate = 0
    if working and not capped and not outlevelled then
        rate = 0.03 + math.random() * 0.05
    elseif bot.o == "FOLLOW" then
        rate = 0.01 + math.random() * 0.01
    end
    bot.combat = rate > 0 and math.random() < 0.3
    if rate > 0 and bot.xpmax > 0 then
        local gain = math.floor(bot.xpmax * rate)
        bot.xp = bot.xp + gain
        bot.xph = gain * math.floor(3600 / Mock.TICK)
        bot.kills = bot.kills + math.random(1, 4)
        local copper = math.random(0, 40 * bot.lvl)
        bot.gold = bot.gold + copper
        bot.gph = copper * math.floor(3600 / Mock.TICK)
        if bot.xp >= bot.xpmax then
            LevelUp(bot)
        end
        if working and math.random() < 0.25 then
            bot.quests = bot.quests + 1
            AddEvent("QUEST_DONE", bot.g, bot.n .. " completed a quest in " .. bot.zn .. ".", { pts = Mock.RENOWN.QUEST_DONE })
            GainRenown(Mock.RENOWN.QUEST_DONE)
        end
        if math.random() < 0.015 then
            bot.dead = true
            bot.combat = false
            bot.hp = 0
            bot.deaths = bot.deaths + 1
            AddEvent("DEATH", bot.g, bot.n .. " died in " .. bot.zn .. ".")
        end
    else
        bot.xph, bot.gph = 0, 0
    end
    if not bot.dead then
        bot.hp = bot.combat and math.random(35, 100) or math.min(100, bot.hp + 25)
    end
    if working then
        ProgressProfession(bot, bot.p1, bot.t1)
        ProgressProfession(bot, bot.p2, bot.t2)
    end
    UpdateWhy(bot)
    MarkBot(bot)
end

function Mock.Tick()
    if not S then
        return
    end
    for _, g in ipairs(SortedGuids()) do
        TickBot(S.bots[g])
    end
    FlushDirty()
end

-------------------------------------------------------------------------------
-- Sync blocks
-------------------------------------------------------------------------------

local function SendRoster()
    Reply("ROSTER_BEGIN", { n = BotCount() })
    for _, g in ipairs(SortedGuids()) do
        Reply("BOT", BotFields(S.bots[g]))
    end
    Reply("ROSTER_END", {})
end

local function SendZones()
    Reply("ZONES_BEGIN", { zv = Mock.ZONE_VERSION, n = #Mock.ZONES })
    for _, z in ipairs(Mock.ZONES) do
        Reply("ZONE", { id = z.id, n = z.n, min = z.min, max = z.max, fac = z.fac, cont = z.cont, orders = LEVEL_ORDERS })
    end
    Reply("ZONES_END", {})
end

local function SendUnlocks()
    Reply("UNLOCKS_BEGIN", { n = #Mock.UNLOCKS })
    for _, u in ipairs(Mock.UNLOCKS) do
        Reply("UNLOCK", UnlockFields(u))
    end
    Reply("UNLOCKS_END", {})
end

local function SendEvents()
    local first = math.max(1, #S.events - 29)
    Reply("EVENTS_BEGIN", { n = #S.events - first + 1 })
    for i = first, #S.events do
        Reply("EVENT", S.events[i])
    end
    Reply("EVENTS_END", {})
end

local function SendAway()
    local a = S.away
    Reply("AWAY_BEGIN", { from = a.from, to = a.to, lv = a.lv, rp = a.rp, gold = a.gold, q = a.q, d = a.d, n = #a.lines })
    for _, line in ipairs(a.lines) do
        Reply("AWAY_LINE", line)
    end
    Reply("AWAY_END", {})
end

-- Talent templates (docs/specs.md): two built-in ones per class, plus what the demo guild saved.
local MOCK_SPEC_NAMES = {
    [1] = { "Arms", "Protection" }, [2] = { "Retribution", "Holy" }, [3] = { "Beast Mastery", "Marksmanship" },
    [4] = { "Combat", "Assassination" }, [5] = { "Shadow", "Holy" }, [6] = { "Blood", "Unholy" },
    [7] = { "Enhancement", "Restoration" }, [8] = { "Frost", "Fire" }, [9] = { "Demonology", "Affliction" },
    [11] = { "Feral", "Restoration" },
}

-- The role of each class's dungeon template (1 tank, 2 healer): what its dungeon role can be.
local MOCK_SPEC_ROLES = { [1] = 1, [2] = 2, [5] = 2, [7] = 2, [11] = 2 }
MOCK_SPEC_ROLES_OF = function(class)
    return MOCK_SPEC_ROLES[class] or 0
end

local function MockSpecs()
    local list = {}
    for class, names in pairs(MOCK_SPEC_NAMES) do
        list[#list + 1] = { id = class * 100 + 1, class = class, n = "Leveling: " .. names[1], p = 0, ro = 0, t = 0, d = true,
            own = false, sum = "51/0/0" }
        list[#list + 1] = { id = class * 100 + 2, class = class, n = "Dungeon and raid: " .. names[2], p = 1,
            ro = MOCK_SPEC_ROLES[class] or 0, t = 1, d = false, own = false, sum = "0/51/0" }
    end
    for _, spec in ipairs(S.specs or {}) do
        list[#list + 1] = spec
    end
    table.sort(list, function(a, b)
        return a.id < b.id
    end)
    return list
end

-- Tiered guild achievements (docs/protocol.md §7.18): a few series with made-up progress.
Mock.MILESTONES = {
    { s = "level_class_1", k = "level_class", grp = "class", sub = 1, n = "Warrior", v = 24, th = { 10, 20, 30, 40, 50, 60, 70, 80 },
        pt = { 5, 5, 5, 5, 5, 10, 5, 5 }, act = 6, got = 2 },
    { s = "level_race_1", k = "level_race", grp = "race", sub = 1, n = "Human", v = 24, th = { 10, 20, 30, 40, 50, 60, 70, 80 },
        pt = { 5, 5, 5, 5, 5, 10, 5, 5 }, act = 6, got = 2 },
    { s = "skill_186", k = "skill", grp = "prof", sub = 186, n = "Mining", v = 112, th = { 75, 150, 225, 300, 375, 450 },
        pt = { 5, 5, 5, 10, 5, 5 }, act = 4, got = 1 },
    { s = "storage_items", k = "storage_items", grp = "guild", sub = 0, n = "", v = 350, th = { 100, 1000, 10000, 100000, 1000000 },
        pt = { 10, 10, 10, 10, 20 }, act = 5, got = 1 },
    { s = "kills_7", k = "kills", grp = "kills", sub = 7, n = "Humanoids", v = 640, th = { 100, 1000, 10000, 100000, 1000000 },
        pt = { 10, 10, 10, 10, 20 }, act = 5, got = 1 },
    { s = "quests", k = "quests", grp = "guild", sub = 0, n = "", v = 87, th = { 10, 100, 1000, 10000, 100000 },
        pt = { 10, 10, 10, 10, 20 }, act = 5, got = 1 },
    { s = "raids", k = "raids", grp = "guild", sub = 0, n = "", v = 0, th = { 1, 5, 10, 25, 50 }, pt = { 20, 20, 20, 20, 40 },
        act = 5, got = 0 },
}

MilestoneTotals = function()
    local got, act, points = 0, 0, 0
    for _, m in ipairs(Mock.MILESTONES) do
        got, act = got + m.got, act + m.act
        for i = 1, m.got do
            points = points + (m.pt[i] or 0)
        end
    end
    return got, act, points
end

-- Dungeons for the readiness numbers (docs/protocol.md §7.20): a few of the classic ones.
Mock.DUNGEONS = {
    { id = 14, map = 389, n = "Ragefire Chasm", min = 15, max = 21, tgt = 16, size = 5, raid = false, gl = 14 },
    { id = 6, map = 36, n = "Deadmines", min = 15, max = 25, tgt = 19, size = 5, raid = false, gl = 17 },
    { id = 1, map = 43, n = "Wailing Caverns", min = 15, max = 25, tgt = 19, size = 5, raid = false, gl = 17 },
    { id = 8, map = 33, n = "Shadowfang Keep", min = 16, max = 26, tgt = 21, size = 5, raid = false, gl = 19, run = false },
    { id = 48, map = 409, n = "Molten Core", min = 60, max = 60, tgt = 60, size = 40, raid = true, gl = 58 },
}

-- The server's numbers (Readiness.cpp): the dungeons two levels ahead to five behind; level part times gear part.
local function MockReadiness(bot)
    local level, gear = bot.lvl or 1, bot.gl or 0
    local list = {}
    for _, d in ipairs(Mock.DUNGEONS) do
        local inReach
        if d.raid then
            inReach = level >= d.min
        else
            inReach = level + 2 >= d.min and level <= d.max + 5
        end
        if inReach then
            local pct = 0
            if level >= d.min then
                local levelPart = 100
                if level < d.tgt and d.tgt > d.min then
                    levelPart = 60 + math.floor(40 * (level - d.min) / (d.tgt - d.min))
                end
                local gearPart = math.min(100, math.floor(gear * 100 / d.gl))
                pct = math.floor(levelPart * gearPart / 100)
            end
            list[#list + 1] = { d.id, pct }
        end
    end
    return list
end
Mock.Readiness = MockReadiness

-- Craft orders (docs/protocol.md §7.21): what the mock's guild can make, and its orders.
-- What the farm order's item picker knows (docs/protocol.md §7.23): lv = the lowest creature that drops it.
Mock.FARM_LEVELS_BELOW = 4
Mock.FARM_STOCK = { [2589] = 143 }  -- what the guild storage holds of an item
Mock.FARM_ITEMS = {
    { item = 2589, n = "Linen Cloth", q = 1, il = 5, cls = 7, k = 143, lv = 3 },
    { item = 2592, n = "Wool Cloth", q = 1, il = 15, cls = 7, k = 118, lv = 14 },
    { item = 4306, n = "Silk Cloth", q = 1, il = 25, cls = 7, k = 131, lv = 24 },
    { item = 4338, n = "Mageweave Cloth", q = 1, il = 35, cls = 7, k = 96, lv = 38 },
    { item = 14047, n = "Runecloth", q = 1, il = 50, cls = 7, k = 120, lv = 50 },
    { item = 2319, n = "Medium Leather Scraps", q = 1, il = 20, cls = 7, k = 4, lv = 18 },
    { item = 1206, n = "Moss Agate", q = 2, il = 25, cls = 7, k = 61, lv = 20 },
    { item = 13022, n = "Gryphonwing Long Bow", q = 3, il = 43, cls = 2, k = 212, lv = 38 },
}

Mock.CRAFTABLES = {
    { item = 11287, n = "Lesser Magic Wand", il = 10, cls = 2, sub = 19, by = "Elara" },
    { item = 11288, n = "Greater Magic Wand", il = 20, cls = 2, sub = 19, by = "Elara" },
    { item = 2996, n = "Bolt of Linen Cloth", il = 10, cls = 7, sub = 5, by = "Brom" },
    { item = 1251, n = "Linen Bandage", il = 1, cls = 0, sub = 7, by = "Brom, Elara" },
}
Mock.craftOrders = {}
Mock.nextCraftOrder = 1

-- The shopping list (docs/protocol.md section 7.25): what vendors sell for gold, and the guild's list.
Mock.SHOP_ITEMS = {
    { e = 159, n = "Refreshing Spring Water", q = 1, lvl = 1, price = 25, bc = 5 },
    { e = 1179, n = "Ice Cold Milk", q = 1, lvl = 5, price = 125, bc = 5 },
    { e = 1205, n = "Melon Juice", q = 1, lvl = 15, price = 500, bc = 5 },
    { e = 1708, n = "Sweet Nectar", q = 1, lvl = 25, price = 1000, bc = 5 },
    { e = 1645, n = "Moonberry Juice", q = 1, lvl = 35, price = 2000, bc = 5 },
    { e = 8766, n = "Morning Glory Dew", q = 1, lvl = 45, price = 4000, bc = 5 },
    { e = 4540, n = "Tough Hunk of Bread", q = 1, lvl = 1, price = 25, bc = 5 },
    { e = 4541, n = "Freshly Baked Bread", q = 1, lvl = 5, price = 125, bc = 5 },
    { e = 4542, n = "Moist Cornbread", q = 1, lvl = 15, price = 500, bc = 5 },
    { e = 4544, n = "Mulgore Spice Bread", q = 1, lvl = 25, price = 1000, bc = 5 },
    { e = 4601, n = "Soft Banana Bread", q = 1, lvl = 35, price = 2000, bc = 5 },
    { e = 8950, n = "Homemade Cherry Pie", q = 1, lvl = 45, price = 4000, bc = 5 },
    { e = 2320, n = "Coarse Thread", q = 1, lvl = 0, price = 10, bc = 1 },
    { e = 3371, n = "Empty Vial", q = 1, lvl = 0, price = 4, bc = 1 },
}
Mock.shopping = {}
Mock.SHOP_SPENT, Mock.SHOP_CAP, Mock.SHOP_VISIT = 12000, 500000, 50000

local function ShopItem(e)
    for _, item in ipairs(Mock.SHOP_ITEMS) do
        if item.e == e then
            return item
        end
    end
end

local function SendShopping()
    Reply("SHOPPING_BEGIN", { n = #Mock.shopping, srv = true, day = Mock.SHOP_SPENT, cap = Mock.SHOP_CAP, visit = Mock.SHOP_VISIT,
        max = 60 })
    for _, entry in ipairs(Mock.shopping) do
        local item = ShopItem(entry.e)
        Reply("SHOP", { e = entry.e, n = item.n, min = entry.min, on = entry.on, have = entry.have or 0, price = item.price,
            bc = item.bc })
    end
    Reply("SHOPPING_END", {})
end

local function SendCraftOrders()
    Reply("CRAFT_ORDERS_BEGIN", { n = #Mock.craftOrders })
    for _, o in ipairs(Mock.craftOrders) do
        Reply("CORDER", o)
    end
    Reply("CRAFT_ORDERS_END", {})
end

local function SendCraftables()
    Reply("CRAFTABLES_BEGIN", { n = #Mock.CRAFTABLES })
    for _, c in ipairs(Mock.CRAFTABLES) do
        Reply("CRAFTABLE", c)
    end
    Reply("CRAFTABLES_END", {})
end

local function SendDungeons()
    Reply("DUNGEONS_BEGIN", { n = #Mock.DUNGEONS })
    -- (ord: the place in the guild's order of dungeons; 0 for a raid and for a dungeon no run can be ordered into.)
    local place = 0
    for _, d in ipairs(Mock.DUNGEONS) do
        if not d.raid and d.run ~= false then
            place = place + 1
            d.ord = place
        else
            d.ord = 0
        end
        Reply("DUNGEON", d)
    end
    Reply("DUNGEONS_END", {})
end

-- What the mock guild's storage holds for a run, and the server's fight rules.
Mock.DUNGEON_ITEMS = {
    { e = 2458, n = "Elixir of Minor Fortitude", q = 1, k = 1, amt = 12, lvl = 2 },
    { e = 3013, n = "Scroll of Protection", q = 1, k = 2, amt = 7, lvl = 1 },
    { e = 2863, n = "Coarse Sharpening Stone", q = 1, k = 3, amt = 20, lvl = 5 },
    { e = 4405, n = "Crude Scope", q = 1, k = 4, amt = 2, lvl = 5 },
}

local function SendDungeonPrep()
    Reply("DPREP_BEGIN", { n = #Mock.DUNGEON_ITEMS, tl = 4, em = 50, cp = 3, cpon = true, ff = true, md = 0, gr = 25, gc = 6 })
    for _, item in ipairs(Mock.DUNGEON_ITEMS) do
        Reply("DITEM", item)
    end
    Reply("DPREP_END", {})
end

-- The dungeon runs of the mock guild (docs/protocol.md "Dungeon runs").
local function SendDungeonRuns()
    S.dungeonRuns = S.dungeonRuns or {}
    Reply("DRUNS_BEGIN", { n = #S.dungeonRuns })
    for _, run in ipairs(S.dungeonRuns) do
        Reply("DRUN", run)
    end
    Reply("DRUNS_END", {})
end

local function SendMilestones()
    Reply("MILESTONES_BEGIN", { n = #Mock.MILESTONES })
    for _, m in ipairs(Mock.MILESTONES) do
        Reply("MILESTONE", { s = m.s, k = m.k, grp = m.grp, sub = m.sub, n = m.n, v = m.v, th = table.concat(m.th, ","),
            pt = table.concat(m.pt, ","), act = m.act, got = m.got })
    end
    Reply("MILESTONES_END", {})
end

local function SendSpecs()
    local list = MockSpecs()
    Reply("SPECS_BEGIN", { n = #list })
    for _, spec in ipairs(list) do
        Reply("SPEC", spec)
    end
    Reply("SPECS_END", {})
end

-- Bot behaviour settings (docs/bot-settings.md): a part of the server's list, same ids and keys, plus one choice setting
-- the server does not have yet, to show what a choice looks like.
Mock.SETTINGS_VERSION = 1
Mock.SETTING_CATS = {
    { k = "combat", n = "Combat and rest", h = "When bots rest, and when they leave a fight." },
    { k = "danger", n = "Danger and deaths", h = "How careful bots choose their spots, and what happens after deaths." },
    { k = "quests", n = "Questing and travel", h = "Which quests bots take and how they travel." },
    { k = "upkeep", n = "Vendors and provisions", h = "Food, drink and vendor trips." },
    { k = "chat", n = "Guild chat", h = "How often the bots' guild chat reaches you." },
}
Mock.SETTING_DEFS = {
    { id = 101, k = "Rest.Enable", cat = "combat", t = "bool", min = 0, max = 1, def = 1, pc = true,
        n = "Rest between fights", h = "Out of combat a bot with low health or mana stops, eats and drinks before it goes on." },
    { id = 102, k = "Rest.BelowHealth", cat = "combat", t = "int", min = 0, max = 95, st = 5, u = "%", def = 65, pc = true,
        n = "Rest below health", h = "A rest starts when health is below this. 0 = never because of health." },
    { id = 106, k = "Rest.MaxSeconds", cat = "combat", t = "int", min = 5, max = 600, st = 5, u = "s", def = 60, pc = true, adv = true,
        n = "Longest rest without food", h = "A rest without food and drink ends after this time." },
    { id = 122, k = "Escape.Margin", cat = "combat", t = "dec", min = 25, max = 400, st = 5, sc = 100, def = 100, pc = true,
        n = "Courage", h = "1.00 = a bot runs when the rest of the fight would cost all its health." },
    { id = 202, k = "Risk.MaxFoeLevelAbove", cat = "danger", t = "int", min = 0, max = 10, u = "levels", def = 1, pc = true,
        n = "Quest targets above its level", h = "Quest targets more levels above the bot wait until it is stronger." },
    { id = 204, k = "Risk.CautiousDeaths", cat = "danger", t = "int", min = 0, max = 20, u = "deaths", def = 4, pc = true,
        n = "Play it safe after deaths", h = "This many deaths in a short time and a bot plays it safe for a while." },
    { id = 301, k = "Quests.Escorts", cat = "quests", t = "bool", min = 0, max = 1, def = 1, pc = true,
        n = "Escort quests", h = "Bots take escort quests and follow the NPC." },
    { id = 303, k = "AutoAdvanceDefault", cat = "quests", t = "bool", min = 0, max = 1, def = 1, pc = false,
        n = "New recruits move on by themselves", h = "A new recruit goes on to the next zone when it has out-levelled its zone." },
    { id = 401, k = "Provisions.MinMoney", cat = "upkeep", t = "money", min = 0, max = 1000000, st = 100, def = 100, pc = true,
        n = "Buy food and drink from", h = "A bot buys food and drink only when it has at least this much money." },
    { id = 499, k = "Vendor.Trips", cat = "upkeep", t = "enum", min = 0, max = 2, def = 0, pc = true,
        opt = "hub=Only at a hub after a hand-in;full=When the bags are full;any=Whenever useful",
        n = "Vendor trips (demo)", h = "When a bot walks to a vendor. Demo mode only: the server's setting comes later." },
    { id = 701, k = "BotGuildChat.MinSeconds", cat = "chat", t = "int", min = 0, max = 3600, st = 5, u = "s", def = 30, pc = false,
        n = "Bots in guild chat at most every", h = "Your client shows one guild chat line of the bots per pause." },
}

local function MockSettingDef(id)
    for _, def in ipairs(Mock.SETTING_DEFS) do
        if def.id == id then
            return def
        end
    end
end

local function SettingScope(g)
    S.settings = S.settings or {}
    local key = tostring(g or 0)
    S.settings[key] = S.settings[key] or {}
    return S.settings[key]
end

local function SettingValFields(g)
    local tuples = {}
    for id, value in pairs(SettingScope(g)) do
        tuples[#tuples + 1] = { tonumber(id), value }
    end
    table.sort(tuples, function(a, b)
        return a[1] < b[1]
    end)
    return { g = g or 0, v = tuples }
end

local function SendSettings(clientSv)
    if clientSv ~= Mock.SETTINGS_VERSION then
        Reply("SETDEFS_BEGIN", { n = #Mock.SETTING_CATS + #Mock.SETTING_DEFS, sv = Mock.SETTINGS_VERSION })
        for i, cat in ipairs(Mock.SETTING_CATS) do
            Reply("SETCAT", { k = cat.k, n = cat.n, h = cat.h, o = i })
        end
        for _, def in ipairs(Mock.SETTING_DEFS) do
            Reply("SETDEF", { id = def.id, k = def.k, cat = def.cat, t = def.t, min = def.min, max = def.max, st = def.st or 1,
                sc = def.sc or 1, u = def.u or "", def = def.def, pc = def.pc and true or false, adv = def.adv and true or false,
                opt = def.opt or "", n = def.n, h = def.h })
        end
        Reply("SETDEFS_END", {})
    end
    local scopes = { 0 }
    for key, values in pairs(S.settings or {}) do
        if key ~= "0" and next(values) then
            scopes[#scopes + 1] = tonumber(key)
        end
    end
    Reply("SETVALS_BEGIN", { n = #scopes })
    for _, g in ipairs(scopes) do
        Reply("SETVAL", SettingValFields(g))
    end
    Reply("SETVALS_END", {})
end

local function Sync(clientZv, clientSv)
    Reply("WELCOME", { v = GA.PROTOCOL_VERSION, min = 1, srv = Mock.SERVER_VERSION, now = time(), zv = Mock.ZONE_VERSION })
    Reply("STATE", StateFields())
    Reply("CONTROL_STATE", ControlFields(false))
    if clientZv ~= Mock.ZONE_VERSION then
        SendZones()
    end
    SendUnlocks()
    SendMilestones()
    SendRoster()
    SendDungeonRuns()
    Reply("LEADER", LeaderFields())
    SendEvents()
    if S.away then
        SendAway()
    end
    SendSpecs()
    SendSettings(clientSv)
end

-------------------------------------------------------------------------------
-- Client intents
-------------------------------------------------------------------------------

local function ValidName(name)
    if type(name) ~= "string" or string.len(name) < 2 or string.len(name) > 12 then
        return nil
    end
    if not string.match(name, "^%a+$") then
        return nil
    end
    return string.upper(string.sub(name, 1, 1)) .. string.lower(string.sub(name, 2))
end

local function NameTaken(name)
    if string.lower(name) == string.lower(S.leader.n or "") then
        return true
    end
    for _, bot in pairs(S.bots) do
        if string.lower(bot.n) == string.lower(name) then
            return true
        end
    end
    return false
end

local function FreeSlotNumber()
    local used = {}
    for _, bot in pairs(S.bots) do
        used[bot.slot] = true
    end
    for i = 1, Mock.MAX_SLOTS do
        if not used[i] then
            return i
        end
    end
    return nil
end

local INTENTS = {}

INTENTS.HELLO = function(f)
    local v = Codec.Int(f.v, 0)
    if v < 1 then
        ReplyError(nil, "BAD_VERSION", "Protocol version " .. v .. " is not supported.")
        return
    end
    Sync(Codec.Int(f.zv, 0), Codec.Int(f.sv, 0))
end

local SendRoleGear
local SendPlan
local SendFightProfilesLater, SendFightProfileLater, SendFightCatalogLater

INTENTS.GET = function(f)
    local what = f.what
    if what == "state" then
        Reply("STATE", StateFields())
    elseif what == "control" then
        Reply("CONTROL_STATE", ControlFields(false))
    elseif what == "roster" then
        SendRoster()
    elseif what == "leader" then
        Reply("LEADER", LeaderFields())
    elseif what == "bot" then
        local bot = S.bots[Codec.Int(f.g, 0)]
        if bot then
            Reply("BOT", BotFields(bot))
        end
    elseif what == "zones" then
        SendZones()
    elseif what == "unlocks" then
        SendUnlocks()
    elseif what == "events" then
        SendEvents()
    elseif what == "settings" then
        SendSettings(-1)
    elseif what == "milestones" then
        SendMilestones()
    elseif what == "craftorders" then
        SendCraftOrders()
    elseif what == "shopping" then
        SendShopping()
    elseif what == "craftables" then
        SendCraftables()
    elseif what == "dungeonruns" then
        SendDungeonRuns()
    elseif what == "dungeonprep" then
        SendDungeonPrep()
    elseif what == "dungeons" then
        SendDungeons()
    elseif what == "rolegear" then
        SendRoleGear(Codec.Int(f.g, 0))
    elseif what == "plan" then
        SendPlan(Codec.Int(f.g, 0))
    elseif what == "fightprofiles" then
        SendFightProfilesLater(Codec.Int(f.g, 0))
    elseif what == "fightprofile" then
        SendFightProfileLater(Codec.Int(f.g, 0), Codec.Int(f.id, 0))
    elseif what == "fightcatalog" then
        SendFightCatalogLater(Codec.Int(f.g, 0))
    end
end

-- The leader's order (docs/orders.md "The leader"): HOLD (the default) or AUTO / LEVEL_ZONE / IDLE / FOLLOW.
local function SetLeaderOrder(r, f)
    local order = f.o
    if order ~= "HOLD" and order ~= "AUTO" and order ~= "LEVEL_ZONE" and order ~= "IDLE" and order ~= "FOLLOW" then
        return ReplyError(r, "BAD_REQUEST", "Unknown order type.")
    end
    local l = S.leader
    if order == "LEVEL_ZONE" then
        local zone = ZONE_BY_ID[Codec.Int(f.zone, 0)]
        if not zone or not ZoneAllowedForFaction(zone) then
            return ReplyError(r, "ZONE_INVALID", "Pick a zone your faction can level in.")
        end
        l.oz, l.ozn = zone.id, zone.n
    else
        l.oz, l.ozn = 0, ""
    end
    l.o = order
    if order == "HOLD" and S.controlled == Mock.LEADER_GUID and S.watching then
        S.watching = false
        Reply("CONTROL_STATE", ControlFields(false))
    end
    AddEvent("ORDER", Mock.LEADER_GUID, order == "HOLD" and (l.n .. " holds its position again (no order).")
        or (l.n .. ": new order " .. Data.OrderLabel(order) .. "."))
    FlushDirty()
    Reply("LEADER", LeaderFields())
    Ack(r)
end

INTENTS.DUNGEON_START = function(f)
    local r = f.r
    local dungeon
    for _, d in ipairs(Mock.DUNGEONS) do
        if d.id == Codec.Int(f.d, 0) and not d.raid and d.run ~= false then
            dungeon = d
        end
    end
    local guids = Codec.IntList(f.bots) or {}
    if not dungeon or #guids == 0 or #guids > 5 then
        return ReplyError(r, "DUNGEON_INVALID", "Not started: choose a dungeon that can be run and 1 to 5 bots.")
    end
    S.dungeonRuns = S.dungeonRuns or {}
    local leader, members = nil, {}
    for _, g in ipairs(guids) do
        -- (The guild leader's own character can come along; it never leads the group.)
        local own = g == Mock.LEADER_GUID
        local bot = S.bots[g]
        if not own and (not bot or bot.s ~= "active") then
            return ReplyError(r, "DUNGEON_INVALID", "Not started: only characters of the guild that are in the world.")
        end
        for _, run in ipairs(S.dungeonRuns) do
            if run.g == g then
                return ReplyError(r, "DUNGEON_INVALID", "Not started: " .. (bot and bot.n or "that character") .. " is on a dungeon run already.")
            end
        end
        if not own and not leader and (bot.dr == "tank" or bot.role == "tank") then
            leader = g
        end
    end
    for _, g in ipairs(guids) do
        if not leader and g ~= Mock.LEADER_GUID then
            leader = g
        end
    end
    if not leader then
        return ReplyError(r, "DUNGEON_INVALID", "Not started: a dungeon group needs a bot that leads it.")
    end
    for _, g in ipairs(guids) do
        if g ~= leader then
            members[#members + 1] = g
        end
    end
    local runs = Codec.Int(f.runs, 1)
    local bot = S.bots[leader]
    bot.o, bot.oz, bot.ozn = "DUNGEON", dungeon.id, dungeon.n
    bot.druns, bot.drund = runs == 0 and 255 or math.max(1, math.min(runs, 10)), 0
    S.dungeonRuns[#S.dungeonRuns + 1] = { g = leader, d = dungeon.id, dn = dungeon.n, ph = 2, b = 0, of = 6,
        nb = "the first boss", w = 0, left = bot.druns, done = 0, min = 0, m = members, why = "In " .. dungeon.n }
    -- (What the preparation step sent, for the tests.)
    Mock.lastDungeonStart = { items = Codec.IntList(f.items) or {}, noitems = Codec.IntList(f.noitems) or {}, ia = Codec.Bool(f.ia),
        tl = Codec.Int(f.tl), em = Codec.Int(f.em),
        cp = Codec.Int(f.cp), cpon = f.cpon, ff = f.ff, md = Codec.Int(f.md), gr = Codec.Int(f.gr), gc = Codec.Int(f.gc) }
    Reply("BOT", BotFields(bot))
    SendDungeonRuns()
    Ack(r)
end

INTENTS.DUNGEON_STOP = function(f)
    local r = f.r
    S.dungeonRuns = S.dungeonRuns or {}
    for i, run in ipairs(S.dungeonRuns) do
        if run.g == Codec.Int(f.g, 0) then
            if Codec.Bool(f.now) then
                table.remove(S.dungeonRuns, i)
                local bot = S.bots[run.g]
                if bot then
                    bot.o, bot.oz, bot.ozn, bot.druns, bot.drund = "AUTO", 0, "", 0, 0
                    Reply("BOT", BotFields(bot))
                end
            else
                run.left = 1
            end
            SendDungeonRuns()
            return Ack(r)
        end
    end
    ReplyError(r, "DUNGEON_INVALID", "Not stopped: that bot leads no dungeon run.")
end

INTENTS.CRAFT_ORDER_ADD = function(f)
    local r = Codec.Int(f.r)
    local item, count = Codec.Int(f.item, 0), Codec.Int(f.n, 0)
    local craftable
    for _, c in ipairs(Mock.CRAFTABLES) do
        if c.item == item then
            craftable = c
        end
    end
    if not craftable or count < 1 or count > 200 then
        return ReplyError(r, "CRAFT_ORDER_INVALID", "No profession recipe makes that.")
    end
    local order
    for _, o in ipairs(Mock.craftOrders) do
        if o.item == item then
            order = o
        end
    end
    if order then
        order.amt = order.amt + count
    else
        order = { id = Mock.nextCraftOrder, item = item, n = craftable.n, amt = count, made = 0,
            note = craftable.by .. " makes it on its next town trip" }
        Mock.nextCraftOrder = Mock.nextCraftOrder + 1
        table.insert(Mock.craftOrders, order)
    end
    SendCraftOrders()
    Ack(r)
end

INTENTS.CRAFT_ORDER_DEL = function(f)
    local r = Codec.Int(f.r)
    local id = Codec.Int(f.id, 0)
    for i, o in ipairs(Mock.craftOrders) do
        if o.id == id then
            table.remove(Mock.craftOrders, i)
            SendCraftOrders()
            return Ack(r)
        end
    end
    ReplyError(r, "CRAFT_ORDER_INVALID", "No such order.")
end

INTENTS.SHOP_SET = function(f)
    local r = Codec.Int(f.r)
    local e, min = Codec.Int(f.e, 0), Codec.Int(f.min, 0)
    local on = f.on ~= "0" and f.on ~= false
    local item = ShopItem(e)
    if not item then
        return ReplyError(r, "SHOP_REFUSED", "no vendor of your side sells it", "NOT_SOLD")
    end
    if min > 100000 then
        return ReplyError(r, "SHOP_REFUSED", "at most 100000 as the minimum", "INVALID")
    end
    local found
    for _, entry in ipairs(Mock.shopping) do
        if entry.e == e then
            found = entry
        end
    end
    if found then
        found.min, found.on = min, on
    else
        table.insert(Mock.shopping, { e = e, min = min, on = on, have = 0 })
    end
    SendShopping()
    Ack(r)
end

INTENTS.SHOP_DEL = function(f)
    local r = Codec.Int(f.r)
    local e = Codec.Int(f.e, 0)
    for i, entry in ipairs(Mock.shopping) do
        if entry.e == e then
            table.remove(Mock.shopping, i)
            SendShopping()
            return Ack(r)
        end
    end
    ReplyError(r, "SHOP_REFUSED", "That item is not on the shopping list.", "NOT_FOUND")
end

INTENTS.SHOP_SEARCH = function(f)
    local r = Codec.Int(f.r)
    local text = string.lower(tostring(f.q or ""))
    local number = string.match(text, "^%d+$")
    if string.len(text) < 2 and not number then
        return ReplyError(r, "BAD_REQUEST", "Type at least two letters of the item's name, or its id.")
    end
    local found = {}
    for _, item in ipairs(Mock.SHOP_ITEMS) do
        if (number and item.e == tonumber(text)) or (not number and string.find(string.lower(item.n), text, 1, true)) then
            found[#found + 1] = item
        end
    end
    Reply("SHOP_ITEMS_BEGIN", { n = #found, q = tostring(f.q or "") })
    for _, item in ipairs(found) do
        Reply("SHOP_ITEM", item)
    end
    Reply("SHOP_ITEMS_END", {})
    Ack(r)
end

-- The farm order's item picker: names that hold the text (or the item of that id), each with what the bot lacks.
INTENTS.FARM_SEARCH = function(f)
    local r = Codec.Int(f.r)
    local g = Codec.Int(f.g, 0)
    local text = string.lower(tostring(f.q or ""))
    local number = string.match(text, "^%d+$")
    if string.len(text) < 2 and not number then
        return ReplyError(r, "BAD_REQUEST", "Type at least two letters of the item's name, or its id.")
    end
    local bot = S.bots[g]
    local level = bot and bot.lvl or (g == Mock.LEADER_GUID and S.leader and S.leader.lvl) or nil
    local found = {}
    for _, item in ipairs(Mock.FARM_ITEMS) do
        local hit
        if number then
            hit = item.item == tonumber(text)
        else
            hit = string.find(string.lower(item.n), text, 1, true) ~= nil
        end
        if hit then
            local need = item.lv + Mock.FARM_LEVELS_BELOW
            found[#found + 1] = { item = item.item, n = item.n, q = item.q, il = item.il, cls = item.cls, k = item.k,
                lv = item.lv, need = need, ok = (not level) or level >= need }
        end
    end
    Reply("FARM_ITEMS_BEGIN", { n = #found, q = tostring(f.q or ""), g = g })
    for _, item in ipairs(found) do
        Reply("FARM_ITEM", item)
    end
    Reply("FARM_ITEMS_END", {})
    Ack(r)
end

INTENTS.ORDER_SET = function(f)
    local r = Codec.Int(f.r)
    if Codec.Int(f.g, 0) == Mock.LEADER_GUID then
        return SetLeaderOrder(r, f)
    end
    local bot = S.bots[Codec.Int(f.g, 0)]
    if not bot then
        return ReplyError(r, "UNKNOWN_BOT", "Unknown bot.")
    end
    local order = f.o
    if not Data.ORDERS[order] then
        return ReplyError(r, "BAD_REQUEST", "Unknown order type.")
    end
    local allowed = false
    for _, o in ipairs(StateFields().orders) do
        if o == order then
            allowed = true
        end
    end
    if not allowed then
        return ReplyError(r, "NOT_AVAILABLE", Data.OrderLabel(order) .. " orders are not available yet.")
    end
    local zone
    if order == "LEVEL_ZONE" then
        zone = ZONE_BY_ID[Codec.Int(f.zone, 0)]
        if not zone or not ZoneAllowedForFaction(zone) then
            return ReplyError(r, "ZONE_INVALID", "Pick a zone your faction can level in.")
        end
    end
    -- A farm order (docs/orders.md "Farm orders"): zone = the item; it must be something creatures drop.
    local farmItem
    if order == "FARM" then
        for _, item in ipairs(Mock.FARM_ITEMS) do
            if item.item == Codec.Int(f.zone, 0) then
                farmItem = item
            end
        end
        if not farmItem then
            return ReplyError(r, "FARM_INVALID", "No creature drops that item.")
        end
    end
    -- A dungeon run (docs/orders.md "Dungeon runs"): zone = the dungeon's id.
    local dungeon
    if order == "DUNGEON" then
        for _, d in ipairs(Mock.DUNGEONS) do
            if d.id == Codec.Int(f.zone, 0) and not d.raid and d.run ~= false then
                dungeon = d
            end
        end
        if not dungeon then
            return ReplyError(r, "DUNGEON_INVALID", "That dungeon cannot be run.")
        end
    end
    local lc, cont, nd, gcap = Codec.Int(f.lc), Codec.Int(f.cont), Codec.Bool(f.nd), Codec.Int(f.gcap)
    local wantsLimits = (lc and lc > 0) or (cont and cont >= 0) or nd or (gcap and gcap > 0)
    if order == "AUTO" and wantsLimits and not HasFeature("auto_limits") then
        return ReplyError(r, "LOCKED", "Autopilot limits unlock at Renown level 3.")
    end
    if lc and (lc < 0 or lc > Mock.MAX_LEVEL) then
        return ReplyError(r, "BAD_REQUEST", "Level cap must be between 0 and 80.")
    end
    if cont and cont >= 0 and Data.ContinentName(cont) == ("Map " .. cont) then
        return ReplyError(r, "BAD_REQUEST", "Unknown continent.")
    end

    bot.o = order
    local note
    if order == "LEVEL_ZONE" then
        bot.oz, bot.ozn = zone.id, zone.n
        bot.aa = Codec.Bool(f.aa, bot.aa)
        if bot.zone ~= zone.id then
            note = "Travelling to " .. zone.n
            MoveTo(bot, zone)
        end
    elseif order == "DUNGEON" then
        bot.oz, bot.ozn = dungeon.id, dungeon.n
        bot.druns, bot.drund = math.max(1, math.min(Codec.Int(f.runs, 1), 10)), 0
        note = "On the way to " .. dungeon.n
    elseif order == "FARM" then
        bot.oz, bot.ozn = farmItem.item, farmItem.n
        -- The target stock (0 = none) and what the guild storage holds of the item.
        bot.ofn = math.max(Codec.Int(f.n, 0), 0)
        bot.ofs = Mock.FARM_STOCK[farmItem.item] or 0
        -- Only creatures far enough below the bot: with none the leader is told (an event), and the bot waits.
        if (bot.lvl or 1) < farmItem.lv + Mock.FARM_LEVELS_BELOW then
            AddEvent("FARM", bot.g, (bot.n or "Bot") .. " cannot farm " .. farmItem.n .. ": every creature that drops it is above level "
                .. math.max((bot.lvl or 1) - Mock.FARM_LEVELS_BELOW, 0) .. ".", { lvl = bot.lvl })
        end
    else
        bot.oz, bot.ozn = 0, ""
    end
    if order ~= "FARM" then
        bot.ofn, bot.ofs = 0, 0
    end
    if order ~= "DUNGEON" then
        bot.druns, bot.drund = 0, 0
    end
    if order == "AUTO" and HasFeature("auto_limits") then
        bot.lc = lc or bot.lc
        bot.cont = cont or bot.cont
        if f.nd ~= nil then
            bot.nd = nd
        end
        bot.gcap = gcap or bot.gcap
    end
    if order == "AUTO" then
        local best = BestZone(bot)
        local current = ZoneOf(bot)
        if best and (not current or bot.lvl > current.max or bot.lvl < current.min
            or (bot.cont >= 0 and current.cont ~= bot.cont)) then
            note = "Autopilot: heading to " .. best.n .. " (best zone for level " .. bot.lvl .. ")"
            MoveTo(bot, best)
        end
    elseif order == "IDLE" then
        local city = CITIES[S.faction]
        bot.zone, bot.zn, bot.map = city.id, city.n, city.map
    elseif order == "FOLLOW" then
        local zoneText = GetRealZoneText()
        if zoneText and zoneText ~= "" then
            bot.zone, bot.zn = 0, zoneText
        end
    end
    UpdateWhy(bot, note)
    local detail = zone and (" (" .. zone.n .. ")") or (farmItem and (" (" .. farmItem.n .. ")")) or ""
    AddEvent("ORDER", bot.g, bot.n .. ": new order " .. Data.OrderLabel(order) .. detail .. ".")
    MarkBot(bot)
    FlushDirty()
    Ack(r)
end

INTENTS.BOT_STATE = function(f)
    local r = Codec.Int(f.r)
    local bot = S.bots[Codec.Int(f.g, 0)]
    if not bot then
        return ReplyError(r, "UNKNOWN_BOT", "Unknown bot.")
    end
    if bot.s == "controlled" then
        return ReplyError(r, "BAD_REQUEST", "Return to your leader before benching " .. bot.n .. ".")
    end
    if f.s == "benched" then
        bot.s = "benched"
        bot.combat = false
        AddEvent("LOGOUT", bot.g, bot.n .. " was benched.")
    elseif f.s == "active" then
        bot.s = "active"
        AddEvent("LOGIN", bot.g, bot.n .. " is back in the world.")
    else
        return ReplyError(r, "BAD_REQUEST", "Unknown bot state.")
    end
    UpdateWhy(bot)
    MarkBot(bot)
    dirtyState = true
    FlushDirty()
    Ack(r)
end

local VALID_CAPS = { [0] = true, [75] = true, [150] = true, [225] = true, [300] = true, [375] = true, [450] = true }

INTENTS.PROF_SET = function(f)
    local r = Codec.Int(f.r)
    local bot = S.bots[Codec.Int(f.g, 0)]
    if not bot then
        return ReplyError(r, "UNKNOWN_BOT", "Unknown bot.")
    end
    if not HasFeature("professions") then
        return ReplyError(r, "LOCKED", "Profession plans unlock at Renown level 2.")
    end
    local p1, p2 = Codec.Int(f.p1, 0), Codec.Int(f.p2, 0)
    local t1, t2 = Codec.Int(f.t1, 0), Codec.Int(f.t2, 0)
    local function IsPrimary(id)
        return id == 0 or (Data.PROFESSIONS[id] and Data.PROFESSIONS[id].primary)
    end
    if not IsPrimary(p1) or not IsPrimary(p2) or (p1 ~= 0 and p1 == p2) then
        return ReplyError(r, "BAD_REQUEST", "Pick two different primary professions.")
    end
    if not VALID_CAPS[t1] or not VALID_CAPS[t2] then
        return ReplyError(r, "BAD_REQUEST", "Invalid target skill.")
    end
    local sec = Codec.IntList(f.sec)
    for _, id in ipairs(sec) do
        if not Data.PROFESSIONS[id] or Data.PROFESSIONS[id].primary then
            return ReplyError(r, "BAD_REQUEST", "Invalid secondary profession.")
        end
    end
    -- drop skills that are no longer planned (a real server would unlearn them)
    local keep = { [p1] = true, [p2] = true }
    for _, id in ipairs(sec) do
        keep[id] = true
    end
    local sk = {}
    for _, entry in ipairs(bot.sk) do
        if keep[entry[1]] then
            sk[#sk + 1] = entry
        end
    end
    bot.sk = sk
    for _, id in ipairs(sec) do
        if not FindSkill(bot, id) then
            bot.sk[#bot.sk + 1] = { id, 1, 75 }
        end
    end
    bot.p1, bot.p2, bot.t1, bot.t2, bot.sec = p1, p2, t1, t2, sec
    local names = {}
    for _, id in ipairs({ p1, p2 }) do
        if id ~= 0 then
            names[#names + 1] = Data.ProfessionName(id)
        end
    end
    AddEvent("ORDER", bot.g, bot.n .. ": profession plan " .. (#names > 0 and table.concat(names, " + ") or "cleared") .. ".")
    MarkBot(bot)
    FlushDirty()
    Ack(r)
end

INTENTS.SPEC_SET = function(f)
    local r = Codec.Int(f.r)
    local bot = S.bots[Codec.Int(f.g, 0)]
    local t = Codec.Int(f.t, -1)
    if not bot then
        return ReplyError(r, "UNKNOWN_BOT", "Unknown bot.")
    end
    local name
    for _, spec in ipairs(MockSpecs()) do
        if spec.class == bot.class and ((t == 0 and spec.d) or spec.id == t) then
            name = spec.n
        end
    end
    if t == 1 then
        name = "Manual"
    end
    if not name then
        return ReplyError(r, "SPEC_INVALID", "Not assigned: no such template for this class.")
    end
    bot.spt, bot.spec = t, name
    MarkBot(bot)
    FlushDirty()
    Ack(r)
end

-- Dungeon roles (docs/dungeon-roles.md): a role the class's dungeon template has; the demo store holds two pieces.
INTENTS.ROLE_SET = function(f)
    local r = Codec.Int(f.r)
    local bot = S.bots[Codec.Int(f.g, 0)]
    local role = f.role or "none"
    if not bot then
        return ReplyError(r, "NOT_OWNER", "That character is not one of your guild bots.")
    end
    local can = MOCK_ROLE_TOKENS[MOCK_SPEC_ROLES[bot.class] or 0]
    if role ~= "none" and role ~= can then
        return ReplyError(r, "ROLE_INVALID", "Not set: its class cannot be that.")
    end
    bot.dr = role
    bot.drt = role ~= "none" and Codec.Int(f.t, 0) or 0
    bot.drn = role ~= "none" and ("Dungeon and raid: " .. MOCK_SPEC_NAMES[bot.class][2]) or ""
    bot.drp = role ~= "none" and 2 or 0
    MarkBot(bot)
    FlushDirty()
    Ack(r)
end

-- What a guild character does now and next (GET what=plan): the control bar's Details window.
SendPlan = function(g)
    local bot = S.bots[g]
    local why = bot and bot.why or (g == Mock.LEADER_GUID and "Questing in its zone") or "Thinking about its next step."
    local lines = {
        { k = "now", t = why },
        { k = "task", t = "Objective: \"Wolves Across the Border\", Young Wolf, 42 yd away, for 1:12" },
        -- a squad member: what its leader does and why it waits (the owner: [quote left out])
        { k = "wait", t = "Wartank: waiting for its squad (Priestyo rests)" },
        { k = "hand", t = "Hand in \"Kobold Camp Cleanup\" (level 4)" },
        { k = "quest", t = "\"Wolves Across the Border\" (level 4): 5/8 - now" },
        { k = "quest", t = "\"Brotherhood of Thieves\" (level 7): 0/12 - waits until level 7 or 12:00" },
        { k = "upkeep", t = "Bags 12 free, gear at 87%, quest log 22 free, money 1g 20s" },
    }
    Reply("PLAN_BEGIN", { g = g, n = #lines, at = time() })
    for _, line in ipairs(lines) do
        Reply("PL", line)
    end
    Reply("PLAN_END", {})
end

SendRoleGear = function(g)
    local bot = S.bots[g]
    if not bot then
        return
    end
    local list = {}
    if bot.dr and bot.dr ~= "none" then
        list = {
            { g = g, id = 1, k = 1, s = 255, e = 2392, rp = 0, n = "Light Mail Belt", q = 1, il = 10, it = 6 },
            { g = g, id = 2, k = 1, s = 255, e = 2129, rp = 0, n = "Large Round Shield", q = 1, il = 10, it = 14 },
        }
    end
    Reply("ROLEGEAR_BEGIN", { g = g, n = #list })
    for _, piece in ipairs(list) do
        Reply("RG", piece)
    end
    Reply("ROLEGEAR_END", {})
end

-- Fight profiles (docs/protocol.md "Fight profiles"): the guild's profiles by id, the template of a class (two rules a
-- list, the bot's talent tree is the first of its class), the bots' own option values.
local MOCK_FIGHT_TEMPLATE = {
    { l = 0, k = 0, ref = 0, a = "shoot", tg = "", tgt = 0, rel = "10.0", en = true, fl = 0, iv = 0, c = "", d = "",
        src = "ranged" },
    { l = 0, k = 0, ref = 0, a = "melee", tg = "enemy is close", tgt = 0, rel = "21.0", en = true, fl = 0, iv = 0, c = "",
        d = "", src = "melee" },
    { l = 1, k = 0, ref = 0, a = "battle shout", tg = "battle shout", tgt = 1, rel = "16.0", en = true, fl = 0, iv = 0,
        c = "", d = "", src = "buff" },
}

local function FightState()
    if not S.fight then
        -- A template imported from wowbot335 (FP-4): read-only, of no guild.
        S.fight = { profiles = {}, nextId = 1, own = {} }
        S.fight.profiles[1000001] = {
            id = 1000001, cls = 1, tab = 0, rev = 1, tpl = true, tr = 0, n = "Warrior Solo Arms (wowbot335)",
            note = "Imported from wowbot335 profiles/fightclass/warrior_solo_arms.json.",
            rules = {
                { l = 0, k = 2, ref = 772, a = "", tg = "", tgt = 0, rel = "13.0", en = true, fl = 0, iv = 0, c = "!tdebuff:772",
                    d = "", src = "" },
                { l = 1, k = 2, ref = 6673, a = "", tg = "", tgt = 1, rel = "5.0", en = true, fl = 0, iv = 0, c = "!aura:6673",
                    d = "", src = "" },
            },
            options = { { k = "useAoE", n = "Use area spells", kind = 0, v = 1, min = 0, max = 1 } },
        }
    end
    return S.fight
end

local function FightProfileName(id)
    local profile = (id or 0) ~= 0 and FightState().profiles[id]
    return profile and profile.n or nil
end

-- The template a bot without a profile of its own plays (FC12): the mock's bots are all in their first talent tree and
-- solo; Mock.fightNoDefault stands for the bot setting Fight.TemplatesByDefault switched off.
local function DefaultFightTemplate(bot)
    if Mock.fightNoDefault then
        return 0
    end
    for id, profile in pairs(FightState().profiles) do
        if profile.tpl and profile.cls == bot.class and profile.tab == 0 and (profile.tr or 0) == 0 then
            return id
        end
    end
    return 0
end

-- What a bot plays now (its solo slot: the mock has no dungeons for the slot) and why not.
local function UpdateFightPlay(bot)
    local profile = (bot.fp or 0) ~= 0 and FightState().profiles[bot.fp] or nil
    bot.fpn, bot.fpsl, bot.fpw, bot.fpid, bot.fpd = "", 0, "", 0, false
    if not profile then
        local id = DefaultFightTemplate(bot)
        if id > 0 then
            bot.fpn, bot.fpid, bot.fpd = FightState().profiles[id].n, id, true
        end
    elseif profile.tab == 0 then
        bot.fpn, bot.fpid = profile.n, profile.id
    elseif profile then
        bot.fpw = "Its talents are not the tree of \"" .. profile.n .. "\": it plays the default."
    end
end

local function SendFightProfiles(g)
    local bot = S.bots[g]
    if not bot then
        return
    end
    local list = {}
    for id, profile in pairs(FightState().profiles) do
        if profile.cls == bot.class then
            local used = 0
            for _, other in pairs(S.bots) do
                if other.fp == id or other.fpg == id then
                    used = used + 1
                end
            end
            local rc, rn = 0, 0
            for _, rule in ipairs(profile.rules) do
                if rule.l == 1 then
                    rn = rn + 1
                else
                    rc = rc + 1
                end
            end
            list[#list + 1] = { g = g, id = id, n = profile.n, tab = profile.tab, rev = profile.rev, rc = rc, rn = rn,
                used = used, opts = #profile.options, tpl = profile.tpl or nil, tr = profile.tpl and (profile.tr or 0) or nil }
        end
    end
    table.sort(list, function(a, b) return a.id < b.id end)
    UpdateFightPlay(bot)
    MarkBot(bot)
    local default = DefaultFightTemplate(bot)
    Reply("FPROFILES_BEGIN", { g = g, cls = bot.class, tab = 0, solo = bot.fp or 0, grp = bot.fpg or 0, max = 20, n = #list,
        dflt = default, dfltg = default })
    for _, item in ipairs(list) do
        Reply("FPROF", item)
    end
    Reply("FPROFILES_END", {})
end

local function SendFightProfile(g, id)
    local bot = S.bots[g]
    if not bot then
        return
    end
    local rules, options, begin = {}, {}, { g = g, id = id, live = bot.s ~= "benched" }
    if id == 0 then
        begin.pn, begin.tab, begin.rep, begin.hid = "Default (mod-playerbots)", 0, "melee, ranged, buff", 4
        if begin.live then
            rules = MOCK_FIGHT_TEMPLATE
        end
    else
        local profile = FightState().profiles[id]
        if not profile or profile.cls ~= bot.class then
            return
        end
        begin.pn, begin.tab, begin.rev, begin.note, begin.rep = profile.n, profile.tab, profile.rev, profile.note or "", "warrior"
        begin.tpl = profile.tpl or nil
        rules = profile.rules
        local own = FightState().own[g] and FightState().own[g][id] or {}
        for _, option in ipairs(profile.options) do
            local value = own[option.k]
            options[#options + 1] = { g = g, id = id, k = option.k, n = option.n, kind = option.kind, v = tostring(option.v),
                min = tostring(option.min), max = tostring(option.max), own = value ~= nil, bv = tostring(value or option.v) }
        end
    end
    begin.n = #rules
    Reply("FPROFILE_BEGIN", begin)
    for _, rule in ipairs(rules) do
        local record = { g = g }
        for k, v in pairs(rule) do
            record[k] = v
        end
        Reply("FRULE", record)
    end
    Reply("FPROFILE_END", {})
    Reply("FOPTS_BEGIN", { g = g, id = id, n = #options })
    for _, option in ipairs(options) do
        Reply("FOPT", option)
    end
    Reply("FOPTS_END", {})
end
Mock.SendFightProfiles = SendFightProfiles

-- The editor's catalogue (FP-3): a few conditions of each group, the class spells and usable items of a bot.
local MOCK_FIGHT_CONDS = {
    { k = "hp", n = "My health %", grp = "me", vk = 2, pk = 0, min = "0", max = "100", tg = false },
    { k = "mana", n = "My mana %", grp = "me", vk = 2, pk = 0, min = "0", max = "100", tg = false },
    { k = "aura", n = "I have the aura", grp = "me", vk = 0, pk = 1, min = "0", max = "0", tg = false },
    { k = "thp", n = "Target health %", grp = "target", vk = 2, pk = 0, min = "0", max = "100", tg = true },
    { k = "tdebuff", n = "Target has my debuff", grp = "target", vk = 0, pk = 1, min = "0", max = "0", tg = true },
    { k = "enemies", n = "Enemies within yards", grp = "around", vk = 1, pk = 3, min = "0", max = "40", tg = false },
    { k = "pet", n = "I have a pet", grp = "pet", vk = 0, pk = 0, min = "0", max = "0", tg = false },
    { k = "ingroup", n = "I am in a group", grp = "group", vk = 0, pk = 0, min = "0", max = "0", tg = false },
    { k = "opt", n = "Option is on", grp = "other", vk = 0, pk = 6, min = "0", max = "0", tg = false },
}
local MOCK_FIGHT_SPELLS = {
    { id = 78, hi = 284, n = "Heroic Strike", tab = "Arms" }, { id = 772, hi = 6546, n = "Rend", tab = "Arms" },
    { id = 6343, hi = 6343, n = "Thunder Clap", tab = "Arms" }, { id = 6673, hi = 5242, n = "Battle Shout", tab = "Fury" },
}
local MOCK_FIGHT_TRIGGERS = {
    { n = "enemy is close", t = "an enemy stands close to it" }, { n = "battle shout", t = "it lacks Battle Shout" },
    { n = "medium aoe", t = "3 or more enemies within 8 yd of its target" },
}
local MOCK_FIGHT_ITEMS = { { e = 118, n = "Minor Healing Potion" } }

local function SendFightCatalog(g)
    local bot = S.bots[g]
    if not bot then
        return
    end
    Reply("FCONDS_BEGIN", { n = #MOCK_FIGHT_CONDS })
    for _, cond in ipairs(MOCK_FIGHT_CONDS) do
        Reply("FCOND", cond)
    end
    Reply("FCONDS_END", {})
    local live = bot.s ~= "benched"
    local spells = live and MOCK_FIGHT_SPELLS or {}
    Reply("FSPELLS_BEGIN", { g = g, live = live, n = #spells })
    for _, spell in ipairs(spells) do
        Reply("FSP", spell)
    end
    Reply("FSPELLS_END", {})
    local items = live and MOCK_FIGHT_ITEMS or {}
    Reply("FITEMS_BEGIN", { g = g, n = #items })
    for _, item in ipairs(items) do
        Reply("FIT", item)
    end
    Reply("FITEMS_END", {})
    Reply("FTRIGS_BEGIN", { g = g, n = #MOCK_FIGHT_TRIGGERS })
    for _, trigger in ipairs(MOCK_FIGHT_TRIGGERS) do
        Reply("FTRIG", trigger)
    end
    Reply("FTRIGS_END", {})
end
SendFightProfilesLater, SendFightProfileLater, SendFightCatalogLater = SendFightProfiles, SendFightProfile, SendFightCatalog

local function SplitTabs(text)
    local parts, start = {}, 1
    text = text or ""
    while true do
        local tab = string.find(text, "\t", start, true)
        if not tab then
            parts[#parts + 1] = string.sub(text, start)
            return parts
        end
        parts[#parts + 1] = string.sub(text, start, tab - 1)
        start = tab + 1
    end
end

local function MockConditionKnown(key)
    for _, cond in ipairs(MOCK_FIGHT_CONDS) do
        if cond.k == key then
            return true
        end
    end
    return false
end

-- FIGHT_SAVE: the editor's whole profile; the mock checks what the server checks first (counts, fields, condition keys,
-- options named in conditions, a spell or item for those kinds) and names the rule ("rule i", list 0 first).
INTENTS.FIGHT_SAVE = function(f)
    local r = Codec.Int(f.r)
    local g, id = Codec.Int(f.g, 0), Codec.Int(f.id, 0)
    local bot = S.bots[g]
    local state = FightState()
    local old = state.profiles[id]
    if bot and old and old.tpl then
        return ReplyError(r, "FIGHT_INVALID", "Not saved: a template is read-only; make a copy of it to edit it.")
    end
    if not bot or not old or old.cls ~= bot.class then
        return ReplyError(r, "FIGHT_INVALID", "Not a bot and a fight profile of your guild for its class.")
    end
    local oc, rc = Codec.Int(f.oc, 0), Codec.Int(f.rc, 0)
    if oc < 0 or oc > 12 or rc < 0 or rc > 60 then
        return ReplyError(r, "FIGHT_INVALID", "Not saved: at most 60 rules and 12 options.")
    end
    local options, keys = {}, {}
    for i = 1, oc do
        local p = SplitTabs(f["o" .. i])
        if #p ~= 6 or not string.match(p[1], "^[%w_]+$") then
            return ReplyError(r, "FIGHT_INVALID", "Not saved: option " .. i .. ": an option is: key, label, kind, value, min, max.")
        end
        options[i] = { k = p[1], n = p[2], kind = tonumber(p[3]) or 1, v = tonumber(p[4]) or 0, min = tonumber(p[5]) or 0,
            max = tonumber(p[6]) or 0 }
        keys[p[1]] = true
    end
    local rules = {}
    for i = 1, rc do
        local p = SplitTabs(f["r" .. i])
        local index = i - 1
        if #p ~= 13 then
            return ReplyError(r, "FIGHT_INVALID", "Not saved: rule " .. index .. ": a rule is: list, kind, ref, action, trigger,"
                .. " target, relevance, on, flags, interval, node, source, conditions.")
        end
        local kind, ref = tonumber(p[2]) or 0, tonumber(p[3]) or 0
        if kind >= 2 and ref <= 0 then
            return ReplyError(r, "FIGHT_INVALID", "Not saved: rule " .. index .. ": a spell, item or trinket rule needs its spell or item.")
        end
        for part in string.gmatch(p[13], "[^;]+") do
            local key = string.match(part, "^!?([%w_]+)")
            local option = string.match(part, "%$([%w_]+)$")
            if not MockConditionKnown(key) then
                return ReplyError(r, "FIGHT_INVALID", "Not saved: rule " .. index .. ": no condition \"" .. tostring(key) .. "\".")
            elseif option and not keys[option] then
                return ReplyError(r, "FIGHT_INVALID", "Not saved: rule " .. index .. ": the profile has no option \"" .. option .. "\".")
            end
        end
        rules[i] = { l = tonumber(p[1]) or 0, k = kind, ref = kind <= 1 and 0 or ref, a = p[4], tg = p[5],
            tgt = tonumber(p[6]) or 0, rel = p[7], en = p[8] == "1", fl = tonumber(p[9]) or 0, iv = tonumber(p[10]) or 0,
            nd = tonumber(p[11]) or 0, src = p[12], c = p[13], d = "" }
    end
    old.n = (f.n and f.n ~= "") and f.n or old.n
    old.note = f.note or ""
    old.rules, old.options = rules, options
    old.rev = (old.rev or 1) + 1
    SendFightProfiles(g)
    SendFightProfile(g, id)
    Ack(r)
end

INTENTS.FIGHT_DEL = function(f)
    local r = Codec.Int(f.r)
    local g, id = Codec.Int(f.g, 0), Codec.Int(f.id, 0)
    local state = FightState()
    if not S.bots[g] or id <= 0 or not state.profiles[id] or state.profiles[id].tpl then
        return ReplyError(r, "FIGHT_INVALID", "Not deleted: no fight profile of your guild with that id.")
    end
    state.profiles[id] = nil
    for guid, other in pairs(S.bots) do
        if other.fp == id then
            other.fp = 0
        end
        if other.fpg == id then
            other.fpg = 0
        end
        if state.own[guid] then
            state.own[guid][id] = nil
        end
        UpdateFightPlay(other)
    end
    SendFightProfiles(g)
    Ack(r)
end

INTENTS.FIGHT_COPY = function(f)
    local r = Codec.Int(f.r)
    local g = Codec.Int(f.g, 0)
    local bot = S.bots[g]
    if not bot then
        return ReplyError(r, "NOT_OWNER", "That character is not one of your guild bots.")
    end
    if bot.s == "benched" then
        return ReplyError(r, "FIGHT_INVALID", "The bot must be in the world: its template is read from it.")
    end
    local state = FightState()
    local from = Codec.Int(f.from, 0)
    local source = from > 0 and state.profiles[from] or nil
    if from > 0 and (not source or source.cls ~= bot.class) then
        return ReplyError(r, "FIGHT_INVALID", "Not copied: no fight profile of your guild for its class with that id.")
    end
    local id = state.nextId
    state.nextId = id + 1
    if source then
        local copied, options = {}, {}
        for i, rule in ipairs(source.rules) do
            local copy = {}
            for k, v in pairs(rule) do
                copy[k] = v
            end
            copied[i] = copy
        end
        for i, option in ipairs(source.options) do
            options[i] = { k = option.k, n = option.n, kind = option.kind, v = option.v, min = option.min, max = option.max }
        end
        state.profiles[id] = { id = id, cls = source.cls, tab = source.tab, rev = 1, note = source.note, rules = copied,
            options = options, n = (f.n and f.n ~= "") and f.n or (source.n .. " copy") }
        SendFightProfiles(g)
        return Ack(r)
    end
    local rules = {}
    for i, rule in ipairs(MOCK_FIGHT_TEMPLATE) do
        local copy = {}
        for k, v in pairs(rule) do
            copy[k] = v
        end
        copy.src = ""
        rules[i] = copy
    end
    state.profiles[id] = {
        id = id, cls = bot.class, tab = 0, rev = 1, note = "", rules = rules,
        n = (f.n and f.n ~= "") and f.n or ((bot.n or "?") .. " copy"),
        options = { { k = "aoe", n = "Area spells", kind = 0, v = 1, min = 0, max = 1 },
            { k = "hp", n = "Heal below", kind = 2, v = 40, min = 0, max = 100 } },
    }
    SendFightProfiles(g)
    Ack(r)
end

INTENTS.FIGHT_ASSIGN = function(f)
    local r = Codec.Int(f.r)
    local g = Codec.Int(f.g, 0)
    local bot = S.bots[g]
    if not bot then
        return ReplyError(r, "NOT_OWNER", "That character is not one of your guild bots.")
    end
    local state = FightState()
    local to = f.to and Codec.IntList(f.to)
    if to then
        local copied = 0
        for _, target in ipairs(to) do
            local other = S.bots[target]
            if other and other ~= bot and other.class == bot.class then
                other.fp, other.fpg = bot.fp or 0, bot.fpg or 0
                state.own[target] = {}
                for id, values in pairs(state.own[g] or {}) do
                    state.own[target][id] = {}
                    for k, v in pairs(values) do
                        state.own[target][id][k] = v
                    end
                end
                UpdateFightPlay(other)
                MarkBot(other)
                copied = copied + 1
            end
        end
        if copied == 0 then
            return ReplyError(r, "FIGHT_INVALID", "No guild bot of its class to copy to.")
        end
        FlushDirty()
        SendFightProfiles(g)
        Ack(r)
        return
    end
    local slot, id = Codec.Int(f.slot, -1), Codec.Int(f.id, -1)
    if (slot ~= 0 and slot ~= 1) or id < 0 then
        return ReplyError(r, "BAD_REQUEST", "A slot and a profile (0 = the default) are needed.")
    end
    local profile = id ~= 0 and state.profiles[id] or nil
    if id ~= 0 and (not profile or profile.cls ~= bot.class) then
        return ReplyError(r, "FIGHT_INVALID", "Not assigned: no fight profile of its class with that id.")
    end
    if slot == 1 then
        bot.fpg = id
    else
        bot.fp = id
    end
    UpdateFightPlay(bot)
    MarkBot(bot)
    FlushDirty()
    SendFightProfiles(g)
    Ack(r)
end

INTENTS.FIGHT_OPTION = function(f)
    local r = Codec.Int(f.r)
    local g = Codec.Int(f.g, 0)
    local id = Codec.Int(f.id, 0)
    local state = FightState()
    local profile = state.profiles[id]
    if not S.bots[g] or not profile then
        return ReplyError(r, "FIGHT_INVALID", "Not a bot and a fight profile of your guild.")
    end
    local option
    for _, o in ipairs(profile.options) do
        if o.k == f.k then
            option = o
        end
    end
    if not option then
        return ReplyError(r, "FIGHT_INVALID", "Not set: the profile has no option \"" .. tostring(f.k) .. "\".")
    end
    state.own[g] = state.own[g] or {}
    state.own[g][id] = state.own[g][id] or {}
    if Codec.Bool(f.def) then
        state.own[g][id][option.k] = nil
    else
        local value = tonumber(f.v or "")
        if not value or value < option.min or value > option.max then
            return ReplyError(r, "FIGHT_INVALID", "Not set: the value is outside " .. option.min .. " to " .. option.max .. ".")
        end
        state.own[g][id][option.k] = value
    end
    SendFightProfile(g, id)
    Ack(r)
end
Mock.FightProfileName = FightProfileName

INTENTS.SQUAD_SET = function(f)
    local r = Codec.Int(f.r)
    local bot = S.bots[Codec.Int(f.g, 0)]
    local l = Codec.Int(f.l, 0)
    if not bot then
        return ReplyError(r, "NOT_OWNER", "That character is not one of your guild bots.")
    end
    local leader = l ~= 0 and S.bots[l] or nil
    if l ~= 0 and (not leader or leader == bot or (leader.sq or 0) ~= 0) then
        return ReplyError(r, "SQUAD_INVALID", "Not changed: that bot cannot lead a squad.")
    end
    local old = (bot.sq or 0) ~= 0 and S.bots[bot.sq] or nil
    bot.sq = l
    bot.sqs = leader and "active" or "none"
    for _, b in pairs(S.bots) do
        local count = 0
        for _, m in pairs(S.bots) do
            if m.sq == b.g then
                count = count + 1
            end
        end
        b.sqm = count
        if count > 0 and (b.sq or 0) == 0 then
            b.sqs = "leads"
        elseif (b.sq or 0) == 0 then
            b.sqs = "none"
        end
    end
    MarkBot(bot)
    if leader then
        MarkBot(leader)
    end
    if old then
        MarkBot(old)
    end
    FlushDirty()
    Ack(r)
end

INTENTS.SPEC_SAVE = function(f)
    local r = Codec.Int(f.r)
    local name = f.n or ""
    if name == "" then
        return ReplyError(r, "BAD_REQUEST", "A template needs a name.")
    end
    local bot = S.bots[S.controlled]
    S.specs = S.specs or {}
    S.specs[#S.specs + 1] = { id = 100000 + #S.specs, class = bot and bot.class or (S.leader.class or 1), n = name,
        p = Codec.Int(f.p, 0), ro = 0, t = 0, d = false, own = true, sum = "31/20/0" }
    SendSpecs()
    Ack(r)
end

INTENTS.SPEC_DEL = function(f)
    local r = Codec.Int(f.r)
    local t = Codec.Int(f.t, 0)
    for i, spec in ipairs(S.specs or {}) do
        if spec.id == t then
            table.remove(S.specs, i)
            for _, bot in pairs(S.bots) do
                if bot.spt == t then
                    bot.spt, bot.spec = 0, ""
                    MarkBot(bot)
                end
            end
            SendSpecs()
            FlushDirty()
            return Ack(r)
        end
    end
    ReplyError(r, "SPEC_INVALID", "Not deleted: not a template of this guild.")
end

INTENTS.RECRUIT = function(f)
    local r = Codec.Int(f.r)
    if not HasFeature("recruit") then
        return ReplyError(r, "LOCKED", "Recruitment is locked.")
    end
    if BotCount() >= OwnedValue("slots") then
        return ReplyError(r, "NO_SLOT", "All bot slots are in use. Unlock more on the Progression tab.")
    end
    local race, class, gender = Codec.Int(f.race, 0), Codec.Int(f.class, 0), Codec.Int(f.gender, 0)
    local raceInfo = Data.RACES[race]
    if not raceInfo or raceInfo.faction ~= S.faction or not Data.IsValidCombo(race, class)
        or (gender ~= 0 and gender ~= 1) then
        return ReplyError(r, "INVALID_COMBO", "That race and class combination is not possible for your faction.")
    end
    if class == Data.DEATH_KNIGHT and not DkUnlocked() then
        return ReplyError(r, "LOCKED", "Death Knights unlock once a guild member reaches level 55.")
    end
    local name = ValidName(f.name)
    if not name then
        return ReplyError(r, "NAME_INVALID", "Names need 2 to 12 letters and no spaces, digits or symbols.")
    end
    if NameTaken(name) then
        return ReplyError(r, "NAME_TAKEN", "The name " .. name .. " is already taken.")
    end
    local isDk = class == Data.DEATH_KNIGHT
    local bot = NewBot(S.nextGuid, FreeSlotNumber(), name, race, class, gender, isDk and Data.DK_START_LEVEL or 1)
    S.nextGuid = S.nextGuid + 1
    if isDk then
        bot.zone, bot.zn, bot.map = DK_START.id, DK_START.n, DK_START.map
    else
        local zone = ZONE_BY_ID[raceInfo.startZone]
        bot.zone, bot.zn, bot.map = zone.id, zone.n, zone.cont
    end
    UpdateWhy(bot, "Just recruited: getting to know " .. bot.zn)
    S.bots[bot.g] = bot
    AddEvent("RECRUIT", bot.g, name .. " (" .. raceInfo.name .. " " .. Data.ClassName(class) .. ") joined the guild.")
    MarkBot(bot)
    dirtyState = true
    FlushDirty()
    Ack(r)
end

local SWAP_DELAY = 0.6

-- docs/protocol.md §9: busy CONTROL_STATE and ACK when the swap starts, the result later.
local function FinishSwap(target)
    S.watching = false
    local previous = S.bots[S.controlled]
    if previous then
        previous.s = "active"
        UpdateWhy(previous)
        MarkBot(previous)
    end
    S.controlled = target and target.g or Mock.LEADER_GUID
    if target then
        target.s = "controlled"
        target.combat = false
        UpdateWhy(target)
        MarkBot(target)
        AddEvent("CONTROL", target.g, "You are now playing " .. target.n .. ".")
    else
        AddEvent("CONTROL", 0, "You are back on " .. S.leader.n .. ".")
    end
    FlushDirty()
    Reply("CONTROL_STATE", ControlFields(false))
end

INTENTS.CONTROL_TAKE = function(f)
    local r = Codec.Int(f.r)
    local bot = S.bots[Codec.Int(f.g, 0)]
    if not bot then
        return ReplyError(r, "UNKNOWN_BOT", "Unknown bot.")
    end
    if S.swapping then
        return ReplyError(r, "BUSY", "A swap is already in progress.")
    end
    local reason
    if bot.s == "controlled" then
        reason = "SAME"
    elseif bot.s == "benched" then
        reason = "BENCHED"
    elseif bot.s == "offline" then
        reason = "OFFLINE"
    elseif bot.dead then
        reason = "DEAD"
    elseif bot.combat then
        reason = "IN_COMBAT"
    end
    if reason then
        return ReplyError(r, "SWAP_REFUSED", nil, reason)
    end
    S.swapping = true
    Reply("CONTROL_STATE", ControlFields(true))
    Ack(r)
    GA.After(SWAP_DELAY, function()
        S.swapping = false
        FinishSwap(bot)
    end)
end

INTENTS.CONTROL_RETURN = function(f)
    local r = Codec.Int(f.r)
    if S.controlled == Mock.LEADER_GUID then
        return ReplyError(r, "SWAP_REFUSED", nil, "SAME")
    end
    if S.swapping then
        return ReplyError(r, "BUSY", "A swap is already in progress.")
    end
    S.swapping = true
    Reply("CONTROL_STATE", ControlFields(true))
    Ack(r)
    GA.After(SWAP_DELAY, function()
        S.swapping = false
        FinishSwap(nil)
    end)
end

-- Watch mode (docs/protocol.md §9 "Watch mode"): CONTROL_STATE^watch and the bot's BOT record, then the ACK.
INTENTS.CONTROL_WATCH = function(f)
    local r = Codec.Int(f.r)
    local on = Codec.Bool(f.on)
    if on == nil then
        return ReplyError(r, "BAD_REQUEST", "CONTROL_WATCH needs on.")
    end
    local bot = S.bots[S.controlled]
    local leader = S.controlled == Mock.LEADER_GUID
    if on and S.watching ~= true then
        if leader and (S.leader.o or "HOLD") == "HOLD" then
            return ReplyError(r, "WATCH_REFUSED", nil, "NO_ORDER")
        end
        if not bot and not leader then
            return ReplyError(r, "WATCH_REFUSED", nil, "NOT_GUILD_BOT")
        end
        if S.swapping then
            return ReplyError(r, "WATCH_REFUSED", nil, "BUSY")
        end
    end
    S.watching = on and (bot ~= nil or leader)
    if bot then
        UpdateWhy(bot)
        MarkBot(bot)
        FlushDirty()
    end
    Reply("CONTROL_STATE", ControlFields(false))
    if leader then
        Reply("LEADER", LeaderFields())
    end
    Ack(r)
end

INTENTS.UNLOCK_BUY = function(f)
    local r = Codec.Int(f.r)
    local u = UNLOCK_BY_ID[Codec.Int(f.id, 0)]
    if not u then
        return ReplyError(r, "BAD_REQUEST", "Unknown unlock.")
    end
    local status = UnlockStatus(u)
    if status == "owned" then
        return ReplyError(r, "BAD_REQUEST", "Already unlocked.")
    elseif status == "covered" then
        return ReplyError(r, "COVERED", "Your guild has that many already: " .. u.desc .. " would add nothing.")
    elseif status == "locked" then
        return ReplyError(r, "LOCKED", "Reach Renown level " .. u.lvl .. " first.")
    end
    if S.bank < u.gold then
        return ReplyError(r, "NOT_ENOUGH_GOLD", "The guild bank needs " .. math.floor(u.gold / 10000) .. "g.")
    end
    S.bank = S.bank - u.gold
    S.owned[u.id] = true
    AddEvent("UNLOCK", 0, "Bought: " .. u.desc .. " (" .. math.floor(u.gold / 10000) .. "g from the guild bank).")
    SendUnlocks()
    dirtyState = true
    FlushDirty()
    Ack(r)
end

-- Bot behaviour settings: g = 0 the guild, else a bot or the leader; v = id:value pairs, del = ids, all = drop all.
INTENTS.SETTING_SET = function(f)
    local r = Codec.Int(f.r)
    local g = Codec.Int(f.g, 0)
    if g ~= 0 and g ~= Mock.LEADER_GUID and not S.bots[g] then
        return ReplyError(r, "SETTING_INVALID", "That character is not in your guild.", "NOT_MEMBER")
    end
    local sets = Codec.Tuples(f.v or "")
    local clears = Codec.IntList(f.del or "")
    for _, pair in ipairs(sets) do
        local def = MockSettingDef(pair[1])
        local value = tonumber(pair[2])
        if not def then
            return ReplyError(r, "SETTING_INVALID", "There is no such setting.", "UNKNOWN")
        end
        if g ~= 0 and not def.pc then
            return ReplyError(r, "SETTING_INVALID", "\"" .. def.n .. "\" is set for the whole guild only.", "GUILD_ONLY")
        end
        if not value or value < def.min or value > def.max then
            return ReplyError(r, "SETTING_INVALID", "\"" .. def.n .. "\" is out of range.", "RANGE")
        end
    end
    for _, id in ipairs(clears) do
        if not MockSettingDef(id) then
            return ReplyError(r, "SETTING_INVALID", "There is no such setting.", "UNKNOWN")
        end
    end
    local scope = SettingScope(g)
    if f.all == "1" then
        for id in pairs(scope) do
            scope[id] = nil
        end
    end
    for _, id in ipairs(clears) do
        scope[tostring(id)] = nil
    end
    for _, pair in ipairs(sets) do
        scope[tostring(pair[1])] = tonumber(pair[2])
    end
    Reply("SETVAL", SettingValFields(g))
    Ack(r)
end

INTENTS.AWAY_ACK = function()
    S.away = nil
end

function Mock.HandleMessage(encoded)
    if not S then
        return
    end
    local op, fields = Codec.Decode(encoded)
    local handler = INTENTS[op]
    if handler then
        handler(fields)
    else
        GA.Debug("mock: ignored " .. tostring(op))
    end
end

local reassembler = Transport.NewReassembler("C", Mock.HandleMessage)

function Mock.ReceiveChunk(chunk)
    if enabled then
        reassembler:Feed(chunk, GetTime())
    end
end

-------------------------------------------------------------------------------
-- Offline window simulation
-------------------------------------------------------------------------------

function Mock.SimulateAway()
    local minutes = OwnedValue("offline")
    local before = {}
    for g, bot in pairs(S.bots) do
        before[g] = { lvl = bot.lvl, zn = bot.zn, gold = bot.gold, quests = bot.quests, deaths = bot.deaths }
    end
    local renownBefore = S.renown.total
    silent = true
    S.awayLog = {}
    local ticks = math.max(10, math.floor(minutes / 2))
    for _ = 1, ticks do
        for _, g in ipairs(SortedGuids()) do
            TickBot(S.bots[g])
        end
    end
    S.awayLog = nil
    dirtyBots = {}
    dirtyState = false
    silent = false
    local away = { from = time() - minutes * 60, to = time(), lv = 0, rp = S.renown.total - renownBefore, gold = 0, q = 0, d = 0, lines = {} }
    for _, g in ipairs(SortedGuids()) do
        local bot, b = S.bots[g], before[g]
        if b then
            away.lv = away.lv + (bot.lvl - b.lvl)
            away.gold = away.gold + (bot.gold - b.gold)
            away.q = away.q + (bot.quests - b.quests)
            away.d = away.d + (bot.deaths - b.deaths)
            local line = bot.n .. ": level " .. b.lvl .. " -> " .. bot.lvl
            if b.zn ~= bot.zn then
                line = line .. ", " .. b.zn .. " -> " .. bot.zn
            end
            if bot.s == "benched" then
                line = bot.n .. ": benched, did not play"
            end
            away.lines[#away.lines + 1] = { g = g, msg = line }
        end
    end
    S.away = away
    Sync(Mock.ZONE_VERSION)
end

-------------------------------------------------------------------------------
-- Control
-------------------------------------------------------------------------------

local function LoadWorld()
    local db = GA.db.mock
    local factionToken = UnitFactionGroup("player")
    local faction = factionToken == "Horde" and "H" or "A"
    if not db.state or db.state.schema ~= 1 or db.state.faction ~= faction then
        db.state = NewWorld()
    end
    S = db.state
    S.leader.n = UnitName("player")
    S.swapping = false
    S.watching = false
    -- like a real login: the client starts on the leader, every bot is a bot again
    S.controlled = Mock.LEADER_GUID
    for _, bot in pairs(S.bots) do
        if bot.s == "controlled" then
            bot.s = "active"
        end
    end
    for _, bot in pairs(S.bots) do
        UpdateWhy(bot)
    end
end

function Mock.IsEnabled()
    return enabled
end

-- Enable(quiet): switch the addon to demo mode.
function Mock.Enable(quiet)
    LoadWorld()
    enabled = true
    GA.db.mock.enabled = true
    reassembler:Reset()
    if tickTimer then
        tickTimer:Cancel()
    end
    tickTimer = GA.Every(Mock.TICK, Mock.Tick)
    GA.Protocol.SetMode("mock")
    if not quiet then
        GA.Print("Demo mode on: a simulated server runs inside your client. /ga mock off to leave.")
    end
end

function Mock.Disable()
    enabled = false
    GA.db.mock.enabled = false
    if tickTimer then
        tickTimer:Cancel()
        tickTimer = nil
    end
    GA.Protocol.SetMode("live")
    GA.Print("Demo mode off.")
end

function Mock.Reset()
    GA.db.mock.state = nil
    if enabled then
        Mock.Enable(true)
    end
    GA.Print("Demo guild reset.")
end

function Mock.SlashCommand(rest)
    local cmd, arg = string.match(rest or "", "^(%S*)%s*(.-)$")
    cmd = string.lower(cmd or "")
    if cmd == "on" then
        Mock.Enable()
    elseif cmd == "off" then
        Mock.Disable()
    elseif cmd == "reset" then
        Mock.Reset()
    elseif not enabled then
        GA.Print("Demo mode is off. /ga mock on to start it.")
    elseif cmd == "renown" then
        GainRenown(math.max(0, tonumber(arg) or 100))
        FlushDirty()
    elseif cmd == "away" then
        Mock.SimulateAway()
    elseif cmd == "tick" then
        Mock.Tick()
    else
        GA.Print("Demo mode is on (renown level " .. S.renown.level .. ", " .. BotCount() .. " bots).")
    end
end
