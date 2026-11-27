-- Client-side model of what the server told us. Only GA.Protocol writes it; the UI reads it
-- and listens to the GA.Fire notifications listed below.
--
-- Notifications: CONNECTION, GUILD, CONTROL, ROSTER, BOT(guid), ZONES, UNLOCKS, EVENTS,
-- EVENT(event), AWAY, ERROR(code, msg, reason, r), NOTICE(msg, kind), STORAGE, LOOT, LOOT_LOG(entry or nil),
-- RECIPES, AUCTIONS, FIGHT(guid)

local GA = GuildAdventure
local State = {}
GA.State = State

State.MAX_EVENTS = 100

function State.Reset()
    State.connection = "idle" -- idle | connecting | connected | failed | missing | badversion
    State.server = {}
    State.serverOffset = 0
    State.guild = {}
    State.control = {}
    State.leader = {} -- the guild leader's record (LEADER, docs/protocol.md §7.15)
    State.bots = {}
    State.zones = {}
    State.zoneList = {}
    State.zv = 0
    State.unlocks = {}
    State.perks = {}
    State.milestones = {}
    -- Dungeons and raids by id (docs/protocol.md §7.20): { id, map, n, min, max, tgt, size, raid, gl }
    State.dungeons = {}
    State.dungeonsLoaded = false
    -- The guild's dungeon runs (docs/protocol.md "Dungeon runs"): { g, d, dn, ph, b, of, nb, w, left, done, min, m, why }
    State.dungeonRuns = {}
    -- The preparation step: the server's fight rules { tl, em, cp, cpon, ff } and what the guild storage holds for a
    -- run: { e, n, q, k, amt, lvl }
    State.dungeonPrep = { rules = {}, items = {}, loaded = false }
    -- Craft orders of the guild: { id, item, n, amt, made, note }; what the guild can make: { item, n, il, cls, sub, by }
    State.craftOrders = {}
    State.craftOrdersLoaded = false
    State.craftables = {}
    State.craftablesLoaded = false
    -- The last answer of the farm order's item picker: q = the text asked for, g = the bot, items = { item, n, q, il,
    -- cls, k, lv, need, ok }
    State.farm = { q = "", g = 0, items = {}, loaded = false }
    -- The guild storage's shopping list (docs/storage.md "Shopping list"): items in the server's order { e, n, min, on,
    -- have, price, bc }, today's spending and the budgets (copper); the last answer of its item search.
    State.shopping = { items = {}, day = 0, cap = 0, visit = 0, srv = true, max = 60, loaded = false }
    State.shopSearch = { q = "", items = {}, loaded = false }
    State.events = {}
    State.away = nil
    -- Guild storage: kinds by "entry:randomProperty" -> { e, rp, n, cls, sub }
    State.storage = { items = {}, kinds = 0, total = 0, loaded = false }
    State.loot = { settings = {}, rules = {}, log = {}, loaded = false, logLoaded = false }
    State.recipes = { g = nil, list = {}, busy = false }
    -- Talent templates (docs/specs.md): { id, class, n, p, ro, t, d, own, sum }
    State.specs = {}
    -- Role stores (docs/dungeon-roles.md): bot guid -> { { id, k, s, e, rp, n, q, il, it }, ... }
    State.roleGear = {}
    State.plan = nil -- the control bar's Details window (State.SetPlan)
    -- Fight profiles (docs/fight-profiles.md), per bot guid: lists[g] = { meta = FPROFILES_BEGIN, list = { FPROF } }
    -- (the profiles of its class and its slots), profiles[g][id] = { meta = FPROFILE_BEGIN, rules = { FRULE } }
    -- (id 0: its template), options[g][id] = { FOPT, ... } (with its own values).
    State.fight = { lists = {}, profiles = {}, options = {}, conds = nil, condByKey = {}, spells = {}, items = {}, trig = {} }
    -- Auction house surplus rules by "entry:randomProperty"
    State.auctions = { rules = {}, cats = {}, listed = {}, srv = true, hours = 24, every = 300, loaded = false }
    -- Bot behaviour settings (docs/bot-settings.md): the schema (SETDEFS) and the values (SETVALS / SETVAL).
    -- values[0] = the guild's own values, values[g] = one guild character's own values; each { [id] = value }.
    State.settings = { sv = 0, cats = {}, defs = {}, byId = {}, byKey = {}, values = { [0] = {} }, loaded = false }
end
State.Reset()
State.mode = "live" -- "live" | "mock"

local function Merge(target, fields)
    for k, v in pairs(fields) do
        target[k] = v
    end
end

