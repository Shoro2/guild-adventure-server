-- The leader panel of the Overview tab (docs/orders.md "The leader"): the guild leader can be given an order like a
-- guild bot. Without one ("Hold position", the default) the AI only keeps it where it stands while you play a bot; with
-- one the server's order planner plays it whenever the AI holds it: while you play a bot, in watch mode while you play
-- the leader yourself, and in the offline window after you log out.
--
-- UI.CreateLeaderPanel(parent) returns the panel; UI.RefreshLeaderPanel(panel) updates it (Overview refresh).

local GA = GuildAdventure
local UI = GA.UI
local W = UI.W
local Data = GA.Data
local State = GA.State
local Protocol = GA.Protocol

local LEADER_ORDERS = { "HOLD", "AUTO", "LEVEL_ZONE", "IDLE", "FOLLOW" }
local HOLD_LABEL = "Hold position"
local HOLD_DESC = "The default: the AI keeps your leader where it stands while you play a bot, and it logs out with you."

local function OrderLabel(order)
    if order == "HOLD" then
        return HOLD_LABEL
    end
    return Data.OrderLabel(order)
end

local function OrderText(leader)
    if not leader.o then
        return "?"
    end
    if leader.o == "LEVEL_ZONE" and leader.ozn and leader.ozn ~= "" then
        return OrderLabel(leader.o) .. ": " .. leader.ozn
    end
    return OrderLabel(leader.o)
end

local function Allowed(order)
    return order == "HOLD" or State.IsOrderAllowed(order)
end

local function UsableZones(level)
    local list = {}
    for _, zone in ipairs(State.zoneList) do
        if State.ZoneMatchesFaction(zone) and State.ZoneAllowsOrder(zone, "LEVEL_ZONE") then
            local fit = State.ZoneFit(zone, level)
            if fit ~= "low" then
                list[#list + 1] = zone
            end
        end
    end
    return list
end

function UI.SetLeaderOrder(order, zoneId)
    local leader = State.leader
    if not leader or not leader.g then
        return
    end
    local extras = nil
    if zoneId then
        extras = { zone = zoneId }
    end
    local text = order == "HOLD" and ((leader.n or "Your leader") .. " holds its position again.")
        or ((leader.n or "Your leader") .. ": " .. OrderLabel(order) .. ".")
    Protocol.SetOrder(leader.g, order, extras, UI.RequestCallback(text))
end

function UI.CreateLeaderPanel(parent)
    local p = W.Panel(parent)
    p:SetHeight(96)

    p.title = W.Label(p, "", "GameFontNormal")
    p.title:SetPoint("TOPLEFT", p, "TOPLEFT", 10, -8)
    p.title:SetPoint("RIGHT", p, "RIGHT", -10, 0)
    p.info = W.Label(p, "", "GameFontHighlightSmall")
    p.info:SetPoint("TOPLEFT", p.title, "BOTTOMLEFT", 0, -3)
    p.info:SetPoint("RIGHT", p, "RIGHT", -10, 0)

    local orderLabel = W.Label(p, "Order", "GameFontNormalSmall")
    orderLabel:SetPoint("TOPLEFT", p.info, "BOTTOMLEFT", 0, -10)
    p.order = W.DropDown(p, 150, function(dd, level, menuValue)
        local leader = State.leader or {}
        if level == 2 and menuValue == "LEVEL_ZONE" then
            local items = {}
            for _, zone in ipairs(UsableZones(leader.lvl)) do
                items[#items + 1] = {
                    text = (zone.n or "?") .. string.format(" |cff9d9d9d(%d-%d)|r", zone.min or 0, zone.max or 0),
                    value = "LEVEL_ZONE:" .. zone.id,
                }
            end
            if #items == 0 then
                items[1] = { text = "No zone for this level", isTitle = true }
            end
            return items
        end
        local items = { { text = "Leader's order", isTitle = true } }
        for _, order in ipairs(LEADER_ORDERS) do
            local def = Data.ORDERS[order]
            if order == "LEVEL_ZONE" then
                items[#items + 1] = { text = OrderLabel(order), value = order, hasArrow = true,
                    disabled = not Allowed(order) }
            else
                items[#items + 1] = { text = OrderLabel(order), value = order, disabled = not Allowed(order),
                    tooltip = order == "HOLD" and HOLD_DESC or (def and def.desc) }
            end
        end
        return items
    end)
    p.order:SetPoint("LEFT", orderLabel, "RIGHT", -8, -2)
    p.order.onSelect = function(value)
        local order, zone = string.match(tostring(value), "^([%u_]+):?(%d*)$")
        if order then
            UI.SetLeaderOrder(order, tonumber(zone))
        end
    end

    p.watch = W.Button(p, "Watch", 80, 20, function()
        UI.ToggleWatch()
    end)
    p.watch:SetPoint("TOPRIGHT", p, "TOPRIGHT", -10, -34)
    W.SetTooltip(p.watch, function()
        return State.IsWatching() and "Take over" or "Watch"
    end, function()
        if State.IsWatching() then
            return { "The AI plays your leader with its order; you watch.", "Click to take the controls back." }
        end
        return { "Let the AI play your leader with its order while you watch (you play it now).",
            "Needs an order other than Hold position." }
    end)

    p.why = W.Label(p, "", "GameFontNormalSmall")
    p.why:SetPoint("TOPLEFT", orderLabel, "BOTTOMLEFT", 0, -12)
    p.why:SetPoint("RIGHT", p, "RIGHT", -10, 0)
    return p
end

function UI.RefreshLeaderPanel(p)
    if not p then
        return
    end
    local leader = State.leader or {}
    if not leader.g then
        p.title:SetText("Your leader")
        p.info:SetText("")
        p.why:SetText("")
        p.order:SetSelected(nil, "")
        p.order:SetEnabled(false)
        W.SetEnabled(p.watch, false)
        return
    end

    p.title:SetText("Your leader: " .. Data.ClassColorCode(leader.class) .. (leader.n or "?") .. "|r")
    p.info:SetText(string.format("Level %d %s - %s", leader.lvl or 1, Data.ClassName(leader.class), leader.zn or "?"))
    p.order:SetSelected(leader.o, OrderText(leader))
    p.order:SetEnabled(State.IsConnected() and State.HasFeature("orders"))
    p.why:SetText("|cffffd100Now:|r " .. (leader.why or ""))

    local playingLeader = State.control.g ~= nil and State.control.g == leader.g
    p.watch:SetText(State.IsWatching() and playingLeader and "Take over" or "Watch")
    W.SetEnabled(p.watch, State.IsConnected() and playingLeader and not State.control.busy
        and (State.IsWatching() or (leader.o ~= nil and leader.o ~= "HOLD")))
end
