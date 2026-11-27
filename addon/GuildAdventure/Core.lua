-- Guild Adventure client addon: namespace, saved variables, callbacks, timers, slash commands.
-- The addon is a thin view: the server (mod-guild-adventure) is authoritative.
-- Design: the design log §2.12 (UI) and docs/protocol.md (GADV protocol).

GuildAdventure = GuildAdventure or {}
local GA = GuildAdventure

GA.ADDON_NAME = "GuildAdventure"
GA.VERSION = "0.1.0"
GA.PREFIX = "GADV"
GA.PROTOCOL_VERSION = 1

-------------------------------------------------------------------------------
-- Output
-------------------------------------------------------------------------------

function GA.Print(msg)
    DEFAULT_CHAT_FRAME:AddMessage("|cff33ff99Guild Adventure|r: " .. tostring(msg))
end

function GA.Debug(msg)
    if GuildAdventureDB and GuildAdventureDB.debug then
        DEFAULT_CHAT_FRAME:AddMessage("|cff888888GA debug|r: " .. tostring(msg))
    end
end

-------------------------------------------------------------------------------
-- Callback bus (state -> UI). One failing listener never blocks the others.
-------------------------------------------------------------------------------

local listeners = {}

function GA.On(event, fn)
    local list = listeners[event]
    if not list then
        list = {}
        listeners[event] = list
    end
    list[#list + 1] = fn
end

function GA.Fire(event, ...)
    local list = listeners[event]
    if not list then
        return
    end
    for i = 1, #list do
        local ok, err = pcall(list[i], ...)
        if not ok then
            geterrorhandler()(err)
        end
    end
end

-------------------------------------------------------------------------------
-- Timers (3.3.5a has no C_Timer): one OnUpdate frame drives all of them.
-------------------------------------------------------------------------------

local timers = {}
local timerFrame = CreateFrame("Frame")
timerFrame:Hide()

local function TimerOnUpdate(self, elapsed)
    local now = GetTime()
    local i = 1
    while i <= #timers do
        local t = timers[i]
        if t.cancelled then
            table.remove(timers, i)
        elseif now >= t.at then
            if t.interval then
                t.at = now + t.interval
                i = i + 1
            else
                table.remove(timers, i)
            end
            local ok, err = pcall(t.fn)
            if not ok then
                geterrorhandler()(err)
            end
        else
            i = i + 1
        end
    end
    if #timers == 0 then
        self:Hide()
    end
end
timerFrame:SetScript("OnUpdate", TimerOnUpdate)

-- Runs fn once after delay seconds. Returns a handle with :Cancel().
function GA.After(delay, fn)
    local t = { at = GetTime() + delay, fn = fn }
    function t:Cancel()
        self.cancelled = true
    end
    timers[#timers + 1] = t
    timerFrame:Show()
    return t
end

-- Runs fn every interval seconds until the handle is cancelled.
function GA.Every(interval, fn)
    local t = GA.After(interval, fn)
    t.interval = interval
    return t
end

-------------------------------------------------------------------------------
-- Saved variables
-------------------------------------------------------------------------------

local DEFAULTS = {
    schema = 1,
    debug = false,
    ui = {
        scale = 1,
        width = 780,
        height = 520,
        point = nil,          -- { point, relativePoint, x, y }
        controlBarPoint = nil,
        minimapAngle = 200,
        minimapHide = false,
        chatEvents = true,
        awayPopup = true,
        settingsView = "behaviour", -- Settings tab: "behaviour" (bot settings) or "addon"
    },
    mock = {
        enabled = false,
        state = nil,          -- MockServer persists its simulated world here
    },
    zoneCache = nil,          -- { zv = n, zones = { ... } } (live server only)
    settingsCache = nil,      -- { sv = n, cats = { ... }, defs = { ... } }: the bot settings schema (live server only)
}

local function ApplyDefaults(target, defaults)
    for k, v in pairs(defaults) do
        if target[k] == nil then
            if type(v) == "table" then
                target[k] = {}
                ApplyDefaults(target[k], v)
            else
                target[k] = v
            end
        elseif type(v) == "table" and type(target[k]) == "table" then
            ApplyDefaults(target[k], v)
        end
    end
end
GA.ApplyDefaults = ApplyDefaults

-------------------------------------------------------------------------------
-- Lifecycle: modules register Init (ADDON_LOADED) and Login (PLAYER_LOGIN) hooks.
-------------------------------------------------------------------------------

local initHooks, loginHooks = {}, {}

function GA.OnInit(fn)
    initHooks[#initHooks + 1] = fn
end

function GA.OnLogin(fn)
    loginHooks[#loginHooks + 1] = fn
end

local function RunHooks(list)
    for i = 1, #list do
        local ok, err = pcall(list[i])
        if not ok then
            geterrorhandler()(err)
        end
    end
end

local eventFrame = CreateFrame("Frame")
GA.eventFrame = eventFrame
eventFrame:RegisterEvent("ADDON_LOADED")
eventFrame:RegisterEvent("PLAYER_LOGIN")
eventFrame:RegisterEvent("PLAYER_ENTERING_WORLD")
eventFrame:RegisterEvent("CHAT_MSG_ADDON")
eventFrame:SetScript("OnEvent", function(self, event, arg1, arg2, arg3, arg4)
    if event == "ADDON_LOADED" then
        if arg1 == GA.ADDON_NAME then
            GuildAdventureDB = GuildAdventureDB or {}
            ApplyDefaults(GuildAdventureDB, DEFAULTS)
            GA.db = GuildAdventureDB
            RunHooks(initHooks)
        end
    elseif event == "PLAYER_LOGIN" then
        RunHooks(loginHooks)
    elseif event == "PLAYER_ENTERING_WORLD" then
        GA.Fire("ENTERING_WORLD")
    elseif event == "CHAT_MSG_ADDON" then
        GA.Transport.OnAddonMessage(arg1, arg2, arg3, arg4)
    end
end)

-------------------------------------------------------------------------------
-- Slash commands
-------------------------------------------------------------------------------

local HELP = {
    "/ga - open or close the Guild Adventure window",
    "/ga roster | overview | recruit | progression - open a tab",
    "/ga reconnect - send a new handshake to the server",
    "/ga watch [on|off] - let the AI play the bot you control while you watch, or take over again",
    "/ga mock on|off - demo mode with a simulated server",
    "/ga mock reset - wipe the demo guild",
    "/ga mock renown <n> - demo: add renown points",
    "/ga mock away - demo: simulate an offline window",
    "/ga debug - toggle protocol debug output",
}

local function SlashHandler(input)
    input = input or ""
    local cmd, rest = input:match("^%s*(%S*)%s*(.-)%s*$")
    cmd = (cmd or ""):lower()

    if cmd == "" or cmd == "toggle" then
        GA.UI.Toggle()
    elseif cmd == "show" then
        GA.UI.Show()
    elseif cmd == "hide" then
        GA.UI.Hide()
    elseif cmd == "overview" or cmd == "roster" or cmd == "recruit" or cmd == "progression"
        or cmd == "settings" then
        GA.UI.Show(cmd == "recruit" and "recruitment" or cmd)
    elseif cmd == "reconnect" then
        GA.Protocol.Connect(true)
    elseif cmd == "watch" then
        local arg = (rest or ""):lower()
        if arg == "on" then
            GA.UI.SetWatch(true)
        elseif arg == "off" then
            GA.UI.SetWatch(false)
        else
            GA.UI.ToggleWatch()
        end
    elseif cmd == "debug" then
        GA.db.debug = not GA.db.debug
        GA.Print("Debug output " .. (GA.db.debug and "on" or "off") .. ".")
    elseif cmd == "mock" then
        GA.MockServer.SlashCommand(rest)
    elseif cmd == "help" then
        for i = 1, #HELP do
            GA.Print(HELP[i])
        end
    else
        GA.Print("Unknown command '" .. cmd .. "'. Type /ga help.")
    end
end

SLASH_GUILDADVENTURE1 = "/ga"
SLASH_GUILDADVENTURE2 = "/guildadventure"
SlashCmdList["GUILDADVENTURE"] = SlashHandler

-- Key binding labels for Bindings.xml (Key Bindings menu, "Guild Adventure" section).
BINDING_HEADER_GUILDADVENTURE = "Guild Adventure"
BINDING_NAME_GUILDADVENTURE_TOGGLE = "Open or close the window"
BINDING_NAME_GUILDADVENTURE_ROSTER = "Open the roster"
BINDING_NAME_GUILDADVENTURE_RETURN = "Return to your leader"
BINDING_NAME_GUILDADVENTURE_WATCH = "Watch mode: let the AI play / take over"