function State.SetConnection(status)
    if State.connection ~= status then
        State.connection = status
        GA.Fire("CONNECTION", status)
    end
end

function State.IsConnected()
    return State.connection == "connected"
end

function State.ServerNow()
    return time() + (State.serverOffset or 0)
end

-------------------------------------------------------------------------------
-- Guild and control
-------------------------------------------------------------------------------

function State.MergeGuild(fields)
    Merge(State.guild, fields)
    GA.Fire("GUILD")
end

function State.MergeControl(fields)
    Merge(State.control, fields)
    GA.Fire("CONTROL")
end

function State.IsControllingBot()
    local c = State.control
    return c.g ~= nil and c.leader ~= nil and c.g ~= c.leader
end

-- Watch mode: the AI plays the character you control (a bot, or your leader once it has an order), your client
-- watches (docs/take-control.md §5.12).
function State.IsWatching()
    return State.control.watch == true and State.control.g ~= nil
end

function State.MergeLeader(fields)
    Merge(State.leader, fields)
    GA.Fire("LEADER")
end

function State.HasFeature(key)
    local feat = State.guild.feat
    if not feat then
        return false
    end
    for i = 1, #feat do
        if feat[i] == key then
            return true
        end
    end
    return false
end

function State.IsOrderAllowed(order)
    local orders = State.guild.orders
    if not orders then
        return false
    end
    for i = 1, #orders do
        if orders[i] == order then
            return true
        end
    end
    return false
end

function State.FreeSlots()
    local g = State.guild
    return math.max(0, (g.slots or 0) - (g.used or State.BotCount()))
end

-------------------------------------------------------------------------------
-- Roster
-------------------------------------------------------------------------------

function State.UpsertBot(fields, silent)
    local g = fields.g
    if not g then
        return
    end
    local bot = State.bots[g]
    local isNew = bot == nil
    if isNew then
        bot = {}
        State.bots[g] = bot
    end
    Merge(bot, fields)
    if not silent then
        if isNew then
            GA.Fire("ROSTER")
        end
        GA.Fire("BOT", g)
    end
end

function State.RemoveBot(g)
    if State.bots[g] then
        State.bots[g] = nil
        GA.Fire("ROSTER")
    end
end

function State.ReplaceRoster(list)
    State.bots = {}
    for i = 1, #list do
        State.UpsertBot(list[i], true)
    end
    GA.Fire("ROSTER")
end

function State.BotCount()
    local n = 0
    for _ in pairs(State.bots) do
        n = n + 1
    end
    return n
end

function State.GetBot(g)
    return g and State.bots[g]
end

local SORTERS = {
    name = function(b) return string.lower(b.n or "") end,
    lvl = function(b) return b.lvl or 0 end,
    class = function(b) return GA.Data.ClassName(b.class) end,
    zone = function(b) return string.lower(b.zn or "") end,
    order = function(b) return GA.Data.OrderLabel(b.o) end,
    state = function(b) return b.s or "" end,
    xp = function(b)
        if not b.xpmax or b.xpmax == 0 then
            return b.lvl or 0
        end
        return (b.lvl or 0) + (b.xp or 0) / b.xpmax
    end,
    slot = function(b) return b.slot or 0 end,
    -- The roster's columns: gear level, dungeon role (tanks, healers, damage; a role the owner set before one from
    -- the talents), the highest primary profession skill.
    ilvl = function(b) return State.BotGearLevel(b) or -1 end,
    role = function(b)
        local rank = ({ tank = 1, heal = 2 })[State.BotGroupRole(b)] or 3
        return rank * 2 + ((b.dr == "tank" or b.dr == "heal") and 0 or 1)
    end,
    prof = function(b)
        local primary = State.BotProfessions(b)
        return primary[1] and primary[1].value or -1
    end,
}

