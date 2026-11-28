-- Bot detail page (Roster tab): header with state and actions, and sub-tabs
-- Orders, Professions, Character, Talents, Inventory.

local GA = GuildAdventure
local UI = GA.UI
local W = UI.W
local Data = GA.Data
local State = GA.State
local Protocol = GA.Protocol

local SUBTABS = {
    { key = "orders", label = "Orders" },
    { key = "professions", label = "Professions", feature = "professions" },
    { key = "crafting", label = "Crafting" },
    { key = "character", label = "Character" },
    { key = "talents", label = "Talents", feature = "talents" },
    { key = "fight", label = "Combat" },
    { key = "role", label = "Dungeon role" },
    { key = "behaviour", label = "Behaviour" },
}

local page                -- the page frame
local selectedGuid = nil
local selectedSubtab = "orders"

local FIT_COLORS = { low = "|cff808080", fit = "|cff40ff40", stretch = "|cffffff00", high = "|cffff4040" }

local function CurrentBot()
    return State.GetBot(selectedGuid)
end

-------------------------------------------------------------------------------
-- Confirmation popup (generic)
-------------------------------------------------------------------------------

StaticPopupDialogs["GUILDADVENTURE_CONFIRM"] = {
    text = "%s",
    button1 = YES,
    button2 = NO,
    OnAccept = function(self, data)
        if data and data.onAccept then
            data.onAccept()
        end
    end,
    timeout = 0,
    whileDead = 1,
    hideOnEscape = 1,
}

function UI.Confirm(text, onAccept)
    StaticPopup_Show("GUILDADVENTURE_CONFIRM", text, nil, { onAccept = onAccept })
end

-------------------------------------------------------------------------------
-- Dismissing a bot: the character is deleted for good, so the bot's name has to be typed in.
-------------------------------------------------------------------------------

local function DismissNameMatches(box, data)
    return box and data and data.name and box:GetText():lower() == data.name:lower()
end

local function DismissBox(self)
    return self.editBox or (self.GetName and _G[self:GetName() .. "EditBox"])
end

StaticPopupDialogs["GUILDADVENTURE_DISMISS"] = {
    text = "%s",
    button1 = "Dismiss",
    button2 = CANCEL,
    hasEditBox = 1,
    maxLetters = 12,
    OnShow = function(self)
        local box = DismissBox(self)
        if box then
            box:SetText("")
            box:SetFocus()
        end
        if self.button1 then
            self.button1:Disable()
        end
    end,
    EditBoxOnTextChanged = function(self, data)
        local button = self:GetParent().button1
        if button then
            if DismissNameMatches(self, data) then
                button:Enable()
            else
                button:Disable()
            end
        end
    end,
    OnAccept = function(self, data)
        if DismissNameMatches(DismissBox(self), data) and data.onAccept then
            data.onAccept()
        end
    end,
    EditBoxOnEnterPressed = function(self)
        local parent = self:GetParent()
        local data = parent.data
        if DismissNameMatches(self, data) and data.onAccept then
            data.onAccept()
            parent:Hide()
        end
    end,
    EditBoxOnEscapePressed = function(self)
        self:GetParent():Hide()
    end,
    timeout = 0,
    whileDead = 1,
    hideOnEscape = 1,
}

function UI.DismissText(bot)
    local name = bot.n or "this bot"
    return string.format("Dismiss %s?\n\n%s is deleted for good. What the guild storage takes of its items (nothing soulbound) "
        .. "goes there and its gold into the guild bank; the slot is free again. Renown and unlocks stay.\n\n"
        .. "|cffff4040This cannot be undone.|r Type the name to confirm:", name, name)
end

function UI.AskDismiss(bot)
    if not bot then
        return
    end
    local g, name = bot.g, bot.n or ""
    StaticPopup_Show("GUILDADVENTURE_DISMISS", UI.DismissText(bot), nil, { name = name, onAccept = function()
        UI.DismissBot(g, name)
    end })
end

function UI.DismissBot(g, name)
    Protocol.DismissBot(g, name, UI.RequestCallback(name .. " was dismissed."))
end

-------------------------------------------------------------------------------
-- Zone helpers
-------------------------------------------------------------------------------

local function ZoneText(zone, level, withBand)
    local fit = State.ZoneFit(zone, level)
    local text = FIT_COLORS[fit] .. (zone.n or "?") .. "|r"
    if withBand ~= false then
        text = text .. string.format(" |cff9d9d9d(%d-%d)|r", zone.min or 0, zone.max or 0)
    end
    return text
end

