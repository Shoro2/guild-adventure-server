-- GADV protocol semantics (docs/protocol.md §4-7): handshake, server message handlers,
-- block assembly, request/ack tracking and the intent API the UI calls.

local GA = GuildAdventure
local Protocol = {}
GA.Protocol = Protocol

local Codec = GA.Codec
local State = GA.State

Protocol.REQUEST_TIMEOUT = 10
Protocol.HELLO_RETRIES = { 5, 10, 20 } -- seconds to wait for WELCOME after each attempt
Protocol.HELLO_DELAY = 2               -- after PLAYER_ENTERING_WORLD

-- Value types per server opcode (docs/protocol.md §7). Unknown keys stay strings.
Protocol.SCHEMA = {
    WELCOME = { v = "int", min = "int", srv = "str", now = "int", zv = "int" },
    ACK = { r = "int" },
    ERROR = { r = "int", code = "str", reason = "str", msg = "str" },
    NOTICE = { msg = "str", kind = "str" },
    STATE = {
        guild = "str", fac = "str", rl = "int", rp = "int", rn = "int", rt = "int",
        slots = "int", used = "int", max = "int", ow = "int", owmax = "int", bank = "int", tax = "int",
        ulgold = "int", ulfrom = "str",
        feat = "list", orders = "list", dk = "bool", ach = "int", achp = "int", achr = "int", gp = "int", gps = "int", gpg = "int",
        stk = "int", sti = "int", ms = "int", msa = "int", msp = "int", msr = "int",
    },
    CONTROL_STATE = { leader = "int", ln = "str", g = "int", cn = "str", busy = "bool", path = "str", watch = "bool" },
    CONTROL_PENDING = { g = "int", cn = "str" },
    BOT = {
        g = "int", n = "str", race = "int", class = "int", gender = "int", slot = "int",
        lvl = "int", xp = "int", xpmax = "int", s = "str", zone = "int", zn = "str", map = "int",
        o = "str", oz = "int", ozn = "str", aa = "bool", lc = "int", cont = "int", nd = "bool",
        ofn = "int", ofs = "int",
        gcap = "int", why = "str", hp = "int", dead = "bool", combat = "bool", gold = "int",
        role = "str", spec = "str", spt = "int", tal = "str", p1 = "int", p2 = "int", t1 = "int", t2 = "int",
        sec = "intlist", sk = "tuples", kills = "int", deaths = "int", quests = "int",
        xph = "int", gph = "int", since = "int",
        sq = "int", sqs = "str", sqm = "int",
        -- Quest partners (docs/protocol.md "Quest partners")
        sqq = "int", sqqn = "str", qw = "intlist", qwn = "str",
        gl = "int", rdy = "tuples", nw = "bool", wm = "str",
        -- Dungeon role (docs/protocol.md "Dungeon roles")
        dr = "str", drs = "str", drt = "int", drn = "str", drw = "bool", drp = "int", drgl = "int", drrdy = "tuples",
        druns = "int", drund = "int",
        -- Fight profiles (docs/protocol.md "Fight profiles")
        fp = "int", fpg = "int", fpn = "str", fpid = "int", fpd = "bool", fpsl = "int", fpw = "str",
    },
    BOT_REMOVE = { g = "int" },
    LEADER = {
        g = "int", n = "str", race = "int", class = "int", gender = "int", s = "str", lvl = "int", xp = "int",
        xpmax = "int", zone = "int", zn = "str", hp = "int", dead = "bool", combat = "bool", o = "str", oz = "int",
        ozn = "str", lc = "int", cont = "int", nd = "bool", gcap = "int", why = "str", xph = "int", gph = "int",
    },
    ZONE = { id = "int", n = "str", min = "int", max = "int", fac = "str", cont = "int", orders = "list" },
    UNLOCK = { id = "int", lvl = "int", kind = "str", val = "str", gold = "int", st = "str", desc = "str" },
    EVENT = { id = "int", t = "int", type = "str", g = "int", msg = "str", lvl = "int", zone = "int", pts = "int" },
    AWAY_LINE = { g = "int", msg = "str" },
    ROSTER_BEGIN = { n = "int" },
    ZONES_BEGIN = { zv = "int", n = "int" },
    UNLOCKS_BEGIN = { n = "int" },
    EVENTS_BEGIN = { n = "int" },
    AWAY_BEGIN = { from = "int", to = "int", lv = "int", rp = "int", gold = "int", q = "int", d = "int", n = "int" },
    -- Guild storage, loot filter, crafting (docs/protocol.md §7.10-§7.13)
    STORAGE_BEGIN = { n = "int", kinds = "int", items = "int", ench = "int", enchn = "str" },
    STORAGE_ENCH = { ench = "int", enchn = "str" },
    STORAGE_ITEMS = { it = "tuples", qi = "tuples" },
    STORAGE_DELTA = { it = "tuples", qi = "tuples", kinds = "int", items = "int" },
    LOOT_SETTINGS = {
        on = "bool", dai = "int", dpl = "int", al = "bool", sold = "int", de = "int", del = "int", st = "int",
        max = "int", allow = "intlist", srv = "bool", salr = "bool", reag = "bool", junk = "int", junkok = "bool", kwg = "bool",
    },
    STORAGE_JUNK = { kinds = "int", items = "int", money = "int", to = "str", sold = "bool", cap = "bool" },
    LOOT_RULES_BEGIN = { n = "int" },
    LOOT_RULE = { id = "int", pos = "int", act = "int", en = "bool", lg = "int", c = "str" },
    LOOT_LOG_BEGIN = { n = "int" },
    LOOT_LOG = { t = "int", g = "int", e = "int", rp = "int", n = "int", o = "int", m = "int", why = "str" },
    -- Talent templates (docs/protocol.md "Talent templates")
    -- Role store of a bot (docs/protocol.md "Dungeon roles")
    ROLEGEAR_BEGIN = { g = "int", n = "int" },
    -- What a guild character does now and next (docs/protocol.md "The plan of a guild character")
    PLAN_BEGIN = { g = "int", n = "int", at = "int" },
    PL = { k = "str", t = "str" },
    RG = { g = "int", id = "int", k = "int", s = "int", e = "int", rp = "int", n = "str", q = "int", il = "int", it = "int" },
    SPECS_BEGIN = { n = "int" },
    SPEC = { id = "int", class = "int", n = "str", p = "int", ro = "int", t = "int", d = "bool", own = "bool", sum = "str" },
    RECIPES_BEGIN = { g = "int", n = "int", busy = "bool" },
    RECIPE = { s = "int", i = "int", max = "int", nm = "str" },
    AUCTION_RULES_BEGIN = { n = "int", srv = "bool", hours = "int", every = "int" },
    AUCTION_RULE = { e = "int", rp = "int", keep = "int", stack = "int", price = "int", on = "bool", auto = "int", listed = "int" },
    AUCTION_CATS_BEGIN = { n = "int" },
    AUCTION_CAT = { cat = "str", keep = "int", stack = "int", on = "bool" },
    AUCTION_LISTINGS_BEGIN = { n = "int" },
    AUCTION_LISTED = { e = "int", rp = "int", n = "int" },
    -- Guild perks (docs/protocol.md §7.15)
    PERKS_BEGIN = { n = "int" },
    PERK = { id = "int", key = "str", n = "str", rank = "int", max = "int", cost = "int", unit = "str", desc = "str",
        next = "str", avail = "bool" },
    -- Bot behaviour settings (docs/protocol.md §7.19)
    SETDEFS_BEGIN = { n = "int", sv = "int" },
    SETCAT = { k = "str", n = "str", h = "str", o = "int" },
    SETDEF = {
        id = "int", k = "str", cat = "str", t = "str", min = "int", max = "int", st = "int", sc = "int", u = "str",
        def = "int", pc = "bool", adv = "bool", opt = "str", n = "str", h = "str",
    },
    SETVALS_BEGIN = { n = "int" },
    SETVAL = { g = "int", v = "tuples" },
    -- Dungeons for the readiness numbers of the BOT records (docs/protocol.md §7.20)
    -- Dungeon runs of the guild (docs/protocol.md "Dungeon runs")
    DRUNS_BEGIN = { n = "int" },
    DRUN = { g = "int", d = "int", dn = "str", ph = "int", b = "int", of = "int", nb = "str", w = "int", left = "int",
        done = "int", min = "int", m = "intlist", why = "str" },
    -- The preparation step of the dungeon run window (docs/protocol.md "Dungeon runs")
    DPREP_BEGIN = { n = "int", tl = "int", em = "int", cp = "int", cpon = "bool", ff = "bool", md = "int", gr = "int", gc = "int" },
    DITEM = { e = "int", n = "str", q = "int", k = "int", amt = "int", lvl = "int" },
    DUNGEONS_BEGIN = { n = "int" },
    DUNGEON = { id = "int", map = "int", n = "str", min = "int", max = "int", tgt = "int", size = "int", raid = "bool",
        gl = "int", run = "bool", ord = "int", fin = "int", lock = "bool", need = "str" },
    -- Craft orders of the guild and what its characters can make (docs/protocol.md §7.21)
    CRAFT_ORDERS_BEGIN = { n = "int" },
    CORDER = { id = "int", item = "int", n = "str", amt = "int", made = "int", note = "str" },
    CRAFTABLES_BEGIN = { n = "int" },
    CRAFTABLE = { item = "int", n = "str", il = "int", cls = "int", sub = "int", by = "str" },
    -- The item picker of the farm order (docs/protocol.md §7.23)
    FARM_ITEMS_BEGIN = { n = "int", q = "str", g = "int" },
    FARM_ITEM = { item = "int", n = "str", q = "int", il = "int", cls = "int", k = "int", lv = "int", need = "int", ok = "bool" },
    -- The guild storage's shopping list (docs/protocol.md section 7.25)
    SHOPPING_BEGIN = { n = "int", srv = "bool", day = "int", cap = "int", visit = "int", max = "int" },
    SHOP = { e = "int", n = "str", min = "int", on = "bool", have = "int", price = "int", bc = "int" },
    SHOP_ITEMS_BEGIN = { n = "int", q = "str" },
    SHOP_ITEM = { e = "int", n = "str", q = "int", lvl = "int", price = "int", bc = "int" },
    -- Fight profiles of a bot's class and one profile's rules and options (docs/protocol.md "Fight profiles")
    FPROFILES_BEGIN = { g = "int", cls = "int", tab = "int", solo = "int", grp = "int", max = "int", n = "int",
        dflt = "int", dfltg = "int" },
    FPROF = { g = "int", id = "int", n = "str", tab = "int", rev = "int", rc = "int", rn = "int", used = "int", opts = "int",
        tpl = "bool", tr = "int" },
    FPROFILE_BEGIN = { g = "int", id = "int", pn = "str", tab = "int", rev = "int", note = "str", rep = "str", hid = "int",
        live = "bool", n = "int", tpl = "bool" },
    FRULE = { g = "int", l = "int", k = "int", ref = "int", a = "str", tg = "str", tgt = "int", rel = "str", en = "bool",
        fl = "int", iv = "int", c = "str", d = "str", src = "str", nd = "int", kn = "bool", lv = "int" },
    -- The fight profile editor's catalogue (FP-3): conditions, the bot's class spells, its usable items.
    FCONDS_BEGIN = { n = "int" },
    FCOND = { k = "str", n = "str", grp = "str", vk = "int", pk = "int", min = "str", max = "str", tg = "bool" },
    FSPELLS_BEGIN = { g = "int", live = "bool", n = "int" },
    FSP = { id = "int", hi = "int", n = "str", tab = "str" },
    FITEMS_BEGIN = { g = "int", n = "int" },
    FIT = { e = "int", n = "str" },
    FTRIGS_BEGIN = { g = "int", n = "int" },
    FTRIG = { n = "str", t = "str" },
    FOPTS_BEGIN = { g = "int", id = "int", n = "int" },
    FOPT = { g = "int", id = "int", k = "str", n = "str", kind = "int", v = "str", min = "str", max = "str", own = "bool",
        bv = "str" },
    -- Tiered guild achievements (docs/protocol.md §7.18)
    MILESTONES_BEGIN = { n = "int" },
    MILESTONE = { s = "str", k = "str", grp = "str", sub = "int", n = "str", v = "int", th = "intlist", pt = "intlist",
        act = "int", got = "int" },
}

