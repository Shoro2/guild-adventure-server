-- Control bar: small always-visible frame while you play a bot ("Playing: <bot>",
-- Return to leader, quick switch, Details: what the bot does now and next). Hidden while you play your leader.
-- Also the single entry point for swap requests (UI.TakeControl / UI.ReturnControl), which
-- remembers the open window so it comes back after a swap that reloads the UI (Path R,
-- docs/take-control.md: the client relogs into the bot and every addon loads fresh), and which
-- waits out a fight: the server refuses a swap while either character is in combat (IN_COMBAT,
-- the design log 2.8), so such a request stays pending and is sent again until the fight is over.

local GA = GuildAdventure
local UI = GA.UI
local W = UI.W
local Data = GA.Data
local State = GA.State

local bar
local details -- the Details window under the bar (UI.ToggleControlDetails)

UI.CONTROL_BAR_WIDTH = 440
UI.DETAILS_REFRESH = 5 -- seconds between two asks for the plan while the Details window is open

-- A swap request the server refused with IN_COMBAT: { kind = "take" | "return", guid, name, since }.
local pending = nil
-- Counts the player's swap requests; a reply to an older one (a retry still on its way when the player
-- cancelled or asked for something else) is ignored.
local requestSeq = 0
UI.COMBAT_RETRY_INTERVAL = 2 -- seconds between two tries while a fight goes on
UI.COMBAT_RETRY_LIMIT = 120  -- seconds after which a pending swap gives up

local function SavePosition(frame)
    local point, _, relativePoint, x, y = frame:GetPoint()
    GA.db.ui.controlBarPoint = { point, relativePoint, x, y }
end

