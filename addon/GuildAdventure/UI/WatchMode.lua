-- Watch mode (docs/take-control.md §5.12, docs/protocol.md §9 "Watch mode"): while you control a guild bot, the server's AI can
-- play that same character on with its order while your client only watches (its movement keys are locked, as
-- under a fear effect). "Take over" gives you the controls back at once. No loading screen either way.
--
-- This file adds the Watch / Take over button to the control bar (UI/ControlBar.lua builds the bar) and the
-- entry points UI.SetWatch / UI.ToggleWatch (slash command /ga watch, key binding GUILDADVENTURE_WATCH).

local GA = GuildAdventure
local UI = GA.UI
local W = UI.W
local Data = GA.Data
local State = GA.State

local button
local requestPending = false
local wasWatching = false

local function Watched()
    local g = State.control.g
    if g and g == State.control.leader then
        return State.leader
    end
    return State.GetBot(g)
end

function UI.SetWatch(on)
    on = on and true or false
    local playingLeader = State.control.g ~= nil and State.control.g == State.control.leader
    if on and not State.IsControllingBot() and not playingLeader then
        UI.Notify("Watch mode needs a guild bot or your leader: take control of one first.", true)
        return
    end
    if requestPending then
        return
    end
    requestPending = true
    GA.Protocol.Watch(on, UI.RequestCallback(nil, function()
        requestPending = false
        UI.UpdateWatchButton()
    end))
    UI.UpdateWatchButton()
end

function UI.ToggleWatch()
    UI.SetWatch(not State.IsWatching())
end

local function Create(bar)
    button = W.Button(bar, "Watch", 72, 18, function()
        UI.ToggleWatch()
    end)
    button:SetPoint("TOPRIGHT", bar, "TOPRIGHT", -6, -6)
    -- the bar's title would run under the button otherwise
    if bar.title then
        bar.title:SetPoint("RIGHT", button, "LEFT", -4, 0)
    end
    W.SetTooltip(button, function()
        return State.IsWatching() and "Take over" or "Watch"
    end, function()
        local bot = Watched()
        local name = bot and bot.n or State.control.cn or "this bot"
        if State.IsWatching() then
            return { "The AI plays " .. name .. " with its order; you watch.", "Click to take the controls back." }
        end
        return { "Let the AI play " .. name .. " with its order while you watch.",
            "Your movement keys are locked until you take over again." }
    end)
end

-- Runs after the control bar's own update (it registers first, UI/ControlBar.lua comes first in the .toc).
function UI.UpdateWatchButton()
    local bar = _G.GuildAdventureControlBar
    if not bar then
        return
    end
    if not button then
        Create(bar)
    end
    local watching = State.IsWatching()
    button:SetText(watching and "Take over" or "Watch")
    W.SetEnabled(button, State.IsConnected() and (State.IsControllingBot() or watching) and not State.control.busy
        and not requestPending)
    -- a swap that waits for the end of a fight (UI.PendingSwap, UI/ControlBar.lua) keeps its own line on the bar
    local pendingSwap = UI.PendingSwap and UI.PendingSwap()
    if watching and bar:IsShown() and not State.control.busy and not pendingSwap then
        local bot = Watched()
        bar.title:SetText("|cff66ccffWatching:|r " .. (bot and (Data.ClassColorCode(bot.class) .. (bot.n or "?") .. "|r")
            or (State.control.cn or "?")))
        local why = bot and bot.why or ""
        bar.info:SetText(why ~= "" and why or "The AI plays, you watch.")
    end
    if UI.FitControlBar then
        UI.FitControlBar()
    end
end

GA.On("CONTROL", function()
    local watching = State.IsWatching()
    if watching ~= wasWatching and State.IsConnected() and not State.control.busy then
        local name = State.control.cn or "your bot"
        if watching then
            UI.Notify("Watch mode: the AI plays " .. name .. ". Take over whenever you like.")
        elseif State.control.g then
            UI.Notify("You control " .. name .. " again.")
        end
    end
    wasWatching = watching
    UI.UpdateWatchButton()
end)
GA.On("CONNECTION", UI.UpdateWatchButton)
GA.On("BOT", function(guid)
    if guid == State.control.g then
        UI.UpdateWatchButton()
    end
end)
GA.On("ROSTER", UI.UpdateWatchButton)
GA.On("LEADER", UI.UpdateWatchButton)