-- Record opcode -> block it belongs to while that block is open.
local RECORD_BLOCK = {
    BOT = "ROSTER",
    ZONE = "ZONES",
    UNLOCK = "UNLOCKS",
    EVENT = "EVENTS",
    AWAY_LINE = "AWAY",
    STORAGE_ITEMS = "STORAGE",
    LOOT_RULE = "LOOT_RULES",
    LOOT_LOG = "LOOT_LOG",
    RECIPE = "RECIPES",
    SPEC = "SPECS",
    RG = "ROLEGEAR",
    PL = "PLAN",
    AUCTION_RULE = "AUCTION_RULES",
    AUCTION_CAT = "AUCTION_CATS",
    AUCTION_LISTED = "AUCTION_LISTINGS",
    PERK = "PERKS",
    SETCAT = "SETDEFS",
    SETDEF = "SETDEFS",
    SETVAL = "SETVALS",
    MILESTONE = "MILESTONES",
    DUNGEON = "DUNGEONS",
    DRUN = "DRUNS",
    DITEM = "DPREP",
    CORDER = "CRAFT_ORDERS",
    CRAFTABLE = "CRAFTABLES",
    FARM_ITEM = "FARM_ITEMS",
    SHOP = "SHOPPING",
    FPROF = "FPROFILES",
    FRULE = "FPROFILE",
    FOPT = "FOPTS",
    FCOND = "FCONDS",
    FSP = "FSPELLS",
    FIT = "FITEMS",
    FTRIG = "FTRIGS",
    SHOP_ITEM = "SHOP_ITEMS",
}