local function Create()
    bar = CreateFrame("Frame", "GuildAdventureControlBar", UIParent)
    bar:SetWidth(UI.CONTROL_BAR_WIDTH)
    bar:SetHeight(62)
    local p = GA.db.ui.controlBarPoint
    if p then
        bar:SetPoint(p[1], UIParent, p[2], p[3], p[4])
    else
        bar:SetPoint("TOP", UIParent, "TOP", 0, -12)
    end
    bar:SetFrameStrata("HIGH")
    bar:SetToplevel(true)
    bar:SetClampedToScreen(true)
    bar:SetBackdrop(W.BACKDROP_PANEL)
    bar:SetBackdropColor(0, 0, 0, 0.75)
    bar:SetBackdropBorderColor(0.25, 0.75, 1, 1)
    bar:EnableMouse(true)
    bar:SetMovable(true)
    bar:RegisterForDrag("LeftButton")
    bar:SetScript("OnDragStart", function(self)
        self:StartMoving()
    end)
    bar:SetScript("OnDragStop", function(self)
        self:StopMovingOrSizing()
        SavePosition(self)
    end)

    bar.icon = W.ClassIcon(bar, 28)
    bar.icon:SetPoint("TOPLEFT", bar, "TOPLEFT", 8, -8)
    bar.title = W.Label(bar, "", "GameFontNormal")
    bar.title:SetPoint("TOPLEFT", bar.icon, "TOPRIGHT", 8, 0)
    bar.title:SetPoint("RIGHT", bar, "RIGHT", -8, 0)
    -- The info line wraps (the bot's why line is often longer than one line): only its top and width are set, so its
    -- height follows the text, and the bar grows with it (FitBar).
    bar.info = W.Label(bar, "", "GameFontHighlightSmall")
    bar.info:SetPoint("TOPLEFT", bar.title, "BOTTOMLEFT", 0, -2)
    bar.info:SetWidth(UI.CONTROL_BAR_WIDTH - 52)

    bar.back = W.Button(bar, "Return to leader", 120, 20, function()
        UI.ReturnControl()
    end)
    bar.back:SetPoint("BOTTOMLEFT", bar, "BOTTOMLEFT", 8, 6)

    bar.switch = W.DropDown(bar, 90, function()
        local items = { { text = "Switch to", isTitle = true } }
        for _, bot in ipairs(State.SortedBots("name", false)) do
            if bot.s == "active" then
                items[#items + 1] = {
                    text = Data.ClassColorCode(bot.class) .. bot.n .. "|r (" .. (bot.lvl or "?") .. ")",
                    value = bot.g,
                    disabled = bot.dead, -- a bot in combat waits: the switch follows after the fight
                    tooltip = bot.why,
                }
            end
        end
        return items
    end)
    bar.switch:SetPoint("LEFT", bar.back, "RIGHT", -12, -2)
    bar.switch.onSelect = function(guid)
        UI.TakeControl(guid)
    end

    bar.open = W.Button(bar, "Guild", 50, 20, function()
        if State.control.g then
            UI.ShowBot(State.control.g)
        else
            UI.Show()
        end
    end)
    bar.open:SetPoint("BOTTOMRIGHT", bar, "BOTTOMRIGHT", -8, 6)

    bar.details = W.Button(bar, "Details", 64, 20, function()
        UI.ToggleControlDetails()
    end)
    bar.details:SetPoint("RIGHT", bar.open, "LEFT", -4, 0)
    W.SetTooltip(bar.details, "Details", { "What the bot does now and what comes next:",
        "its task, its quest log in the planner's eyes, upkeep, places it keeps away from." })
    bar:Hide()
end

-- The bar's height follows its info line (one to three lines).
function UI.FitControlBar()
    if not bar then
        return
    end
    local h = bar.info:GetStringHeight() or 12
    bar:SetHeight(math.max(62, 50 + math.min(h, 40)))
end

-------------------------------------------------------------------------------
-- Details: what the bot does now and next (GET what=plan, docs/protocol.md "The plan of a guild character")
-------------------------------------------------------------------------------

local KIND_COLOR = {
    now = "|cffffffff", task = "|cffffd100", squad = "|cff66ccff", wait = "|cffff9933", run = "|cff66ccff", errand = "|cffffd100",
    hand = "|cff40ff40", quest = "|cffe0e0e0", upkeep = "|cff99bbdd", aside = "|cff999999",
}
local KIND_SECTION = {
    now = "Now", task = "Now", squad = "Now", wait = "Now", run = "Now", errand = "Now",
    hand = "Quest log", quest = "Quest log", upkeep = "Upkeep", aside = "Upkeep",
}
UI.DETAILS_MAX_LINES = 40

local function DetailsName()
    local g = State.control.g
    local bot = State.GetBot(g)
    if bot then
        return Data.ClassColorCode(bot.class) .. (bot.n or "?") .. "|r"
    end
    return State.control.cn or "?"
end

-- The window's text, also for the tests.
function UI.ControlDetailsText()
    local plan = State.PlanOf(State.control.g)
    if not plan then
        return "|cff999999Asking the server...|r"
    end
    local out, section, shown = {}, nil, 0
    for _, line in ipairs(plan.lines) do
        if shown >= UI.DETAILS_MAX_LINES then
            out[#out + 1] = "|cff999999... and " .. (#plan.lines - shown) .. " more|r"
            break
        end
        local s = KIND_SECTION[line.k] or "More"
        if s ~= section then
            section = s
            out[#out + 1] = "|cffffd100" .. s .. "|r"
        end
        out[#out + 1] = "  " .. (KIND_COLOR[line.k] or "|cffe0e0e0") .. (line.t or "") .. "|r"
        shown = shown + 1
    end
    if #plan.lines == 0 then
        out[#out + 1] = "|cff999999Nothing planned.|r"
    end
    return table.concat(out, "\n")
end

local function RefreshDetails()
    if not details or not details:IsShown() then
        return
    end
    details.title:SetText("Now and next: " .. DetailsName())
    details.text:SetText(UI.ControlDetailsText())
    details:SetHeight(math.min(520, (details.text:GetStringHeight() or 14) + 40))
end

local function AskPlan()
    if details and details:IsShown() and State.control.g and State.IsConnected() then
        GA.Protocol.Get("plan", State.control.g)
    end
end

local function CreateDetails()
    details = CreateFrame("Frame", "GuildAdventureControlDetails", UIParent)
    details:SetWidth(UI.CONTROL_BAR_WIDTH)
    details:SetHeight(80)
    details:SetPoint("TOP", bar, "BOTTOM", 0, -2)
    details:SetFrameStrata("DIALOG") -- above the main window (HIGH), which it would hide under otherwise
    details:SetClampedToScreen(true)
    details:SetBackdrop(W.BACKDROP_PANEL)
    details:SetBackdropColor(0, 0, 0, 0.96) -- the main window shows through a lighter one
    details:SetBackdropBorderColor(0.25, 0.75, 1, 1)
    details:EnableMouse(true)
    details.title = W.Label(details, "", "GameFontNormal")
    details.title:SetPoint("TOPLEFT", details, "TOPLEFT", 10, -8)
    details.close = W.Button(details, "Close", 56, 18, function()
        details:Hide()
    end)
    details.close:SetPoint("TOPRIGHT", details, "TOPRIGHT", -6, -5)
    details.text = W.Label(details, "", "GameFontHighlightSmall")
    details.text:SetPoint("TOPLEFT", details.title, "BOTTOMLEFT", 0, -6)
    details.text:SetWidth(UI.CONTROL_BAR_WIDTH - 20)
    details:Hide()
end

function UI.ToggleControlDetails(show)
    if not bar then
        return
    end
    if not details then
        CreateDetails()
    end
    if show == nil then
        show = not details:IsShown()
    end
    if show then
        details:Show()
        RefreshDetails()
        AskPlan()
    else
        details:Hide()
    end
end

function UI.ControlDetailsShown()
    return details ~= nil and details:IsShown()
end

GA.Every(UI.DETAILS_REFRESH, AskPlan)
GA.On("PLAN", RefreshDetails)

local function Update()
    if not bar then
        Create()
    end
    -- shown while you play a bot, and while the AI plays your leader in watch mode (its Take over lives here too)
    if not State.IsConnected() or not (State.IsControllingBot() or State.IsWatching()) then
        bar:Hide()
        if details then
            details:Hide()
        end
        return
    end
    local bot = State.GetBot(State.control.g)
    if bot then
        bar.icon:SetClass(bot.class)
        bar.icon:Show()
        bar.title:SetText("Playing: " .. Data.ClassColorCode(bot.class) .. (bot.n or "?") .. "|r")
        bar.info:SetText(string.format("Level %d %s - %s", bot.lvl or 1, Data.ClassName(bot.class), bot.zn or "?"))
    else
        bar.icon:Hide()
        bar.title:SetText("Playing: " .. (State.control.cn or "?"))
        bar.info:SetText("")
    end
    if State.control.busy then
        bar.info:SetText("|cffffd100Swapping...|r")
    elseif pending then
        bar.info:SetText("|cffffd100Waiting for the fight to end: " .. (pending.kind == "return" and "back to your leader"
            or ("switch to " .. (pending.name or "bot"))) .. "|r")
    end
    W.SetEnabled(bar.back, not State.control.busy and State.IsControllingBot())
    bar.switch:SetSelected(nil, "Switch")
    bar.switch:SetEnabled(not State.control.busy)
    bar:Show()
    UI.FitControlBar()
    RefreshDetails()
end

-------------------------------------------------------------------------------
-- Swap requests
-------------------------------------------------------------------------------

UI.RESUME_WINDOW = 180 -- seconds a remembered window stays valid across a relog

local resumePending = false
local wasBusy = false
local swapTarget = nil -- set while a swap requested in this UI session has not finished

local function RememberWindow(target)
    swapTarget = target
    if UI.IsShown() then
        GA.db.ui.resume = { page = UI.CurrentPage(), bot = UI.SelectedBot(), at = time() }
    else
        GA.db.ui.resume = nil
    end
end

-- Tells the player in the chat as well: the red error line fades in seconds, and while you play
-- a bot the main window (with its status line) is usually closed.
local function Tell(text, isError)
    UI.Notify(text, isError)
    GA.Print(text)
end

-- The ACK only means "swap accepted"; the result is the next CONTROL_STATE (protocol §9).
local SendSwap -- forward declaration (the retry calls it)

local function StopPending(text, isError)
    pending = nil
    requestSeq = requestSeq + 1
    if text then
        Tell(text, isError)
    end
    if bar then
        Update()
    end
end

-- One swap request; a resend of a pending one stays quiet while the fight goes on.
SendSwap = function(kind, guid, name)
    RememberWindow(kind == "return" and State.control.leader or guid)
    local seq = requestSeq
    local callback = function(ok, code, msg, reason)
        if seq ~= requestSeq then
            return -- the player has cancelled or replaced this request meanwhile
        end
        if ok then
            if pending then
                pending = nil
                Update()
            end
            UI.Notify(kind == "return" and "Returning to your leader..." or ("Switching to " .. (name or "bot") .. "..."))
            return
        end
        GA.db.ui.resume = nil
        swapTarget = nil
        if reason == "IN_COMBAT" then
            if not pending then
                pending = { kind = kind, guid = guid, name = name, since = time() }
                Tell((msg and msg ~= "" and msg or "In combat") .. ": the switch follows as soon as the fight is over.")
                Update()
            end
            return
        end
        StopPending(GA.Protocol.ErrorText(code, msg, reason), true)
    end
    if kind == "return" then
        GA.Protocol.ReturnControl(callback)
    else
        GA.Protocol.TakeControl(guid, callback)
    end
end

-- Pressing the same switch again while it waits for a fight cancels it.
local function Request(kind, guid, name)
    if pending and pending.kind == kind and (kind == "return" or pending.guid == guid) then
        StopPending("Switch cancelled.")
        return
    end
    pending = nil
    requestSeq = requestSeq + 1
    SendSwap(kind, guid, name)
end

function UI.TakeControl(guid)
    local bot = State.GetBot(guid)
    Request("take", guid, bot and bot.n)
end

function UI.ReturnControl()
    Request("return", nil, nil)
end

-- The pending request, for the views: { kind, guid, name, since } or nil.
function UI.PendingSwap()
    return pending
end

local function RetryPending()
    if not pending or State.control.busy or not State.IsConnected() then
        return
    end
    if time() - pending.since > UI.COMBAT_RETRY_LIMIT then
        StopPending("The switch was given up: the fight lasted too long.", true)
        return
    end
    SendSwap(pending.kind, pending.guid, pending.name)
end

GA.Every(UI.COMBAT_RETRY_INTERVAL, RetryPending)

-- Reopens the remembered page once the new session has synced its roster.
local function TryResume()
    local r = GA.db and GA.db.ui.resume
    if not r then
        resumePending = false
        return
    end
    if time() - (r.at or 0) > UI.RESUME_WINDOW then
        GA.db.ui.resume = nil
        resumePending = false
        return
    end
    if not State.IsConnected() or State.BotCount() == 0 then
        return
    end
    resumePending = false
    GA.db.ui.resume = nil
    if UI.IsShown() then
        return
    end
    if r.page == "bot" and r.bot and State.GetBot(r.bot) then
        UI.ShowBot(r.bot)
    else
        UI.Show(r.page ~= "bot" and r.page or "roster")
    end
end

GA.On("ENTERING_WORLD", function()
    if GA.db.ui.resume then
        resumePending = true
        TryResume()
    end
end)
GA.On("ROSTER", function()
    if resumePending then
        TryResume()
    end
end)

GA.On("CONTROL", function()
    local busy = State.control.busy and true or false
    if busy and not wasBusy then
        local path = State.control.path
        if path == "relog" then
            UI.Notify("Switching characters through the character screen...")
        elseif path == "reload" then
            UI.Notify("Switching characters: loading screen ahead.")
        else
            UI.Notify("Switching characters... a short loading screen may follow.")
        end
    end
    wasBusy = busy
    -- the swap finished without reloading the UI (in-world switch): nothing to resume
    if swapTarget and not busy and State.control.g == swapTarget then
        swapTarget = nil
        GA.db.ui.resume = nil
    end
end)

GA.On("CONTROL_PENDING", function(_, name)
    -- a relog follows: keep the remembered window for the next UI session
    swapTarget = nil
    UI.Notify("Switching to " .. (name or "your character") .. ": loading screen ahead.")
end)

GA.On("CONTROL", function()
    -- the active character changed (any way, also a chat command): an older pending request is void
    if pending and ((pending.kind == "return" and State.control.g and State.control.g == State.control.leader)
            or (pending.kind == "take" and State.control.g == pending.guid)) then
        pending = nil
        requestSeq = requestSeq + 1
    end
end)
GA.On("CONTROL", Update)
GA.On("CONNECTION", Update)
GA.On("BOT", function(guid)
    if guid == State.control.g then
        Update()
    end
end)
GA.On("ROSTER", Update)
