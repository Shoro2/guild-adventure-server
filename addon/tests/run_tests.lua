-- Offline tests for the GuildAdventure addon.
-- Usage: lua run_tests.lua [path\to\addon\tests]   (Lua 5.1 or 5.2; see addon/tests/README.md)
-- Optional: set GA_FRAMEXML to a folder with the 3.3.5a FrameXML (UIPanelTemplates.lua) to run
-- the real tab and faux-scroll helpers instead of the stub fallbacks.

local testsDir = arg and arg[1] or (arg and arg[0] and string.match(arg[0], "^(.*)[/\\]") ) or "."
testsDir = string.gsub(testsDir, "\\", "/")
local addonDir = testsDir .. "/../GuildAdventure"

local Stub = dofile(testsDir .. "/wowstub.lua")

local frameXml = os.getenv("GA_FRAMEXML")
if frameXml and frameXml ~= "" then
    local chunk = assert(loadfile(frameXml .. "/UIPanelTemplates.lua"))
    chunk()
    print("using real FrameXML UIPanelTemplates.lua from " .. frameXml)
end

-------------------------------------------------------------------------------
-- Tiny test framework
-------------------------------------------------------------------------------

local passed, failed = 0, 0
local current = "?"

local function Check(cond, msg)
    if cond then
        passed = passed + 1
    else
        failed = failed + 1
        print("FAIL [" .. current .. "] " .. tostring(msg))
    end
    return cond
end

local function Equal(actual, expected, msg)
    return Check(actual == expected, (msg or "") .. ": expected " .. tostring(expected) .. ", got " .. tostring(actual))
end

local function Test(name, fn)
    current = name
    local errorsBefore = #Stub.errors
    local ok, err = pcall(fn)
    if not ok then
        failed = failed + 1
        print("ERROR [" .. name .. "] " .. tostring(err))
    end
    if #Stub.errors > errorsBefore then
        failed = failed + 1
        print("FAIL [" .. name .. "] Lua errors were reported through geterrorhandler")
    end
end

-------------------------------------------------------------------------------
-- Load the addon like the client does
-------------------------------------------------------------------------------

local globalsBefore = {}
for k in pairs(_G) do
    globalsBefore[k] = true
end