local openBlocks = {}
local pending = {}
local nextRequest = 0
local helloAttempt = 0
local helloTimer = nil

-------------------------------------------------------------------------------
-- Sending
-------------------------------------------------------------------------------

function Protocol.Send(op, fields)
    return GA.Transport.Send(Codec.Encode(op, fields))
end

local function Resolve(r, ok, code, msg, reason)
    local p = pending[r]
    if not p then
        return
    end
    pending[r] = nil
    if p.timer then
        p.timer:Cancel()
    end
    if p.cb then
        p.cb(ok, code, msg, reason)
    end
end

-- Request(op, fields, cb): sends an intent with a request id; cb(ok, code, msg, reason) is
-- called once with the ACK/ERROR, or with "TIMEOUT"/"NOT_CONNECTED".
function Protocol.Request(op, fields, cb)
    if not State.IsConnected() then
        if cb then
            cb(false, "NOT_CONNECTED", "Not connected to a Guild Adventure server.")
        end
        return nil
    end
    nextRequest = nextRequest + 1
    local r = nextRequest
    fields = fields or {}
    fields.r = r
    pending[r] = {
        cb = cb,
        op = op,
        timer = GA.After(Protocol.REQUEST_TIMEOUT, function()
            Resolve(r, false, "TIMEOUT", "No answer from the server.")
        end),
    }
    Protocol.Send(op, fields)
    return r
end

-------------------------------------------------------------------------------
-- Handshake
-------------------------------------------------------------------------------

local function CancelHello()
    if helloTimer then
        helloTimer:Cancel()
        helloTimer = nil
    end
end

local function SendHello()
    local zv = 0
    if State.mode == "live" then
        local cache = GA.db and GA.db.zoneCache
        if cache and cache.zv and cache.zones then
            zv = cache.zv
            if State.zv ~= cache.zv then
                State.ReplaceZones(cache.zones, cache.zv)
            end
        end
    end
    -- the bot settings schema works like the zone cache: the server sends SETDEFS only when sv changed
    local sv = 0
    if State.mode == "live" then
        local cache = GA.db and GA.db.settingsCache
        if cache and cache.sv and cache.records then
            sv = cache.sv
            if State.settings.sv ~= cache.sv then
                State.ReplaceSettingDefs(cache.records, cache.sv)
            end
        end
    end
    -- the whisper-to-self target: a swap that keeps the UI loaded changes it (Path W / S)
    State.helloName = UnitName("player")
    State.helloGuid = GA.Transport.PlayerGuid()
    Protocol.Send("HELLO", { v = GA.PROTOCOL_VERSION, ui = GA.VERSION, zv = zv, sv = sv })
end

local function HelloTimeout()
    helloTimer = nil
    if State.connection == "connected" then
        return
    end
    helloAttempt = helloAttempt + 1
    if helloAttempt > #Protocol.HELLO_RETRIES then
        if GA.Transport.echoSeen then
            State.SetConnection("missing")
        else
            State.SetConnection("failed")
        end
        return
    end
    SendHello()
    helloTimer = GA.After(Protocol.HELLO_RETRIES[helloAttempt], HelloTimeout)
end

-- Connect(restart): sends HELLO with retries. restart = true begins a fresh attempt series
-- even if one is running (used by /ga reconnect and mode switches).
function Protocol.Connect(restart)
    if helloTimer and not restart then
        return
    end
    CancelHello()
    helloAttempt = 1
    if State.connection ~= "connected" then
        State.SetConnection("connecting")
    end
    SendHello()
    helloTimer = GA.After(Protocol.HELLO_RETRIES[1], HelloTimeout)
end

-- SetMode("live" | "mock"): drops all server state and reconnects through the new backend.
function Protocol.SetMode(mode)
    CancelHello()
    for r in pairs(pending) do
        Resolve(r, false, "NOT_CONNECTED", "Connection reset.")
    end
    openBlocks = {}
    State.Reset()
    State.mode = mode
    GA.Transport.SetBackend(mode)
    GA.Fire("CONNECTION", State.connection)
    GA.Fire("ROSTER")
    GA.Fire("GUILD")
    Protocol.Connect(true)