local function UsableZones(order)
    local list = {}
    for _, zone in ipairs(State.zoneList) do
        if State.ZoneMatchesFaction(zone) and State.ZoneAllowsOrder(zone, order) then
            list[#list + 1] = zone
        end
    end
    return list
end

local function RecommendedZone(bot, order)
    local best
    for _, zone in ipairs(UsableZones(order or "LEVEL_ZONE")) do
        if State.ZoneFit(zone, bot.lvl) == "fit" and (not best or (zone.min or 0) > (best.min or 0)) then
            best = zone
        end
    end
    return best
end

-- The dungeons a bot can be sent into: those of its readiness list that are no raids, nearest to its level last.
function UI.DungeonChoices(bot)
    local list = {}
    for _, entry in ipairs(State.BotReadiness(bot)) do
        if not entry.d.raid and (entry.d.run == nil or entry.d.run) then
            list[#list + 1] = entry
        end
    end
    return list
end

-- The roles of the group a bot would lead into a dungeon (itself and the bots of its squad): the dungeon role its
-- owner gave a bot, else the role of the talents it levels with: { size, tank, heal, dps, tanks = {bots}, heals = {bots} }.
function UI.SquadRoles(bot)
    local roles = { size = 0, tank = 0, heal = 0, dps = 0, tanks = {}, heals = {} }
    local function count(member)
        roles.size = roles.size + 1
        local role = State.BotGroupRole(member)
        if role == "tank" then
            roles.tank = roles.tank + 1
            roles.tanks[#roles.tanks + 1] = member
        elseif role == "heal" then
            roles.heal = roles.heal + 1
            roles.heals[#roles.heals + 1] = member
        else
            roles.dps = roles.dps + 1
        end
    end
    count(bot)
    for _, other in pairs(State.bots) do
        if other.g ~= bot.g and other.sq == bot.g then
            count(other)
        end
    end
    return roles
end

-- What the second field of an order names: a zone, a dungeon (DUNGEON) or an item (FARM).
local function TargetKind(order)
    local info = Data.ORDERS[order]
    if info and info.needsDungeon then
        return "dungeon"
    elseif info and info.needsItem then
        return "item"
    end
    return "zone"
end

-- What the farm order's search box asks the server for: the id of a shift-clicked item link, a typed id, or a text
-- of at least two letters; nil for anything shorter.
function UI.FarmQuery(text)
    text = string.gsub(tostring(text or ""), "^%s*(.-)%s*$", "%1")
    local id = string.match(text, "item:(%d+)")
    if id then
        return id
    end
    if string.match(text, "^%d+$") or string.len(text) >= 2 then
        return text
    end
    return nil
end

-- A row of the farm order's item list: the name in its quality colour, and what the bot lacks for it.
function UI.FarmItemText(entry, bot)
    local c = ITEM_QUALITY_COLORS and ITEM_QUALITY_COLORS[entry.q or 1]
    local color = c and c.hex or "|cffffffff"
    local text = color .. (entry.n or "?") .. "|r"
    if entry.ok == false then
        text = text .. string.format(" |cff808080(from level %d)|r", entry.need or 0)
    end
    return text
end

function UI.DungeonText(entry)
    local color = entry.pct >= 90 and "|cff40ff40" or (entry.pct >= 50 and "|cffffd100" or "|cffff4040")
    return string.format("%s |cff9d9d9d(%d-%d)|r %s%d%%|r", entry.d.n or "?", entry.d.min or 0, entry.d.max or 0, color, entry.pct)
end

-------------------------------------------------------------------------------
-- Orders sub-tab
-------------------------------------------------------------------------------

local function DraftOrder(bot)
    local d = {
        o = bot.o or "AUTO",
        zone = (bot.oz and bot.oz ~= 0) and bot.oz or nil,
        aa = bot.aa ~= false,
        lc = bot.lc or 0,
        cont = bot.cont or -1,
        nd = bot.nd and true or false,
        gcap = bot.gcap or 0,
        -- A dungeon order: how often the dungeon is run (the runs the order still has).
        runs = (bot.o == "DUNGEON" and (bot.druns or 0) > 0) and bot.druns or 1,
    }
    if TargetKind(d.o) == "item" then
        d.itemName = bot.ozn
        d.count = bot.ofn or 0
    elseif not d.zone then
        local z = RecommendedZone(bot)
        d.zone = z and z.id or nil
    end
    return d
end

local function CreateOrders(parent)
    local f = CreateFrame("Frame", nil, parent)
    f:SetAllPoints(parent)

    local left = CreateFrame("Frame", nil, f)
    left:SetPoint("TOPLEFT", f, "TOPLEFT", 8, -8)
    left:SetPoint("BOTTOMLEFT", f, "BOTTOMLEFT", 8, 36)
    left:SetWidth(360)

    f.current = W.Label(left, "", "GameFontHighlight")
    f.current:SetPoint("TOPLEFT", left, "TOPLEFT", 0, 0)
    f.current:SetWidth(350)

    local orderLabel = W.Label(left, "New order", "GameFontNormalSmall")
    orderLabel:SetPoint("TOPLEFT", f.current, "BOTTOMLEFT", 0, -14)
    f.orderDD = W.DropDown(left, 160, function()
        local items = {}
        for _, key in ipairs(Data.ORDER_ORDER) do
            local allowed = State.IsOrderAllowed(key)
            local info = Data.ORDERS[key]
            items[#items + 1] = {
                text = info.label .. (allowed and "" or " |cff808080(later)|r"),
                value = key,
                disabled = not allowed,
                tooltip = info.desc .. (allowed and "" or "\n|cffff8040Not available on this server yet.|r"),
            }
        end
        return items
    end)
    f.orderDD:SetPoint("TOPLEFT", orderLabel, "BOTTOMLEFT", -16, -2)
    f.orderDD.onSelect = function(value)
        local was = TargetKind(f.draft.o)
        local now = TargetKind(value)
        f.draft.o = value
        -- (A zone id is no dungeon id and no item id.)
        if was ~= now then
            local bot = CurrentBot()
            local z = now == "zone" and bot and RecommendedZone(bot) or nil
            f.draft.zone = z and z.id or nil
            f.draft.itemName = nil
            f.draft.count = 0
        end
        f.dirty = true
        UI.RequestRefresh()
    end

    f.desc = W.Label(left, "", "GameFontHighlightSmall")
    f.desc:SetPoint("TOPLEFT", f.orderDD, "BOTTOMLEFT", 16, -4)
    f.desc:SetWidth(340)
    f.desc:SetHeight(42)
    f.desc:SetJustifyV("TOP")

    f.zoneLabel = W.Label(left, "Zone", "GameFontNormalSmall")
    f.zoneLabel:SetPoint("TOPLEFT", f.desc, "BOTTOMLEFT", 0, -6)
    f.zoneDD = W.DropDown(left, 220, function(_, level, menuValue)
        local bot = CurrentBot()
        if not bot then
            return {}
        end
        local order = f.draft and f.draft.o or "LEVEL_ZONE"
        -- A farm order: what the last search found for this bot.
        if TargetKind(order) == "item" then
            local items = {}
            if level == 1 then
                local found = State.farm.g == bot.g and State.farm.items or {}
                if #found == 0 then
                    items[#items + 1] = { text = State.farm.loaded and State.farm.g == bot.g and
                        "|cff808080Nothing creatures drop matches|r" or "|cff808080Search for an item first|r", isTitle = true }
                end
                for _, entry in ipairs(found) do
                    items[#items + 1] = {
                        text = UI.FarmItemText(entry, bot),
                        value = entry.item,
                        tooltip = string.format("%d kind%s of creature drop it, the lowest at level %d.", entry.k or 0,
                            (entry.k or 0) == 1 and "" or "s", entry.lv or 0)
                            .. (entry.ok == false and ("\n|cffff8040" .. (bot.n or "The bot") .. " needs level " .. (entry.need or 0)
                                .. " for it.|r") or ""),
                    }
                end
            end
            return items
        end
        -- A dungeon run: the dungeons in the bot's reach, with its readiness for each.
        if Data.ORDERS[order] and Data.ORDERS[order].needsDungeon then
            local items = {}
            if level == 1 then
                local list = UI.DungeonChoices(bot)
                if #list == 0 then
                    items[#items + 1] = { text = "|cff808080No dungeon in its reach|r", isTitle = true }
                end
                for _, entry in ipairs(list) do
                    items[#items + 1] = { text = UI.DungeonText(entry), value = entry.d.id }
                end
            end
            return items
        end
        local zones = UsableZones(order)
        local items = {}
        if level == 1 then
            items[#items + 1] = { text = "Recommended for level " .. (bot.lvl or 1), isTitle = true }
            local count = 0
            for _, zone in ipairs(zones) do
                local fit = State.ZoneFit(zone, bot.lvl)
                if (fit == "fit" or fit == "stretch") and count < 6 then
                    items[#items + 1] = { text = ZoneText(zone, bot.lvl), value = zone.id }
                    count = count + 1
                end
            end
            if count == 0 then
                items[#items + 1] = { text = "|cff808080No fitting zone|r", isTitle = true }
            end
            items[#items + 1] = { text = "All zones", isTitle = true }
            for _, c in ipairs(Data.CONTINENTS) do
                if c.id >= 0 then
                    items[#items + 1] = { text = c.name, value = "cont:" .. c.id, hasArrow = true }
                end
            end
        else
            local cont = tonumber(string.match(tostring(menuValue or ""), "^cont:(%-?%d+)$"))
            for _, zone in ipairs(zones) do
                if zone.cont == cont then
                    items[#items + 1] = { text = ZoneText(zone, bot.lvl), value = zone.id }
                end
            end
        end
        return items
    end)
    f.zoneDD:SetPoint("TOPLEFT", f.zoneLabel, "BOTTOMLEFT", -16, -2)
    f.zoneDD.onSelect = function(value)
        f.draft.zone = value
        if TargetKind(f.draft.o) == "item" then
            local entry = State.FarmItem(value)
            f.draft.itemName = entry and entry.n or nil
        end
        f.dirty = true
        UI.RequestRefresh()
    end

    -- A farm order (docs/orders.md "Farm orders"): the item is looked for by name, by id or by a shift-clicked item;
    -- what the server finds fills the list below.
    f.farmEdit = W.EditBox(left, 150, false, 80)
    f.farmEdit:SetPoint("LEFT", f.zoneLabel, "LEFT", 96, 0)
    function f.RunFarmSearch(text)
        local bot = CurrentBot()
        local query = UI.FarmQuery(text or f.farmEdit:GetText())
        if not bot then
            return
        end
        if not query then
            UI.Notify("Type at least two letters of the item's name, or its id.", true)
            return
        end
        Protocol.FarmSearch(bot.g, query, UI.RequestCallback(nil, function(ok)
            if ok and f.draft and TargetKind(f.draft.o) == "item" then
                -- One answer: that is the item.
                local found = State.farm.g == bot.g and State.farm.items or {}
                if #found == 1 then
                    f.draft.zone = found[1].item
                    f.draft.itemName = found[1].n
                    f.dirty = true
                end
            end
            UI.RequestRefresh()
        end))
    end
    f.farmEdit:SetScript("OnEnterPressed", function(self)
        self:ClearFocus()
        f.RunFarmSearch()
    end)
    f.farmSearch = W.Button(left, "Search", 64, 20, function()
        f.RunFarmSearch()
    end)
    f.farmSearch:SetPoint("LEFT", f.farmEdit, "RIGHT", 6, 0)
    f.farmEdit:Hide()
    f.farmSearch:Hide()
    -- The target stock: the order ends when the guild storage holds that many (0: it farms on and on).
    f.farmTargetLabel = W.Label(left, "Target stock (0 = no limit)", "GameFontNormalSmall")
    f.farmTargetLabel:SetPoint("TOPLEFT", f.zoneInfo, "BOTTOMLEFT", 0, -10)
    f.farmTarget = W.EditBox(left, 60, true, 7)
    f.farmTarget:SetPoint("LEFT", f.farmTargetLabel, "RIGHT", 12, 0)
    f.farmTarget:SetScript("OnTextChanged", function(self)
        local value = self:GetNumber() or 0
        if f.draft and TargetKind(f.draft.o) == "item" and value ~= (f.draft.count or 0) then
            f.draft.count = value
            f.dirty = true
        end
    end)
    f.farmTargetLabel:Hide()
    f.farmTarget:Hide()
    -- A shift-clicked item goes into the search box while it has the focus.
    if hooksecurefunc and ChatEdit_InsertLink then
        hooksecurefunc("ChatEdit_InsertLink", function(link)
            if link and f.farmEdit:IsVisible() and f.farmEdit.gaFocused and string.match(link, "item:%d+") then
                f.farmEdit:SetText(string.match(link, "%[(.-)%]") or "")
                f.farmEdit:ClearFocus()
                f.RunFarmSearch(link)
            end
        end)
    end
    f.zoneInfo = W.Label(left, "", "GameFontHighlightSmall")
    f.zoneInfo:SetPoint("TOPLEFT", f.zoneDD, "BOTTOMLEFT", 16, -4)
    f.zoneInfo:SetWidth(340)

    f.autoAdvance = W.CheckBox(left, "Move on to the next zone when out-levelled", function(self)
        f.draft.aa = W.IsChecked(self)
        f.dirty = true
    end)
    f.autoAdvance:SetPoint("TOPLEFT", f.zoneInfo, "BOTTOMLEFT", -4, -6)

    -- A dungeon order can ask for several runs: the group runs the dungeon that often, with repairs in between.
    f.runsLabel = W.Label(left, "Runs", "GameFontNormalSmall")
    f.runsLabel:SetPoint("TOPLEFT", f.zoneInfo, "BOTTOMLEFT", 0, -12)
    f.runsDD = W.DropDown(left, 70, function()
        local items = {}
        for _, n in ipairs({ 1, 2, 3, 5, 10 }) do
            items[#items + 1] = { text = tostring(n), value = n }
        end
        return items
    end)
    f.runsDD:SetPoint("LEFT", f.runsLabel, "RIGHT", -6, -2)
    f.runsDD.onSelect = function(value)
        f.draft.runs = value
        f.dirty = true
        UI.RequestRefresh()
    end

    -- Squad (docs/orders.md "Squads"): the bot follows another guild bot as a member of its squad; its own order waits
    -- meanwhile. Applied at once, not with the order.
    local squadLabel = W.Label(left, "Squad: follow", "GameFontNormalSmall")
    squadLabel:SetPoint("TOPLEFT", f.autoAdvance, "BOTTOMLEFT", 4, -12)
    f.squadDD = W.DropDown(left, 180, function()
        local bot = CurrentBot()
        local items = {}
        if not bot then
            return items
        end
        items[#items + 1] = { text = "Nobody (own order)", value = 0 }
        local others = {}
        for g, other in pairs(State.bots) do
            if g ~= bot.g and (other.sq or 0) == 0 then
                others[#others + 1] = other
            end
        end
        table.sort(others, function(a, b) return (a.n or "") < (b.n or "") end)
        for _, other in ipairs(others) do
            local gap = math.abs((other.lvl or 0) - (bot.lvl or 0))
            items[#items + 1] = {
                text = string.format("%s (level %d)", other.n or "?", other.lvl or 0) .. (gap > 3 and " |cff808080(too far apart)|r" or ""),
                value = other.g,
                tooltip = "Follows " .. (other.n or "?") .. ", fights beside it and takes the same quests. Its own order waits.",
            }
        end
        return items
    end)
    f.squadDD:SetPoint("TOPLEFT", squadLabel, "BOTTOMLEFT", -16, -2)
    f.squadDD.onSelect = function(value)
        local bot = CurrentBot()
        if not bot then
            return
        end
        local leader = value ~= 0 and State.bots[value]
        local text = leader and ((bot.n or "Bot") .. " follows " .. (leader.n or "?") .. " now.") or ((bot.n or "Bot") .. " plays its own order.")
        Protocol.SetSquad(bot.g, value, UI.RequestCallback(text, function()
            UI.RequestRefresh()
        end))
    end
    f.squadInfo = W.Label(left, "", "GameFontHighlightSmall")
    f.squadInfo:SetPoint("TOPLEFT", f.squadDD, "BOTTOMLEFT", 16, -4)
    f.squadInfo:SetWidth(340)

    -- Autopilot limits (feature auto_limits)
    local limits = W.Panel(f, 0.3)
    limits:SetPoint("TOPLEFT", left, "TOPRIGHT", 12, 0)
    limits:SetPoint("BOTTOMRIGHT", f, "BOTTOMRIGHT", -8, 36)
    f.limits = limits
    local limitsTitle = W.Header(limits, "Autopilot limits")
    limitsTitle:SetPoint("TOPLEFT", limits, "TOPLEFT", 10, -10)

    local lcLabel = W.Label(limits, "Level cap (0 = none)", "GameFontNormalSmall")
    lcLabel:SetPoint("TOPLEFT", limitsTitle, "BOTTOMLEFT", 0, -14)
    f.levelCap = W.EditBox(limits, 40, true, 2)
    f.levelCap:SetPoint("LEFT", lcLabel, "RIGHT", 12, 0)
    -- the client fires OnTextChanged one frame after SetText too, so only a value that
    -- differs from the draft counts as the player's change
    f.levelCap:SetScript("OnTextChanged", function(self)
        local value = self:GetNumber() or 0
        if f.draft and value ~= f.draft.lc then
            f.draft.lc = value
            f.dirty = true
        end
    end)

    local contLabel = W.Label(limits, "Stay on", "GameFontNormalSmall")
    contLabel:SetPoint("TOPLEFT", lcLabel, "BOTTOMLEFT", 0, -16)
    f.contDD = W.DropDown(limits, 140, function()
        local items = {}
        for _, c in ipairs(Data.CONTINENTS) do
            items[#items + 1] = { text = c.name, value = c.id }
        end
        return items
    end)
    f.contDD:SetPoint("LEFT", contLabel, "RIGHT", -6, -2)
    f.contDD.onSelect = function(value)
        f.draft.cont = value
        f.dirty = true
    end

    f.noDungeons = W.CheckBox(limits, "Avoid dungeons", function(self)
        f.draft.nd = W.IsChecked(self)
        f.dirty = true
    end)
    f.noDungeons:SetPoint("TOPLEFT", contLabel, "BOTTOMLEFT", -4, -14)

    local goldLabel = W.Label(limits, "Max gold per trip (0 = no limit)", "GameFontNormalSmall")
    goldLabel:SetPoint("TOPLEFT", f.noDungeons, "BOTTOMLEFT", 4, -10)
    f.goldCap = W.EditBox(limits, 60, true, 6)
    f.goldCap:SetPoint("LEFT", goldLabel, "RIGHT", 12, 0)
    f.goldCap:SetScript("OnTextChanged", function(self)
        local value = (self:GetNumber() or 0) * 10000
        if f.draft and math.floor(value / 10000) ~= math.floor((f.draft.gcap or 0) / 10000) then
            f.draft.gcap = value
            f.dirty = true
        end
    end)
    f.limitsNote = W.Label(limits, "", "GameFontDisableSmall")
    f.limitsNote:SetPoint("BOTTOMLEFT", limits, "BOTTOMLEFT", 10, 10)
    f.limitsNote:SetPoint("RIGHT", limits, "RIGHT", -10, 0)
    f.limitsLock = W.LockedOverlay(limits)

    f.apply = W.Button(f, "Apply order", 120, 22, function()
        local bot = CurrentBot()
        if not bot or not f.draft then
            return
        end
        local d = f.draft
        local opts = {}
        if Data.ORDERS[d.o] and Data.ORDERS[d.o].needsDungeon then
            if not d.zone or not State.dungeons[d.zone] then
                UI.Notify("Pick a dungeon first.", true)
                return
            end
            opts.zone = d.zone
            opts.runs = d.runs or 1
        elseif TargetKind(d.o) == "item" then
            if not d.zone then
                UI.Notify("Pick an item first: search for it by name.", true)
                return
            end
            opts.zone = d.zone
            opts.n = d.count or 0
        elseif d.o == "LEVEL_ZONE" or (Data.ORDERS[d.o] and Data.ORDERS[d.o].needsZone) then
            if not d.zone then
                UI.Notify("Pick a zone first.", true)
                return
            end
            opts.zone = d.zone
            opts.aa = d.aa
        end
        if d.o == "AUTO" and State.HasFeature("auto_limits") then
            opts.lc, opts.cont, opts.nd, opts.gcap = d.lc, d.cont, d.nd, d.gcap
        end
        W.SetEnabled(f.apply, false)
        Protocol.SetOrder(bot.g, d.o, opts, UI.RequestCallback("Order sent to " .. (bot.n or "bot") .. ".", function(ok)
            if ok then
                f.dirty = false
            end
            W.SetEnabled(f.apply, true)
            UI.RequestRefresh()
        end))
    end)
    f.apply:SetPoint("BOTTOMLEFT", f, "BOTTOMLEFT", 8, 6)
    f.reset = W.Button(f, "Reset", 80, 22, function()
        f.dirty = false
        f.draft = nil
        UI.RequestRefresh()
    end)
    f.reset:SetPoint("LEFT", f.apply, "RIGHT", 6, 0)
    f.pendingNote = W.Label(f, "", "GameFontDisableSmall")
    f.pendingNote:SetPoint("LEFT", f.reset, "RIGHT", 10, 0)
    return f
end

local function RefreshOrders(f, bot)
    if not f.draft or f.draftGuid ~= bot.g or not f.dirty then
        f.draft = DraftOrder(bot)
        f.draftGuid = bot.g
    end
    local d = f.draft
    f.loading = true

    local current = "Current order: |cffffd100" .. Data.OrderLabel(bot.o) .. "|r"
    if bot.o == "FARM" and bot.ozn and bot.ozn ~= "" then
        current = current .. ": " .. bot.ozn .. ((bot.ofn or 0) > 0 and
            string.format(" (%d of %d in the guild storage)", bot.ofs or 0, bot.ofn) or
            string.format(" (%d in the guild storage, no target)", bot.ofs or 0))
    elseif bot.o == "DUNGEON" and bot.ozn and bot.ozn ~= "" then
        current = current .. ": " .. bot.ozn
    elseif bot.o == "LEVEL_ZONE" and bot.ozn and bot.ozn ~= "" then
        current = current .. " in " .. bot.ozn .. (bot.aa and " (auto-advance)" or "")
    elseif bot.o == "AUTO" and bot.lc and bot.lc > 0 then
        current = current .. " (cap " .. bot.lc .. ")"
    end
    f.current:SetText(current)

    f.orderDD:SetSelected(d.o, Data.OrderLabel(d.o))
    f.desc:SetText((Data.ORDERS[d.o] and Data.ORDERS[d.o].desc) or "")

    local needsZone = Data.ORDERS[d.o] and Data.ORDERS[d.o].needsZone
    local needsDungeon = Data.ORDERS[d.o] and Data.ORDERS[d.o].needsDungeon
    local needsItem = TargetKind(d.o) == "item"
    f.zoneDD:SetEnabled((needsZone or needsDungeon or needsItem) and State.IsConnected())
    local zone = d.zone and State.zones[d.zone]
    if needsItem then
        f.farmEdit:Show()
        f.farmSearch:Show()
        W.SetEditEnabled(f.farmEdit, State.IsConnected())
        W.SetEnabled(f.farmSearch, State.IsConnected())
        f.farmTargetLabel:Show()
        f.farmTarget:Show()
        W.SetEditText(f.farmTarget, d.count or 0)
        W.SetEditEnabled(f.farmTarget, State.IsConnected())
    else
        f.farmEdit:Hide()
        f.farmSearch:Hide()
        f.farmTargetLabel:Hide()
        f.farmTarget:Hide()
    end
    if needsItem then
        f.zoneLabel:SetText("Item to farm")
        local mine = State.farm.g == bot.g
        local entry = d.zone and mine and State.FarmItem(d.zone) or nil
        local name = entry and entry.n or d.itemName
        if d.zone and name and name ~= "" then
            f.zoneDD:SetSelected(d.zone, name)
            if entry then
                f.zoneInfo:SetText(string.format("%d kind%s of creature drop it, the lowest at level %d.%s", entry.k or 0,
                    (entry.k or 0) == 1 and "" or "s", entry.lv or 0, entry.ok == false and
                    (" |cffff8040" .. (bot.n or "It") .. " needs level " .. (entry.need or 0) .. " for it and will tell you so.|r") or ""))
            else
                f.zoneInfo:SetText("")
            end
        else
            local found = mine and #State.farm.items or 0
            f.zoneDD:SetSelected(nil, found > 0 and (found .. " found: choose one") or "Search for an item first")
            f.zoneInfo:SetText("|cff9d9d9dType a name or an item id (or shift-click an item) and search. Only what creatures drop can be farmed.|r")
        end
        f.autoAdvance:Hide()
        f.runsLabel:Hide()
        f.runsDD:Hide()
    elseif needsDungeon then
        -- The dungeon names come once per connection.
        if not State.dungeonsLoaded and State.IsConnected() and not f.askedDungeons then
            f.askedDungeons = true
            Protocol.Get("dungeons")
        end
        f.zoneLabel:SetText("Dungeon")
        local chosen
        for _, entry in ipairs(UI.DungeonChoices(bot)) do
            if entry.d.id == d.zone then
                chosen = entry
            end
        end
        if chosen then
            f.zoneDD:SetSelected(chosen.d.id, chosen.d.n or "?")
            local members = bot.sqm or 0
            local roles = UI.SquadRoles(bot)
            local group = string.format("%d tank%s, %d healer%s, %d damage", roles.tank, roles.tank == 1 and "" or "s",
                roles.heal, roles.heal == 1 and "" or "s", roles.dps)
            if roles.tank == 0 or roles.heal == 0 then
                group = group .. " |cffff8040(" .. (roles.tank == 0 and "no tank" or "")
                    .. ((roles.tank == 0 and roles.heal == 0) and ", " or "") .. (roles.heal == 0 and "no healer" or "") .. ")|r"
            end
            -- How ready the tanks and healers are in their dungeon sets (a role the owner gave them), and a hint when
            -- the tank does not lead: the group walks behind its leader.
            local sets = ""
            local function setOf(member, what)
                local pct = State.BotRoleReadiness(member, chosen.d.id)
                if pct then
                    local color = pct >= 90 and "|cff40ff40" or (pct >= 50 and "|cffffd100" or "|cffff4040")
                    sets = sets .. string.format(" %s %s: dungeon set %s%d%%|r.", what, member.n or "?", color, pct)
                end
            end
            for _, member in ipairs(roles.tanks) do
                setOf(member, "Tank")
            end
            for _, member in ipairs(roles.heals) do
                setOf(member, "Healer")
            end
            if roles.tank > 0 and State.BotGroupRole(bot) ~= "tank" then
                sets = sets .. " |cffff8040The tank does not lead the squad: the group follows " .. (bot.n or "this bot") .. ".|r"
            end
            -- An order that is going: the runs it still has.
            if bot.o == "DUNGEON" and bot.oz == chosen.d.id and (bot.druns or 0) > 0 then
                sets = sets .. string.format(" |cff9d9d9dThe order has %d run%s left (%d done).|r", bot.druns,
                    bot.druns == 1 and "" or "s", bot.drund or 0)
            end
            f.zoneInfo:SetText(string.format("%s is %d%% ready (level %d-%d, gear level %d asked). It leads %s: %s.%s",
                bot.n or "It", chosen.pct, chosen.d.min or 0, chosen.d.max or 0, chosen.d.gl or 0,
                members > 0 and ("a squad of " .. (members + 1)) or "|cffff8040no squad, it would go alone|r", group, sets))
        else
            f.zoneDD:SetSelected(nil, "Choose a dungeon")
            f.zoneInfo:SetText(bot.gl and "" or "|cff9d9d9dKnown while the bot is in the world.|r")
        end
        f.autoAdvance:Hide()
        f.runsLabel:Show()
        f.runsDD:Show()
        f.runsDD:SetSelected(d.runs or 1, tostring(d.runs or 1))
    elseif needsZone then
        f.runsLabel:Hide()
        f.runsDD:Hide()
        f.zoneLabel:SetText("Zone")
        if zone then
            f.zoneDD:SetSelected(zone.id, ZoneText(zone, bot.lvl, false))
            local fit = State.ZoneFit(zone, bot.lvl)
            local fitText = {
                low = "Too low: " .. bot.n .. " out-levels this zone (no XP from quests).",
                fit = "Good fit for level " .. bot.lvl .. ".",
                stretch = "A bit high for level " .. bot.lvl .. ": slower, more deaths.",
                high = "Too dangerous for level " .. bot.lvl .. ".",
            }
            f.zoneInfo:SetText(FIT_COLORS[fit] .. fitText[fit] .. "|r  |cff9d9d9d" .. Data.ContinentName(zone.cont) .. "|r")
        else
            f.zoneDD:SetSelected(nil, "Choose a zone")
            f.zoneInfo:SetText("")
        end
        f.autoAdvance:Show()
        f.autoAdvance:SetChecked(d.aa)
    else
        f.zoneLabel:SetText("|cff808080Zone (not used by this order)|r")
        f.zoneDD:SetSelected(nil, "-")
        f.zoneInfo:SetText("")
        f.autoAdvance:Hide()
        f.runsLabel:Hide()
        f.runsDD:Hide()
    end

    -- limits box
    local unlocked = State.HasFeature("auto_limits")
    if not unlocked then
        f.limitsLock:SetLock("Autopilot limits", State.FeatureRequirement("auto_limits"),
            "Set a level cap, keep bots on one continent, avoid dungeons or cap their spending.")
    else
        f.limitsLock:Hide()
    end
    local isAuto = d.o == "AUTO"
    local editable = unlocked and isAuto and State.IsConnected()
    W.SetEditText(f.levelCap, d.lc or 0)
    W.SetEditText(f.goldCap, math.floor((d.gcap or 0) / 10000))
    f.contDD:SetSelected(d.cont, Data.ContinentName(d.cont))
    f.noDungeons:SetChecked(d.nd)
    W.SetEditEnabled(f.levelCap, editable)
    W.SetEditEnabled(f.goldCap, editable)
    W.SetEnabled(f.noDungeons, editable)
    f.contDD:SetEnabled(editable)
    f.limitsNote:SetText(isAuto and "" or "Limits only apply to the Autopilot order.")

    -- squad
    local leader = (bot.sq or 0) ~= 0 and State.bots[bot.sq] or nil
    local members = tonumber(bot.sqm) or 0
    if leader then
        f.squadDD:SetSelected(bot.sq, (leader.n or "?") .. " (level " .. (leader.lvl or 0) .. ")")
    else
        f.squadDD:SetSelected(0, "Nobody (own order)")
    end
    f.squadDD:SetEnabled(State.IsConnected() and members == 0)
    local squadText = {
        active = "Follows its squad leader; its own order waits.",
        off = "Squads are switched off on this server.",
        benched = "Its squad leader is benched: it plays its own order meanwhile.",
        away = "Its squad leader is offline: it plays its own order meanwhile.",
        gap = "Too many levels apart from its squad leader: it plays its own order meanwhile.",
        leads = "Leads a squad of " .. (members + 1) .. ": the members follow it and fight beside it.",
    }
    local info = squadText[bot.sqs or "none"] or (members > 0 and squadText.leads or "")
    -- Quest partners (docs/orders.md "Quest partners"): a squad the server formed for one hard quest, and the hard quests
    -- the bot keeps until another guild bot has them too.
    if (tonumber(bot.sqq) or 0) > 0 then
        local title = (bot.sqqn and bot.sqqn ~= "") and bot.sqqn or ("quest " .. bot.sqq)
        if leader then
            info = "Quest partner of " .. (leader.n or "?") .. " for \"" .. title .. "\": they finish it together, then part."
        else
            info = "Leads a quest partner squad for \"" .. title .. "\": they finish it together, then part."
        end
    end
    if bot.qwn and bot.qwn ~= "" then
        info = (info ~= "" and (info .. "\n") or "") .. "|cffffd100Waits for a guild partner:|r " .. bot.qwn
    end
    f.squadInfo:SetText(info)

    W.SetEnabled(f.apply, State.IsConnected() and State.IsOrderAllowed(d.o))
    f.pendingNote:SetText(f.dirty and "|cffffd100Unsaved changes|r" or "")
    f.loading = false
end

-------------------------------------------------------------------------------
-- Professions sub-tab
-------------------------------------------------------------------------------

local function DraftProfessions(bot)
    local sec = {}
    for _, id in ipairs(bot.sec or {}) do
        sec[id] = true
    end
    return { p1 = bot.p1 or 0, p2 = bot.p2 or 0, t1 = bot.t1 or 0, t2 = bot.t2 or 0, sec = sec }
end

local function SkillOf(bot, skillId)
    for _, entry in ipairs(bot.sk or {}) do
        if entry[1] == skillId then
            return entry[2], entry[3]
        end
    end
    return nil
end

local function TargetItems(bot)
    local items = { { text = "No target (train as it comes)", value = 0 } }
    for _, rank in ipairs(Data.PROFESSION_RANKS) do
        local reachable = (bot.lvl or 1) >= rank.level
        items[#items + 1] = {
            text = rank.name .. " (" .. rank.cap .. ")" .. (reachable and "" or (" |cff808080- needs level " .. rank.level .. "|r")),
            value = rank.cap,
        }
    end
    return items
end

local function TargetText(cap)
    if not cap or cap == 0 then
        return "No target"
    end
    local rank = Data.RankForCap(cap)
    return rank and (rank.name .. " (" .. cap .. ")") or tostring(cap)
end

local function CreatePrimaryRow(f, parent, index, anchor)
    local row = {}
    row.label = W.Label(parent, "Primary " .. index, "GameFontNormalSmall")
    row.label:SetPoint("TOPLEFT", anchor, "BOTTOMLEFT", 0, index == 1 and -14 or -22)
    row.icon = W.Icon(parent, 20, "Interface\\Icons\\INV_Misc_QuestionMark")
    row.icon:SetPoint("LEFT", row.label, "RIGHT", 8, 0)
    row.prof = W.DropDown(parent, 130, function()
        local d = f.draft or {}
        local other = index == 1 and d.p2 or d.p1
        local items = { { text = "None", value = 0 } }
        for _, id in ipairs(Data.PRIMARY_ORDER) do
            local p = Data.PROFESSIONS[id]
            local tip = p.gathering and "Gathering profession." or "Crafting profession."
            items[#items + 1] = { text = p.name, value = id, disabled = id == other, tooltip = tip }
        end
        return items
    end)
    row.prof:SetPoint("LEFT", row.icon, "RIGHT", -10, -2)
    row.prof.onSelect = function(value)
        if index == 1 then
            f.draft.p1 = value
        else
            f.draft.p2 = value
        end
        f.dirty = true
        UI.RequestRefresh()
    end
    row.target = W.DropDown(parent, 150, function()
        local bot = CurrentBot()
        return bot and TargetItems(bot) or {}
    end)
    row.target:SetPoint("LEFT", row.prof, "RIGHT", -20, 0)
    row.target.onSelect = function(value)
        if index == 1 then
            f.draft.t1 = value
        else
            f.draft.t2 = value
        end
        f.dirty = true
        UI.RequestRefresh()
    end
    row.bar = W.Bar(parent, 150, 14, 0, 0.6, 0.1)
    row.bar:SetPoint("LEFT", row.target, "RIGHT", -6, 2)
    return row
end

local function CreateProfessions(parent)
    local f = CreateFrame("Frame", nil, parent)
    f:SetAllPoints(parent)

    local title = W.Header(f, "Profession plan")
    title:SetPoint("TOPLEFT", f, "TOPLEFT", 10, -10)
    f.rows = {}
    f.rows[1] = CreatePrimaryRow(f, f, 1, title)
    f.rows[2] = CreatePrimaryRow(f, f, 2, f.rows[1].label)

    local secTitle = W.Label(f, "Secondary", "GameFontNormalSmall")
    secTitle:SetPoint("TOPLEFT", f.rows[2].label, "BOTTOMLEFT", 0, -24)
    f.secondary = {}
    local anchor = secTitle
    for i, id in ipairs(Data.SECONDARY_ORDER) do
        local cb = W.CheckBox(f, Data.ProfessionName(id), function(self)
            f.draft.sec[id] = W.IsChecked(self) or nil
            f.dirty = true
            UI.RequestRefresh()
        end)
        if i == 1 then
            cb:SetPoint("LEFT", secTitle, "RIGHT", 40, 0)
        else
            cb:SetPoint("LEFT", f.secondary[i - 1].bar, "RIGHT", 16, 0)
        end
        cb.bar = W.Bar(f, 90, 12, 0, 0.6, 0.1)
        cb.bar:SetPoint("LEFT", cb.label, "RIGHT", 6, 0)
        cb.skillId = id
        f.secondary[i] = cb
        anchor = cb
    end

    f.hint = W.Label(f, "Pairs that work well: Herbalism + Alchemy or Inscription, Mining + Blacksmithing, "
        .. "Engineering or Jewelcrafting, Skinning + Leatherworking. Bots learn and train ranks at trainers "
        .. "(real cost) while on Autopilot or Level in zone.", "GameFontDisableSmall")
    f.hint:SetPoint("TOPLEFT", secTitle, "BOTTOMLEFT", 0, -18)
    f.hint:SetPoint("RIGHT", f, "RIGHT", -12, 0)
    f.hint:SetHeight(28)
    f.hint:SetJustifyV("TOP")

    f.coverageTitle = W.Header(f, "Guild coverage")
    f.coverageTitle:SetPoint("TOPLEFT", f.hint, "BOTTOMLEFT", 0, -14)
    f.coverage = W.Label(f, "", "GameFontHighlightSmall")
    f.coverage:SetPoint("TOPLEFT", f.coverageTitle, "BOTTOMLEFT", 0, -6)
    f.coverage:SetPoint("RIGHT", f, "RIGHT", -12, 0)
    f.coverage:SetHeight(42)
    f.coverage:SetJustifyV("TOP")

    f.apply = W.Button(f, "Apply plan", 120, 22, function()
        local bot = CurrentBot()
        if not bot or not f.draft then
            return
        end
        local d = f.draft
        local sec = {}
        for _, id in ipairs(Data.SECONDARY_ORDER) do
            if d.sec[id] then
                sec[#sec + 1] = id
            end
        end
        W.SetEnabled(f.apply, false)
        Protocol.SetProfessions(bot.g, { p1 = d.p1, p2 = d.p2, t1 = d.p1 ~= 0 and d.t1 or 0, t2 = d.p2 ~= 0 and d.t2 or 0, sec = sec },
            UI.RequestCallback("Profession plan sent.", function(ok)
                if ok then
                    f.dirty = false
                end
                W.SetEnabled(f.apply, true)
                UI.RequestRefresh()
            end))
    end)
    f.apply:SetPoint("BOTTOMLEFT", f, "BOTTOMLEFT", 8, 6)
    f.reset = W.Button(f, "Reset", 80, 22, function()
        f.dirty = false
        f.draft = nil
        UI.RequestRefresh()
    end)
    f.reset:SetPoint("LEFT", f.apply, "RIGHT", 6, 0)
    f.pendingNote = W.Label(f, "", "GameFontDisableSmall")
    f.pendingNote:SetPoint("LEFT", f.reset, "RIGHT", 10, 0)
    f.lock = W.LockedOverlay(f)
    return f
end

-- The server reports skills (BOT^sk) only while the bot is in the world.
local function SetSkillBar(bar, bot, skillId)
    local value, maxValue = SkillOf(bot, skillId)
    if value then
        bar:SetProgress(value, maxValue, value .. " / " .. maxValue)
        bar:SetAlpha(1)
    elseif bot.s == "benched" or bot.s == "offline" then
        bar:SetProgress(0, 1, "unknown while offline")
        bar:SetAlpha(0.6)
    else
        bar:SetProgress(0, 1, "not learned")
        bar:SetAlpha(0.6)
    end
end

local function GuildCoverage()
    local byProf = {}
    for _, bot in pairs(State.bots) do
        for _, id in ipairs({ bot.p1 or 0, bot.p2 or 0 }) do
            if id ~= 0 then
                byProf[id] = byProf[id] or {}
                table.insert(byProf[id], bot.n or "?")
            end
        end
    end
    local parts = {}
    local missing = {}
    for _, id in ipairs(Data.PRIMARY_ORDER) do
        if byProf[id] then
            table.sort(byProf[id])
            parts[#parts + 1] = "|cffffd100" .. Data.ProfessionName(id) .. ":|r " .. table.concat(byProf[id], ", ")
        else
            missing[#missing + 1] = Data.ProfessionName(id)
        end
    end
    local text = #parts > 0 and table.concat(parts, "   ") or "No profession plans yet."
    if #missing > 0 then
        text = text .. "\n|cff9d9d9dNobody plans: " .. table.concat(missing, ", ") .. "|r"
    end
    return text
end

local function RefreshProfessions(f, bot)
    if not State.HasFeature("professions") then
        f.lock:SetLock("Professions", State.FeatureRequirement("professions"),
            "Plan up to two primary and the secondary professions per bot.")
    else
        f.lock:Hide()
    end
    if not f.draft or f.draftGuid ~= bot.g or not f.dirty then
        f.draft = DraftProfessions(bot)
        f.draftGuid = bot.g
    end
    local d = f.draft
    for i = 1, 2 do
        local row = f.rows[i]
        local id = i == 1 and d.p1 or d.p2
        local target = i == 1 and d.t1 or d.t2
        row.prof:SetSelected(id, id ~= 0 and Data.ProfessionName(id) or "None")
        row.icon:SetTexture(id ~= 0 and Data.ProfessionIcon(id) or "Interface\\Icons\\INV_Misc_QuestionMark")
        row.target:SetSelected(target, TargetText(target))
        row.target:SetEnabled(id ~= 0)
        if id ~= 0 then
            SetSkillBar(row.bar, bot, id)
            row.bar:Show()
        else
            row.bar:Hide()
        end
    end
    for _, cb in ipairs(f.secondary) do
        cb:SetChecked(d.sec[cb.skillId] and true or false)
        SetSkillBar(cb.bar, bot, cb.skillId)
    end
    f.coverage:SetText(GuildCoverage())
    W.SetEnabled(f.apply, State.IsConnected() and State.HasFeature("professions"))
    f.pendingNote:SetText(f.dirty and "|cffffd100Unsaved changes|r" or "")
end

-------------------------------------------------------------------------------
-- Character sub-tab
-------------------------------------------------------------------------------

local CHARACTER_ROWS = {
    { "Race / class", function(b) return Data.RaceName(b.race) .. " " .. Data.ClassName(b.class) .. (b.gender == 1 and " (female)" or " (male)") end },
    { "Level", function(b)
        if b.xpmax and b.xpmax > 0 then
            return string.format("%d  (%d / %d XP)", b.lvl or 1, b.xp or 0, b.xpmax)
        end
        return tostring(b.lvl or 1)
    end },
    { "Health", function(b) return (b.dead and "|cffff4040dead|r" or ((b.hp or 100) .. "%")) .. (b.combat and "  |cffff8040in combat|r" or "") end },
    { "Location", function(b) return (b.zn or "?") .. "  |cff9d9d9d" .. Data.ContinentName(b.map) .. "|r" end },
    { "Gold", function(b) return Data.FormatMoney(b.gold or 0) end },
    { "Role / spec", function(b) return (b.role and b.role ~= "none" and b.role or "no role") .. ((b.spec and b.spec ~= "") and (" - " .. b.spec) or "") end },
    { "Roster slot", function(b) return tostring(b.slot or "?") end },
    { "Recruited", function(b) return b.since and b.since > 0 and date("%Y-%m-%d", b.since) or "?" end },
    { "Kills / deaths", function(b) return (b.kills or 0) .. " / " .. (b.deaths or 0) end },
    { "Quests done", function(b) return tostring(b.quests or 0) end },
    { "XP per hour", function(b) return tostring(b.xph or 0) end },
    { "Gold per hour", function(b) return Data.FormatMoney(b.gph or 0) end },
    -- The average item level of its armour and weapons (docs/orders.md "Readiness"); about its level is fine.
    { "Gear level", function(b)
        if not b.gl then
            return "|cff9d9d9d(not in the world)|r"
        end
        local level = b.lvl or 1
        local color = (b.gl + 8 <= level) and "|cffff4040" or ((b.gl + 3 <= level) and "|cffffd100" or "|cff40ff40")
        return color .. b.gl .. "|r  |cff9d9d9d(level " .. level .. ")|r"
    end },
    -- A caster without a wand, and who of the guild could make one (Storage tab, Craft orders).
    { "Wand", function(b)
        if not b.gl then
            return "|cff9d9d9d(not in the world)|r"
        end
        if not b.nw then
            return "|cff9d9d9d-|r"
        end
        if b.wm and b.wm ~= "" then
            return "|cffff4040none|r  |cffffd100" .. b.wm .. "|r"
        end
        return "|cffff4040none|r  |cff9d9d9d(nobody of the guild can make one yet)|r"
    end },
}

-- How many dungeons the Character sub-tab lists at most (those nearest to the bot's level come first).
local READINESS_ROWS = 8

local function ReadinessColor(pct)
    if pct >= 90 then
        return 0.25, 0.8, 0.25
    elseif pct >= 60 then
        return 0.9, 0.75, 0.1
    end
    return 0.8, 0.25, 0.2
end

local function CreateCharacter(parent)
    local f = CreateFrame("Frame", nil, parent)
    f:SetAllPoints(parent)
    f.rows = {}
    local half = math.ceil(#CHARACTER_ROWS / 2)
    for i, def in ipairs(CHARACTER_ROWS) do
        local label = W.Label(f, def[1], "GameFontNormalSmall")
        local column = i <= half and 0 or 1
        local line = column == 0 and i or (i - half)
        label:SetPoint("TOPLEFT", f, "TOPLEFT", 12 + column * 360, -14 - (line - 1) * 22)
        label:SetWidth(110)
        local value = W.Label(f, "", "GameFontHighlightSmall")
        value:SetPoint("LEFT", label, "RIGHT", 8, 0)
        value:SetWidth(230)
        f.rows[i] = value
    end
    -- Dungeon readiness: a bar per dungeon in the bot's reach.
    local top = -14 - half * 22 - 16
    f.readyTitle = W.Label(f, "Dungeon readiness", "GameFontNormal")
    f.readyTitle:SetPoint("TOPLEFT", f, "TOPLEFT", 12, top)
    f.readyNote = W.Label(f, "", "GameFontDisableSmall")
    f.readyNote:SetPoint("LEFT", f.readyTitle, "RIGHT", 12, 0)
    f.ready = {}
    for i = 1, READINESS_ROWS do
        local column = (i - 1) % 2
        local line = math.floor((i - 1) / 2)
        local name = W.Label(f, "", "GameFontHighlightSmall")
        name:SetPoint("TOPLEFT", f, "TOPLEFT", 12 + column * 360, top - 22 - line * 20)
        name:SetWidth(200)
        local bar = W.Bar(f, 120, 12)
        bar:SetPoint("LEFT", name, "RIGHT", 6, 0)
        f.ready[i] = { name = name, bar = bar }
    end
    return f
end

local function RefreshCharacter(f, bot)
    for i, def in ipairs(CHARACTER_ROWS) do
        f.rows[i]:SetText(def[2](bot))
    end
    -- The dungeon names come once per connection.
    if not State.dungeonsLoaded and State.IsConnected() and not f.askedDungeons then
        f.askedDungeons = true
        Protocol.Get("dungeons")
    end
    local list = State.BotReadiness(bot)
    -- More than fit: the ones the bot is nearest to in level (the last of the dungeons, then the raids).
    local first = math.max(1, #list - READINESS_ROWS + 1)
    for i = 1, READINESS_ROWS do
        local row, entry = f.ready[i], list[first + i - 1]
        if entry then
            local d = entry.d
            row.name:SetText(string.format("%s |cff9d9d9d(%s%d-%d)|r", d.n or "?", d.raid and (tostring(d.size or 40) .. ", ") or "",
                d.min or 0, d.max or 0))
            row.bar:SetStatusBarColor(ReadinessColor(entry.pct))
            row.bar:SetProgress(entry.pct, 100, entry.pct .. "%")
            W.SetTooltip(row.bar, d.n or "?", {
                string.format("Level %d-%d, made for level %d, %d players.", d.min or 0, d.max or 0, d.tgt or 0, d.size or 5),
                string.format("Asks for gear level %d; %s has %s.", d.gl or 0, bot.n or "it", tostring(bot.gl or "?")),
                "Readiness = its level against the dungeon's, times its gear level against what the dungeon asks for.",
            })
            row.name:Show()
            row.bar:Show()
        else
            row.name:Hide()
            row.bar:Hide()
        end
    end
    if not bot.gl then
        f.readyNote:SetText("Known while the bot is in the world.")
    elseif #list == 0 then
        f.readyNote:SetText(State.dungeonsLoaded and "No dungeon in its reach yet." or "")
    else
        f.readyNote:SetText("")
    end
end

-- For the tests: what the Character sub-tab lists.
function UI.BotReadinessRows(bot)
    return State.BotReadiness(bot)
end

-------------------------------------------------------------------------------
-- Locked or later sub-tabs (Talents, Inventory)
-------------------------------------------------------------------------------

local function CreatePlaceholder(feature, title, detail)
    return function(parent)
        local f = CreateFrame("Frame", nil, parent)
        f:SetAllPoints(parent)
        f.lock = W.LockedOverlay(f)
        f.feature, f.title, f.detail = feature, title, detail
        return f
    end
end

local function RefreshPlaceholder(f)
    if State.HasFeature(f.feature) then
        f.lock:SetLock(f.title, "Unlocked - coming in a later milestone of the addon.", f.detail)
    else
        f.lock:SetLock(f.title, State.FeatureRequirement(f.feature), f.detail)
    end
end

local SUBPAGES = {
    orders = { create = CreateOrders, refresh = RefreshOrders },
    professions = { create = CreateProfessions, refresh = RefreshProfessions },
    -- UI/Crafting.lua: the bot's recipes, crafted from its bags and the guild storage
    crafting = { create = function(parent) return UI.CreateBotCrafting(parent) end,
        refresh = function(frame, bot) UI.RefreshBotCrafting(frame, bot) end },
    character = { create = CreateCharacter, refresh = RefreshCharacter },
    -- UI/Talents.lua: the talent template the bot plays by
    talents = { create = function(parent) return UI.CreateBotTalents(parent) end,
        refresh = function(frame, bot) UI.RefreshBotTalents(frame, bot) end },
    -- UI/Talents.lua: the bot's fight profiles (solo, group), the rules of a profile, its own option values
    fight = { create = function(parent) return UI.CreateBotFight(parent) end,
        refresh = function(frame, bot) UI.RefreshBotFight(frame, bot) end },
    -- UI/Talents.lua: the dungeon role of the bot, the talents of the role and its role store
    role = { create = function(parent) return UI.CreateBotRole(parent) end,
        refresh = function(frame, bot) UI.RefreshBotRole(frame, bot) end },
    -- UI/Behaviour.lua: the bot settings this bot has of its own (docs/bot-settings.md)
    behaviour = { create = function(parent) return UI.CreateBotBehaviour(parent) end,
        refresh = function(frame, bot) UI.RefreshBotBehaviour(frame, bot) end },
}

-------------------------------------------------------------------------------
-- Page
-------------------------------------------------------------------------------

local function NeighbourBot(step)
    local list = State.SortedBots("slot", false)
    for i, bot in ipairs(list) do
        if bot.g == selectedGuid then
            local other = list[((i - 1 + step) % #list) + 1]
            return other and other.g
        end
    end
    return list[1] and list[1].g
end

local function ShowSubtab(key)
    selectedSubtab = key
    for i, def in ipairs(SUBTABS) do
        local sub = SUBPAGES[def.key]
        if def.key == key then
            if not sub.frame then
                sub.frame = sub.create(page.subContent)
            end
            sub.frame:Show()
            PanelTemplates_SetTab(page, i)
        elseif sub.frame then
            sub.frame:Hide()
        end
    end
    UI.RequestRefresh()
end

local function Create(parent)
    local f = CreateFrame("Frame", "GuildAdventureBotDetail", parent)
    page = f

    f.back = W.Button(f, "< Roster", 90, 22, function()
        UI.ShowPage("roster")
    end)
    f.back:SetPoint("TOPLEFT", f, "TOPLEFT", 2, -2)
    f.next = W.Button(f, ">", 28, 22, function()
        UI.ShowBot(NeighbourBot(1))
    end)
    f.next:SetPoint("TOPRIGHT", f, "TOPRIGHT", -2, -2)
    f.prev = W.Button(f, "<", 28, 22, function()
        UI.ShowBot(NeighbourBot(-1))
    end)
    f.prev:SetPoint("RIGHT", f.next, "LEFT", -2, 0)

    f.icon = W.ClassIcon(f, 40)
    f.icon:SetPoint("TOPLEFT", f.back, "BOTTOMLEFT", 4, -8)
    f.name = W.Label(f, "", "GameFontNormalLarge")
    f.name:SetPoint("TOPLEFT", f.icon, "TOPRIGHT", 10, 0)
    f.info = W.Label(f, "", "GameFontHighlightSmall")
    f.info:SetPoint("TOPLEFT", f.name, "BOTTOMLEFT", 0, -4)
    f.xp = W.Bar(f, 260, 12, 0.58, 0, 0.55)
    f.xp:SetPoint("TOPLEFT", f.info, "BOTTOMLEFT", 0, -4)
    f.why = W.Label(f, "", "GameFontNormalSmall")
    f.why:SetPoint("TOPLEFT", f.icon, "BOTTOMLEFT", 0, -8)
    f.why:SetPoint("RIGHT", f, "RIGHT", -8, 0)

    f.control = W.Button(f, "Take control", 120, 22, function()
        local bot = CurrentBot()
        if not bot then
            return
        end
        if bot.s == "controlled" then
            UI.ReturnControl()
        else
            UI.TakeControl(bot.g)
        end
    end)
    f.control:SetPoint("TOPRIGHT", f.next, "BOTTOMRIGHT", 0, -10)
    f.bench = W.Button(f, "Bench", 90, 22, function()
        local bot = CurrentBot()
        if not bot then
            return
        end
        if bot.s == "benched" then
            Protocol.SetBotState(bot.g, "active", UI.RequestCallback((bot.n or "Bot") .. " is coming back."))
        else
            UI.Confirm("Bench " .. (bot.n or "this bot") .. "? The bot logs out but keeps its slot.", function()
                Protocol.SetBotState(bot.g, "benched", UI.RequestCallback((bot.n or "Bot") .. " was benched."))
            end)
        end
    end)
    f.bench:SetPoint("RIGHT", f.control, "LEFT", -6, 0)
    f.dismiss = W.Button(f, "Dismiss", 80, 22, function()
        UI.AskDismiss(CurrentBot())
    end)
    f.dismiss:SetPoint("RIGHT", f.bench, "LEFT", -6, 0)
    W.SetTooltip(f.dismiss, "Dismiss", "Deletes the bot for good and frees its slot. Its items that are not soulbound go into "
        .. "the guild storage, its gold into the guild bank. You are asked first.")

    -- sub-tabs
    f.subContent = W.Panel(f, 0.3)
    f.subContent:SetPoint("TOPLEFT", f, "TOPLEFT", 0, -128)
    f.subContent:SetPoint("BOTTOMRIGHT", f, "BOTTOMRIGHT", 0, 0)
    for i, def in ipairs(SUBTABS) do
        local tab = CreateFrame("Button", "GuildAdventureBotDetailTab" .. i, f, "OptionsFrameTabButtonTemplate")
        tab:SetID(i)
        tab:SetText(def.label)
        if i == 1 then
            tab:SetPoint("BOTTOMLEFT", f.subContent, "TOPLEFT", 6, -2)
        else
            tab:SetPoint("LEFT", _G["GuildAdventureBotDetailTab" .. (i - 1)], "RIGHT", -16, 0)
        end
        tab:SetScript("OnClick", function(self)
            PlaySound("igCharacterInfoTab")
            ShowSubtab(SUBTABS[self:GetID()].key)
        end)
        PanelTemplates_TabResize(tab, 0)
    end
    PanelTemplates_SetNumTabs(f, #SUBTABS)
    f:SetScript("OnShow", function()
        ShowSubtab(selectedSubtab)
    end)
    return f
end

local function RefreshHeader(f, bot)
    f.icon:SetClass(bot.class)
    f.name:SetText(Data.ClassColorCode(bot.class) .. (bot.n or "?") .. "|r  " .. Data.StateLabel(bot.s, true))
    f.info:SetText(string.format("Level %d %s %s - %s", bot.lvl or 1, Data.RaceName(bot.race), Data.ClassName(bot.class), bot.zn or "?"))
    if bot.xpmax and bot.xpmax > 0 then
        f.xp:SetProgress(bot.xp or 0, bot.xpmax, string.format("%d / %d XP", bot.xp or 0, bot.xpmax))
    else
        f.xp:SetProgress(1, 1, "Maximum level")
    end
    f.why:SetText("|cffffd100Now:|r " .. (bot.why or ""))

    local connected = State.IsConnected()
    if bot.s == "controlled" then
        f.control:SetText("Return to leader")
        W.SetEnabled(f.control, connected and not State.control.busy)
    else
        f.control:SetText("Take control")
        W.SetEnabled(f.control, connected and not State.control.busy and bot.s == "active" and not bot.dead)
    end
    f.bench:SetText(bot.s == "benched" and "Activate" or "Bench")
    W.SetEnabled(f.bench, connected and bot.s ~= "controlled")
    W.SetEnabled(f.dismiss, connected and bot.s ~= "controlled")

    for i, def in ipairs(SUBTABS) do
        local tab = _G["GuildAdventureBotDetailTab" .. i]
        local locked = def.feature and not State.HasFeature(def.feature)
        tab:SetText(locked and ("|cff808080" .. def.label .. "|r") or def.label)
    end
end

local function Refresh(f)
    local bot = CurrentBot()
    if not bot then
        UI.ShowPage("roster")
        return
    end
    RefreshHeader(f, bot)
    local sub = SUBPAGES[selectedSubtab]
    if sub and sub.frame and sub.frame:IsShown() then
        sub.refresh(sub.frame, bot)
    end
end

function UI.ShowBot(guid)
    if not guid then
        return
    end
    selectedGuid = guid
    UI.ShowPage("bot")
end

function UI.SelectedBot()
    return selectedGuid
end

function UI.ShowBotSubtab(key)
    if page and SUBPAGES[key] then
        ShowSubtab(key)
    else
        selectedSubtab = key
    end
end

UI.RegisterPage("bot", { tab = "roster", create = Create, refresh = Refresh })