math.randomseed(1234)
Stub.player.level = 20 -- below 55, so Death Knight recruits stay locked
local files
Test("load toc", function()
    files = Stub.LoadToc(addonDir, "GuildAdventure.toc")
    Check(#files >= 15, "toc lists the addon files")
end)

local GA = GuildAdventure
Test("addon loaded event", function()
    Stub.FireEvent("ADDON_LOADED", "GuildAdventure")
    Stub.FireEvent("PLAYER_LOGIN")
    Check(GA.db ~= nil, "saved variables initialised")
    Check(_G.GuildAdventureMinimapButton ~= nil, "minimap button created")
end)

-------------------------------------------------------------------------------
-- Codec
-------------------------------------------------------------------------------

Test("codec escape round trip", function()
    local raw = "a~b^c|d\ne\rf\tg\0h=~c"
    local enc = GA.Codec.Escape(raw)
    Check(not string.find(enc, "[%^|\n\r\t%z]"), "escaped text has no separators or control chars")
    Equal(GA.Codec.Unescape(enc), raw, "unescape(escape(x)) == x")
    Equal(GA.Codec.Unescape("~x~"), "~x~", "unknown escape stays as is")
end)

Test("codec encode/decode", function()
    local s = GA.Codec.Encode("BOT", { g = 101, n = "Al^dric", aa = true, nd = false, sk = { { 186, 88, 150 }, { 182, 40, 75 } }, sec = { 185, 129 }, big = 3000000000 })
    Equal(s, "BOT^aa=1^big=3000000000^g=101^n=Al~cdric^nd=0^sec=185,129^sk=186:88:150,182:40:75", "encoded form")
    local op, f = GA.Codec.Decode(s)
    Equal(op, "BOT", "opcode")
    Equal(f.n, "Al^dric", "escaped value")
    local typed = GA.Codec.ApplySchema(f, GA.Protocol.SCHEMA.BOT)
    Equal(typed.g, 101, "int")
    Equal(typed.aa, true, "bool true")
    Equal(typed.nd, false, "bool false")
    Equal(typed.sk[1][3], 150, "tuple")
    Equal(typed.sec[2], 129, "intlist")
    Equal(typed.big, "3000000000", "unknown key stays a string")
    local op2, f2 = GA.Codec.Decode("ACK")
    Equal(op2, "ACK", "opcode without fields")
    Check(next(f2) == nil, "no fields")
    local _, f3 = GA.Codec.Decode("X^flag^k=a=b")
    Equal(f3.flag, "", "key without value")
    Equal(f3.k, "a=b", "split at first =")
end)

Test("protocol.md wire examples", function()
    local C = GA.Codec
    Equal(C.Encode("HELLO", { v = 1, ui = "0.1.0", zv = 0 }), "HELLO^ui=0.1.0^v=1^zv=0", "HELLO")
    Equal(C.Encode("WELCOME", { v = 1, min = 1, srv = "0.1.0", now = 1794229200, zv = 3 }),
        "WELCOME^min=1^now=1794229200^srv=0.1.0^v=1^zv=3", "WELCOME")
    Equal(C.Encode("BOT", { g = 101, n = "Aldric", race = 1, class = 1, gender = 0, lvl = 12, xp = 5120, xpmax = 9800,
        s = "active", zone = 40, zn = "Westfall", o = "AUTO", why = "Questing in Westfall: 6 eligible quests left",
        sk = { { 186, 88, 150 }, { 182, 40, 75 } } }),
        "BOT^class=1^g=101^gender=0^lvl=12^n=Aldric^o=AUTO^race=1^s=active^sk=186:88:150,182:40:75^why=Questing in Westfall: 6 eligible quests left^xp=5120^xpmax=9800^zn=Westfall^zone=40",
        "BOT")
    Equal(C.Encode("ORDER_SET", { r = 4, g = 101, o = "LEVEL_ZONE", zone = 44, aa = true }),
        "ORDER_SET^aa=1^g=101^o=LEVEL_ZONE^r=4^zone=44", "ORDER_SET")
    Equal(C.Encode("ERROR", { r = 5, code = "SWAP_REFUSED", reason = "IN_COMBAT", msg = "Aldric is in combat." }),
        "ERROR^code=SWAP_REFUSED^msg=Aldric is in combat.^r=5^reason=IN_COMBAT", "ERROR")
    Equal(C.Encode("EVENT", { id = 88, t = 1794229260, type = "INFO", g = 0, msg = "Rate: 50% ^ 2 ~ |cff" }),
        "EVENT^g=0^id=88^msg=Rate: 50% ~c 2 ~~ ~pcff^t=1794229260^type=INFO", "EVENT escaping")
    local chunks = GA.Transport.Split(string.rep("x", 500), "S", 12)
    Equal(#chunks, 3, "500 bytes -> 3 chunks")
    Equal(chunks[1], "S12,1,3:" .. string.rep("x", 228), "first chunk")
    Equal(chunks[3], "S12,3,3:" .. string.rep("x", 44), "last chunk")
end)

-- docs/protocol-vectors.txt: shared with the C++ module, generated by an independent encoder.
local function LoadVectors(path)
    local vectors, v = {}, nil
    local CESC = { ["\\"] = "\\", n = "\n", r = "\r", t = "\t", ["0"] = "\0" }
    for line in io.lines(path) do
        line = string.gsub(line, "\r$", "")
        local kind, rest = string.match(line, "^(%a+) (.*)$")
        if kind == "vector" then
            v = { name = rest, fields = {}, chunks = {} }
            vectors[#vectors + 1] = v
        elseif v and kind == "op" then
            v.op = rest
        elseif v and kind == "field" then
            local k, raw = string.match(rest, "^([^=]*)=(.*)$")
            v.fields[k] = string.gsub(raw, "\\(.)", CESC)
        elseif v and kind == "encoded" then
            v.encoded = rest
        elseif v and kind == "split" then
            local dir, id = string.match(rest, "^(%a) (%d+)$")
            v.dir, v.id = dir, tonumber(id)
        elseif v and kind == "chunk" then
            v.chunks[#v.chunks + 1] = rest
        end
    end
    return vectors
end

Test("protocol-vectors.txt", function()
    local vectors = LoadVectors(testsDir .. "/../../docs/protocol-vectors.txt")
    Check(#vectors >= 10, "vectors loaded (" .. #vectors .. ")")
    for _, v in ipairs(vectors) do
        Equal(GA.Codec.Encode(v.op, v.fields), v.encoded, v.name .. ": encode")
        local op, fields = GA.Codec.Decode(v.encoded)
        Equal(op, v.op, v.name .. ": decoded opcode")
        local count = 0
        for k, value in pairs(fields) do
            count = count + 1
            Equal(value, v.fields[k], v.name .. ": decoded field " .. k)
        end
        for _ in pairs(v.fields) do
            count = count - 1
        end
        Equal(count, 0, v.name .. ": same number of fields")
        local chunks = GA.Transport.Split(v.encoded, v.dir, v.id)
        Equal(#chunks, #v.chunks, v.name .. ": chunk count")
        for i = 1, #v.chunks do
            Equal(chunks[i], v.chunks[i], v.name .. ": chunk " .. i)
        end
        local got
        local r = GA.Transport.NewReassembler(v.dir, function(m) got = m end)
        for i = #v.chunks, 1, -1 do
            r:Feed(v.chunks[i], 0)
        end
        Equal(got, v.encoded, v.name .. ": reassembled")
    end
end)

-------------------------------------------------------------------------------
-- Chunk layer
-------------------------------------------------------------------------------

Test("chunk split and reassembly", function()
    local T = GA.Transport
    -- 3-byte UTF-8 characters force cuts that must move back to a boundary
    local msg = "EVENT^msg=" .. string.rep("\226\134\146ab", 300)
    local chunks = T.Split(msg, "S", 7)
    Check(#chunks > 1, "long message is split")
    for _, c in ipairs(chunks) do
        Check(string.len(c) <= T.MAX_CHUNK, "chunk within 240 bytes")
        local _, _, _, _, body = T.ParseChunk(c)
        local first = string.byte(body, 1)
        Check(not first or first < 0x80 or first >= 0xC0, "chunk body does not start inside a UTF-8 sequence")
    end
    local got = {}
    local r = T.NewReassembler("S", function(m) got[#got + 1] = m end)
    -- deliver out of order, with a duplicate and a foreign chunk
    Equal(r:Feed("C1,1,1:HELLO", 0), "foreign", "wrong direction")
    Equal(r:Feed("garbage", 0), "bad", "malformed")
    Equal(r:Feed("S1,2,1:x", 0), "bad", "seq > total")
    for i = #chunks, 1, -1 do
        r:Feed(chunks[i], 0)
        if i == #chunks then
            r:Feed(chunks[i], 0)
        end
    end
    Equal(#got, 1, "one message completed")
    Equal(got[1], msg, "reassembled message equals original")
    Check(T.Split(string.rep("y", 64 * 228 + 1), "S", 1) == nil, "more than 64 chunks is refused")
    -- expiry
    r:Feed("S9,1,2:half", 0)
    Equal(r.count, 1, "one partial")
    r:Expire(20)
    Equal(r.count, 0, "partial expired after 15 s")
end)

-------------------------------------------------------------------------------
-- Live mode without a server
-------------------------------------------------------------------------------

Test("live handshake retries and fails", function()
    Stub.sentAddon = {}
    Stub.FireEvent("PLAYER_ENTERING_WORLD")
    Stub.Advance(3)
    Equal(#Stub.sentAddon, 1, "HELLO sent after entering world")
    local sent = Stub.sentAddon[1]
    Equal(sent.prefix, "GADV", "prefix")
    Equal(sent.distribution, "WHISPER", "whisper to self")
    Equal(sent.target, "Tester", "target is own name")
    Check(string.match(sent.text, "^C%d+,1,1:HELLO%^"), "HELLO chunk format: " .. sent.text)
    Equal(GA.State.connection, "connecting", "connecting")
    Stub.Advance(40)
    Equal(#Stub.sentAddon, 3, "two retries")
    Equal(GA.State.connection, "failed", "no server -> failed")
end)

Test("live echo detection", function()
    Stub.sentAddon = {}
    Stub.onAddonSend = function(prefix, text, dist, target)
        -- realm without the module: the core delivers our whisper back to us
        GA.Transport.OnAddonMessage(prefix, text, dist, target)
    end
    GA.Protocol.Connect(true)
    Stub.Advance(40)
    Equal(GA.State.connection, "missing", "echo -> server module missing")
    Stub.onAddonSend = nil
end)

Test("live mode with a scripted server", function()
    -- A fake server answering on the wire: proves the client parses S chunks from CHAT_MSG_ADDON.
    Stub.onAddonSend = function(prefix, text)
        local _, _, _, _, body = GA.Transport.ParseChunk(text)
        if body and string.match(body, "^HELLO") then
            local id = 0
            local function Send(op, fields)
                id = id + 1
                for _, c in ipairs(GA.Transport.Split(GA.Codec.Encode(op, fields), "S", id)) do
                    Stub.FireEvent("CHAT_MSG_ADDON", "GADV", c, "WHISPER", "Tester-Realm")
                end
            end
            Send("WELCOME", { v = 1, min = 1, srv = "test", now = time(), zv = 7 })
            Send("STATE", { guild = "Wire Test", fac = "A", rl = 1, slots = 3, used = 1, feat = { "orders" }, orders = { "AUTO", "IDLE" } })
            Send("ZONES_BEGIN", { zv = 7, n = 1 })
            Send("ZONE", { id = 12, n = "Elwynn Forest", min = 1, max = 10, fac = "A", cont = 0 })
            Send("ZONES_END", {})
            Send("ROSTER_BEGIN", { n = 1 })
            Send("BOT", { g = 5, n = "Wirebot", race = 1, class = 1, lvl = 3, s = "active", o = "AUTO", why = string.rep("long ", 80) })
            Send("ROSTER_END", {})
            -- foreign sender is ignored
            Stub.FireEvent("CHAT_MSG_ADDON", "GADV", "S99,1,1:BOT^g=6^n=Intruder", "WHISPER", "Someone")
        end
    end
    GA.Protocol.Connect(true)
    Stub.Advance(1)
    Equal(GA.State.connection, "connected", "connected over the wire")
    Equal(GA.State.guild.guild, "Wire Test", "STATE applied")
    Equal(GA.State.BotCount(), 1, "one bot, the foreign sender was ignored")
    Equal(GA.State.bots[5].n, "Wirebot", "multi-chunk BOT reassembled")
    Equal(GA.db.zoneCache.zv, 7, "zone cache stored for live mode")
    Stub.onAddonSend = nil
end)

Test("warm relog swap over the wire (Path R)", function()
    local id = 100
    local function Send(op, fields)
        id = id + 1
        for _, c in ipairs(GA.Transport.Split(GA.Codec.Encode(op, fields), "S", id)) do
            Stub.FireEvent("CHAT_MSG_ADDON", "GADV", c, "WHISPER", UnitName("player"))
        end
    end
    Send("CONTROL_STATE", { leader = 1, ln = "Tester", g = 1, cn = "Tester", busy = false })
    -- old session: the server accepts the swap and announces the relog
    Stub.onAddonSend = function(prefix, text)
        local _, _, _, _, body = GA.Transport.ParseChunk(text)
        local op, f = GA.Codec.Decode(body)
        if op == "CONTROL_TAKE" then
            Send("CONTROL_STATE", { busy = true })
            Send("ACK", { r = f.r })
            Send("CONTROL_PENDING", { g = f.g, cn = "Wirebot" })
        end
    end
    GA.UI.ShowBot(5)
    Stub.Advance(0.3)
    GA.UI.TakeControl(5)
    Stub.Advance(1)
    Equal(Stub.cvars.gaSwapTarget, "Wirebot", "swap target stored in the CVar for GlueXML")
    Check(GA.db.ui.resume and GA.db.ui.resume.page == "bot", "open window remembered")

    -- new session: the client relogged into the bot; every addon starts fresh
    Stub.player.name = "Wirebot"
    Stub.player.guid = "0x0000000000000005"
    _G.GuildAdventureMainFrame:Hide()
    GA.State.Reset()
    GA.Transport.SetBackend("live")
    Stub.onAddonSend = function(prefix, text)
        local _, _, _, _, body = GA.Transport.ParseChunk(text)
        if body and string.match(body, "^HELLO") then
            Send("WELCOME", { v = 1, min = 1, srv = "test", now = time(), zv = 7 })
            Send("STATE", { guild = "Wire Test", fac = "A", rl = 1, slots = 3, used = 1 })
            Send("CONTROL_STATE", { leader = 1, ln = "Tester", g = 5, cn = "Wirebot", busy = false })
            Send("ROSTER_BEGIN", { n = 1 })
            Send("BOT", { g = 5, n = "Wirebot", race = 1, class = 1, lvl = 3, s = "controlled", o = "AUTO" })
            Send("ROSTER_END", {})
        end
    end
    Stub.FireEvent("PLAYER_ENTERING_WORLD")
    Equal(Stub.cvars.gaSwapTarget, "", "CVar cleared on entering the world")
    Stub.Advance(3)
    Equal(GA.State.connection, "connected", "new session connected")
    Check(_G.GuildAdventureMainFrame:IsShown(), "window reopened after the relog")
    Equal(GA.UI.CurrentPage(), "bot", "same page")
    Equal(GA.UI.SelectedBot(), 5, "same bot")
    Check(GA.db.ui.resume == nil, "resume consumed")
    Check(_G.GuildAdventureControlBar:IsShown(), "control bar shown while playing the bot")
    Check(string.find(GA.Protocol.ErrorText("SWAP_REFUSED", nil, "FLIGHT"), "flight", 1, true), "reason text for FLIGHT")

    Stub.onAddonSend = nil
    Stub.player.name = "Tester"
    Stub.player.guid = "0x0000000000000001"
    _G.GuildAdventureMainFrame:Hide()
end)

Test("world reload swap over the wire (Path W)", function()
    -- Measured on the real client (take-control T2, 2026-11-09): after a world reload
    -- UnitGUID("player") is the new character's, UnitName("player") still the old one.
    local id = 200
    local controlled = 1
    local function ServerName()
        return controlled == 1 and "Tester" or "Wirebot"
    end
    local function Send(op, fields)
        id = id + 1
        for _, c in ipairs(GA.Transport.Split(GA.Codec.Encode(op, fields), "S", id)) do
            -- the server always whispers as the character the session plays now
            Stub.FireEvent("CHAT_MSG_ADDON", "GADV", c, "WHISPER", ServerName())
        end
    end
    local lastTarget
    Stub.onAddonSend = function(prefix, text, dist, target)
        lastTarget = target
        local _, _, _, _, body = GA.Transport.ParseChunk(text)
        local op, f = GA.Codec.Decode(body)
        if op == "HELLO" then
            Send("WELCOME", { v = 1, min = 1, srv = "test", now = time(), zv = 7 })
            Send("CONTROL_STATE", { leader = 1, ln = "Tester", g = controlled, cn = ServerName(), busy = false })
            Send("ROSTER_BEGIN", { n = 1 })
            Send("BOT", { g = 5, n = "Wirebot", race = 1, class = 1, lvl = 3, s = controlled == 5 and "controlled" or "active", o = "AUTO" })
            Send("ROSTER_END", {})
        elseif op == "CONTROL_TAKE" or op == "CONTROL_RETURN" then
            Send("CONTROL_STATE", { busy = true, path = "reload" })
            Send("ACK", { r = f.r })
        end
    end
    GA.Protocol.Connect(true)
    Stub.Advance(1)
    Equal(GA.State.control.g, 1, "playing the leader")
    Equal(lastTarget, "Tester", "whisper to the leader")

    GA.UI.ShowBot(5)
    Stub.Advance(0.3)
    GA.UI.TakeControl(5)
    Stub.Advance(1)
    Equal(GA.State.control.path, "reload", "swap path known")
    Equal(GA.State.control.busy, true, "swap running")

    -- loading screen over: new GUID, old name; the server pushes the result as the bot
    controlled = 5
    Stub.player.guid = "0x0000000000000005"
    Send("CONTROL_STATE", { leader = 1, g = 5, cn = "Wirebot", busy = false })
    Stub.FireEvent("PLAYER_ENTERING_WORLD")
    Stub.Advance(3)
    Equal(UnitName("player"), "Tester", "the client still reports the old name")
    Equal(GA.State.control.g, 5, "push from the bot accepted (it is one of our characters)")
    Equal(GA.Transport.SelfName(), "Wirebot", "own name resolved through the GUID")
    Equal(lastTarget, "Wirebot", "new handshake whispered to the bot, not to the old name")
    Equal(GA.State.helloGuid, 5, "handshake from the bot's GUID")
    Equal(GA.State.control.busy, false, "swap over")
    Equal(GA.State.control.path, "", "swap path forgotten")
    Check(_G.GuildAdventureMainFrame:IsShown(), "window stayed open")
    Check(GA.db.ui.resume == nil, "nothing left to resume")
    Check(_G.GuildAdventureControlBar:IsShown(), "control bar shown")

    -- back to the leader; this time the completion push is lost, the GUID change alone
    -- triggers the new handshake
    GA.UI.ReturnControl()
    Stub.Advance(1)
    controlled = 1
    Stub.player.guid = "0x0000000000000001"
    Stub.FireEvent("PLAYER_ENTERING_WORLD")
    Stub.Advance(3)
    Equal(lastTarget, "Tester", "handshake whispered to the leader again")
    Equal(GA.State.control.g, 1, "back on the leader after the sync")
    Check(not _G.GuildAdventureControlBar:IsShown(), "control bar hidden")
    -- strangers still cannot talk to the addon
    Stub.FireEvent("CHAT_MSG_ADDON", "GADV", "S999,1,1:BOT^g=77^n=Spoof", "WHISPER", "Someone")
    Check(GA.State.bots[77] == nil, "whisper from a foreign character ignored")
    Check(string.find(GA.Protocol.ErrorText("SWAP_REFUSED", nil, "PATH_UNAVAILABLE"), "instance", 1, true),
        "reason text for PATH_UNAVAILABLE")

    Stub.onAddonSend = nil
    _G.GuildAdventureMainFrame:Hide()
end)

Test("key bindings", function()
    local names = 0
    local body = {}
    for line in io.lines(addonDir .. "/Bindings.xml") do
        line = string.gsub(line, "\r$", "")
        local name = string.match(line, '<Binding name="([%w_]+)"')
        if name then
            names = names + 1
            Check(type(_G["BINDING_NAME_" .. name]) == "string", "label for binding " .. name)
        elseif not string.match(line, "^%s*</?Binding") and string.match(line, "%S") then
            body[#body + 1] = line
        end
    end
    Equal(names, 4, "four bindings")
    Check(type(BINDING_HEADER_GUILDADVENTURE) == "string", "binding header label")
    for _, code in ipairs(body) do
        local chunk = assert(loadstring(code))
        local ok, err = pcall(chunk)
        Check(ok, "binding code runs: " .. code .. " " .. tostring(err))
    end
    _G.GuildAdventureMainFrame:Hide()
end)

-------------------------------------------------------------------------------
-- Mock server and UI
-------------------------------------------------------------------------------

local function Bots()
    return GA.State.SortedBots("slot", false)
end

local function MockState()
    return GA.db.mock.state
end

local function Pump(seconds)
    Stub.Advance(seconds or 1)
end

Test("demo mode connects", function()
    SlashCmdList["GUILDADVENTURE"]("mock on")
    Pump(1.5)
    Equal(GA.State.mode, "mock", "mock mode")
    Equal(GA.State.connection, "connected", "connected to mock")
    Equal(GA.State.BotCount(), 3, "three starter bots")
    Equal(#GA.State.zoneList, #GA.MockServer.ZONES, "zones synced")
    Equal(#GA.State.unlocks, #GA.MockServer.UNLOCKS, "unlocks synced")
    Equal(GA.State.guild.rl, 2, "renown level 2")
    Equal(GA.State.guild.slots, 5, "5 slots")
    Check(GA.State.HasFeature("professions"), "professions unlocked")
    Check(not GA.State.HasFeature("auto_limits"), "auto limits locked")
    Equal(GA.State.control.g, GA.State.control.leader, "playing the leader")
end)

Test("main window and tabs", function()
    SlashCmdList["GUILDADVENTURE"]("")
    Pump(0.2)
    local main = _G.GuildAdventureMainFrame
    Check(main and main:IsShown(), "main window shown")
    for i, tab in ipairs(GA.UI.TABS) do
        _G["GuildAdventureMainFrameTab" .. i]:Click()
        Pump(0.2)
        Equal(GA.UI.CurrentPage(), tab.page, "tab " .. tab.label)
    end
    local seenOverview = false
    for _, line in ipairs(Stub.chat) do
        seenOverview = seenOverview or string.find(line.text, "Demo mode on", 1, true) ~= nil
    end
    Check(seenOverview, "demo mode message printed")
end)

Test("tiered guild achievements", function()
    Equal(#GA.State.milestones, #GA.MockServer.MILESTONES, "milestone series synced")
    local warrior = GA.State.milestones[1]
    Equal(warrior.s, "level_class_1", "first series is the warrior's")
    Equal(#warrior.th, 8, "eight warrior tiers, two of them above the level cap")
    Equal(warrior.pt[6], 10, "the last active tier gives double points")
    Equal(GA.State.NextMilestoneTier(warrior), 3, "next warrior tier is level 30")
    Equal(GA.State.guild.ms, 8, "STATE counts the reached tiers")
    GA.UI.ShowPage("achievements")
    Pump(0.3)
    Equal(GA.UI.CurrentPage(), "achievements", "achievements page shown")
    Equal(#GA.UI.AchievementSeries("all"), #GA.MockServer.MILESTONES, "every series of the guild's faction listed")
    Equal(#GA.UI.AchievementSeries("race"), 1, "one race series")
    Equal(#GA.UI.AchievementSeries("kills"), 1, "one kill series")
    local found = false
    for _, o in ipairs(Stub.Objects()) do
        if rawget(o, "__type") == "FontString" and o:IsVisible() and string.find(o:GetText() or "", "highest level 24", 1, true) then
            found = true
        end
    end
    Check(found, "a row shows the warrior's highest level")
end)

Test("roster rows and sorting", function()
    GA.UI.ShowPage("roster")
    Pump(0.2)
    local items = 0
    for _ = 1, 1 do
        items = #Bots()
    end
    Equal(items, 3, "three bots in state")
    -- the roster list shows bots, 2 free slots and one locked line
    local page
    for _, o in ipairs(Stub.Objects()) do
        if o.rows and o.items and #o.items == 6 then
            page = o
        end
    end
    Check(page ~= nil, "roster list has 3 bots + 2 free + 1 locked item")
    if page then
        Equal(page.items[4].kind, "free", "free slot row")
        Equal(page.items[6].kind, "locked", "locked slots row")
        Check(#page.rows >= 6, "enough rows for the list height (" .. #page.rows .. ")")
        page.rows[1]:Click()
        Pump(0.2)
        Equal(GA.UI.CurrentPage(), "bot", "row click opens bot detail")
    end
end)

Test("roster: gear level, role and profession columns", function()
    GA.UI.ShowPage("roster")
    Pump(0.2)
    local page, header
    for _, o in ipairs(Stub.Objects()) do
        if o.rows and o.items and #o.items == 6 then
            page = o
        end
        if rawget(o, "cells") and rawget(o, "cells").prof and rawget(o, "cells").prof.key == "prof" then
            header = o
        end
    end
    Check(page ~= nil, "roster list found")
    Check(header ~= nil, "roster header with the new columns found")
    if not page or not header then
        return
    end
    local function RowOf(bot)
        for _, row in ipairs(page.rows) do
            if row.item and row.item.kind == "bot" and row.item.bot.g == bot.g then
                return row
            end
        end
    end
    -- every bot shows its gear level (glk, also while benched), a role and its two primary professions
    for _, bot in ipairs(Bots()) do
        local row = RowOf(bot)
        Check(row ~= nil, "row for " .. tostring(bot.n))
        if row then
            local gl = GA.State.BotGearLevel(bot)
            Check(gl ~= nil, "gear level known for " .. tostring(bot.n))
            Check(string.find(row.cells.ilvl:GetText() or "", tostring(gl), 1, true) ~= nil, "iLvl cell shows " .. tostring(gl))
            local role = row.cells.role:GetText() or ""
            Check(string.find(role, "DPS", 1, true) or string.find(role, "Tank", 1, true) or string.find(role, "Heal", 1, true),
                "role cell shows a role: " .. role)
            local primary = GA.State.BotProfessions(bot)
            local prof = row.cells.prof:GetText() or ""
            Equal(#primary, 2, "two primary professions for " .. tostring(bot.n))
            Check(string.find(prof, "|T", 1, true) ~= nil, "profession cell has an icon")
            Check(string.find(prof, tostring(primary[1].value), 1, true) ~= nil, "profession cell shows the best skill")
            Check(string.find(prof, tostring(primary[2].value), 1, true) ~= nil, "profession cell shows the second skill")
        end
    end
    -- sort by the highest profession skill (highest first), then by gear level
    header.cells.prof:Click()
    Pump(0.2)
    Equal(GA.db.ui.rosterSort.key, "prof", "sorted by professions")
    Check(GA.db.ui.rosterSort.desc, "highest skill first")
    local best = -1
    for _, bot in ipairs(Bots()) do
        local primary = GA.State.BotProfessions(bot)
        best = math.max(best, primary[1] and primary[1].value or -1)
    end
    local first = GA.State.BotProfessions(page.items[1].bot)
    Equal(first[1].value, best, "the best profession skill comes first")
    header.cells.ilvl:Click()
    Pump(0.2)
    Equal(GA.db.ui.rosterSort.key, "ilvl", "sorted by gear level")
    Check((GA.State.BotGearLevel(page.items[1].bot) or 0) >= (GA.State.BotGearLevel(page.items[2].bot) or 0),
        "the highest gear level comes first")
    -- a dungeon role the owner set: bright, sorted first, named in the tooltip
    local tankBot = page.items[3].bot
    local oldRole = tankBot.dr
    tankBot.dr = "tank"
    header.cells.role:Click()
    Pump(0.2)
    Equal(GA.db.ui.rosterSort.key, "role", "sorted by role")
    Equal(page.items[1].bot.g, tankBot.g, "the tank comes first")
    local row = RowOf(tankBot)
    Check(row and string.find(row.cells.role:GetText() or "", "|cff60a0ffTank", 1, true) ~= nil, "a set role is bright")
    tankBot.dr = oldRole
    -- benched: the last known gear level stays, in grey
    local benched = page.items[2].bot
    local oldState, oldGl = benched.s, benched.gl
    benched.s, benched.gl = "benched", nil
    GA.UI.RequestRefresh()
    Pump(0.2)
    row = RowOf(benched)
    Check(row and string.find(row.cells.ilvl:GetText() or "", "|cff9d9d9d" .. tostring(benched.glk), 1, true) ~= nil,
        "a benched bot keeps its last known gear level in grey")
    benched.s, benched.gl = oldState, oldGl
    GA.db.ui.rosterSort = { key = "slot", desc = false }
    GA.UI.RequestRefresh()
    Pump(0.2)
end)

Test("bot detail sub-tabs", function()
    local first = Bots()[1]
    GA.UI.ShowBot(first.g)
    Pump(0.2)
    for i = 1, 8 do
        _G["GuildAdventureBotDetailTab" .. i]:Click()
        Pump(0.2)
    end
    _G["GuildAdventureBotDetailTab1"]:Click()
    Pump(0.2)
    Check(_G.GuildAdventureBotDetail:IsShown(), "detail page visible")
end)

local function FindDropDown(predicate)
    for _, o in ipairs(Stub.Objects()) do
        if rawget(o, "builder") and rawget(o, "initialize") and predicate(o) then
            return o
        end
    end
end

Test("set LEVEL_ZONE order through the UI", function()
    local bot = Bots()[1]
    GA.UI.ShowBot(bot.g)
    GA.UI.ShowBotSubtab("orders")
    Pump(0.2)
    local orderDD = FindDropDown(function(dd)
        local items = Stub.OpenDropDown(dd, 1)
        return items[1] and items[1].arg1 == "AUTO"
    end)
    Check(orderDD ~= nil, "order dropdown found")
    local items = Stub.OpenDropDown(orderDD, 1)
    local levelZone
    for _, item in ipairs(items) do
        if item.arg1 == "LEVEL_ZONE" then
            levelZone = item
        end
        if item.arg1 == "GRIND" then
            Check(item.disabled, "GRIND is not offered by the server yet")
        end
    end
    Stub.ClickItem(levelZone)
    Pump(0.2)
    local zoneDD = FindDropDown(function(dd)
        local list = Stub.OpenDropDown(dd, 1)
        return list[1] and list[1].isTitle and string.find(list[1].text or "", "Recommended", 1, true)
    end)
    Check(zoneDD ~= nil, "zone dropdown found")
    local zoneItems = Stub.OpenDropDown(zoneDD, 1)
    local recommended
    for _, item in ipairs(zoneItems) do
        if item.func and not recommended then
            recommended = item
        end
    end
    Check(recommended ~= nil, "a recommended zone exists for level " .. bot.lvl)
    local sub = Stub.OpenDropDown(zoneDD, 2, "cont:0")
    Check(#sub > 3, "Eastern Kingdoms submenu lists zones")
    Stub.ClickItem(recommended)
    Pump(0.2)
    -- click Apply order
    local apply
    for _, o in ipairs(Stub.Objects()) do
        if rawget(o, "__type") == "Button" and o:GetText() == "Apply order" then
            apply = o
        end
    end
    apply:Click()
    Pump(1)
    local updated = GA.State.bots[bot.g]
    Equal(updated.o, "LEVEL_ZONE", "order applied by the mock server")
    Equal(updated.oz, recommended.arg1, "zone applied")
end)

Test("opening the orders tab leaves no unsaved changes", function()
    -- regression from the first in-client screenshots: the deferred OnTextChanged of the
    -- limit boxes marked the untouched draft as changed
    local bot = Bots()[2]
    GA.UI.ShowBot(bot.g)
    GA.UI.ShowBotSubtab("orders")
    Pump(0.5)
    local orders
    for _, o in ipairs(Stub.Objects()) do
        if rawget(o, "levelCap") and rawget(o, "draft") then
            orders = o
        end
    end
    Check(orders ~= nil, "orders sub-page found")
    -- a fresh page: the refresh writes the boxes, the client then fires OnTextChanged
    orders.dirty = false
    orders.levelCap:SetText("")
    orders.goldCap:SetText("")
    Pump(0.1)
    orders.dirty = false
    orders.levelCap:SetText("")
    orders.goldCap:SetText("")
    GA.UI.RefreshNow()
    Pump(0.3)
    Check(not orders.dirty, "draft not marked as changed by the page's own refresh")
    Pump(5) -- a mock tick refreshes the page
    local flagged = false
    for _, o in ipairs(Stub.Objects()) do
        if rawget(o, "__type") == "FontString" and o:IsVisible()
            and string.find(o:GetText() or "", "Unsaved changes", 1, true) then
            flagged = true
        end
    end
    Check(not flagged, "no 'Unsaved changes' shown")
end)

Test("autopilot limits locked, then unlocked by renown", function()
    local bot = Bots()[2]
    local done
    GA.Protocol.SetOrder(bot.g, "AUTO", { lc = 20 }, function(ok, code) done = { ok, code } end)
    Pump(1)
    Equal(done and done[2], "LOCKED", "limits rejected while locked")
    SlashCmdList["GUILDADVENTURE"]("mock renown 2000")
    Pump(1)
    Check(GA.State.guild.rl >= 3, "renown level 3 reached")
    Check(GA.State.HasFeature("auto_limits"), "auto limits unlocked")
    done = nil
    GA.Protocol.SetOrder(bot.g, "AUTO", { lc = 20, cont = 0, nd = true, gcap = 50000 }, function(ok, code) done = { ok, code } end)
    Pump(1)
    Equal(done and done[1], true, "limits accepted")
    Equal(GA.State.bots[bot.g].lc, 20, "level cap stored")
    Equal(GA.State.bots[bot.g].cont, 0, "continent stored")
end)

Test("profession plan", function()
    local bot = Bots()[1]
    local done
    GA.Protocol.SetProfessions(bot.g, { p1 = 171, p2 = 182, t1 = 300, t2 = 300, sec = { 185, 356 } }, function(ok) done = ok end)
    Pump(1)
    Equal(done, true, "plan accepted")
    local b = GA.State.bots[bot.g]
    Equal(b.p1, 171, "primary 1")
    Equal(b.sec[2], 356, "secondary list")
    done = nil
    GA.Protocol.SetProfessions(bot.g, { p1 = 171, p2 = 171 }, function(ok, code) done = code end)
    Pump(1)
    Equal(done, "BAD_REQUEST", "same profession twice rejected")
    GA.UI.ShowBot(bot.g)
    GA.UI.ShowBotSubtab("professions")
    Pump(0.3)
end)

Test("gear level and dungeon readiness", function()
    -- The highest of the mock's bots: the first dungeons come into its reach.
    local bot = Bots()[1]
    for _, other in ipairs(Bots()) do
        if (other.lvl or 0) > (bot.lvl or 0) then
            bot = other
        end
    end
    Check(bot.gl and bot.gl > 0, "a bot in the world has a gear level")
    Equal(type(bot.rdy), "table", "its readiness list arrived with the BOT record")
    GA.UI.ShowBot(bot.g)
    GA.UI.ShowBotSubtab("character")
    Pump(1)
    Check(GA.State.dungeonsLoaded, "the Character sub-tab asked for the dungeons")
    Equal(GA.State.dungeons[6].n, "Deadmines", "dungeon records are kept by id")
    Equal(GA.State.dungeons[48].raid, true, "a raid is marked")
    local rows = GA.UI.BotReadinessRows(GA.State.bots[bot.g])
    Check(#rows >= 1, "the bot has dungeons in its reach")
    for i = 2, #rows do
        Check((rows[i - 1].d.min or 0) <= (rows[i].d.min or 0) or rows[i].d.raid, "sorted by level, raids last")
    end
    -- The mock's numbers follow the server's rule: a level-19 character in gear level 16 at the Deadmines (15-25,
    -- made for 19, asks for gear level 17) is 94 % ready; below the lowest level it is 0.
    local list = GA.MockServer.Readiness({ lvl = 19, gl = 16 })
    local byId = {}
    for _, pair in ipairs(list) do
        byId[pair[1]] = pair[2]
    end
    Equal(byId[6], 94, "level part 100, gear part 94")
    Equal(byId[48], nil, "Molten Core is out of a level-19 character's reach")
    Equal(GA.MockServer.Readiness({ lvl = 14, gl = 14 })[1][2], 0, "below the lowest level: in reach, not ready")
    Equal(#GA.State.BotReadiness({ rdy = { { 9999, 50 } } }), 0, "an unknown dungeon id is left out")
end)

Test("the dungeon order", function()
    Check(GA.State.IsOrderAllowed("DUNGEON"), "the server offers the dungeon order")
    Check(GA.State.IsOrderAllowed("CRAFT"), "... and the profession mode")
    local bot = Bots()[1]
    for _, other in ipairs(Bots()) do
        if (other.lvl or 0) > (bot.lvl or 0) then
            bot = other
        end
    end
    GA.UI.ShowBot(bot.g)
    GA.UI.ShowBotSubtab("orders")
    Pump(1)
    Check(GA.State.dungeonsLoaded, "the dungeons are known")
    local choices = GA.UI.DungeonChoices(GA.State.bots[bot.g])
    Check(#choices >= 1, "the bot has dungeons to choose from")
    for _, entry in ipairs(choices) do
        Check(not entry.d.raid and entry.d.run ~= false, entry.d.n .. " can be run and is no raid")
    end
    local roles = GA.UI.SquadRoles(GA.State.bots[bot.g])
    Check(roles.size >= 1 and roles.tank + roles.heal + roles.dps == roles.size, "the group's roles add up")
    local done, code
    GA.Protocol.SetOrder(bot.g, "DUNGEON", { zone = choices[1].d.id, runs = 3 }, function(ok, c) done, code = ok, c end)
    Pump(1)
    Equal(done, true, "dungeon order accepted")
    Equal(GA.State.bots[bot.g].o, "DUNGEON", "the bot has the order")
    Equal(GA.State.bots[bot.g].druns, 3, "the order has its three runs")
    Equal(GA.State.bots[bot.g].ozn, choices[1].d.n, "its dungeon is named")
    done, code = nil, nil
    GA.Protocol.SetOrder(bot.g, "DUNGEON", { zone = 8 }, function(ok, c) done, code = ok, c end)
    Pump(1)
    Equal(code, "DUNGEON_INVALID", "a dungeon that cannot be run is refused")
    GA.Protocol.SetOrder(bot.g, "AUTO", {}, function() end)
    Pump(1)
end)

Test("quest partners in the BOT record", function()
    local _, f = GA.Codec.Decode("BOT^g=7^sq=5^sqq=244^sqqn=Encroaching Gnolls^qw=246,180^qwn=Assessing the Threat; Lieutenant Fangore")
    local typed = GA.Codec.ApplySchema(f, GA.Protocol.SCHEMA.BOT)
    Equal(typed.sqq, 244, "the quest of the partner squad is a number")
    Equal(typed.sqqn, "Encroaching Gnolls", "... with its title")
    Equal(typed.qw[2], 180, "the quests kept for a partner are a list of numbers")
    Equal(typed.qwn, "Assessing the Threat; Lieutenant Fangore", "... with their titles")
end)

Test("the farm order", function()
    Check(GA.State.IsOrderAllowed("FARM"), "the server offers the farm order")
    Equal(GA.UI.FarmQuery(" l "), nil, "one letter is no search")
    Equal(GA.UI.FarmQuery("  linen "), "linen", "a name is trimmed")
    Equal(GA.UI.FarmQuery("|cffffffff|Hitem:2589:0:0:0:0:0:0:0:80|h[Linen Cloth]|h|r"), "2589", "an item link gives its id")
    Equal(GA.UI.FarmQuery("4306"), "4306", "an id is a search")
    -- the lowest and the highest bot
    local low, high = Bots()[1], Bots()[1]
    for _, other in ipairs(Bots()) do
        if (other.lvl or 0) < (low.lvl or 0) then
            low = other
        end
        if (other.lvl or 0) > (high.lvl or 0) then
            high = other
        end
    end
    GA.UI.ShowBot(high.g)
    GA.UI.ShowBotSubtab("orders")
    Pump(1)
    local done, code
    GA.Protocol.FarmSearch(high.g, "cloth", function(ok, c) done, code = ok, c end)
    Pump(1)
    Equal(done, true, "the search is answered")
    Equal(GA.State.farm.q, "cloth", "the answer names the text")
    Equal(GA.State.farm.g, high.g, "... and the bot")
    Equal(#GA.State.farm.items, 5, "five kinds of cloth")
    local linen = GA.State.FarmItem(2589)
    Check(linen and linen.n == "Linen Cloth" and linen.k > 0, "Linen Cloth with its sources")
    Equal(linen.need, linen.lv + 4, "the level a bot needs is four above the lowest creature")
    Equal(linen.ok, (high.lvl or 0) >= linen.need, "whether this bot can farm it now")
    Check(string.find(GA.UI.FarmItemText({ n = "Runecloth", q = 1, ok = false, need = 54 }, high), "from level 54", 1, true),
        "an item out of the bot's reach says from which level")
    done, code = nil, nil
    GA.Protocol.FarmSearch(high.g, "x", function(ok, c) done, code = ok, c end)
    Pump(1)
    Equal(code, "BAD_REQUEST", "a search of one letter is refused")
    done, code = nil, nil
    GA.Protocol.FarmSearch(high.g, "13022", function(ok, c) done, code = ok, c end)
    Pump(1)
    Equal(#GA.State.farm.items, 1, "an item id finds that item")
    Equal(GA.State.farm.items[1].n, "Gryphonwing Long Bow", "... a rare bind-on-equip bow")

    done, code = nil, nil
    GA.Protocol.SetOrder(high.g, "FARM", { zone = 2589 }, function(ok, c) done, code = ok, c end)
    Pump(1)
    Equal(done, true, "the farm order is accepted")
    Equal(GA.State.bots[high.g].o, "FARM", "the bot has the order")
    Equal(GA.State.bots[high.g].oz, 2589, "... for the item")
    Equal(GA.State.bots[high.g].ozn, "Linen Cloth", "... which is named")
    Equal(GA.State.bots[high.g].ofn, 0, "... without a target stock")
    done, code = nil, nil
    GA.Protocol.SetOrder(high.g, "FARM", { zone = 2589, n = 200 }, function(ok, c) done, code = ok, c end)
    Pump(1)
    Equal(done, true, "the farm order with a target stock is accepted")
    Equal(GA.State.bots[high.g].ofn, 200, "the BOT record carries the target stock")
    Equal(GA.State.bots[high.g].ofs, 143, "... and what the guild storage holds of the item")
    done, code = nil, nil
    GA.Protocol.SetOrder(high.g, "FARM", { zone = 6948 }, function(ok, c) done, code = ok, c end)
    Pump(1)
    Equal(code, "FARM_INVALID", "an item no creature drops is refused")
    Equal(GA.State.bots[high.g].oz, 2589, "the order stays")
    GA.Protocol.SetOrder(high.g, "AUTO", {}, function() end)
    Pump(1)
    Equal(GA.State.bots[high.g].ofn, 0, "another order drops the target stock")

    -- A bot below the item's creatures: the order is taken, and the leader is told that there is nothing to farm.
    if (low.lvl or 0) < 54 then
        done, code = nil, nil
        GA.Protocol.SetOrder(low.g, "FARM", { zone = 14047 }, function(ok, c) done, code = ok, c end)
        Pump(1)
        Equal(done, true, "the order is taken for a bot too low for it")
        local told = false
        for _, e in ipairs(GA.State.events) do
            if e.type == "FARM" and string.find(e.msg or "", "cannot farm Runecloth", 1, true) then
                told = true
            end
        end
        Check(told, "the leader gets the notice that there is nothing to farm")
        Check(string.find(GA.State.bots[low.g].why or "", "nothing to farm", 1, true), "the why line says so too")
        GA.Protocol.SetOrder(low.g, "AUTO", {}, function() end)
        Pump(1)
    end
end)

Test("the dungeon run window", function()
    -- A bot that has outgrown a dungeon is ready for it (the server lists readiness only within its reach).
    do
        local id, d, outside
        for key, dungeon in pairs(GA.State.dungeons) do
            if not dungeon.raid and dungeon.run ~= false and (dungeon.ord or 0) > 0 then
                id, d = key, dungeon
            elseif not dungeon.raid then
                outside = key
            end
        end
        Check(d ~= nil, "the dungeons carry their place in the guild's order")
        if d then
            Equal(GA.State.BotDungeonReadiness({ lvl = (d.max or 60) + 9, rdy = {} }, id), 100, "a bot above a dungeon's levels is ready for it")
            Equal(GA.State.BotDungeonReadiness({ lvl = math.max((d.min or 1) - 1, 0), rdy = {} }, id), nil, "a bot below it is not")
        end
        if outside then
            Equal(GA.State.BotDungeonReadiness({ lvl = 60, rdy = {} }, outside), nil,
                "... and nobody is ready for a dungeon that is in no order of the guild (no run can be ordered into it)")
        end
    end
    GA.UI.ShowPage("groups")
    Pump(1)
    Check(GA.State.dungeonsLoaded, "the Dungeons tab asked for the dungeons")
    local wizard = GA.UI.DungeonWizard
    Equal(wizard.step, 1, "it starts with the choice of the dungeon")
    Equal(#GA.State.dungeonRuns, 0, "no run is going")
    -- A dungeon that can be run, and two bots of the guild that are in the world.
    local dungeon
    for _, d in pairs(GA.State.dungeons) do
        if not d.raid and d.run ~= false and (not dungeon or d.id < dungeon.id) then
            dungeon = d
        end
    end
    Check(dungeon ~= nil, "a dungeon can be run")
    GA.UI.OpenDungeonRun(dungeon.id)
    Pump(0.5)
    Equal(wizard.step, 2, "a chosen dungeon leads to the group")
    local picked = {}
    for _, bot in ipairs(Bots()) do
        if bot.s == "active" and #picked < 2 then
            picked[#picked + 1] = bot.g
            wizard.picked[bot.g] = true
        end
    end
    Check(#picked == 2, "two bots are in the world")
    -- The preparation step: what the storage holds for a run, the talents of the group, the fight rules.
    wizard.step = 3
    GA.UI.RequestRefresh()
    Pump(1)
    Check(GA.State.dungeonPrep.loaded, "the preparation step asked for what the storage holds")
    Check(#GA.State.dungeonPrep.items >= 3, "elixirs, scrolls, stones and scopes are listed")
    Equal(GA.State.dungeonPrep.rules.tl, 4, "the server's fight rules came with them")
    local choices = GA.UI.RunTalentChoices(GA.State.bots[picked[1]])
    Equal(choices[1].role, "none", "a bot can go as it levels")
    Check(next(wizard.itemsOff) == nil, "every kind of item is used unless the leader takes it out")
    wizard.itemsOff[GA.State.dungeonPrep.items[1].e] = true
    wizard.rules.tl, wizard.rules.cpon = 6, false
    wizard.step, wizard.runs = 4, 0
    GA.UI.RequestRefresh()
    Pump(0.5)
    local done, code
    GA.Protocol.StartDungeon(dungeon.id, picked, 0, nil, function(ok, c) done, code = ok, c end,
        { noitems = { GA.State.dungeonPrep.items[1].e }, tl = 6, em = 50, cp = 3, cpon = false, ff = true, md = 2, gr = 30, gc = 8 })
    Pump(1)
    Equal(done, true, "the run starts")
    Equal(GA.MockServer.lastDungeonStart.md, 2, "the mode (clear in big pulls) went with the start")
    Equal(GA.MockServer.lastDungeonStart.gr, 30, "a big pull's yards went with the start")
    Equal(GA.MockServer.lastDungeonStart.gc, 8, "a big pull's creatures went with the start")
    Equal(#GA.MockServer.lastDungeonStart.noitems, 1, "the kind of item that was taken out went with the start")
    Check(GA.MockServer.lastDungeonStart.ia and #GA.MockServer.lastDungeonStart.items == 0, "... and every other kind is used")
    wizard.itemsOff = {}
    Equal(GA.MockServer.lastDungeonStart.tl, 6, "... and the fight rules")
    Equal(#GA.State.dungeonRuns, 1, "the run is listed")
    local run = GA.State.dungeonRuns[1]
    Equal(run.d, dungeon.id, "... with its dungeon")
    Equal(run.left, 255, "... until it is stopped")
    Equal(#run.m, 1, "... and the other bot of the group")
    Check(GA.State.BotDungeonRun(picked[1]) == run and GA.State.BotDungeonRun(picked[2]) == run, "both bots belong to the run")
    Equal(GA.State.bots[run.g].o, "DUNGEON", "its leader has the dungeon order")
    done, code = nil, nil
    GA.Protocol.StartDungeon(dungeon.id, { run.g }, 1, nil, function(ok, c) done, code = ok, c end)
    Pump(1)
    Equal(code, "DUNGEON_INVALID", "a bot on a run cannot start another")
    GA.Protocol.StopDungeon(run.g, false, function() end)
    Pump(1)
    Equal(GA.State.dungeonRuns[1].left, 1, "'last run' makes the run that is going the last")
    done = nil
    GA.Protocol.StopDungeon(run.g, true, function(ok) done = ok end)
    Pump(1)
    Equal(done, true, "the run is stopped")
    Equal(#GA.State.dungeonRuns, 0, "... and gone from the list")
    Equal(GA.State.bots[run.g].o, "AUTO", "its leader has another order again")
    GA.UI.OpenDungeonRun(nil)
    Pump(0.5)
end)

Test("the guild leader in a dungeon group", function()
    GA.UI.ShowPage("groups")
    Pump(1)
    local wizard = GA.UI.DungeonWizard
    local dungeon
    for _, d in pairs(GA.State.dungeons) do
        if not d.raid and d.run ~= false and (not dungeon or d.id < dungeon.id) then
            dungeon = d
        end
    end
    GA.UI.OpenDungeonRun(dungeon.id)
    Pump(0.5)
    local own = GA.State.leader.g
    Check(own ~= nil, "the leader's record is there")
    local candidates = GA.UI.DungeonGroupCandidates()
    Check(candidates[1] and candidates[1].own and candidates[1].g == own, "the group step offers the leader's own character first")
    Equal(#candidates, 1 + #Bots(), "... and every bot after it")
    -- Alone it is no group.
    local done, code
    GA.Protocol.StartDungeon(dungeon.id, { own }, 1, nil, function(ok, c) done, code = ok, c end)
    Pump(1)
    Equal(code, "DUNGEON_INVALID", "the leader's own character alone is no group")
    -- With a bot: the bot leads, the leader's character is a member.
    local bot
    for _, b in ipairs(Bots()) do
        if b.s == "active" and not bot then
            bot = b
        end
    end
    wizard.picked[own], wizard.picked[bot.g] = true, true
    GA.UI.RequestRefresh()
    Pump(0.5)
    Check(wizard.picked[own], "the leader's own character stays picked over a refresh")
    done, code = nil, nil
    GA.Protocol.StartDungeon(dungeon.id, { own, bot.g }, 1, nil, function(ok, c) done, code = ok, c end)
    Pump(1)
    Equal(done, true, "the run with the leader's own character starts")
    local run = GA.State.dungeonRuns[1]
    Equal(run.g, bot.g, "the bot leads it")
    Equal(run.m[1], own, "... and the leader's own character is its member")
    Check(GA.State.BotDungeonRun(own) == run, "the leader's own character belongs to the run")
    GA.Protocol.StopDungeon(run.g, true, function() end)
    Pump(1)
    Equal(#GA.State.dungeonRuns, 0, "the run is stopped")
    GA.UI.OpenDungeonRun(nil)
    Pump(0.5)
end)

Test("a raid's Whole guild button picks every member it can", function()
    GA.UI.ShowPage("groups")
    Pump(1)
    local wizard = GA.UI.DungeonWizard
    local raid, dungeon
    for _, d in pairs(GA.State.dungeons) do
        if d.raid then
            raid = d
        elseif d.run ~= false and (not dungeon or d.id < dungeon.id) then
            dungeon = d
        end
    end
    Check(raid ~= nil, "the mock lists a raid")
    GA.UI.OpenDungeonRun(dungeon.id)
    Pump(0.5)
    local button
    for _, o in ipairs(Stub.Objects()) do
        if rawget(o, "__type") == "Button" and (o:GetText() or "") == "Whole guild" then
            button = o
        end
    end
    Check(button ~= nil, "the group step has a Whole guild button")
    Check(not button:IsVisible(), "... which a five-player dungeon does not show")
    Equal(GA.UI.PickWholeGuild(), 0, "... and which picks nobody for one")
    GA.UI.OpenDungeonRun(raid.id)
    Pump(0.5)
    Check(button:IsVisible(), "a raid's group step shows it")
    local free = 0
    for _, bot in ipairs(GA.UI.DungeonGroupCandidates()) do
        if bot.own or (bot.s == "active" and bot.gl and not GA.State.BotDungeonRun(bot.g)) then
            free = free + 1
        end
    end
    button:Click()
    Pump(0.5)
    local picked = 0
    for _ in pairs(wizard.picked) do
        picked = picked + 1
    end
    Equal(picked, math.min(free, raid.size), "every member that can go is picked, up to the raid's size")
    Check(wizard.picked[GA.State.leader.g], "the leader's own character among them")
    for g in pairs(wizard.picked) do
        local bot = GA.State.bots[g]
        Check(not bot or bot.s ~= "benched", "no benched bot is picked")
    end
    GA.UI.OpenDungeonRun(nil)
    Pump(0.5)
end)

Test("craft orders", function()
    GA.UI.ShowStorageView("orders")
    Pump(1)
    Check(GA.UI.StorageViewFrame("orders") and not GA.UI.StorageViewFrame("orders").gaBroken, "the Craft orders view is built")
    Check(GA.State.craftOrdersLoaded and GA.State.craftablesLoaded, "the view asked for the orders and for what the guild can make")
    Equal(#GA.UI.CraftableChoices(""), 4, "what the guild can make")
    Equal(#GA.UI.CraftableChoices("wand"), 2, "the search narrows the list")
    Equal(#GA.UI.CraftableChoices("brom"), 2, "... by the crafter's name too")
    Equal(GA.UI.OrderCraft(nil, 3), false, "no order without an item")
    Check(GA.UI.OrderCraft(11287, 3), "three Lesser Magic Wands are ordered")
    Pump(1)
    Equal(#GA.State.craftOrders, 1, "the order is listed")
    Equal(GA.State.craftOrders[1].amt, 3, "... with its amount")
    Equal(GA.State.craftOrders[1].n, "Lesser Magic Wand", "... and the item's name")
    Check(GA.UI.OrderCraft(11287, 2), "the same item again")
    Pump(1)
    Equal(#GA.State.craftOrders, 1, "adds to the order")
    Equal(GA.State.craftOrders[1].amt, 5, "five now")
    local done, code
    GA.Protocol.AddCraftOrder(774, 1, function(ok, c) done, code = ok, c end)
    Pump(1)
    Equal(code, "CRAFT_ORDER_INVALID", "an item no recipe makes is refused")
    GA.Protocol.DeleteCraftOrder(GA.State.craftOrders[1].id, function(ok) done = ok end)
    Pump(1)
    Equal(done, true, "the order is taken back")
    Equal(#GA.State.craftOrders, 0, "no orders left")
end)

Test("shopping list", function()
    GA.UI.ShowStorageView("shopping")
    Pump(1)
    local view = GA.UI.StorageViewFrame("shopping")
    Check(view and not view.gaBroken, "the Shopping list view is built")
    Check(view and view:IsShown(), "... and shown")
    Check(GA.State.shopping.loaded, "the view asked for the list")
    Equal(#GA.State.shopping.items, 0, "the list is empty")
    Equal(GA.State.shopping.cap, 500000, "the day's budget")
    Equal(GA.UI.AddShoppingItem(nil, 50), false, "nothing is added without an item")
    Equal(GA.UI.SearchShopItems("w"), false, "a search needs two letters")
    Check(GA.UI.SearchShopItems("water"), "a search for water")
    Pump(1)
    Equal(#GA.State.shopSearch.items, 1, "one vendor water by that name")
    Equal(GA.State.shopSearch.items[1].bc, 5, "... sold in fives")
    Check(GA.UI.AddShoppingItem(159, 60), "Refreshing Spring Water at least 60")
    Pump(1)
    Equal(#GA.State.shopping.items, 1, "it is on the list")
    Equal(GA.State.shopping.items[1].min, 60, "... with its minimum")
    Equal(GA.UI.AddShoppingTiers("drink", 100), 5, "the other five drink tiers are added")
    Pump(2)
    Equal(#GA.State.shopping.items, 6, "six drink tiers on the list")
    Equal(GA.State.ShoppingEntry(159).min, 60, "a tier already on the list keeps its minimum")
    Equal(GA.State.ShoppingEntry(8766).min, 100, "the new tiers have the minimum given")
    Equal(GA.UI.AddShoppingTiers("drink", 100), 0, "no tier twice")
    local done, code
    GA.Protocol.SetShoppingItem(8766, 100, false, function(ok) done = ok end)
    Pump(1)
    Equal(GA.State.ShoppingEntry(8766).on, false, "a tier can be paused")
    GA.Protocol.SetShoppingItem(2589, 10, true, function(ok, c) done, code = ok, c end)
    Pump(1)
    Equal(code, "SHOP_REFUSED", "an item no vendor sells is refused")
    GA.Protocol.DeleteShoppingItem(159, function(ok) done = ok end)
    Pump(1)
    Equal(done, true, "an item is taken off")
    Equal(#GA.State.shopping.items, 5, "five left")
    local wasLoaded = GA.State.storage.loaded
    GA.State.storage.loaded = true
    GA.State.storage.items["1205:0"] = { e = 1205, rp = 0, n = 35, cls = 0, sub = 5 }
    Equal(GA.State.ShoppingStock(1205), 35, "the stock follows the guild storage")
    GA.State.storage.items["1205:0"] = nil
    GA.State.storage.loaded = wasLoaded
    for _, entry in ipairs(GA.State.shopping.items) do
        GA.Protocol.DeleteShoppingItem(entry.e, nil)
    end
    Pump(2)
    Equal(#GA.State.shopping.items, 0, "the list is empty again")
end)

Test("talent templates", function()
    local bot = Bots()[1]
    Check(#GA.State.specs >= 20, "templates arrived with the sync")
    local mine = GA.State.SpecsForClass(bot.class)
    Equal(#mine, 2, "two templates for the bot's class")
    GA.UI.ShowBot(bot.g)
    GA.UI.ShowBotSubtab("talents")
    Pump(0.3)
    local done
    GA.Protocol.SetSpec(bot.g, mine[2].id, function(ok) done = ok end)
    Pump(1)
    Equal(done, true, "template assigned")
    Equal(GA.State.bots[bot.g].spt, mine[2].id, "the bot plays by it")
    Equal(GA.State.bots[bot.g].spec, mine[2].n, "its name is shown")
    done = nil
    GA.Protocol.SetSpec(bot.g, 99999, function(ok, code) done = code end)
    Pump(1)
    Equal(done, "SPEC_INVALID", "unknown template refused")
    GA.Protocol.SaveSpec("My build", 1, function(ok) done = ok end)
    Pump(1)
    Equal(done, true, "template saved")
    local saved
    for _, spec in ipairs(GA.State.specs) do
        if spec.own then
            saved = spec
        end
    end
    Check(saved and saved.n == "My build", "the guild's template is listed")
    GA.Protocol.DeleteSpec(saved.id, function(ok) done = ok end)
    Pump(1)
    for _, spec in ipairs(GA.State.specs) do
        Check(not spec.own, "the guild's template is gone")
    end
    GA.Protocol.SetSpec(bot.g, 0)
    Pump(1)
    Equal(GA.State.bots[bot.g].spt, 0, "back to the class default")
end)

Test("dungeon roles", function()
    local warrior, mage
    for _, bot in ipairs(Bots()) do
        if bot.class == 1 then
            warrior = warrior or bot
        elseif bot.class == 8 then
            mage = mage or bot
        end
    end
    local bot = warrior or Bots()[1]
    GA.UI.ShowBot(bot.g)
    GA.UI.ShowBotSubtab("role")
    Pump(0.5)
    Equal(GA.State.BotGroupRole({ role = "heal", dr = "none" }), "heal", "without a dungeon role the talents' role counts")
    Equal(GA.State.BotGroupRole({ role = "dps", dr = "tank" }), "tank", "the dungeon role wins")
    if warrior then
        Equal(warrior.drs, "tank", "a warrior can be a tank")
        local done, code
        GA.Protocol.SetRole(warrior.g, "tank", 0, function(ok) done = ok end)
        Pump(1)
        Equal(done, true, "role set")
        Equal(GA.State.bots[warrior.g].dr, "tank", "the bot is a tank now")
        Check((GA.State.bots[warrior.g].drn or "") ~= "", "the role's talents are named")
        GA.Protocol.Get("rolegear", warrior.g)
        Pump(1)
        Equal(#(GA.State.roleGear[warrior.g] or {}), 2, "the role store arrived")
        GA.UI.RequestRefresh()
        Pump(0.5)
        local roles = GA.UI.SquadRoles(GA.State.bots[warrior.g])
        Equal(roles.tank, 1, "the squad's roles count the dungeon role")
        GA.Protocol.SetRole(warrior.g, "heal", 0, function(ok, c) done, code = ok, c end)
        Pump(1)
        Equal(code, "ROLE_INVALID", "a warrior cannot be a healer")
        GA.Protocol.SetRole(warrior.g, "none", 0, function(ok) done = ok end)
        Pump(1)
        Equal(GA.State.bots[warrior.g].dr, "none", "the role is taken away again")
    end
    if mage then
        Equal(mage.drs or "", "", "a mage has no dungeon role to take")
    end
end)

local function VisibleText(fragment)
    for _, o in ipairs(Stub.Objects()) do
        if rawget(o, "__type") == "FontString" and o:IsVisible()
            and string.find(o:GetText() or "", fragment, 1, true) then
            return true
        end
    end
    return false
end

-- Fight profiles (docs/fight-profiles.md, FP-2): the Combat sub-tab, the template, a copy, the slots, the bot's own
-- option values, "Copy to bots".
-- The guild's own fight profiles in a list (the wowbot335 templates come after them: their ids start at 1000001).
local function GuildProfiles(list)
    local n = 0
    for _, profile in ipairs(list and list.list or {}) do
        if not profile.tpl then
            n = n + 1
        end
    end
    return n
end

Test("fight profiles: the Combat sub-tab", function()
    local bot, other
    for _, b in ipairs(Bots()) do
        if b.s ~= "benched" then
            for _, c in ipairs(Bots()) do
                if c ~= b and c.class == b.class and not bot then
                    bot, other = b, c
                end
            end
        end
    end
    bot = bot or Bots()[1]
    GA.MockServer.fightNoDefault = true  -- the bot setting Fight.TemplatesByDefault off: the default is mod-playerbots'
    GA.Protocol.GetFightProfiles(bot.g)
    Pump(4.5)  -- the mock pushes the bot's changed record with its next step (every 4 s)
    GA.UI.ShowBot(bot.g)
    GA.UI.ShowBotSubtab("fight")
    Pump(1)
    local list, profiles = GA.State.FightOf(bot.g)
    Check(list ~= nil, "the profiles of its class arrived")
    Equal(GuildProfiles(list), 0, "no profile of the guild yet")
    Check(profiles[0] ~= nil, "the template arrived")
    Check(VisibleText("fights by mod-playerbots' default rotation"), "the status names the default")
    Check(VisibleText("Template of"), "the info line names the template")

    local rows, rules = GA.UI.FightRows(profiles[0], 0)
    Equal(rules, 2, "two rules in the fight")
    Check(rows[1].divider ~= nil, "the list begins with a divider")
    Equal(rows[1].divider, "High", "the highest band first")
    Equal(rows[2].a, "melee", "the highest relevance first")
    Equal(rows[3].divider, "Normal", "a divider where the next band begins")
    local _, buffs = GA.UI.FightRows(profiles[0], 1)
    Equal(buffs, 1, "one rule between fights")
    Equal(GA.UI.FightRuleName({ k = 0, a = "battle shout" }), "Battle Shout", "an action's name in title case")
    Equal(GA.UI.FightRuleName({ k = 4, ref = 2 }), "Use trinket 2", "a trinket rule")
    Equal(GA.UI.FightTreeName(1, 2), "Protection", "the third tree of a warrior")

    local done
    GA.Protocol.CopyFightProfile(bot.g, "My rotation", function(ok) done = ok end)
    Pump(1)
    Equal(done, true, "copied as a new profile")
    list = GA.State.FightOf(bot.g)
    Equal(GuildProfiles(list), 1, "the new profile is in the list")
    local id = list.list[1].id
    Equal(list.list[1].n, "My rotation", "with its name")

    GA.Protocol.AssignFight(bot.g, 0, id, function(ok) done = ok end)
    Pump(1)
    Equal(done, true, "solo slot assigned")
    Equal(GA.State.bots[bot.g].fp, id, "the bot record has its solo profile")
    Equal(GA.State.bots[bot.g].fpn, "My rotation", "it plays the profile now")
    GA.UI.RequestRefresh()
    Pump(0.5)
    Check(VisibleText("fights by My rotation") or VisibleText("My rotation"), "the status names the profile")

    GA.Protocol.GetFightProfile(bot.g, id)
    Pump(1)
    local _, _, options = GA.State.FightOf(bot.g)
    Equal(#(options[id] or {}), 2, "the profile's options with the bot's values")
    GA.Protocol.SetFightOption(bot.g, id, "hp", 55, function(ok) done = ok end)
    Pump(1)
    Equal(done, true, "own value set")
    _, _, options = GA.State.FightOf(bot.g)
    local hp
    for _, option in ipairs(options[id]) do
        if option.k == "hp" then
            hp = option
        end
    end
    Equal(hp and hp.own, true, "the bot has its own value")
    Equal(hp and hp.bv, 55, "of 55")
    local code
    GA.Protocol.SetFightOption(bot.g, id, "hp", 500, function(ok, c) done, code = ok, c end)
    Pump(1)
    Equal(code, "FIGHT_INVALID", "a value outside the range is refused")
    GA.Protocol.SetFightOption(bot.g, id, "hp", nil, function(ok) done = ok end)
    Pump(1)
    _, _, options = GA.State.FightOf(bot.g)
    for _, option in ipairs(options[id]) do
        if option.k == "hp" then
            hp = option
        end
    end
    Equal(hp.own, false, "back to the profile's value")

    if other then
        GA.Protocol.CopyFightTo(bot.g, { other.g }, function(ok) done = ok end)
        Pump(1)
        Equal(done, true, "copied to another bot")
        Equal(GA.State.bots[other.g].fp, id, "the other bot has the solo profile")
    end
    GA.Protocol.AssignFight(bot.g, 0, 0, function(ok) done = ok end)
    Pump(1)
    Equal(GA.State.bots[bot.g].fp, 0, "back to the default")
    Equal(GA.State.bots[bot.g].fpn, "", "it plays the default again")
    GA.MockServer.fightNoDefault = nil
end)

-- FC12: a bot without a profile of its own plays the template of its class, talent tree and role by default.
Test("fight profiles: the template of its spec by default", function()
    local bot
    for _, b in ipairs(Bots()) do
        if b.s ~= "benched" and b.class == 1 and not bot then
            bot = b
        end
    end
    Check(bot ~= nil, "a warrior bot in the world")
    GA.Protocol.AssignFight(bot.g, 0, 0)
    GA.Protocol.AssignFight(bot.g, 1, 0)
    GA.UI.ShowBot(bot.g)
    GA.UI.ShowBotSubtab("fight")
    Pump(1)
    local state = GA.State.bots[bot.g]
    Equal(state.fpid, 1000001, "it plays the template of its spec")
    Equal(state.fpd, true, "by default")
    Check(VisibleText("by default: the template of its spec"), "the status says so")
    local list = GA.State.FightOf(bot.g)
    Equal(list.meta.dflt, 1000001, "the list names the default of the solo slot")
    local ui = GA.UI.FightSlotChoices and GA.UI.FightSlotChoices(bot.g, 0)
    if ui then
        Check(ui[1].text:find("Warrior Solo Arms", 1, true) ~= nil, "the solo slot's first choice names the template")
    end
end)

-- The fight profile editor (FP-3): catalogue, the read-only template, rules added, moved and switched, a condition and
-- an option from the forms, save, a refused save that selects its rule, discard, a copy of a profile, delete.
Test("fight profiles: the wowbot335 templates", function()
    local bot
    for _, b in ipairs(Bots()) do
        if b.s ~= "benched" and b.class == 1 and not bot then
            bot = b
        end
    end
    Check(bot ~= nil, "a warrior bot in the world")
    GA.Protocol.GetFightProfiles(bot.g)
    Pump(1)
    local list = GA.State.FightOf(bot.g)
    local template
    for _, profile in ipairs(list and list.list or {}) do
        if profile.tpl then
            template = profile
        end
    end
    Check(template ~= nil, "a template of its class is listed")
    Equal(template and template.id, 1000001, "by its id")

    -- Read-only in the editor; the guild may copy it.
    local E = GA.UI.FightEditor
    GA.UI.OpenFightEditor(bot.g, template.id)
    Pump(1)
    local ed = GA.UI.FightEditorFrame()
    Equal(E.id, template.id, "the editor shows it")
    Check(E.draft ~= nil and #E.draft.rules[0] == 1 and #E.draft.rules[1] == 1, "with its rules in both lists")
    Check(GA.UI.FightEditorReadOnly(), "read-only")
    Check(VisibleText("Template (wowbot335)"), "the banner says it is a template")
    Check(not ed.save:IsEnabled() and not ed.deleteBtn:IsEnabled(), "no Save and no Delete")
    GA.UI.FightAddRule(2, 78)
    Equal(#E.draft.rules[0], 1, "no rule can be added to it")
    Check(ed.newBtn:IsEnabled() and ed.assignSolo:IsEnabled(), "New copy and Assign are there")

    -- Given to the bot as it is.
    local done
    GA.Protocol.AssignFight(bot.g, 0, template.id, function(ok) done = ok end)
    Pump(1)
    Equal(done, true, "assigned to the solo slot")
    Equal(GA.State.FightOf(bot.g).meta.solo, template.id, "the slot has it")

    -- The server refuses a save; a copy is the guild's and can be edited.
    local code
    GA.Protocol.SaveFightProfile(bot.g, template.id, { n = "x", note = "", rules = { [0] = {}, [1] = {} }, options = {} },
        function(ok, c) done, code = ok, c end)
    Pump(1)
    Equal(done, false, "a save of the template is refused")
    GA.Protocol.CopyFightProfile(bot.g, "From wowbot335", function(ok) done = ok end, template.id)
    Pump(1)
    Equal(done, true, "copied")
    local copy
    for _, profile in ipairs(GA.State.FightOf(bot.g).list) do
        if profile.n == "From wowbot335" then
            copy = profile
        end
    end
    Check(copy ~= nil and not copy.tpl, "the copy is the guild's")
    GA.UI.OpenFightEditor(bot.g, copy.id)
    Pump(1)
    Check(not GA.UI.FightEditorReadOnly(), "and can be edited")
    Equal(#E.draft.options, 1, "with the template's option")
    GA.Protocol.AssignFight(bot.g, 0, 0)
    GA.Protocol.DeleteFightProfile(bot.g, copy.id)
    Pump(1)
    GA.UI.FightEditorFrame():Hide()
end)

Test("fight profiles: the editor", function()
    local bot
    for _, b in ipairs(Bots()) do
        if b.s ~= "benched" and not bot then
            bot = b
        end
    end
    local E = GA.UI.FightEditor
    local done, code, msg
    GA.Protocol.CopyFightProfile(bot.g, "Editor test", function(ok) done = ok end)
    Pump(1)
    Equal(done, true, "a profile to edit")
    local list = GA.State.FightOf(bot.g)
    local id = 0
    for _, profile in ipairs(list.list) do
        if not profile.tpl and profile.id > id then
            id = profile.id
        end
    end

    GA.UI.OpenFightEditor(bot.g, 0)
    Pump(1)
    local ed = GA.UI.FightEditorFrame()
    Check(ed and ed:IsShown(), "the editor window is open")
    Check(#(GA.State.fight.conds or {}) > 0, "the condition catalogue arrived")
    Check(#(GA.State.fight.spells[bot.g] or {}) > 0, "the bot's class spells arrived")
    Check(#(GA.State.fight.items[bot.g] or {}) > 0, "its usable items arrived")
    Check(E.draft ~= nil, "the template is shown")
    Check(VisibleText("read-only"), "the template says it is read-only")
    Check(not ed.save:IsEnabled(), "nothing to save on the template")
    GA.UI.FightAddRule(2, 78)
    Equal(#E.draft.rules[0], 2, "no rule can be added to the template")

    GA.UI.OpenFightEditor(bot.g, id)
    Pump(1)
    Equal(E.id, id, "the copy is shown")
    Equal(#E.draft.rules[0], 2, "with its two rules in the fight")
    Equal(E.dirty, false, "nothing changed yet")

    -- Add a spell from the dropdown (its submenu of spells), then move it about.
    local top = Stub.OpenDropDown(ed.addSpell, 1)
    Check(#top >= 1 and top[1].hasArrow, "the spells come in submenus")
    local spells = Stub.OpenDropDown(ed.addSpell, 2, top[1].value)
    local strike
    for _, item in ipairs(spells) do
        if item.value == 78 then
            strike = item
        end
    end
    Check(strike ~= nil, "Heroic Strike is offered")
    Check(string.find(top[1].text, "Arms", 1, true) == 1, "the first submenu is a tab of the spellbook")
    Equal(#spells, 3, "with that tab's spells only")
    local tabs = GA.UI.FightSpellTabs(bot.g)
    Equal(#tabs, 2, "two tabs: Arms and Fury")
    Equal(tabs[2].spells[1].n, "Battle Shout", "Battle Shout stands on the Fury tab")

    -- A row's trigger of mod-playerbots: in words in the list and as the first condition, which X takes away.
    local melee
    for _, r in ipairs(E.draft.rules[0]) do
        if r.tg == "enemy is close" then
            melee = r
        end
    end
    Check(melee ~= nil, "the copy has the melee row with its trigger")
    Equal(GA.UI.FightTriggerText(bot.g, "enemy is close"), "an enemy stands close to it", "the trigger in words")
    Equal(GA.UI.FightWhenText(bot.g, melee.tg, ""), "an enemy stands close to it", "the list's grey column says what it checks")
    Equal(GA.UI.FightWhenText(bot.g, "no such trigger", ""), "check \"no such trigger\"", "an unknown trigger by its name")
    E.sel = melee
    GA.UI.RefreshFightEditor()
    local first = ed.conds.rows[1]
    Equal(first.trigger, "enemy is close", "the trigger is the first condition")
    Check(string.find(first.full or "", "an enemy stands close to it", 1, true), "in words")
    first.remove:Click()
    Equal(melee.tg, "", "X takes the trigger away")
    Equal(E.dirty, true, "a change to save")
    melee.tg = "enemy is close"
    E.sel = nil
    Stub.ClickItem(strike)
    Equal(#E.draft.rules[0], 3, "a rule was added")
    Equal(E.dirty, true, "the profile is changed")
    local rule = E.sel
    Equal(rule and rule.ref, 78, "the new rule is selected")
    Equal(rule.rel, 19, "at the top of the normal band")
    GA.UI.FightMoveRule(rule, -1)
    Equal(rule.rel, 21, "moved up into the next band: the other rule's relevance")
    GA.UI.FightMoveRule(rule, -1)
    local order = {}
    for _, row in ipairs(GA.UI.FightRows({ rules = E.draft.rules[0] }, 0)) do
        if not row.divider then
            order[#order + 1] = row
        end
    end
    Equal(order[1].ref, 78, "past an equal relevance: it is first now")
    GA.UI.FightMoveRule(rule, 1)
    order = {}
    for _, row in ipairs(GA.UI.FightRows({ rules = E.draft.rules[0] }, 0)) do
        if not row.divider then
            order[#order + 1] = row
        end
    end
    Equal(order[2].ref, 78, "moved down again past the melee rule")
    ed.toggle:Click()
    Equal(rule.en, false, "switched off")
    ed.toggle:Click()
    Equal(rule.en, true, "and on again")

    -- An option and a condition that names it.
    Equal(#E.draft.options, 2, "the copy's two options")
    ed.optionsBtn:Click()
    Equal(E.view, "options", "the options view")
    ed.optKey:SetText("low")
    ed.optLabel:SetText("Low health")
    Stub.ClickItem(Stub.OpenDropDown(ed.optKindDD, 1)[3])
    ed.optValue:SetText("30")
    ed.optMin:SetText("0")
    ed.optMax:SetText("100")
    ed.optSave:Click()
    Equal(#E.draft.options, 3, "an option was added")
    Equal(E.draft.options[3].kind, 2, "a percent")
    ed.optionsBtn:Click()
    Equal(E.view, "rule", "back to the rule")
    local groups = Stub.OpenDropDown(ed.condDD, 1)
    local me = Stub.OpenDropDown(ed.condDD, 2, groups[1].value)
    local hp
    for _, item in ipairs(me) do
        if item.value == "hp" then
            hp = item
        end
    end
    Check(hp ~= nil, "my health is a condition of the group Me")
    Stub.ClickItem(hp)
    Stub.ClickItem(Stub.OpenDropDown(ed.condOpDD, 1)[1])
    ed.condValue:SetText("$nothing")
    local text, why = E.ConditionFromForm()
    Equal(text, nil, "an option the profile has not is refused")
    Check(why and string.find(why, "nothing", 1, true), "with the reason")
    ed.condValue:SetText("$low")
    ed.condAdd:Click()
    Equal(rule.conds[1], "hp<$low", "the condition was added")
    Equal(GA.UI.FightDescribeCondition("!tdebuff:772"), "not: Target has my debuff Rend", "a condition in words")

    -- Save.
    ed.name:SetText("Editor test 2")
    E.draft.n = "Editor test 2"
    GA.UI.FightSaveProfile()
    Pump(1)
    Equal(E.dirty, false, "saved")
    Check(E.message and string.find(E.message, "Saved", 1, true), "the message says so")
    local _, profiles = GA.State.FightOf(bot.g)
    local saved = profiles[id]
    Equal(saved.meta.pn, "Editor test 2", "its new name")
    local found
    for _, r in ipairs(saved.rules) do
        if r.k == 2 and r.ref == 78 then
            found = r
        end
    end
    Check(found ~= nil, "the spell rule is stored")
    Equal(found and found.c, "hp<$low", "with its condition")
    Equal(#E.draft.options, 3, "the option is stored")
    list = GA.State.FightOf(bot.g)
    for _, p in ipairs(list.list) do
        if p.id == id then
            Equal(p.rc, 3, "the list counts three rules in the fight")
        end
    end

    -- A save the server refuses selects the rule it names.
    for _, r in ipairs(E.draft.rules[0]) do
        if r.k == 2 then
            rule = r
        end
    end
    rule.conds[#rule.conds + 1] = "bogus>1"
    E.dirty = true
    E.sel = nil
    GA.UI.FightSaveProfile()
    Pump(1)
    Equal(E.messageError, true, "the save is refused")
    Check(E.message and string.find(E.message, "bogus", 1, true), "naming the condition")
    Equal(E.sel, rule, "the rule it names is selected")
    ed.discard:Click()
    Equal(E.dirty, false, "discarded")
    local again
    for _, r in ipairs(E.draft.rules[0]) do
        if r.k == 2 then
            again = r
        end
    end
    Equal(#again.conds, 1, "back to the stored conditions")

    -- A copy of the profile, then delete it.
    local before = #GA.State.FightOf(bot.g).list
    ed.newBtn:Click()
    Pump(1)
    Equal(#GA.State.FightOf(bot.g).list, before + 1, "copied")
    Check(E.id ~= id, "the copy is shown")
    Equal(GA.State.FightOf(bot.g).list[GuildProfiles(GA.State.FightOf(bot.g))].n, "Editor test 2 copy", "named after the profile")
    Equal(#E.draft.rules[0], 3, "with the profile's rules")
    ed.deleteBtn:Click()
    Stub.AcceptPopup()
    Pump(1)
    Equal(#GA.State.FightOf(bot.g).list, before, "deleted")
    Equal(E.id, 0, "the template is shown again")
    GA.Protocol.DeleteFightProfile(bot.g, id, function(ok) done = ok end)
    Pump(1)
    Equal(done, true, "the test profile deleted")
    ed:Hide()
end)

-- Bot behaviour settings (docs/bot-settings.md): schema and values from the server, guild and bot scope, the panel.
Test("bot behaviour settings: values as text", function()
    local B = GA.UI.Behaviour
    local money = { t = "money" }
    Equal(B.Parse(money, "1g 20s 5c"), 12005, "money parsed")
    Equal(B.Parse(money, "75"), 75, "plain number = copper")
    Equal(B.Parse(money, "3x"), nil, "bad money refused")
    Equal(B.Format(money, 12005), "1g 20s 5c", "money formatted")
    Equal(B.Format(money, 0), "0c", "no money")
    local dec = { t = "dec", sc = 100 }
    Equal(B.Parse(dec, "1.25"), 125, "decimal parsed")
    Equal(B.Format(dec, 125), "1.25", "decimal formatted")
    local pct = { t = "int", u = "%" }
    Equal(B.Parse(pct, "65%"), 65, "percent with its unit")
    Equal(B.Format(pct, 65), "65%", "percent formatted")
    local enum = { t = "enum", opt = "hub=Only at a hub;full=When full" }
    Equal(B.Parse(enum, "full"), 1, "enum by token")
    Equal(B.Parse(enum, "only at a hub"), 0, "enum by label")
    Equal(B.Format(enum, 1), "When full", "enum formatted")
    Equal(B.Parse({ t = "bool" }, "off"), 0, "bool off")
end)

Test("bot behaviour settings: guild and bot values", function()
    local s = GA.State.settings
    local Mock = GA.MockServer
    Equal(#s.defs, #Mock.SETTING_DEFS, "settings arrived with the sync")
    Equal(#s.cats, #Mock.SETTING_CATS, "categories arrived with the sync")
    Check(s.loaded, "values arrived with the sync")
    local rest = GA.State.SettingDef("Rest.BelowHealth")
    Check(rest ~= nil and rest.id == 102, "lookup by key")
    local value, source = GA.State.SettingValue(rest, 0)
    Equal(value, 65, "the default counts")
    Equal(source, "default", "nothing set yet")

    local done
    GA.Protocol.SetSettings(0, { [102] = 50 }, nil, false, function(ok) done = ok end)
    Pump(1)
    Equal(done, true, "guild value accepted")
    value, source = GA.State.SettingValue(rest, 0)
    Equal(value, 50, "the guild's value")
    Equal(source, "guild", "comes from the guild")

    done = nil
    GA.Protocol.SetSettings(0, { [102] = 99 }, nil, false, function(ok, code, msg, reason) done = reason end)
    Pump(1)
    Equal(done, "RANGE", "out of range refused")

    local bot = Bots()[1]
    done = nil
    GA.Protocol.SetSettings(bot.g, { [303] = 0 }, nil, false, function(ok, code, msg, reason) done = reason end)
    Pump(1)
    Equal(done, "GUILD_ONLY", "a guild-wide setting has no bot value")
    done = nil
    GA.Protocol.SetSettings(bot.g, { [102] = 30 }, nil, false, function(ok) done = ok end)
    Pump(1)
    Equal(done, true, "bot value accepted")
    value, source = GA.State.SettingValue(rest, bot.g)
    Equal(value, 30, "the bot's own value")
    Equal(source, "own", "comes from the bot")
    Equal(GA.State.SettingValue(rest, Bots()[2].g), 50, "another bot follows the guild")
    Equal(GA.State.ChangedSettingCount(bot.g), 1, "one own value")

    GA.Protocol.SetSettings(bot.g, nil, { 102 }, false)
    Pump(1)
    value, source = GA.State.SettingValue(rest, bot.g)
    Equal(source, "guild", "back to the guild's value")
end)

Test("bot behaviour settings: the panels", function()
    GA.UI.ShowSettingsView("behaviour")
    Pump(0.3)
    Equal(GA.UI.CurrentPage(), "settings", "settings page")
    Check(VisibleText("Rest between fights"), "the first category's settings are listed")
    Check(VisibleText("1 changed from the server's defaults"), "the changed count")
    local panel
    for _, o in ipairs(Stub.Objects()) do
        if rawget(o, "catButtons") and o:IsVisible() then
            panel = o
        end
    end
    Check(panel ~= nil, "behaviour panel found")
    if panel then
        -- a checkbox row: Rest between fights off for the guild
        local row
        for _, r in ipairs(panel.list.rows) do
            if r:IsShown() and r.item and r.item.def and r.item.def.id == 101 then
                row = r
            end
        end
        Check(row ~= nil, "row of Rest between fights")
        if row then
            row.check:Click() -- checked (on by default): a click turns it off
            Pump(1)
            Equal(GA.State.SettingValue(GA.State.SettingDef(101), 0), 0, "the checkbox set the guild's value")
        end
        -- search across categories
        panel.search:SetText("courage")
        Pump(0.3)
        local defs, headers = 0, 0
        for _, item in ipairs(panel.list.items) do
            if item.def then defs = defs + 1 else headers = headers + 1 end
        end
        Equal(defs, 1, "one setting matches the search")
        Equal(headers, 1, "with its category")
        panel.search:SetText("")
        Pump(0.3)
        -- a choice setting in its category
        panel.category = "upkeep"
        GA.UI.RefreshNow()
        Pump(0.3)
        Check(VisibleText("Vendor trips (demo)"), "the choice setting is listed")
    end
    GA.UI.ShowSettingsView("addon")
    Pump(0.3)
    Check(VisibleText("Window scale"), "the addon options are a view of their own")
    GA.UI.ShowSettingsView("behaviour")
    Pump(0.3)

    local bot = Bots()[1]
    GA.UI.ShowBot(bot.g)
    GA.UI.ShowBotSubtab("behaviour")
    Pump(0.3)
    Check(VisibleText("hold for this bot only"), "the bot's behaviour tab")
    Check(not VisibleText("New recruits move on"), "guild-wide settings are not offered for a bot")

    local done
    GA.Protocol.SetSettings(0, nil, nil, true, function(ok) done = ok end)
    Pump(1)
    Equal(done, true, "reset all")
    Equal(GA.State.ChangedSettingCount(0), 0, "the guild has no values left")
end)

-- Regression (2026-11-12, the owner's client): the Loot filter view raised a Lua error while it was built (a tooltip on a
-- FontString) and the whole Storage tab stayed black.
Test("storage tab: every view is built, anchored and shown", function()
    GA.UI.ShowPage("storage")
    Pump(0.3)
    Equal(GA.UI.CurrentPage(), "storage", "storage page")
    local keys = { "items", "loot", "log", "items" }
    for _, key in ipairs(keys) do
        GA.UI.ShowStorageView(key)
        Pump(0.3)
        local frame = GA.UI.StorageViewFrame(key)
        Check(frame ~= nil, key .. ": view frame exists")
        if frame then
            Check(not rawget(frame, "gaBroken"), key .. ": view built without an error")
            Check(frame:IsVisible(), key .. ": view visible")
            Check(frame:GetNumPoints() > 0 and frame:GetLeft() ~= nil, key .. ": view anchored")
        end
        for _, other in ipairs({ "items", "loot", "log" }) do
            local o = GA.UI.StorageViewFrame(other)
            if other ~= key and o then
                Check(not o:IsShown(), key .. ": view " .. other .. " hidden")
            end
        end
    end
    -- the loot filter's controls are there
    GA.UI.ShowStorageView("loot")
    Pump(0.3)
    Check(VisibleText("Loot filter on"), "loot filter checkbox shown")
    Check(VisibleText("Bots' loot that no rule decides"), "default action label shown")
end)

Test("storage tab: clear empty states", function()
    GA.UI.ShowStorageView("items")
    Pump(0.3)
    Check(VisibleText("Loading the guild storage"), "items: loading text before the storage arrived")
    GA.State.ReplaceStorage({}, { kinds = 0, items = 0 })
    Pump(0.3)
    Check(VisibleText("The guild storage is empty."), "items: empty storage explained")
    GA.UI.ShowStorageView("loot")
    Pump(0.3)
    Check(VisibleText("Loading the loot filter"), "loot: loading text before the settings arrived")
    GA.State.SetLootSettings({ on = true, dai = 4, dpl = 0, al = true })
    GA.State.ReplaceLootRules({})
    Pump(0.3)
    Check(VisibleText("Built in: crafting materials"), "loot: no rules: the built-in default row explains the way")
    Check(not VisibleText("Built in: grey items"), "loot: no junk row while the server sells no junk")
    GA.State.SetLootSettings({ on = true, dai = 4, dpl = 0, al = true, junk = 1, junkok = true })
    Pump(0.3)
    Check(VisibleText("Built in: grey items and white weapons and armour (junk), except what the looter"), "loot: the junk rule shows as a built-in rule")
    GA.State.SetLootSettings({ on = true, dai = 4, dpl = 0, al = true, junk = 1, junkok = true, kwg = true })
    Pump(0.3)
    Check(VisibleText("Built in: grey items and white weapons and armour (junk), except gear a guild"), "loot: with KeepWornGear worn gear stays")
    GA.State.SetLootSettings({ on = true, dai = 4, dpl = 0, al = true, junk = 1, junkok = true })
    Pump(0.3)
    GA.UI.ShowStorageView("items")
    Pump(0.3)
    Check(GA.UI.JunkQuality() == 1, "items: junk quality from the loot settings")
    GA.State.SetLootSettings({ on = true, dai = 4, dpl = 0, al = true, junk = -1, junkok = true })
    Check(GA.UI.JunkQuality() == -1, "items: no junk sold when the guild switched it off")
    -- Sell / Sell all / Disenchant on the storage items (docs/storage.md "Selling and disenchanting"); the rows need
    -- item data the stub has not, so the logic behind their buttons is checked.
    GA.State.ReplaceStorage({ { it = { { 100, 0, 3, 2, 7, 250, 25 }, { 200, 0, 5, 15, 0, 0, -1 } } } },
        { kinds = 2, items = 8, ench = 10, enchn = "Enchbot" })
    local green = GA.State.storage.items["100:0"]
    local junk = GA.State.storage.items["200:0"]
    Check(green and green.sell == 250 and green.ds == 25 and junk and junk.ds == -1, "items: sell price and disenchant skill from the tuples")
    Check(not GA.UI.CanDisenchant(green), "items: no disenchant while the enchanter's skill is too low")
    Check(GA.UI.DisenchantHint(green)[2]:find("needs Enchanting 25", 1, true) ~= nil, "items: the hint names the skill it needs")
    Check(not GA.UI.CanDisenchant(junk), "items: no disenchant for an item that cannot be disenchanted")
    GA.State.SetEnchanter(40, "Enchbot")
    Check(GA.UI.CanDisenchant(green), "items: disenchant once an enchanter has the skill")
    Check(GA.UI.SellHint(green, true)[1]:find("Sells 3", 1, true) ~= nil, "items: Sell all names the whole stack")
    Check(GA.UI.SellHint(junk, false)[1]:find("No vendor buys this", 1, true) ~= nil, "items: no Sell without a vendor price")
    Check(GA.UI.SellHint(green, false)[1]:find("guild bank", 1, true) ~= nil, "items: the sale's gold goes into the guild bank")
    Check(GA.UI.DeleteHint(junk, true)[1]:find("deletes 5", 1, true) ~= nil, "items: Delete all names the whole stack of what no vendor buys")
    -- Types and slots inside a category (the owner: [quote left out]): only what the storage holds.
    GA.State.ReplaceStorage({ { it = {
        { 301, 0, 1, 4, 4, 100, 30, 1 },   -- plate head
        { 302, 0, 1, 4, 4, 100, 30, 5 },   -- plate chest
        { 303, 0, 1, 4, 2, 100, 30, 20 },  -- leather robe (chest)
        { 304, 0, 1, 2, 7, 100, 30, 13 },  -- one-handed sword
        { 305, 0, 4, 7, 5, 10, -1, 0 },    -- cloth (trade goods)
        { 306, 0, 1, 4, 1, 100, 30 },      -- cloth armour from an older server (no inventory type)
    } } }, { kinds = 6, items = 9 })
    local types, slots = GA.UI.StorageFilterChoices(GA.State.storage.items, "armor", "all")
    Check(#types == 3, "filter: the armour types present (cloth, leather, plate)")
    local labels = {}
    for _, key in ipairs(types) do
        labels[#labels + 1] = GA.UI.StorageTypeLabel(key, false)
    end
    Check(table.concat(labels, ",") == "Cloth,Leather,Plate", "filter: armour types by name, " .. table.concat(labels, ","))
    Check(#slots == 2 and slots[1] == 1 and slots[2] == 5, "filter: the slots of all armour (head, chest; the robe counts as chest)")
    local _, plateSlots = GA.UI.StorageFilterChoices(GA.State.storage.items, "armor", 404)
    Check(#plateSlots == 2, "filter: plate -> head and chest")
    local _, leatherSlots = GA.UI.StorageFilterChoices(GA.State.storage.items, "armor", 402)
    Check(#leatherSlots == 1 and GA.UI.StorageSlotLabel(leatherSlots[1]) == "Chest", "filter: leather -> chest only")
    Check(GA.UI.StorageSlotOf(GA.State.storage.items["306:0"]) == 0, "filter: no slot without the inventory type or item data")
    local allTypes = GA.UI.StorageFilterChoices(GA.State.storage.items, "all", "all")
    Check(#allTypes == 5, "filter: every type in the category All")
    Check(GA.UI.StorageTypeLabel(214, true) == "Weapon: Miscellaneous" and GA.UI.StorageTypeLabel(207, false) == "One-handed swords",
        "filter: type labels with and without the class")
    Check(GA.UI.StorageEmptyText({ loaded = true, kinds = 6, items = GA.State.storage.items }, 0, "", "armor", true)
        == "Nothing of this type, slot, quality or item level here.", "filter: the empty text says the filter is the reason")
    -- Quality and item level (the owner: [quote left out]): from the
    -- server's qi list, beside the tuples.
    GA.State.ReplaceStorage({ { it = {
        { 401, 0, 1, 4, 4, 100, 30, 1 },   -- plate head, epic, 60
        { 402, 0, 1, 4, 2, 100, 30, 5 },   -- leather chest, rare, 45
        { 403, 0, 2, 2, 7, 100, 30, 13 },  -- sword, uncommon, 52
        { 404, 0, 40, 7, 5, 10, -1, 0 },   -- cloth, common, 50
    }, qi = { { 4, 60 }, { 3, 45 }, { 2, 52 }, { 1, 50 } } } }, { kinds = 4, items = 44 })
    Equal(GA.State.storage.items["401:0"].q, 4, "quality from the qi list")
    Equal(GA.State.storage.items["403:0"].il, 52, "item level from the qi list")
    local byLevel = GA.State.StorageList(nil, "ilvl")
    Equal(byLevel[1].e, 401, "sort by item level: highest first")
    Equal(byLevel[2].e, 403, "sort by item level: then the sword")
    local byQuality = GA.State.StorageList(nil, "quality")
    Check(byQuality[1].e == 401 and byQuality[4].e == 404, "sort by quality: epic first, common last")
    local byCount = GA.State.StorageList(nil, "count")
    Equal(byCount[1].e, 404, "sort by amount: the most first")
    GA.State.ApplyStorageDelta({ it = { { 402, 0, 3, 4, 2, 100, 30, 5 } }, qi = { { 3, 45 } } })
    Equal(GA.State.storage.items["402:0"].il, 45, "a delta keeps quality and item level")
    GA.State.ApplyStorageDelta({ it = { { 405, 0, 1, 4, 3, 100, 30, 7 } } })
    Check(GA.State.storage.items["405:0"].q == nil and GA.State.StorageQuality(GA.State.storage.items["405:0"]) >= 0,
        "an older server's delta: quality from the item cache")
    -- The reserve of an item (the owner: [quote left out]).
    GA.State.ReplaceStorage({ { it = { { 401, 0, 30, 0, 2, 50, -1, 0, 20 }, { 402, 0, 5, 0, 2, 50, -1, 0 } } } }, { kinds = 2, items = 35 })
    local reserved = GA.State.storage.items["401:0"]
    Check(reserved and reserved.res == 20 and GA.State.storage.items["402:0"].res == 0, "reserve: from the ninth tuple field (0 from an older server)")
    Check(GA.UI.ReserveHint(reserved)[1]:find("leave 20", 1, true) ~= nil and GA.UI.ReserveHint(reserved)[1]:find("take 10", 1, true) ~= nil,
        "reserve: the hint names the reserve and what the bots may take")
    Check(GA.UI.ReserveHint(GA.State.storage.items["402:0"])[1]:find("No reserve", 1, true) ~= nil, "reserve: the hint without a reserve")
    Check(GA.State.AuctionSurplus(reserved, { keep = 5, stack = 5, on = true }, 20) == 10,
        "reserve: the auction surplus keeps the reserve when it is above the rule's keep")
    Check(GA.State.AuctionSurplus(reserved, { keep = 25, stack = 5, on = true }, 20) == 5,
        "reserve: the rule's keep counts when it is above the reserve")
    GA.State.ReplaceStorage({}, { kinds = 0, items = 0 })
    GA.UI.ShowStorageView("items")
    local itemsView = GA.UI.StorageViewFrame("items")
    Check(itemsView and itemsView.typeDD and itemsView.slotDD, "filter: the Items view has the type and slot dropdowns")
    -- Auction rules for whole categories and the mark of what a rule will sell (docs/storage.md section 8).
    GA.State.ReplaceAuctionRules({ { e = 100, rp = 0, keep = 1, stack = 1, price = 0, on = true, auto = 10, listed = 0 } }, { srv = true })
    GA.State.ReplaceAuctionCategories({ { cat = "cloth", keep = 20, stack = 10, on = true } })
    GA.State.ReplaceAuctionListings({ { e = 300, rp = 0, n = 10 } })
    local own, ownCat = GA.State.EffectiveAuctionRule(green, GA.UI.StorageCategoryOf)
    Check(own and own.keep == 1 and ownCat == nil, "auction: an item's own rule comes first")
    local cloth = { e = 300, rp = 0, n = 45, cls = 7, sub = 5 }
    local catRule, catKey = GA.State.EffectiveAuctionRule(cloth, GA.UI.StorageCategoryOf)
    Check(catRule and catKey == "cloth" and catRule.keep == 20, "auction: a kind without its own rule takes its category's")
    Check(GA.State.AuctionSurplus(cloth, catRule, 20) == 20, "auction: what is above keep, in whole stacks (45 - 20 = 25 -> 20)")
    Check(GA.State.AuctionSurplus({ n = 25 }, catRule, 20) == 0, "auction: nothing below keep plus one stack")
    Check(GA.State.AuctionListedCount(300, 0) == 10, "auction: the listed count of a category's kind")
    local meat = { e = 400, rp = 0, n = 99, cls = 7, sub = 8 }
    Check(GA.State.EffectiveAuctionRule(meat, GA.UI.StorageCategoryOf) == nil, "auction: no rule for a category without one")
    GA.State.ReplaceAuctionCategories({ { cat = "cloth", keep = 20, stack = 10, on = false } })
    Check(GA.State.AuctionSurplus(cloth, GA.State.AuctionCategoryRule("cloth"), 20) == 0, "auction: a rule that is off sells nothing")
    -- The Darkmoon Faire cards and decks (the owner, 2026-11-25: [quote left out]): the server names the category in the
    -- tuple's tenth field; quest items (class 12) without it stay "other".
    GA.State.ReplaceStorage({ { it = { { 19234, 0, 3, 12, 0, 12500, -1, 0, 0, "darkmoon" }, { 5000, 0, 1, 12, 0, 0, -1, 0, 0 } } } },
        { kinds = 2, items = 4 })
    local card = GA.State.storage.items["19234:0"]
    Check(card and card.cat == "darkmoon" and GA.UI.StorageCategoryOf(card.cls, card.sub, card.cat) == "darkmoon",
        "darkmoon: a card is in the category Darkmoon cards")
    Check(GA.UI.StorageCategoryOf(12, 0, GA.State.storage.items["5000:0"].cat) == "other", "darkmoon: another quest item stays Other")
    Check(GA.UI.StorageCategoryLabel("darkmoon") == "Darkmoon cards", "darkmoon: the category's label")
    GA.State.ReplaceAuctionCategories({ { cat = "darkmoon", keep = 1, stack = 1, on = true } })
    local dmRule, dmKey = GA.State.EffectiveAuctionRule(card, GA.UI.StorageCategoryOf)
    Check(dmRule and dmKey == "darkmoon", "darkmoon: an auction rule of the category holds for the cards")
    GA.State.ReplaceAuctionCategories({})
    GA.State.ReplaceAuctionRules({}, { srv = true })
    GA.State.ReplaceStorage({}, { kinds = 0, items = 0 })
    GA.UI.ShowStorageView("loot")
    Pump(0.3)
    GA.UI.ShowStorageView("log")
    Pump(0.3)
    Check(VisibleText("Loading the recent loot"), "log: loading text before the log arrived")
    GA.State.ReplaceLootLog({})
    Pump(0.3)
    Check(VisibleText("Nothing looted yet."), "log: empty log explained")
    GA.State.storage = { items = {}, kinds = 0, total = 0, loaded = false }
    GA.State.loot = { settings = {}, rules = {}, log = {}, loaded = false, logLoaded = false }
end)

Test("tooltip on a label", function()
    local parent = CreateFrame("Frame", nil, UIParent)
    local label = GA.UI.W.Label(parent, "label")
    GA.UI.W.SetTooltip(label, "Title", { "line" })
    local hit = rawget(label, "gaTooltipFrame")
    Check(hit ~= nil, "a frame carries the label's tooltip")
    Check(hit and hit:IsMouseEnabled(), "the frame takes the mouse")
end)

Test("recruit through the UI", function()
    GA.UI.ShowPage("recruitment")
    Pump(0.3)
    local nameBox, recruitButton
    for _, o in ipairs(Stub.Objects()) do
        if rawget(o, "__type") == "EditBox" and rawget(o, "__maxLetters") == 12 then
            nameBox = o
        end
        if rawget(o, "__type") == "Button" and o:GetText() == "Recruit" then
            recruitButton = o
        end
    end
    Check(nameBox and recruitButton, "recruit widgets found")
    nameBox:SetText("X1")
    Pump(0.2)
    Check(not recruitButton:IsEnabled(), "invalid name disables Recruit")
    nameBox:SetText("Aldric")
    Pump(0.2)
    Check(not recruitButton:IsEnabled() or GA.State.guild.fac ~= "A", "taken name disables Recruit")
    nameBox:SetText("Testbot")
    Pump(0.2)
    Check(recruitButton:IsEnabled(), "valid name enables Recruit")
    local before = GA.State.BotCount()
    recruitButton:Click()
    Pump(1)
    Equal(GA.State.BotCount(), before + 1, "new bot in roster")
    local found = false
    for _, bot in pairs(GA.State.bots) do
        found = found or (bot.n == "Testbot" and bot.lvl == 1 and bot.o == "AUTO")
    end
    Check(found, "recruit starts at level 1 on Autopilot")
    local done
    GA.Protocol.Recruit(1, 6, 0, "Deathbot", function(ok, code) done = code end)
    Pump(1)
    Equal(done, "LOCKED", "Death Knight locked below level 55")
    GA.Protocol.Recruit(2, 1, 0, "Orcbot", function(ok, code) done = code end)
    Pump(1)
    Equal(done, "INVALID_COMBO", "other faction rejected")
end)

Test("take control and return", function()
    local bot = Bots()[2]
    MockState().bots[bot.g].combat = true
    local done
    GA.Protocol.TakeControl(bot.g, function(ok, code, msg, reason) done = { ok, code, reason } end)
    Pump(1)
    Equal(done and done[3], "IN_COMBAT", "swap refused in combat")
    MockState().bots[bot.g].combat = false
    MockState().bots[bot.g].dead = false
    done = nil
    GA.Protocol.TakeControl(bot.g, function(ok) done = ok end)
    Pump(2)
    Equal(done, true, "swap accepted")
    Equal(GA.State.control.g, bot.g, "controlling the bot")
    Equal(GA.State.bots[bot.g].s, "controlled", "bot state controlled")
    Check(_G.GuildAdventureControlBar:IsShown(), "control bar visible")
    -- The bar is wider and its info line wraps (the owner: [quote left out]); Details shows what the bot
    -- does now and next (GET what=plan).
    Equal(_G.GuildAdventureControlBar:GetWidth(), GA.UI.CONTROL_BAR_WIDTH, "control bar is wider")
    Check(not GA.UI.ControlDetailsShown(), "details closed at first")
    GA.UI.ToggleControlDetails(true)
    Check(GA.UI.ControlDetailsShown(), "details open")
    Pump(1)
    local plan = GA.State.PlanOf(bot.g)
    Check(plan ~= nil and #plan.lines >= 4, "the plan of the controlled bot arrived")
    local text = GA.UI.ControlDetailsText()
    Check(text:find("Now", 1, true) ~= nil and text:find("Quest log", 1, true) ~= nil and text:find("Upkeep", 1, true) ~= nil,
        "details are grouped: now, quest log, upkeep")
    Check(text:find("Hand in", 1, true) ~= nil and text:find("waits until level", 1, true) ~= nil,
        "details name what waits to be handed in and what waits")
    -- why the squad's leader waits (a "wait" line) stands under Now, before the quest log, in its own colour
    local waitAt, logAt = text:find("Wartank: waiting for its squad", 1, true), text:find("Quest log", 1, true)
    Check(waitAt ~= nil and logAt ~= nil and waitAt < logAt, "details show why the squad's leader waits, under Now")
    Check(text:find("|cffff9933Wartank: waiting", 1, true) ~= nil, "the leader's wait has its own colour")
    GA.UI.ToggleControlDetails()
    Check(not GA.UI.ControlDetailsShown(), "details close again")
    GA.UI.ToggleControlDetails(true)
    done = nil
    GA.Protocol.ReturnControl(function(ok) done = ok end)
    Pump(2)
    Equal(done, true, "return accepted")
    Equal(GA.State.control.g, GA.State.control.leader, "back on the leader")
    Check(not _G.GuildAdventureControlBar:IsShown(), "control bar hidden")
    Check(not GA.UI.ControlDetailsShown(), "details hidden with the bar")
end)

Test("bench and activate", function()
    local bot = Bots()[3]
    GA.UI.ShowBot(bot.g)
    Pump(0.2)
    local bench
    for _, o in ipairs(Stub.Objects()) do
        if rawget(o, "__type") == "Button" and o:GetText() == "Bench" and o:IsVisible() then
            bench = o
        end
    end
    bench:Click()
    Check(#Stub.popups == 1, "bench asks for confirmation")
    Stub.AcceptPopup()
    Pump(1)
    Equal(GA.State.bots[bot.g].s, "benched", "benched")
    local done
    GA.Protocol.TakeControl(bot.g, function(ok, code, msg, reason) done = reason end)
    Pump(1)
    Equal(done, "BENCHED", "benched bots cannot be taken over")
    GA.Protocol.SetBotState(bot.g, "active", function() end)
    Pump(1)
    Equal(GA.State.bots[bot.g].s, "active", "active again")
end)

Test("buy a gold unlock", function()
    SlashCmdList["GUILDADVENTURE"]("mock renown 3000")
    Pump(1)
    Check(GA.State.guild.rl >= 4, "renown level 4")
    local slotUnlock
    for _, u in ipairs(GA.State.unlocks) do
        if u.id == 10 then
            slotUnlock = u
        end
    end
    Equal(slotUnlock.st, "available", "10 slots can be bought")
    GA.UI.ShowPage("progression")
    Pump(0.3)
    local buy
    for _, o in ipairs(Stub.Objects()) do
        if rawget(o, "__type") == "Button" and string.find(o:GetText() or "", "^Buy") and o:IsVisible() then
            buy = o
        end
    end
    Check(buy ~= nil, "Buy button visible")
    local bank = GA.State.guild.bank
    buy:Click()
    Stub.AcceptPopup()
    Pump(1)
    Equal(GA.State.guild.slots, 10, "10 slots after purchase")
    Equal(GA.State.guild.bank, bank - 1000000, "100g paid from the guild bank")
end)

Test("paid slot tiers the free slots cover are included, not for sale", function()
    SlashCmdList["GUILDADVENTURE"]("mock renown 25000")
    Pump(1)
    Check(GA.State.guild.rl >= 6, "renown level 6")
    Check((GA.State.guild.slots or 0) >= 20, "20 free slots")
    local tier15, tier40
    for _, u in ipairs(GA.State.unlocks) do
        if u.id == 12 then
            tier15 = u
        elseif u.id == 23 then
            tier40 = u
        end
    end
    Equal(tier15.st, "covered", "15 slots are covered by the 20 free ones")
    Check(tier40.st ~= "covered", "39 slots are not covered")
    GA.UI.ShowPage("progression")
    Pump(0.3)
    local included, buy15 = false, false
    for _, o in ipairs(Stub.Objects()) do
        if rawget(o, "__type") == "FontString" and o:IsVisible() and string.find(o:GetText() or "", "Included", 1, true) then
            included = true
        end
        if rawget(o, "__type") == "Button" and o:IsVisible() and (o:GetText() or "") == "Buy 200g" then
            buy15 = true
        end
    end
    Check(included, "a covered tier says Included")
    Check(not buy15, "no Buy button for the covered 15 slots")
    local code
    GA.Protocol.BuyUnlock(12, function(ok, c)
        code = ok and "OK" or c
    end)
    Pump(1)
    Equal(code, "COVERED", "buying a covered tier is refused")
    Equal(GA.State.guild.ulfrom, "bank", "unlocks are paid from the guild bank")
    Equal(GA.State.guild.ulgold, GA.State.guild.bank, "what unlocks can pay is the guild bank")
end)

Test("simulation runs", function()
    local before = 0
    for _, bot in pairs(GA.State.bots) do
        before = before + bot.lvl
    end
    local events = #GA.State.events
    Pump(120)
    local after = 0
    for _, bot in pairs(GA.State.bots) do
        after = after + bot.lvl
    end
    Check(after > before, "bots gained levels (" .. before .. " -> " .. after .. ")")
    Check(#GA.State.events >= math.min(events + 5, GA.State.MAX_EVENTS), "events arrived")
    for _, bot in pairs(GA.State.bots) do
        Check(bot.why and bot.why ~= "", bot.n .. " has a why line")
    end
    GA.UI.ShowPage("overview")
    Pump(0.3)
end)

Test("while you were away", function()
    SlashCmdList["GUILDADVENTURE"]("mock away")
    Pump(1.5)
    Check(GA.State.away ~= nil, "away summary received")
    Check(_G.GuildAdventureAwayFrame and _G.GuildAdventureAwayFrame:IsShown(), "away popup shown")
    _G.GuildAdventureAwayFrame:Hide()
    Pump(1)
    Check(GA.State.away == nil, "summary acknowledged")
    Check(MockState().away == nil, "mock server cleared the pending summary")
end)

Test("watch mode", function()
    local done
    GA.Protocol.Watch(true, function(ok, code, msg, reason) done = { ok, code, reason } end)
    Pump(1)
    Equal(done and done[3], "NO_ORDER", "the leader without an order cannot be watched")
    Check(not GA.State.IsWatching(), "not watching on the leader")

    local bot
    for _, b in ipairs(Bots()) do
        if b.s == "active" and not b.dead and not b.combat and not bot then
            bot = b
        end
    end
    GA.Protocol.TakeControl(bot.g, function() end)
    Pump(2)
    Equal(GA.State.control.g, bot.g, "controlling the bot")
    Equal(GA.State.control.watch, false, "control state says not watching")

    local watch
    for _, o in ipairs(Stub.Objects()) do
        if rawget(o, "__type") == "Button" and o:GetText() == "Watch" and o:IsVisible()
            and o:GetParent() == _G.GuildAdventureControlBar then
            watch = o
        end
    end
    Check(watch ~= nil, "Watch button on the control bar")
    Check(watch and watch:IsEnabled(), "Watch button enabled")
    watch:Click()
    Pump(1)
    Check(GA.State.IsWatching(), "watching after the click")
    Equal(watch:GetText(), "Take over", "the button offers to take over")
    Check(string.find(GA.State.bots[bot.g].why or "", "^Watch mode: ") ~= nil, "why line says watch mode")
    Check(string.find(_G.GuildAdventureControlBar.title:GetText() or "", "Watching") ~= nil, "bar title says watching")

    SlashCmdList["GUILDADVENTURE"]("watch off")
    Pump(1)
    Check(not GA.State.IsWatching(), "/ga watch off takes over")
    Equal(watch:GetText(), "Watch", "the button offers to watch again")
    Equal(GA.State.bots[bot.g].why, "Played by you", "why line back to played by you")

    SlashCmdList["GUILDADVENTURE"]("watch")
    Pump(1)
    Check(GA.State.IsWatching(), "/ga watch toggles it on")
    done = nil
    GA.Protocol.ReturnControl(function(ok) done = ok end)
    Pump(2)
    Equal(done, true, "return accepted while watching")
    Equal(GA.State.control.g, GA.State.control.leader, "back on the leader")
    Check(not GA.State.IsWatching(), "a swap ends watch mode")
    Check(not _G.GuildAdventureControlBar:IsShown(), "control bar hidden")
end)

Test("leader order and watch mode on the leader", function()
    local leader = GA.State.control.leader
    Equal(GA.State.control.g, leader, "playing the leader")
    Equal(GA.State.leader.g, leader, "LEADER record arrived")
    Equal(GA.State.leader.o, "HOLD", "the leader holds by default")
    GA.UI.ShowPage("overview")
    Pump(0.3)
    local watch
    for _, o in ipairs(Stub.Objects()) do
        if rawget(o, "__type") == "Button" and o:GetText() == "Watch" and o:IsVisible()
            and o:GetParent() ~= _G.GuildAdventureControlBar then
            watch = o
        end
    end
    Check(watch ~= nil, "Watch button in the leader panel")
    Check(watch and not watch:IsEnabled(), "Watch disabled while the leader holds")

    GA.UI.SetLeaderOrder("AUTO")
    Pump(1)
    Equal(GA.State.leader.o, "AUTO", "the leader is on Autopilot")
    Pump(0.3)
    Check(watch:IsEnabled(), "Watch enabled with an order")
    watch:Click()
    Pump(1)
    Check(GA.State.IsWatching(), "watching the leader")
    Check(string.find(GA.State.leader.why or "", "^Watch mode: ") ~= nil, "the leader's why line says watch mode")
    Check(_G.GuildAdventureControlBar:IsShown(), "control bar shown while watching the leader")
    Check(string.find(_G.GuildAdventureControlBar.title:GetText() or "", "Watching") ~= nil, "bar title says watching")

    GA.UI.SetLeaderOrder("HOLD")
    Pump(1)
    Equal(GA.State.leader.o, "HOLD", "back to hold")
    Check(not GA.State.IsWatching(), "hold ends watch mode")
    Check(not _G.GuildAdventureControlBar:IsShown(), "control bar hidden again on the leader")

    local zone
    for _, z in ipairs(GA.State.zoneList) do
        if GA.State.ZoneMatchesFaction(z) and not zone then
            zone = z
        end
    end
    GA.UI.SetLeaderOrder("LEVEL_ZONE", zone.id)
    Pump(1)
    Equal(GA.State.leader.o, "LEVEL_ZONE", "the leader levels in a zone")
    Equal(GA.State.leader.oz, zone.id, "in the chosen zone")
    GA.UI.SetLeaderOrder("HOLD")
    Pump(1)
end)

Test("settings and leaving demo mode", function()
    GA.UI.ShowPage("settings")
    Pump(0.2)
    Stub.sentAddon = {}
    SlashCmdList["GUILDADVENTURE"]("mock off")
    Pump(3)
    Equal(GA.State.mode, "live", "live mode again")
    Check(#Stub.sentAddon >= 1, "HELLO sent to the live server")
    Check(_G.GuildAdventureMainFrame:IsShown(), "window still open")
    SlashCmdList["GUILDADVENTURE"]("mock on")
    Pump(1.5)
    Equal(GA.State.BotCount() >= 4, true, "demo guild persisted across mode switches")
end)

Test("mouse wheel scrolls the roster", function()
    GA.UI.ShowPage("roster")
    Pump(0.3)
    for _, o in ipairs(Stub.Objects()) do
        if o.rows and o.items and o.scroll and #o.items > 0 then
            Stub.Fire(o, "OnMouseWheel", -1)
        end
    end
    Pump(0.2)
end)

-- the owner, 2026-11-26: an open dropdown closed by itself whenever a bot's update came in. In the 3.3.5a FrameXML
-- UIDropDownMenu_Initialize of any dropdown hides every DropDownList, and the template's OnHide closes them all.
Test("dropdown menus stay open while the UI refreshes", function()
    local W = GA.UI.W
    local inits, closes = 0, 0
    local oldInit, oldClose = UIDropDownMenu_Initialize, CloseDropDownMenus
    UIDropDownMenu_Initialize = function(...) inits = inits + 1; return oldInit(...) end
    CloseDropDownMenus = function() closes = closes + 1 end
    local ok, err = pcall(function()
        local parent = CreateFrame("Frame", nil, UIParent)
        local a = W.DropDown(parent, 100, function() return { { text = "A", value = 1 } } end)
        local b = W.DropDown(parent, 100, function() return { { text = "B", value = 2 } } end)
        Equal(inits, 0, "making a dropdown builds no menu (it would close the one that is open)")
        Equal(#Stub.OpenDropDown(a, 1), 1, "the menu is built when it opens")
        UIDROPDOWNMENU_OPEN_MENU = a
        b:Hide()
        Equal(closes, 0, "hiding another dropdown leaves the open menu alone")
        a:Hide()
        Equal(closes, 1, "hiding the dropdown whose menu is open closes it")
        DropDownList1 = CreateFrame("Frame", nil, UIParent)
        DropDownList1:Show()
        Check(W.MenuOpen(), "a menu of the addon counts as open")
        UIDROPDOWNMENU_OPEN_MENU = CreateFrame("Frame", nil, UIParent)
        Check(not W.MenuOpen(), "a menu of another addon does not")
        DropDownList1:Hide()
        UIDROPDOWNMENU_OPEN_MENU = a
        Check(not W.MenuOpen(), "a hidden list is no open menu")
    end)
    UIDROPDOWNMENU_OPEN_MENU, DropDownList1 = nil, nil
    UIDropDownMenu_Initialize, CloseDropDownMenus = oldInit, oldClose
    Check(ok, tostring(err))
end)

Test("no stray globals", function()
    local allowed = {
        GuildAdventure = true, GuildAdventureDB = true,
        SLASH_GUILDADVENTURE1 = true, SLASH_GUILDADVENTURE2 = true,
        UIDROPDOWNMENU_MENU_VALUE = true, -- FrameXML global, written by the stub's dropdown helper
    }
    for k in pairs(_G) do
        if not globalsBefore[k] and not allowed[k] and not string.match(k, "^GuildAdventure")
            and not string.match(k, "^BINDING_[%u_]+GUILDADVENTURE") then
            Check(false, "unexpected global " .. tostring(k))
        end
    end
end)

Test("textures used", function()
    local list = {}
    for path in pairs(Stub.textures) do
        if path ~= "" then
            list[#list + 1] = path
        end
    end
    table.sort(list)
    -- GA_TEXTURES_OUT=<file> writes the list, e.g. to check it against the client MPQs
    local outPath = os.getenv("GA_TEXTURES_OUT")
    if outPath and outPath ~= "" then
        local out = assert(io.open(outPath, "w"))
        out:write(table.concat(list, "\n"), "\n")
        out:close()
    end
    Check(#list > 5, "textures recorded")
end)

print(string.format("\n%d checks passed, %d failed, %d Lua errors", passed, failed, #Stub.errors))
os.exit(failed == 0 and #Stub.errors == 0 and 0 or 1)