end

-------------------------------------------------------------------------------
-- Receiving
-------------------------------------------------------------------------------

local BLOCK_COMMIT = {
    ROSTER = function(block)
        State.ReplaceRoster(block.items)
    end,
    ZONES = function(block)
        State.ReplaceZones(block.items, block.meta.zv)
        if State.mode == "live" and GA.db then
            GA.db.zoneCache = { zv = block.meta.zv or 0, zones = block.items }
        end
    end,
    UNLOCKS = function(block)
        State.ReplaceUnlocks(block.items)
    end,
    PERKS = function(block)
        State.ReplacePerks(block.items)
    end,
    MILESTONES = function(block)
        State.ReplaceMilestones(block.items)
    end,
    DUNGEONS = function(block)
        State.ReplaceDungeons(block.items)
    end,
    DRUNS = function(block)
        State.SetDungeonRuns(block.items)
    end,
    DPREP = function(block)
        State.SetDungeonPrep(block.meta, block.items)
    end,
    CRAFT_ORDERS = function(block)
        State.ReplaceCraftOrders(block.items)
    end,
    CRAFTABLES = function(block)
        State.ReplaceCraftables(block.items)
    end,
    FARM_ITEMS = function(block)
        State.ReplaceFarmItems(block.items, block.meta)
    end,
    SHOPPING = function(block)
        State.ReplaceShopping(block.items, block.meta)
    end,
    FPROFILES = function(block)
        State.SetFightProfiles(block.meta, block.items)
    end,
    FPROFILE = function(block)
        State.SetFightProfile(block.meta, block.items)
    end,
    FOPTS = function(block)
        State.SetFightOptions(block.meta, block.items)
    end,
    FCONDS = function(block)
        State.SetFightConditions(block.items)
    end,
    FSPELLS = function(block)
        State.SetFightSpells(block.meta, block.items)
    end,
    FITEMS = function(block)
        State.SetFightItems(block.meta, block.items)
    end,
    FTRIGS = function(block)
        State.SetFightTriggers(block.meta, block.items)
    end,
    SHOP_ITEMS = function(block)
        State.ReplaceShopItems(block.items, block.meta)
    end,
    EVENTS = function(block)
        State.ReplaceEvents(block.items)
    end,
    AWAY = function(block)
        local summary = block.meta
        summary.lines = block.items
        State.SetAway(summary)
    end,
    STORAGE = function(block)
        State.ReplaceStorage(block.items, block.meta)
    end,
    LOOT_RULES = function(block)
        State.ReplaceLootRules(block.items)
    end,
    LOOT_LOG = function(block)
        State.ReplaceLootLog(block.items)
    end,
    RECIPES = function(block)
        State.SetRecipes(block.meta, block.items)
    end,
    SPECS = function(block)
        State.ReplaceSpecs(block.items)
    end,
    ROLEGEAR = function(block)
        State.SetRoleGear(block.meta.g, block.items)
    end,
    PLAN = function(block)
        State.SetPlan(block.meta.g, block.items, block.meta.at)
    end,
    AUCTION_RULES = function(block)
        State.ReplaceAuctionRules(block.items, block.meta)
    end,
    AUCTION_CATS = function(block)
        State.ReplaceAuctionCategories(block.items)
    end,
    AUCTION_LISTINGS = function(block)
        State.ReplaceAuctionListings(block.items)
    end,
    SETDEFS = function(block)
        State.ReplaceSettingDefs(block.items, block.meta.sv)
        if State.mode == "live" and GA.db then
            GA.db.settingsCache = { sv = block.meta.sv or 0, records = block.items }
        end
    end,
    SETVALS = function(block)
        State.ReplaceSettingValues(block.items)
    end,
}

local HANDLERS = {}

HANDLERS.WELCOME = function(f)
    CancelHello()
    State.server = f
    State.serverOffset = (f.now or time()) - time()
    if f.min and GA.PROTOCOL_VERSION < f.min then
        State.SetConnection("badversion")
        GA.Print("This addon is too old for the server (protocol " .. GA.PROTOCOL_VERSION
            .. ", server needs " .. f.min .. "). Please update GuildAdventure.")
        return
    end
    State.SetConnection("connected")
end

HANDLERS.ACK = function(f)
    if f.r then
        Resolve(f.r, true)
    end
end

HANDLERS.ERROR = function(f)
    if f.code == "BAD_VERSION" then
        CancelHello()
        State.SetConnection("badversion")
    end
    if f.r then
        Resolve(f.r, false, f.code, f.msg, f.reason)
    end
    GA.Fire("ERROR", f.code, f.msg, f.reason, f.r)
end

HANDLERS.NOTICE = function(f)
    GA.Print(f.msg or "")
    GA.Fire("NOTICE", f.msg, f.kind)
end

HANDLERS.STATE = function(f)
    State.MergeGuild(f)
end

HANDLERS.LEADER = function(f)
    State.MergeLeader(f)
end

HANDLERS.CONTROL_STATE = function(f)
    if f.busy == false and f.path == nil then
        f.path = "" -- the swap is over: forget its path
    end
    if f.busy == false then
        -- A swap that ends while this UI is still in the world (refused, failed, or done without a relog) leaves
        -- no character-screen entry to make: a target stored by CONTROL_PENDING must not auto-enter a later,
        -- ordinary visit to the character screen.
        Protocol.SetSwapTargetCVar("")
    end
    local before = State.control.g
    State.MergeControl(f)
    if before and f.g and before ~= f.g then
        -- the active character changed: handshake again from the new character
        Protocol.Connect(true)
    end
end

-- Warm relog (Path R): the client is about to leave the world for the character screen.
-- The patched GlueXML reads the CVar to enter the target character automatically.
HANDLERS.CONTROL_PENDING = function(f)
    Protocol.SetSwapTargetCVar(f.cn or "")
    GA.Fire("CONTROL_PENDING", f.g, f.cn)