-- SortedBots(sortKey, descending, filter) -> array of bots. filter(bot) may be nil.
function State.SortedBots(sortKey, descending, filter)
    local list = {}
    for _, bot in pairs(State.bots) do
        if not filter or filter(bot) then
            list[#list + 1] = bot
        end
    end
    local key = SORTERS[sortKey] or SORTERS.slot
    table.sort(list, function(a, b)
        local ka, kb = key(a), key(b)
        if ka == kb then
            return (a.g or 0) < (b.g or 0)
        end
        if descending then
            return ka > kb
        end
        return ka < kb
    end)
    return list
end

-------------------------------------------------------------------------------
-- Zones
-------------------------------------------------------------------------------

local function RebuildZoneList()
    local list = {}
    for _, z in pairs(State.zones) do
        list[#list + 1] = z
    end
    table.sort(list, function(a, b)
        if (a.min or 0) ~= (b.min or 0) then
            return (a.min or 0) < (b.min or 0)
        end
        return (a.n or "") < (b.n or "")
    end)
    State.zoneList = list
end

function State.ReplaceZones(list, zv)
    State.zones = {}
    for i = 1, #list do
        local z = list[i]
        if z.id then
            State.zones[z.id] = z
        end
    end
    State.zv = zv or 0
    RebuildZoneList()
    GA.Fire("ZONES")
end

function State.UpsertZone(fields)
    if not fields.id then
        return
    end
    local z = State.zones[fields.id] or {}
    Merge(z, fields)
    State.zones[fields.id] = z
    RebuildZoneList()
    GA.Fire("ZONES")
end

-- Zone usable by this guild's faction ("C" = contested, open to both).
function State.ZoneMatchesFaction(zone)
    local fac = State.guild.fac
    return zone.fac == "C" or fac == nil or zone.fac == fac
end

-- Difficulty colour like quest colours: grey (out-levelled), green (fits), yellow (slightly
-- high), red (too high).
function State.ZoneFit(zone, level)
    level = level or 1
    if (zone.max or 0) < level then
        return "low"
    elseif (zone.min or 0) > level + 2 then
        return "high"
    elseif (zone.min or 0) > level then
        return "stretch"
    end
    return "fit"
end

function State.ZoneAllowsOrder(zone, order)
    if not zone.orders or #zone.orders == 0 then
        return true
    end
    for i = 1, #zone.orders do
        if zone.orders[i] == order then
            return true
        end
    end
    return false
end

-------------------------------------------------------------------------------
-- Unlocks
-------------------------------------------------------------------------------

local function SortUnlocks()
    table.sort(State.unlocks, function(a, b)
        if (a.lvl or 0) ~= (b.lvl or 0) then
            return (a.lvl or 0) < (b.lvl or 0)
        end
        return (a.id or 0) < (b.id or 0)
    end)
end

function State.ReplaceUnlocks(list)
    State.unlocks = list
    SortUnlocks()
    GA.Fire("UNLOCKS")
end

function State.UpsertUnlock(fields)
    for i = 1, #State.unlocks do
        local u = State.unlocks[i]
        if u.id == fields.id then
            Merge(u, fields)
            GA.Fire("UNLOCKS")
            return
        end
    end
    State.unlocks[#State.unlocks + 1] = fields
    SortUnlocks()
    GA.Fire("UNLOCKS")
end

-------------------------------------------------------------------------------
-- Guild perks (docs/protocol.md §7.15)
-------------------------------------------------------------------------------

-- The guild owns at least one rank of the perk `key`.
function State.HasPerk(key)
    for _, p in ipairs(State.perks) do
        if p.key == key then
            return (p.rank or 0) > 0
        end
    end
    return false
end

function State.ReplacePerks(list)
    table.sort(list, function(a, b)
        return (a.id or 0) < (b.id or 0)
    end)
    State.perks = list
    GA.Fire("PERKS")
end

-------------------------------------------------------------------------------
-- Tiered guild achievements (docs/protocol.md §7.18)
-------------------------------------------------------------------------------

-- The server sends the series in its order (kind, then subject); a series of the guild's other faction stays in the
-- list (it simply cannot be reached), the UI may hide it.
function State.ReplaceMilestones(list)
    State.milestones = list
    GA.Fire("MILESTONES")
end

-------------------------------------------------------------------------------
-- Dungeon readiness (docs/protocol.md §7.20)
-------------------------------------------------------------------------------

-- Craft orders (docs/protocol.md §7.21)
function State.ReplaceCraftOrders(list)
    State.craftOrders = list or {}
    State.craftOrdersLoaded = true
    GA.Fire("CRAFT_ORDERS")
end

function State.ReplaceCraftables(list)
    State.craftables = list or {}
    State.craftablesLoaded = true
    GA.Fire("CRAFT_ORDERS")
end

function State.SetDungeonPrep(meta, list)
    State.dungeonPrep = { rules = meta or {}, items = list or {}, loaded = true }
end

function State.SetDungeonRuns(list)
    State.dungeonRuns = list or {}
end

-- The dungeon run a bot is part of (it leads it or is in its group), nil: none.
function State.BotDungeonRun(g)
    for _, run in ipairs(State.dungeonRuns) do
        if run.g == g then
            return run
        end
        for _, member in ipairs(run.m or {}) do
            if member == g then
                return run
            end
        end
    end
end

-- A bot's readiness for a dungeon (0..100), nil: not known (not in the world, or the dungeon is above its reach).
-- The server lists a bot's readiness only for dungeons up to five levels below it; a bot above a dungeon's levels is
-- ready for it (the server takes it: a guild that unlocks the dungeons in their order runs the first ones with bots
-- that have outgrown them).
function State.BotDungeonReadiness(bot, dungeonId)
    for _, pair in ipairs(bot and bot.rdy or {}) do
        if tonumber(pair[1]) == dungeonId then
            return tonumber(pair[2]) or 0
        end
    end
    -- (Only for a dungeon a run of this guild can be ordered into: not a raid, not one the server cannot run, and
    -- not one that is in no order of the guild, like the other side's capital dungeon on a server that still lists it.)
    local dungeon = State.dungeons and State.dungeons[dungeonId]
    if bot and dungeon and dungeon.run ~= false and not dungeon.raid and (dungeon.ord or 0) > 0 and
        (bot.lvl or 0) > (dungeon.max or 255) then
        return 100
    end
    return nil
end

-- The farm order's item picker (docs/protocol.md §7.23)
function State.ReplaceFarmItems(list, meta)
    State.farm = { q = meta and meta.q or "", g = meta and meta.g or 0, items = list or {}, loaded = true }
    GA.Fire("FARM_ITEMS")
end

-- The guild storage's shopping list (docs/protocol.md section 7.25)
function State.ReplaceShopping(list, meta)
    meta = meta or {}
    State.shopping = { items = list or {}, day = meta.day or 0, cap = meta.cap or 0, visit = meta.visit or 0,
        srv = meta.srv ~= false, max = meta.max or 60, loaded = true }
    GA.Fire("SHOPPING")
end

function State.ReplaceShopItems(list, meta)
    State.shopSearch = { q = meta and meta.q or "", items = list or {}, loaded = true }
    GA.Fire("SHOPPING")
end

function State.ShoppingEntry(entry)
    for _, item in ipairs(State.shopping.items) do
        if item.e == entry then
            return item
        end
    end
end

-- What the guild storage holds of an item now (every random property; the server's count until the storage is loaded).
function State.ShoppingStock(entry)
    if State.storage.loaded then
        local total = 0
        for _, item in pairs(State.storage.items) do
            if item.e == entry then
                total = total + (item.n or 0)
            end
        end
        return total
    end
    local item = State.ShoppingEntry(entry)
    return item and item.have or 0
end

function State.FarmItem(item)
    for _, entry in ipairs(State.farm.items) do
        if entry.item == item then
            return entry
        end
    end
    return nil
end

function State.ReplaceDungeons(list)
    State.dungeons = {}
    for _, d in ipairs(list) do
        if d.id then
            State.dungeons[d.id] = d
        end
    end
    State.dungeonsLoaded = true
    GA.Fire("DUNGEONS")
end

-- The dungeons in a bot's reach with its readiness for each: { { d = dungeon, pct = 0..100 }, ... }, dungeons before
-- raids, by level. Entries whose dungeon the client has not been told about yet are left out.
function State.BotReadiness(bot)
    local list = {}
    for _, pair in ipairs(bot and bot.rdy or {}) do
        local d = State.dungeons[tonumber(pair[1]) or 0]
        if d then
            list[#list + 1] = { d = d, pct = tonumber(pair[2]) or 0 }
        end
    end
    table.sort(list, function(a, b)
        if (a.d.raid and 1 or 0) ~= (b.d.raid and 1 or 0) then
            return not a.d.raid
        end
        if (a.d.min or 0) ~= (b.d.min or 0) then
            return (a.d.min or 0) < (b.d.min or 0)
        end
        return (a.d.id or 0) < (b.d.id or 0)
    end)
    return list
end

-- Index of the next tier of a series (1-based), or nil when every active tier is reached.
function State.NextMilestoneTier(series)
    local got, act = series.got or 0, series.act or 0
    if got < act then
        return got + 1
    end
    return nil
end

-- Lowest renown level of a feature unlock that is not owned yet (nil if unknown).
function State.FeatureUnlockLevel(key)
    for i = 1, #State.unlocks do
        local u = State.unlocks[i]
        if u.kind == "feature" and u.val == key then
            return u.lvl, u
        end
    end
    return nil
end

-- The next unlock of a kind that is not owned yet.
function State.NextUnlock(kind)
    for i = 1, #State.unlocks do
        local u = State.unlocks[i]
        if u.kind == kind and u.st ~= "owned" and u.st ~= "covered" then
            return u
        end
    end
    return nil
end

-- Unlocks that become available at the next renown level.
function State.UnlocksAtLevel(level)
    local list = {}
    for i = 1, #State.unlocks do
        if State.unlocks[i].lvl == level then
            list[#list + 1] = State.unlocks[i]
        end
    end
    return list
end

-- Human-readable requirement for a locked feature.
function State.FeatureRequirement(key)
    local lvl, u = State.FeatureUnlockLevel(key)
    if not lvl then
        return "Not available yet"
    end
    if u.st == "available" then
        return "Available now on the Progression tab"
    end
    return "Unlocks at Renown level " .. lvl
end

-------------------------------------------------------------------------------
-- Events and away summary
-------------------------------------------------------------------------------

function State.ReplaceEvents(list)
    -- the server sends oldest first; we keep newest first
    local events = {}
    for i = #list, 1, -1 do
        events[#events + 1] = list[i]
    end
    State.events = events
    GA.Fire("EVENTS")
end

function State.AddEvent(event)
    for i = 1, math.min(#State.events, 5) do
        if event.id and State.events[i].id == event.id then
            return -- duplicate push
        end
    end
    table.insert(State.events, 1, event)
    while #State.events > State.MAX_EVENTS do
        table.remove(State.events)
    end
    GA.Fire("EVENTS")
    GA.Fire("EVENT", event)
end

function State.SetAway(summary)
    State.away = summary
    GA.Fire("AWAY")
end

-------------------------------------------------------------------------------
-- Guild storage, loot filter, recipes (docs/protocol.md §7.10-§7.13)
-------------------------------------------------------------------------------

State.MAX_LOOT_LOG = 50

local function StorageKey(e, rp)
    return tostring(e) .. ":" .. tostring(rp or 0)
end

-- qi (2026-11-25): one { quality, itemLevel } per tuple, in the same order; nil from an older server (the Storage tab
-- takes both from the client's item cache then).
local function ApplyTuples(tuples, qi)
    for index, t in ipairs(tuples or {}) do
        local e, rp, n = tonumber(t[1]), tonumber(t[2]) or 0, tonumber(t[3]) or 0
        if e then
            local key = StorageKey(e, rp)
            if n > 0 then
                -- sell: vendor price per item; ds: Enchanting skill to disenchant it from the storage, -1 = cannot;
                -- inv: the inventory type (the slot it goes in), nil from an older server; res: what the bots leave there;
                -- cat: the category when the class does not say it ("darkmoon"), nil otherwise
                local cat = t[10]
                if cat == "" then
                    cat = nil
                end
                State.storage.items[key] = { e = e, rp = rp, n = n, cls = tonumber(t[4]) or 0, sub = tonumber(t[5]) or 0,
                    sell = tonumber(t[6]) or 0, ds = tonumber(t[7]) or -1, inv = tonumber(t[8]), res = tonumber(t[9]) or 0,
                    cat = cat, q = qi and qi[index] and tonumber(qi[index][1]), il = qi and qi[index] and tonumber(qi[index][2]) }
            else
                State.storage.items[key] = nil
            end
        end
    end
end

function State.ReplaceStorage(records, meta)
    State.storage.items = {}
    for _, record in ipairs(records or {}) do
        ApplyTuples(record.it, record.qi)
    end
    State.storage.kinds = meta and meta.kinds or 0
    State.storage.total = meta and meta.items or 0
    State.storage.ench = meta and meta.ench or -1
    State.storage.enchn = meta and meta.enchn or ""
    State.storage.loaded = true
    GA.Fire("STORAGE")
end

-- The guild's best enchanter in the world (STORAGE_ENCH): its Enchanting skill (-1 = none) and name.
function State.SetEnchanter(skill, name)
    State.storage.ench = skill or -1
    State.storage.enchn = name or ""
    GA.Fire("STORAGE")
end

function State.ApplyStorageDelta(fields)
    ApplyTuples(fields.it, fields.qi)
    if fields.kinds then
        State.storage.kinds = fields.kinds
    end
    if fields.items then
        State.storage.total = fields.items
    end
    GA.Fire("STORAGE")
end

-- Amount of an item kind in the guild storage (rp 0 = no random property; reagents are always rp 0).
function State.StorageCount(entry, rp)
    local item = State.storage.items[StorageKey(entry, rp)]
    return item and item.n or 0
end

-- Every kind (that passes filter) as a list, sorted by name; kinds the client has not seen yet sort by id at the end.
-- The quality and the item level of a storage kind: from the server (qi), else from the client's item cache.
function State.StorageQuality(item)
    if item.q then
        return item.q
    end
    local _, _, quality = (GetItemInfo or function() end)(item.e)
    return quality or 1
end

function State.StorageItemLevel(item)
    if item.il then
        return item.il
    end
    local _, _, _, level = (GetItemInfo or function() end)(item.e)
    return level or 0
end

-- sortKey: "name" (default), "quality", "ilvl" (both highest first), "count" (most first); a tie goes by the name.
function State.StorageList(filter, sortKey)
    local list = {}
    for _, item in pairs(State.storage.items) do
        if not filter or filter(item) then
            list[#list + 1] = item
        end
    end
    table.sort(list, function(a, b)
        if sortKey == "quality" or sortKey == "ilvl" or sortKey == "count" then
            local va, vb
            if sortKey == "quality" then
                va, vb = State.StorageQuality(a), State.StorageQuality(b)
            elseif sortKey == "ilvl" then
                va, vb = State.StorageItemLevel(a), State.StorageItemLevel(b)
            else
                va, vb = a.n or 0, b.n or 0
            end
            if va ~= vb then
                return va > vb
            end
        end
        local info = GetItemInfo or function() end
        local na = info(a.e)
        local nb = info(b.e)
        if na and nb and na ~= nb then
            return na < nb
        end
        if (na ~= nil) ~= (nb ~= nil) then
            return na ~= nil
        end
        if a.e ~= b.e then
            return a.e < b.e
        end
        return (a.rp or 0) < (b.rp or 0)
    end)
    return list
end

function State.SetLootSettings(fields)
    State.loot.settings = fields
    State.loot.loaded = true
    GA.Fire("LOOT")
end

function State.ReplaceLootRules(list)
    table.sort(list, function(a, b)
        return (a.pos or 0) < (b.pos or 0)
    end)
    State.loot.rules = list
    GA.Fire("LOOT")
end

function State.ReplaceLootLog(list)
    State.loot.log = list or {}
    State.loot.logLoaded = true
    GA.Fire("LOOT_LOG")
end

function State.AddLootLog(entry)
    local log = State.loot.log
    log[#log + 1] = entry
    while #log > State.MAX_LOOT_LOG do
        table.remove(log, 1)
    end
    GA.Fire("LOOT_LOG", entry)
end

function State.ReplaceSpecs(list)
    table.sort(list, function(a, b)
        return (a.id or 0) < (b.id or 0)
    end)
    State.specs = list
    GA.Fire("SPECS")
end

function State.SetFightProfiles(meta, list)
    local g = meta and meta.g
    if not g then
        return
    end
    table.sort(list, function(a, b)
        return (a.id or 0) < (b.id or 0)
    end)
    State.fight.lists[g] = { meta = meta, list = list }
    GA.Fire("FIGHT", g)
end

function State.SetFightProfile(meta, rules)
    local g = meta and meta.g
    if not g then
        return
    end
    for _, rule in ipairs(rules) do
        rule.rel = tonumber(rule.rel) or 0
    end
    State.fight.profiles[g] = State.fight.profiles[g] or {}
    State.fight.profiles[g][meta.id or 0] = { meta = meta, rules = rules }
    GA.Fire("FIGHT", g)
end

function State.SetFightOptions(meta, list)
    local g = meta and meta.g
    if not g then
        return
    end
    for _, option in ipairs(list) do
        option.v, option.bv = tonumber(option.v) or 0, tonumber(option.bv) or 0
        option.min, option.max = tonumber(option.min) or 0, tonumber(option.max) or 0
    end
    State.fight.options[g] = State.fight.options[g] or {}
    State.fight.options[g][meta.id or 0] = list
    GA.Fire("FIGHT", g)
end

-- The editor's catalogue (FP-3): the conditions (the same for every bot), the class spells a bot knows (by their first
-- rank) and the usable items it carries.
function State.SetFightConditions(list)
    for _, cond in ipairs(list) do
        cond.min, cond.max = tonumber(cond.min) or 0, tonumber(cond.max) or 0
    end
    State.fight.conds = list
    State.fight.condByKey = {}
    for _, cond in ipairs(list) do
        State.fight.condByKey[cond.k] = cond
    end
    GA.Fire("FIGHT")
end

function State.SetFightSpells(meta, list)
    if meta and meta.g then
        table.sort(list, function(a, b) return (a.n or "") < (b.n or "") end)
        State.fight.spells[meta.g] = list
        GA.Fire("FIGHT", meta.g)
    end
end

function State.SetFightItems(meta, list)
    if meta and meta.g then
        State.fight.items[meta.g] = list
        GA.Fire("FIGHT", meta.g)
    end
end

-- The triggers of mod-playerbots in plain language for a bot's class: name -> text ("" where the server has none).
function State.SetFightTriggers(meta, list)
    if meta and meta.g then
        local texts = {}
        for _, trigger in ipairs(list) do
            texts[trigger.n or ""] = trigger.t or ""
        end
        State.fight.trig[meta.g] = texts
        GA.Fire("FIGHT", meta.g)
    end
end

-- What the addon knows of a bot's fight profiles: the list of its class with its slots, the profiles it was shown, the
-- options of each with its values.
function State.FightOf(g)
    return State.fight.lists[g], (State.fight.profiles[g] or {}), (State.fight.options[g] or {})
end

function State.SetRoleGear(g, list)
    if not g then
        return
    end
    table.sort(list, function(a, b)
        if (a.k or 0) ~= (b.k or 0) then
            return (a.k or 0) < (b.k or 0)
        end
        return (a.id or 0) < (b.id or 0)
    end)
    State.roleGear[g] = list
    GA.Fire("ROLEGEAR", g)
end

-- What a guild character does now and next (GET what=plan): { g, at, lines = { { k, t }, ... } }, the last one asked for.
function State.SetPlan(g, list, at)
    if not g then
        return
    end
    State.plan = { g = g, at = at, lines = list or {} }
    GA.Fire("PLAN", g)
end

function State.PlanOf(g)
    if State.plan and State.plan.g == g then
        return State.plan
    end
    return nil
end

-- The dungeon role a bot fills in a group: the one its owner gave it, else the role of the talents it levels with.
function State.BotGroupRole(bot)
    if bot and bot.dr and bot.dr ~= "none" and bot.dr ~= "" then
        return bot.dr
    end
    return bot and bot.role or "dps"
end

-- A bot's gear level: the live one while it is in the world (gl), else the last known one (glk, servers since the
-- roster's item level column). nil: unknown.
function State.BotGearLevel(bot)
    if not bot then
        return nil
    end
    local gl = tonumber(bot.gl)
    if gl and gl > 0 and bot.s ~= "benched" and bot.s ~= "offline" then
        return gl
    end
    local glk = tonumber(bot.glk)
    if glk and glk > 0 then
        return glk
    end
    return (gl and gl > 0) and gl or nil
end

-- A bot's known professions (BOT^sk) as two lists of { id, value, max }: the primary ones, highest skill first,
-- and the secondary ones in the client's order.
function State.BotProfessions(bot)
    local primary, secondary = {}, {}
    for _, entry in ipairs(bot and bot.sk or {}) do
        local id, value, max = tonumber(entry[1]), tonumber(entry[2]) or 0, tonumber(entry[3]) or 0
        local info = id and GA.Data.PROFESSIONS[id]
        if info then
            local list = info.primary and primary or secondary
            list[#list + 1] = { id = id, value = value, max = max }
        end
    end
    table.sort(primary, function(a, b)
        if a.value ~= b.value then
            return a.value > b.value
        end
        return a.id < b.id
    end)
    return primary, secondary
end

-- A bot's readiness for a dungeon in its dungeon set (nil: no role, or not in the world).
function State.BotRoleReadiness(bot, dungeonId)
    for _, pair in ipairs(bot and bot.drrdy or {}) do
        if tonumber(pair[1]) == dungeonId then
            return tonumber(pair[2]) or 0
        end
    end
end

function State.SpecsForClass(class)
    local list = {}
    for _, spec in ipairs(State.specs) do
        if spec.class == class then
            list[#list + 1] = spec
        end
    end
    return list
end

function State.SetRecipes(meta, list)
    State.recipes = { g = meta and meta.g, busy = meta and meta.busy, list = list or {} }
    GA.Fire("RECIPES")
end

function State.ReplaceAuctionRules(list, meta)
    local rules = {}
    for _, rule in ipairs(list or {}) do
        rules[StorageKey(rule.e, rule.rp)] = rule
    end
    State.auctions = { rules = rules, srv = not meta or meta.srv ~= false, hours = meta and meta.hours or 24,
        every = meta and meta.every or 300, loaded = true, cats = State.auctions.cats or {}, listed = State.auctions.listed or {} }
    GA.Fire("AUCTIONS")
end

function State.AuctionRule(entry, rp)
    return State.auctions.rules[StorageKey(entry, rp)]
end

-- Rules for whole storage categories (AUCTION_CATS), by category key.
function State.ReplaceAuctionCategories(list)
    local cats = {}
    for _, rule in ipairs(list or {}) do
        cats[rule.cat] = rule
    end
    State.auctions.cats = cats
    GA.Fire("AUCTIONS")
end

function State.AuctionCategoryRule(category)
    return (State.auctions.cats or {})[category]
end

-- What of each kind is in the auction house now (AUCTION_LISTINGS).
function State.ReplaceAuctionListings(list)
    local listed = {}
    for _, row in ipairs(list or {}) do
        listed[StorageKey(row.e, row.rp)] = row.n
    end
    State.auctions.listed = listed
    GA.Fire("AUCTIONS")
end

function State.AuctionListedCount(entry, rp)
    local n = (State.auctions.listed or {})[StorageKey(entry, rp)]
    if n then
        return n
    end
    local rule = State.AuctionRule(entry, rp)
    return rule and rule.listed or 0
end

-- The rule that applies to a storage item: its own, else its category's (category = the key it came from).
-- `categoryOf(cls, sub, cat)` names the category (the Storage tab's list; `cat` is the server's own name for it).
function State.EffectiveAuctionRule(item, categoryOf)
    local own = State.AuctionRule(item.e, item.rp)
    if own then
        return own, nil
    end
    local key = categoryOf and categoryOf(item.cls, item.sub, item.cat)
    local cat = key and State.AuctionCategoryRule(key)
    if cat then
        return cat, key
    end
    return nil, nil
end

-- How many of a storage item the next auction pass would list: what is above `keep`, in whole stacks.
function State.AuctionSurplus(item, rule, stackSize)
    if not rule or rule.on == false then
        return 0
    end
    local stack = rule.stack or 0
    if stack <= 0 then
        stack = math.min(stackSize or 20, 20)
    end
    stack = math.max(1, math.min(stack, stackSize or stack))
    -- What stays: the rule's keep, at least the item's reserve (the server keeps a reserved stock out of the auctions).
    local above = (item.n or 0) - math.max(rule.keep or 0, item.res or 0)
    if above < stack then
        return 0
    end
    return math.floor(above / stack) * stack
end

-------------------------------------------------------------------------------
-- Bot behaviour settings (docs/bot-settings.md, docs/protocol.md §7.19)
-------------------------------------------------------------------------------

-- SETDEFS block: categories (SETCAT) and settings (SETDEF) in the server's order.
function State.ReplaceSettingDefs(records, sv)
    local s = State.settings
    s.sv = sv or 0
    s.cats, s.defs, s.byId, s.byKey = {}, {}, {}, {}
    for _, r in ipairs(records or {}) do
        if r.op == "SETCAT" or (r.k and r.id == nil) then
            s.cats[#s.cats + 1] = r
        elseif r.id then
            s.defs[#s.defs + 1] = r
            s.byId[r.id] = r
            if r.k then
                s.byKey[string.lower(r.k)] = r
            end
        end
    end
    table.sort(s.cats, function(a, b)
        return (a.o or 0) < (b.o or 0)
    end)
    GA.Fire("SETTINGS")
end

local function ValueMap(tuples)
    local map = {}
    for _, t in ipairs(tuples or {}) do
        if type(t[1]) == "number" and type(t[2]) == "number" then
            map[t[1]] = t[2]
        end
    end
    return map
end

-- One SETVAL record: the whole set of own values of a scope (g = 0: the guild).
function State.SetSettingValues(g, tuples, silent)
    State.settings.values[g or 0] = ValueMap(tuples)
    if not silent then
        GA.Fire("SETTINGS")
    end
end

-- SETVALS block: every scope that has values; scopes not in it have none.
function State.ReplaceSettingValues(records)
    State.settings.values = { [0] = {} }
    for _, r in ipairs(records or {}) do
        State.SetSettingValues(r.g or 0, r.v, true)
    end
    State.settings.loaded = true
    GA.Fire("SETTINGS")
end

function State.SettingDef(idOrKey)
    local s = State.settings
    if type(idOrKey) == "number" then
        return s.byId[idOrKey]
    end
    return idOrKey and s.byKey[string.lower(idOrKey)]
end

-- The value that counts for scope g (0 or nil = the guild) and where it comes from:
-- "own" (the character's value), "guild" (the guild's value) or "default" (the server's conf).
function State.SettingValue(def, g)
    local s = State.settings
    if not def then
        return nil, "default"
    end
    if g and g ~= 0 and def.pc then
        local own = s.values[g]
        if own and own[def.id] ~= nil then
            return own[def.id], "own"
        end
    end
    local guild = s.values[0]
    if guild and guild[def.id] ~= nil then
        return guild[def.id], "guild"
    end
    return def.def, "default"
end

-- How many settings of scope g have a value of their own (the guild: differ from the server's default).
function State.ChangedSettingCount(g)
    local count = 0
    local values = State.settings.values[g or 0] or {}
    for id in pairs(values) do
        if State.settings.byId[id] then
            count = count + 1
        end
    end
    return count
end