end

HANDLERS.BOT = function(f)
    State.UpsertBot(f)
end

HANDLERS.BOT_REMOVE = function(f)
    State.RemoveBot(f.g)
end

HANDLERS.ZONE = function(f)
    State.UpsertZone(f)
end

HANDLERS.UNLOCK = function(f)
    State.UpsertUnlock(f)
end

HANDLERS.EVENT = function(f)
    State.AddEvent(f)
end

HANDLERS.AWAY_LINE = function()
    -- only meaningful inside an AWAY block
end

HANDLERS.STORAGE_DELTA = function(f)
    State.ApplyStorageDelta(f)
end

HANDLERS.LOOT_SETTINGS = function(f)
    State.SetLootSettings(f)
end

HANDLERS.STORAGE_ENCH = function(f)
    State.SetEnchanter(f.ench, f.enchn)
end

HANDLERS.STORAGE_JUNK = function(f)
    State.storage.junk = f
    GA.Fire("STORAGE")
end

HANDLERS.LOOT_LOG = function(f)
    State.AddLootLog(f)
end

-- After a change: the whole set of own values of one scope (g = 0: the guild).
HANDLERS.SETVAL = function(f)
    State.SetSettingValues(f.g or 0, f.v)
end

-- OnMessage(encoded): one complete logical message from the server.
function Protocol.OnMessage(encoded)
    local op, raw = Codec.Decode(encoded)
    local fields = Codec.ApplySchema(raw, Protocol.SCHEMA[op] or {})

    local beginKind = string.match(op, "^(.+)_BEGIN$")
    if beginKind and BLOCK_COMMIT[beginKind] then
        openBlocks[beginKind] = { meta = fields, items = {} }
        return
    end
    local endKind = string.match(op, "^(.+)_END$")
    if endKind and BLOCK_COMMIT[endKind] then
        local block = openBlocks[endKind]
        openBlocks[endKind] = nil
        if block then
            BLOCK_COMMIT[endKind](block)
        end
        return
    end

    local blockKind = RECORD_BLOCK[op]
    local block = blockKind and openBlocks[blockKind]
    if block then
        block.items[#block.items + 1] = fields
        return
    end

    local handler = HANDLERS[op]
    if handler then
        handler(fields)
    else
        GA.Debug("ignored opcode " .. tostring(op))
    end
end

-------------------------------------------------------------------------------
-- Intent API (called by the UI)
-------------------------------------------------------------------------------

function Protocol.Get(what, g)
    if State.IsConnected() then
        Protocol.Send("GET", { what = what, g = g })
    end
end

-- opts: zone, aa, lc, cont, nd, gcap, runs, n (all optional; see docs/protocol.md §6). runs: how often a dungeon is
-- run; n: the target stock of a farm order.
function Protocol.SetOrder(g, order, opts, cb)
    opts = opts or {}
    return Protocol.Request("ORDER_SET", {
        g = g, o = order, zone = opts.zone, aa = opts.aa, lc = opts.lc, cont = opts.cont,
        nd = opts.nd, gcap = opts.gcap, runs = opts.runs, n = opts.n,
    }, cb)
end

function Protocol.SetBotState(g, state, cb)
    return Protocol.Request("BOT_STATE", { g = g, s = state }, cb)
end

-- plan: { p1, p2, t1, t2, sec = { skillId, ... } }
function Protocol.SetProfessions(g, plan, cb)
    return Protocol.Request("PROF_SET", {
        g = g, p1 = plan.p1 or 0, p2 = plan.p2 or 0, t1 = plan.t1 or 0, t2 = plan.t2 or 0,
        sec = plan.sec or {},
    }, cb)
end

function Protocol.Recruit(race, class, gender, name, cb)
    return Protocol.Request("RECRUIT", { race = race, class = class, gender = gender, name = name }, cb)
end

function Protocol.TakeControl(g, cb)
    return Protocol.Request("CONTROL_TAKE", { g = g }, cb)
end

function Protocol.ReturnControl(cb)
    return Protocol.Request("CONTROL_RETURN", {}, cb)
end

-- Watch mode (docs/protocol.md §9 "Watch mode"): on = true lets the AI play the bot you control, false takes the controls back.
function Protocol.Watch(on, cb)
    return Protocol.Request("CONTROL_WATCH", { on = on and true or false }, cb)
end

function Protocol.BuyUnlock(id, cb)
    return Protocol.Request("UNLOCK_BUY", { id = id }, cb)
end

-- Guild perks (docs/protocol.md §7.15): the next rank of perk `id`, paid with guild points.
function Protocol.BuyPerk(id, cb)
    return Protocol.Request("PERK_BUY", { id = id }, cb)
end

-- Craft orders of the guild (docs/protocol.md §7.21): "make n x item"; a bot that knows a recipe makes it on its next
-- town trip, the result goes into the guild storage.
function Protocol.AddCraftOrder(item, count, cb)
    return Protocol.Request("CRAFT_ORDER_ADD", { item = item, n = count or 1 }, cb)
end

function Protocol.DeleteCraftOrder(id, cb)
    return Protocol.Request("CRAFT_ORDER_DEL", { id = id }, cb)
end

-- The item picker of the farm order (docs/protocol.md §7.23): items whose name holds the text (or the item of that
-- id) that creatures drop; g (optional) = the bot it is for, so each item says whether that bot can farm it now.
function Protocol.FarmSearch(g, text, cb)
    return Protocol.Request("FARM_SEARCH", { g = g, q = text }, cb)
end

-- The guild storage's shopping list (docs/protocol.md section 7.25): an item at least `min` in the guild storage (bots
-- buy what is missing at vendors they visit anyway, paid from the guild bank); on = false pauses it.
function Protocol.SetShoppingItem(entry, min, on, cb)
    return Protocol.Request("SHOP_SET", { e = entry, min = min or 0, on = on ~= false }, cb)
end

function Protocol.DeleteShoppingItem(entry, cb)
    return Protocol.Request("SHOP_DEL", { e = entry }, cb)
end

-- Items vendors of the guild's side sell for gold whose name holds the text (or the item of that id).
function Protocol.ShopSearch(text, cb)
    return Protocol.Request("SHOP_SEARCH", { q = text }, cb)
end

-- Guild storage (docs/protocol.md §6, §7.10): n items of a kind into the bags of the character you play.
function Protocol.StorageTake(entry, randomProperty, count, cb)
    return Protocol.Request("STORAGE_TAKE", { e = entry, rp = randomProperty or 0, n = count or 1 }, cb)
end

-- Sells n of a kind of the guild storage (0 = all) at the vendor price; the copper goes into the guild bank.
function Protocol.StorageSell(entry, randomProperty, count, cb)
    return Protocol.Request("STORAGE_SELL", { e = entry, rp = randomProperty or 0, n = count or 0 }, cb)
end

-- Destroys n of a kind of the guild storage that no vendor buys (0 = all).
-- How many of a kind the bots leave in the guild storage (0: no reserve).
function Protocol.SetStorageReserve(entry, randomProperty, reserve, cb)
    return Protocol.Request("STORAGE_RESERVE", { e = entry, rp = randomProperty or 0, n = reserve or 0 }, cb)
end

function Protocol.StorageDelete(entry, randomProperty, count, cb)
    return Protocol.Request("STORAGE_DELETE", { e = entry, rp = randomProperty or 0, n = count or 0 }, cb)
end

-- Has the guild's best enchanter in the world disenchant one of a kind of the guild storage.
function Protocol.StorageDisenchant(entry, randomProperty, cb)
    return Protocol.Request("STORAGE_DISENCHANT", { e = entry, rp = randomProperty or 0 }, cb)
end

-- The junk in the guild storage: sell = false asks what would be sold (STORAGE_JUNK, then the ACK), true sells it; the
-- copper goes into the guild bank.
function Protocol.SellStorageJunk(sell, cb)
    return Protocol.Request("STORAGE_SELL_JUNK", { sell = sell and true or false }, cb)
end

-- One stack from your bags (client bag 0-4, slot 1-n), or every material when bag is nil.
function Protocol.StorageDeposit(bag, slot, cb)
    if bag == nil then
        return Protocol.Request("STORAGE_DEPOSIT", { all = true }, cb)
    end
    return Protocol.Request("STORAGE_DEPOSIT", { b = bag, s = slot }, cb)
end

-- Loot filter (docs/protocol.md §7.11). rule: { id (0 = new), pos, act, en, lg, c (condition codec) }
function Protocol.SetLootRule(rule, cb)
    return Protocol.Request("LOOT_RULE_SET", {
        id = rule.id or 0, pos = rule.pos or 0, act = rule.act or 0, en = rule.en ~= false, lg = rule.lg or 0, c = rule.c or "",
    }, cb)
end

function Protocol.DeleteLootRule(id, cb)
    return Protocol.Request("LOOT_RULE_DEL", { id = id }, cb)
end

function Protocol.MoveLootRule(id, pos, cb)
    return Protocol.Request("LOOT_RULE_MOVE", { id = id, pos = pos }, cb)
end

function Protocol.EnableLootRule(id, enabled, cb)
    return Protocol.Request("LOOT_RULE_ENABLE", { id = id, en = enabled and true or false }, cb)
end

-- opts: on (filter), dai (default for AI-played characters), dpl (default for yours), al (auto loot)
function Protocol.SetLootOptions(opts, cb)
    return Protocol.Request("LOOT_OPTIONS", { on = opts.on, dai = opts.dai, dpl = opts.dpl, al = opts.al }, cb)
end

-- Talent templates (docs/specs.md): t = 0 (default of the class), 1 (manual) or a template id.
function Protocol.SetSpec(g, t, cb)
    return Protocol.Request("SPEC_SET", { g = g, t = t or 0 }, cb)
end

-- Dungeon roles (docs/protocol.md "Dungeon roles"): role "none", "tank" or "heal"; t = the talent template of the role
-- (0 = the default of the class for it).
-- The dungeon run window: a dungeon, the bots of the group (guids), the runs (0 = until it is stopped), and
-- optionally the bot that leads.
-- options (the preparation step, all optional): items = the kinds of items of the guild storage the group may use,
-- tl / em / cp / cpon / ff = the fight rules of the run.
function Protocol.StartDungeon(d, bots, runs, lead, cb, options)
    local fields = { d = d, bots = bots, runs = runs or 1, lead = lead }
    if options then
        -- noitems: the kinds of items the leader took out in the preparation step; every other kind is used (ia).
        -- items: only these kinds (the form of before 2026-11-14).
        if options.noitems then
            fields.ia = true
            if #options.noitems > 0 then
                fields.noitems = options.noitems
            end
        elseif options.items and #options.items > 0 then
            fields.items = options.items
        end
        fields.tl, fields.em, fields.cp, fields.cpon, fields.ff = options.tl, options.em, options.cp, options.cpon, options.ff
        fields.md, fields.gr, fields.gc = options.md, options.gr, options.gc
    end
    return Protocol.Request("DUNGEON_START", fields, cb)
end

-- g: the bot that leads the run. now: it ends at once; else the run that is going is the last.
function Protocol.StopDungeon(g, now, cb)
    return Protocol.Request("DUNGEON_STOP", { g = g, now = now and true or false }, cb)
end

function Protocol.SetRole(g, role, t, cb)
    return Protocol.Request("ROLE_SET", { g = g, role = role or "none", t = t or 0 }, cb)
end

-- Fight profiles (docs/protocol.md "Fight profiles"): the guild's profiles of a bot's class with its two slots, one
-- profile's rules and options (id 0 = the bot's template), a slot's profile (slot 0 solo, 1 group; id 0 = the default),
-- "Copy to bots" (its two slots and its own option values for the bots of the list), a bot's own value of an option
-- (nil = back to the profile's value), a new profile of the guild from the bot's template.
function Protocol.GetFightProfiles(g)
    if State.IsConnected() then
        Protocol.Send("GET", { what = "fightprofiles", g = g })
    end
end

function Protocol.GetFightProfile(g, id)
    if State.IsConnected() then
        Protocol.Send("GET", { what = "fightprofile", g = g, id = id or 0 })
    end
end

function Protocol.AssignFight(g, slot, id, cb)
    return Protocol.Request("FIGHT_ASSIGN", { g = g, slot = slot or 0, id = id or 0 }, cb)
end

function Protocol.CopyFightTo(g, bots, cb)
    return Protocol.Request("FIGHT_ASSIGN", { g = g, to = bots }, cb)
end

function Protocol.SetFightOption(g, id, key, value, cb)
    if value == nil then
        return Protocol.Request("FIGHT_OPTION", { g = g, id = id, k = key, def = true }, cb)
    end
    return Protocol.Request("FIGHT_OPTION", { g = g, id = id, k = key, v = tostring(value) }, cb)
end

function Protocol.CopyFightProfile(g, name, cb, from)
    return Protocol.Request("FIGHT_COPY", { g = g, n = name or "", from = from }, cb)
end

-- The editor (FP-3): what it offers (conditions, the bot's class spells and usable items), a whole profile saved as the
-- editor has it, a profile deleted.
function Protocol.GetFightCatalog(g)
    if State.IsConnected() then
        Protocol.Send("GET", { what = "fightcatalog", g = g })
    end
end

local function RuleField(rule)
    local fields = {
        rule.l or 0, rule.k or 0, rule.ref or 0, rule.a or "", rule.tg or "", rule.tgt or 0,
        string.format("%.2f", rule.rel or 0), rule.en == false and 0 or 1, rule.fl or 0, rule.iv or 0, rule.nd or 0,
        rule.src or "", rule.c or "",
    }
    for i = 1, #fields do
        fields[i] = tostring(fields[i])
    end
    return table.concat(fields, "\t")
end

local function OptionField(option)
    return table.concat({ option.k or "", option.n or "", tostring(option.kind or 1), tostring(option.v or 0),
        tostring(option.min or 0), tostring(option.max or 100) }, "\t")
end

-- draft = { n, note, rules = { list 0 first, then list 1 }, options = { ... } }
function Protocol.SaveFightProfile(g, id, draft, cb)
    local fields = { g = g, id = id, n = draft.n or "", note = draft.note or "", rc = #draft.rules, oc = #draft.options }
    for i, rule in ipairs(draft.rules) do
        fields["r" .. i] = RuleField(rule)
    end
    for i, option in ipairs(draft.options) do
        fields["o" .. i] = OptionField(option)
    end
    return Protocol.Request("FIGHT_SAVE", fields, cb)
end
Protocol.FightRuleField = RuleField

function Protocol.DeleteFightProfile(g, id, cb)
    return Protocol.Request("FIGHT_DEL", { g = g, id = id }, cb)
end

-- Squads (docs/protocol.md "Squads"): bot g follows the guild bot l as a squad member; l = 0 takes it out of its squad.
function Protocol.SetSquad(g, l, cb)
    return Protocol.Request("SQUAD_SET", { g = g, l = l or 0 }, cb)
end

-- Saves the talents of the character you play as a template of the guild. purpose: 0 leveling, 1 dungeon and raid,
-- 2 raid, 3 PvP.
function Protocol.SaveSpec(name, purpose, cb)
    return Protocol.Request("SPEC_SAVE", { n = name, p = purpose or 0 }, cb)
end

function Protocol.DeleteSpec(t, cb)
    return Protocol.Request("SPEC_DEL", { t = t }, cb)
end

-- Crafting from the guild storage (docs/protocol.md §7.13): g = a guild bot, nil = the character you play.
function Protocol.Craft(spellId, count, g, cb)
    return Protocol.Request("CRAFT", { s = spellId, n = count or 1, g = g }, cb)
end

function Protocol.StopCraft(spellId, g, cb)
    return Protocol.Request("CRAFT", { s = spellId, n = 1, g = g, stop = true }, cb)
end

-- Auction house surplus (docs/protocol.md §7.14): keep `keep` of a kind in the storage, auction the rest in stacks of
-- `stack` at `price` copper per item (0 = the server's default price).
function Protocol.SetAuctionRule(rule, cb)
    return Protocol.Request("AUCTION_RULE_SET", {
        e = rule.e, rp = rule.rp or 0, keep = rule.keep or 0, stack = rule.stack or 0, price = rule.price or 0, on = rule.on ~= false,
    }, cb)
end

function Protocol.DeleteAuctionRule(entry, randomProperty, cb)
    return Protocol.Request("AUCTION_RULE_DEL", { e = entry, rp = randomProperty or 0 }, cb)
end

-- An auction rule for a whole storage category ("cloth", "herb", ...): every kind of it without a rule of its own is
-- auctioned above `keep` (per kind) at the default price.
function Protocol.SetAuctionCategoryRule(rule, cb)
    return Protocol.Request("AUCTION_CAT_SET", { cat = rule.cat, keep = rule.keep or 0, stack = rule.stack or 0, on = rule.on ~= false }, cb)
end

function Protocol.DeleteAuctionCategoryRule(category, cb)
    return Protocol.Request("AUCTION_CAT_DEL", { cat = category }, cb)
end

-- Bot behaviour settings (docs/protocol.md §7.19). g = 0 (or nil): the guild's values, else one guild character's.
-- set = { [id] = value, ... } (values to store), clear = { id, ... } (back to the guild's value or the default),
-- all = true drops every value of the scope first.
function Protocol.SetSettings(g, set, clear, all, cb)
    local tuples = {}
    for id, value in pairs(set or {}) do
        tuples[#tuples + 1] = { id, value }
    end
    table.sort(tuples, function(a, b)
        return a[1] < b[1]
    end)
    local fields = { g = g or 0, v = tuples }
    if clear and #clear > 0 then
        fields.del = clear
    end
    if all then
        fields.all = true
    end
    return Protocol.Request("SETTING_SET", fields, cb)
end

function Protocol.AckAway()
    State.SetAway(nil)
    if State.IsConnected() then
        Protocol.Send("AWAY_ACK", {})
    end
end

-------------------------------------------------------------------------------
-- Swap target CVar (docs/protocol.md §7.3, read by the GlueXML auto-enter)
-------------------------------------------------------------------------------

Protocol.SWAP_TARGET_CVAR = "gaSwapTarget"
local cvarRegistered = false

function Protocol.SetSwapTargetCVar(name)
    if not cvarRegistered then
        cvarRegistered = pcall(RegisterCVar, Protocol.SWAP_TARGET_CVAR, "")
    end
    local ok, err = pcall(SetCVar, Protocol.SWAP_TARGET_CVAR, name or "")
    if not ok then
        GA.Debug("could not set " .. Protocol.SWAP_TARGET_CVAR .. ": " .. tostring(err))
    end
end

-------------------------------------------------------------------------------
-- Lifecycle
-------------------------------------------------------------------------------

GA.OnLogin(function()
    if GA.db.mock.enabled then
        GA.MockServer.Enable(true)
    end
end)

GA.On("ENTERING_WORLD", function()
    -- the swap (if any) is done: never let a stale target auto-enter a later login
    Protocol.SetSwapTargetCVar("")
    if State.mode ~= "live" then
        return
    end
    -- handshake when not connected, and after a swap that kept the UI loaded (world reload):
    -- the character changed; a world reload (Path W) changes UnitGUID("player") but keeps
    -- the old UnitName("player"), so compare both
    local guid = GA.Transport.PlayerGuid()
    local switched = (State.helloGuid ~= nil and guid ~= nil and State.helloGuid ~= guid)
        or (State.helloName ~= nil and State.helloName ~= UnitName("player"))
    if State.connection ~= "connected" or switched or State.control.busy then
        GA.After(Protocol.HELLO_DELAY, function()
            if State.mode == "live" then
                Protocol.Connect(switched or State.control.busy)
            end
        end)
    elseif Protocol.enteredWorld then
        -- A loading screen with the connection up (a teleport, an instance, a ship): what the server pushed meanwhile
        -- did not reach the addon (no addon messages during a loading screen), and it sends only what changed. A new
        -- handshake brings the whole state again.
        GA.After(Protocol.HELLO_DELAY, function()
            if State.mode == "live" and State.connection == "connected" then
                Protocol.Connect(true)
            end
        end)
    end
    Protocol.enteredWorld = true
end)

GA.On("TRANSPORT_ECHO", function()
    GA.Debug("own addon message echoed back: server module not installed")
end)

-- Readable error text for codes (docs/protocol.md §8.5/§8.6).
local ERROR_TEXT = {
    BAD_REQUEST = "The server rejected the request.",
    BAD_VERSION = "The addon is too old for this server.",
    RATE_LIMIT = "Too many requests, slow down.",
    NOT_OWNER = "That bot is not in your guild.",
    UNKNOWN_BOT = "Unknown bot.",
    LOCKED = "Not unlocked yet.",
    NOT_AVAILABLE = "Not available on this server yet.",
    NO_SLOT = "No free bot slot.",
    NAME_INVALID = "That name is not allowed.",
    NAME_TAKEN = "That name is already taken.",
    INVALID_COMBO = "That race and class combination is not possible.",
    ZONE_INVALID = "That zone cannot be used for this order.",
    NOT_ENOUGH_GOLD = "The guild bank cannot pay for that.",
    COVERED = "Your guild has that much already.",
    SWAP_REFUSED = "Control swap refused.",
    WATCH_REFUSED = "Watch mode refused.",
    SPEC_INVALID = "The talent template was refused.",
    STORAGE_REFUSED = "The guild storage refused that.",
    LOOT_REFUSED = "The loot rule was refused.",
    CRAFT_REFUSED = "Crafting refused.",
    CRAFT_ORDER_INVALID = "That cannot be ordered.",
    CRAFT_ORDER_LIMIT = "The guild has too many craft orders.",
    AUCTION_REFUSED = "The auction rule was refused.",
    NO_POINTS = "Not enough guild points.",
    MAX_RANK = "That perk is at its highest rank.",
    UNKNOWN_PERK = "Unknown perk.",
    SETTING_INVALID = "The setting was refused.",
    BUSY = "A control swap is already in progress.",
    INTERNAL = "Server error.",
    TIMEOUT = "No answer from the server.",
    NOT_CONNECTED = "Not connected to a Guild Adventure server.",
}
local REASON_TEXT = {
    IN_COMBAT = "in combat",
    LOADING = "loading or teleporting",
    VEHICLE = "on a vehicle",
    CHARMED = "charmed or possessed",
    DUEL = "in a duel",
    TRADE = "trading",
    DEAD = "dead",
    BENCHED = "benched",
    OFFLINE = "not in the world",
    SAME = "already played by you",
    FLIGHT = "on a flight path",
    CONTROLS_UNIT = "controlling another unit",
    BATTLEGROUND = "in a battleground or arena",
    LFG = "queued for or inside a dungeon finder group",
    GM_MODE = "GM mode is on",
    SELF_BOT = "running as a self-bot",
    SHUTDOWN = "the server is shutting down",
    PATH_UNAVAILABLE = "this swap path cannot reach the target there (instance, ship or Ebon Hold)",
    BUSY = "another swap is in progress",
    NOT_OWNER = "not one of your bots",
    NOT_GUILD_BOT = "not a guild bot with an order (take control of one first)",
    NO_ORDER = "your leader has no order (give it one in the Overview tab first)",
}

function Protocol.ErrorText(code, msg, reason)
    if msg and msg ~= "" then
        return msg
    end
    local text = ERROR_TEXT[code] or tostring(code)
    if reason and REASON_TEXT[reason] then
        text = text .. " (" .. REASON_TEXT[reason] .. ")"
    end
    return text
end
