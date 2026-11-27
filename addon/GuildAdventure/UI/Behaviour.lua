-- Bot behaviour settings (docs/bot-settings.md): what the guild leader sets for how the bots play. The server sends
-- the list of settings (label, help, range, default) and the values, so a setting added on the server shows up here
-- without an addon change. One panel, two places: the Settings tab ("Bot behaviour", the guild's values) and the bot
-- detail's "Behaviour" sub-tab (one bot's own values; the rest follow the guild).

local GA = GuildAdventure
local UI = GA.UI
local W = UI.W
local State = GA.State
local Protocol = GA.Protocol

local Behaviour = {}
UI.Behaviour = Behaviour

Behaviour.ROW_HEIGHT = 26
Behaviour.CATEGORY_WIDTH = 160

-------------------------------------------------------------------------------
-- Values as text (the server's BotSettings::Format / Parse, docs/bot-settings.md)
-------------------------------------------------------------------------------

function Behaviour.Options(def)
    local options = {}
    for item in string.gmatch(def.opt or "", "[^;]+") do
        local token, label = string.match(item, "^([^=]*)=(.*)$")
        options[#options + 1] = { token = token or item, label = label or item }
    end
    return options
end

function Behaviour.Format(def, value)
    if value == nil then
        return "?"
    end
    local t = def.t
    if t == "bool" then
        return value ~= 0 and "on" or "off"
    elseif t == "enum" then
        local option = Behaviour.Options(def)[value + 1]
        return option and option.label or tostring(value)
    elseif t == "money" then
        local copper = math.max(0, math.floor(value))
        local g, s, c = math.floor(copper / 10000), math.floor(copper % 10000 / 100), copper % 100
        local parts = {}
        if g > 0 then parts[#parts + 1] = g .. "g" end
        if s > 0 then parts[#parts + 1] = s .. "s" end
        if c > 0 or #parts == 0 then parts[#parts + 1] = c .. "c" end
        return table.concat(parts, " ")
    elseif t == "dec" then
        local scale = math.max(def.sc or 1, 1)
        local decimals = scale >= 100 and 2 or (scale >= 10 and 1 or 0)
        return string.format("%." .. decimals .. "f", value / scale)
    end
    local unit = def.u or ""
    if unit == "" then
        return tostring(value)
    elseif unit == "%" then
        return value .. "%"
    end
    return value .. " " .. unit
end

-- The text of the edit box: like Format, but without the unit (the unit stands beside the box).
function Behaviour.EditText(def, value)
    if def.t == "money" or def.t == "dec" then
        return Behaviour.Format(def, value)
    end
    return tostring(value or "")
end

-- Text -> value, or nil.
function Behaviour.Parse(def, text)
    text = string.lower(string.gsub(text or "", "^%s*(.-)%s*$", "%1"))
    if text == "" then
        return nil
    end
    local t = def.t
    if t == "bool" then
        if text == "1" or text == "on" or text == "yes" or text == "true" then return 1 end
        if text == "0" or text == "off" or text == "no" or text == "false" then return 0 end
        return nil
    elseif t == "enum" then
        for i, option in ipairs(Behaviour.Options(def)) do
            if string.lower(option.token) == text or string.lower(option.label) == text then
                return i - 1
            end
        end
        return tonumber(text)
    elseif t == "money" then
        local total, any = 0, false
        local rest = string.gsub(text, "(%d+)%s*([gsc]?)", function(amount, unit)
            amount = tonumber(amount)
            any = true
            if unit == "g" then
                total = total + amount * 10000
            elseif unit == "s" then
                total = total + amount * 100
            else
                total = total + amount
            end
            return ""
        end)
        if not any or string.find(rest, "%S") then
            return nil
        end
        return total
    elseif t == "dec" then
        local number = tonumber(text)
        return number and math.floor(number * math.max(def.sc or 1, 1) + 0.5)
    end
    local unit = string.lower(def.u or "")
    if unit ~= "" and string.sub(text, -#unit) == unit then
        text = string.gsub(string.sub(text, 1, -#unit - 1), "%s+$", "")
    end
    local number = tonumber(text)
    return number and math.floor(number)
end

local function InRange(def, value)
    return value ~= nil and value >= (def.min or 0) and value <= (def.max or value)
end

-------------------------------------------------------------------------------
-- Which settings a panel lists
-------------------------------------------------------------------------------

local function Matches(def, search)
    if search == "" then
        return true
    end
    for _, text in ipairs({ def.n or "", def.k or "", def.h or "" }) do
        if string.find(string.lower(text), search, 1, true) then
            return true
        end
    end
    return false
end

-- opts: g (0 = the guild), category, search, changedOnly, showAll. Returns list rows: { header = text } or { def = }.
function Behaviour.BuildItems(opts)
    local s = State.settings
    local g = opts.g or 0
    local search = string.lower(opts.search or "")
    local items = {}
    for _, cat in ipairs(s.cats) do
        if search ~= "" or cat.k == opts.category then
            local first = true
            for _, def in ipairs(s.defs) do
                local _, source = State.SettingValue(def, g)
                local own = (g == 0 and source == "guild") or (g ~= 0 and source == "own")
                local shown = def.cat == cat.k
                    and (g == 0 or def.pc)
                    and (opts.showAll or not def.adv or own)
                    and (not opts.changedOnly or own)
                    and Matches(def, search)
                if shown then
                    if first and search ~= "" then
                        items[#items + 1] = { header = cat.n or cat.k }
                    end
                    first = false
                    items[#items + 1] = { def = def }
                end
            end
        end
    end
    return items
end

-------------------------------------------------------------------------------
-- Sending
-------------------------------------------------------------------------------

local function Send(panel, set, clear, all, success)
    local g = panel.scope()
    Protocol.SetSettings(g, set, clear, all, UI.RequestCallback(success))
end

local function SetValue(panel, def, value)
    if not InRange(def, value) then
        UI.Notify(string.format("%s takes %s to %s.", def.n or def.k, Behaviour.Format(def, def.min),
            Behaviour.Format(def, def.max)), true)
        UI.RequestRefresh()
        return
    end
    Send(panel, { [def.id] = value })
end

local function ClearValue(panel, def)
    Send(panel, nil, { def.id })
end

-------------------------------------------------------------------------------
-- Rows
-------------------------------------------------------------------------------

local SOURCE_TEXT = {
    own = W.COLORS.green .. "this bot|r",
    guild = W.COLORS.blue .. "guild|r",
    default = W.COLORS.grey .. "default|r",
}

local function RowTooltipLines(row)
    local def = row.item and row.item.def
    if not def then
        return nil
    end
    local panel = row.panel
    local g = panel.scope()
    local lines = { def.h or "" }
    lines[#lines + 1] = " "
    lines[#lines + 1] = "Server default: " .. Behaviour.Format(def, def.def)
    if g ~= 0 then
        local guildValue = State.SettingValue(def, 0)
        lines[#lines + 1] = "Guild: " .. Behaviour.Format(def, guildValue)
    end
    if def.t == "int" or def.t == "dec" or def.t == "money" then
        lines[#lines + 1] = "Range: " .. Behaviour.Format(def, def.min) .. " to " .. Behaviour.Format(def, def.max)
    end
    lines[#lines + 1] = W.COLORS.grey .. "GuildAdventure." .. (def.k or "") .. "|r"
    return lines
end

local function CreateRow(list, index)
    local panel = list.panel
    local row = CreateFrame("Frame", nil, list)
    row.panel = panel

    row.header = W.Label(row, "", "GameFontNormal")
    row.header:SetPoint("LEFT", row, "LEFT", 4, 0)

    row.label = W.Label(row, "", "GameFontHighlightSmall")
    row.label:SetPoint("LEFT", row, "LEFT", 8, 0)
    row.label:SetWidth(250)
    row.label:SetHeight(Behaviour.ROW_HEIGHT)
    W.SetTooltip(row.label, function()
        return row.item and row.item.def and (row.item.def.n or row.item.def.k)
    end, function()
        return RowTooltipLines(row)
    end)

    row.check = W.CheckBox(row, "", function(self)
        local def = row.item and row.item.def
        if def then
            SetValue(panel, def, W.IsChecked(self) and 1 or 0)
        end
    end)
    row.check:SetPoint("LEFT", row, "LEFT", 262, 0)

    row.dd = W.DropDown(row, 150, function()
        local def = row.item and row.item.def
        local items = {}
        if def then
            for i, option in ipairs(Behaviour.Options(def)) do
                items[#items + 1] = { text = option.label, value = i - 1 }
            end
        end
        return items
    end)
    row.dd:SetPoint("LEFT", row, "LEFT", 246, -2)
    row.dd.onSelect = function(value)
        local def = row.item and row.item.def
        if def then
            SetValue(panel, def, value)
        end
    end

    row.minus = W.Button(row, "-", 22, 20, function()
        local def = row.item and row.item.def
        if def then
            local value = State.SettingValue(def, panel.scope())
            SetValue(panel, def, math.max(def.min or 0, (value or 0) - math.max(def.st or 1, 1)))
        end
    end)
    row.minus:SetPoint("LEFT", row, "LEFT", 264, 0)
    row.edit = W.EditBox(row, 70, false, 16)
    row.edit:SetPoint("LEFT", row.minus, "RIGHT", 8, 0)
    row.edit:SetScript("OnEnterPressed", function(self)
        self:ClearFocus()
        local def = row.item and row.item.def
        if not def then
            return
        end
        local value = Behaviour.Parse(def, self:GetText())
        if value == nil then
            UI.Notify("That is no value for " .. (def.n or def.k) .. ".", true)
            UI.RequestRefresh()
            return
        end
        SetValue(panel, def, value)
    end)
    row.edit:SetScript("OnEscapePressed", function(self)
        self:ClearFocus()
        UI.RequestRefresh()
    end)
    row.plus = W.Button(row, "+", 22, 20, function()
        local def = row.item and row.item.def
        if def then
            local value = State.SettingValue(def, panel.scope())
            SetValue(panel, def, math.min(def.max or 0, (value or 0) + math.max(def.st or 1, 1)))
        end
    end)
    row.plus:SetPoint("LEFT", row.edit, "RIGHT", 4, 0)
    row.unit = W.Label(row, "", "GameFontDisableSmall")
    row.unit:SetPoint("LEFT", row.plus, "RIGHT", 4, 0)

    row.source = W.Label(row, "", "GameFontDisableSmall", "RIGHT")
    row.source:SetPoint("RIGHT", row, "RIGHT", -64, 0)
    row.source:SetWidth(60)
    row.reset = W.Button(row, "Reset", 56, 20, function()
        local def = row.item and row.item.def
        if def then
            ClearValue(panel, def)
        end
    end)
    row.reset:SetPoint("RIGHT", row, "RIGHT", -4, 0)
    W.SetTooltip(row.reset, "Reset", function()
        return panel.scope() == 0 and "Back to the server's default." or "Back to the guild's value."
    end)
    return row
end

local function ShowControls(row, kind)
    local function shown(widget, on)
        if on then widget:Show() else widget:Hide() end
    end
    shown(row.check, kind == "bool")
    shown(row.dd, kind == "enum")
    local numeric = kind == "int" or kind == "dec" or kind == "money"
    shown(row.minus, numeric)
    shown(row.edit, numeric)
    shown(row.plus, numeric)
    shown(row.unit, numeric)
end

local function UpdateRow(row, item)
    row.item = item
    if item.header then
        row.header:SetText(item.header)
        row.header:Show()
        row.label:Hide()
        row.source:Hide()
        row.reset:Hide()
        ShowControls(row, nil)
        return
    end
    local def = item.def
    local panel = row.panel
    local g = panel.scope()
    local value, source = State.SettingValue(def, g)
    row.header:Hide()
    row.label:SetText(def.n or def.k or "?")
    row.label:Show()
    ShowControls(row, def.t == "bool" and "bool" or (def.t == "enum" and "enum" or (def.t == "money" and "money"
        or (def.t == "dec" and "dec" or "int"))))
    local connected = State.IsConnected()
    if def.t == "bool" then
        row.check:SetChecked(value ~= nil and value ~= 0)
        W.SetEnabled(row.check, connected)
    elseif def.t == "enum" then
        row.dd:SetSelected(value, Behaviour.Format(def, value))
        row.dd:SetEnabled(connected)
    else
        W.SetEditText(row.edit, Behaviour.EditText(def, value))
        W.SetEditEnabled(row.edit, connected)
        row.unit:SetText(def.t == "int" and (def.u or "") or "")
        W.SetEnabled(row.minus, connected and value ~= nil and value > (def.min or 0))
        W.SetEnabled(row.plus, connected and value ~= nil and value < (def.max or 0))
    end
    row.source:SetText(SOURCE_TEXT[source] or "")
    row.source:Show()
    local own = (g == 0 and source == "guild") or (g ~= 0 and source == "own")
    row.reset:Show()
    W.SetEnabled(row.reset, connected and own)
end

-------------------------------------------------------------------------------
-- Panel
-------------------------------------------------------------------------------

-- opts.scope(): 0 for the guild, a guild character's guid for one bot. opts.intro: a line above the list.
function UI.CreateBehaviourPanel(parent, opts)
    local panel = CreateFrame("Frame", nil, parent)
    panel.scope = opts.scope
    panel.category = nil
    panel.catButtons = {}

    panel.intro = W.Label(panel, opts.intro or "", "GameFontDisableSmall")
    panel.intro:SetPoint("TOPLEFT", panel, "TOPLEFT", 8, -6)
    panel.intro:SetPoint("RIGHT", panel, "RIGHT", -8, 0)
    panel.intro:SetHeight(24)
    panel.intro:SetJustifyV("TOP")

    panel.searchLabel = W.Label(panel, "Search:", "GameFontNormalSmall")
    panel.searchLabel:SetPoint("TOPLEFT", panel.intro, "BOTTOMLEFT", 0, -6)
    panel.search = W.EditBox(panel, 150, false, 40)
    panel.search:SetPoint("LEFT", panel.searchLabel, "RIGHT", 10, 0)
    panel.search:SetScript("OnTextChanged", function()
        UI.RequestRefresh()
    end)
    panel.search:SetScript("OnEscapePressed", function(self)
        self:SetText("")
        self:ClearFocus()
    end)
    panel.changed = W.CheckBox(panel, "Changed only", function()
        UI.RequestRefresh()
    end)
    panel.changed:SetPoint("LEFT", panel.search, "RIGHT", 14, 0)
    panel.all = W.CheckBox(panel, "Show all options", function()
        UI.RequestRefresh()
    end)
    panel.all:SetPoint("LEFT", panel.changed, "RIGHT", 100, 0)
    W.SetTooltip(panel.all, "Show all options", "Also the finer settings (time windows, distances, thresholds).")

    panel.cats = CreateFrame("Frame", nil, panel)
    panel.cats:SetPoint("TOPLEFT", panel.searchLabel, "BOTTOMLEFT", -2, -12)
    panel.cats:SetPoint("BOTTOM", panel, "BOTTOM", 0, 34)
    panel.cats:SetWidth(Behaviour.CATEGORY_WIDTH)

    panel.list = W.ScrollList(panel, { rowHeight = Behaviour.ROW_HEIGHT, createRow = CreateRow, updateRow = UpdateRow })
    panel.list.panel = panel
    panel.list:SetPoint("TOPLEFT", panel.cats, "TOPRIGHT", 8, 0)
    panel.list:SetPoint("BOTTOMRIGHT", panel, "BOTTOMRIGHT", -4, 34)

    panel.empty = W.Label(panel, "", "GameFontDisable", "CENTER")
    panel.empty:SetPoint("TOPLEFT", panel.list, "TOPLEFT", 0, -20)
    panel.empty:SetPoint("RIGHT", panel.list, "RIGHT", -24, 0)

    panel.count = W.Label(panel, "", "GameFontHighlightSmall")
    panel.count:SetPoint("BOTTOMLEFT", panel, "BOTTOMLEFT", 8, 10)
    panel.resetAll = W.Button(panel, "Reset all...", 100, 22, function()
        local g = panel.scope()
        local text = g == 0 and "Reset every bot setting of the guild to the server's defaults?\n(Values set for single bots stay.)"
            or "Reset every setting of this bot to the guild's values?"
        UI.Confirm(text, function()
            Send(panel, nil, nil, true, g == 0 and "The guild's bot settings are back to the defaults."
                or "This bot follows the guild's settings again.")
        end)
    end)
    panel.resetAll:SetPoint("BOTTOMRIGHT", panel, "BOTTOMRIGHT", -8, 6)
    panel.resetCat = W.Button(panel, "Reset category", 120, 22, function()
        local g = panel.scope()
        local clear = {}
        for _, def in ipairs(State.settings.defs) do
            local _, source = State.SettingValue(def, g)
            if def.cat == panel.category and ((g == 0 and source == "guild") or (g ~= 0 and source == "own")) then
                clear[#clear + 1] = def.id
            end
        end
        if #clear > 0 then
            Send(panel, nil, clear, false)
        end
    end)
    panel.resetCat:SetPoint("RIGHT", panel.resetAll, "LEFT", -6, 0)
    return panel
end

local function CategoryButton(panel, index)
    local button = panel.catButtons[index]
    if not button then
        button = W.Button(panel.cats, "", Behaviour.CATEGORY_WIDTH - 4, 22, function(self)
            panel.category = self.catKey
            panel.search:SetText("")
            UI.RequestRefresh()
        end)
        button:SetPoint("TOPLEFT", panel.cats, "TOPLEFT", 2, -(index - 1) * 24)
        -- the small font: category names like "Vendors and provisions" are wider than the button in the normal one
        button:SetNormalFontObject(GameFontNormalSmall)
        button:SetHighlightFontObject(GameFontHighlightSmall)
        panel.catButtons[index] = button
    end
    return button
end

local function ChangedIn(category, g)
    local count = 0
    for _, def in ipairs(State.settings.defs) do
        if def.cat == category then
            local _, source = State.SettingValue(def, g)
            if (g == 0 and source == "guild") or (g ~= 0 and source == "own") then
                count = count + 1
            end
        end
    end
    return count
end

function UI.RefreshBehaviourPanel(panel)
    local s = State.settings
    local g = panel.scope() or 0
    if not panel.category and s.cats[1] then
        panel.category = s.cats[1].k
    end

    local search = panel.search:GetText() or ""
    -- categories (a bot's panel lists only those with settings a bot can have of its own)
    local shownCats = 0
    for _, cat in ipairs(s.cats) do
        local hasAny = false
        for _, def in ipairs(s.defs) do
            if def.cat == cat.k and (g == 0 or def.pc) then
                hasAny = true
                break
            end
        end
        if hasAny then
            shownCats = shownCats + 1
            local button = CategoryButton(panel, shownCats)
            button.catKey = cat.k
            local changed = ChangedIn(cat.k, g)
            local label = (cat.n or cat.k) .. (changed > 0 and (" (" .. changed .. ")") or "")
            -- the category shown: white text and a held highlight (search lists every category: none is selected)
            local selected = cat.k == panel.category and search == ""
            button:SetText(selected and (W.COLORS.white .. label .. "|r") or label)
            if selected then
                button:LockHighlight()
            else
                button:UnlockHighlight()
            end
            W.SetTooltip(button, cat.n or cat.k, cat.h)
            button:Show()
        end
    end
    for i = shownCats + 1, #panel.catButtons do
        panel.catButtons[i]:Hide()
    end

    local items = Behaviour.BuildItems({
        g = g, category = panel.category, search = search,
        changedOnly = W.IsChecked(panel.changed), showAll = W.IsChecked(panel.all),
    })
    panel.list:SetItems(items)
    if #s.defs == 0 then
        panel.empty:SetText(State.IsConnected() and "Waiting for the server's settings..."
            or "Connect to the Guild Adventure server to change how your bots play.")
    elseif #items == 0 then
        panel.empty:SetText(search ~= "" and "No setting matches." or "Nothing changed here.")
    else
        panel.empty:SetText("")
    end

    local changed = State.ChangedSettingCount(g)
    panel.count:SetText(string.format(g == 0 and "%d changed from the server's defaults" or "%d set for this bot only", changed))
    local connected = State.IsConnected()
    W.SetEnabled(panel.resetAll, connected and changed > 0)
    W.SetEnabled(panel.resetCat, connected and search == "" and ChangedIn(panel.category, g) > 0)
end

-------------------------------------------------------------------------------
-- Bot detail: Behaviour sub-tab
-------------------------------------------------------------------------------

function UI.CreateBotBehaviour(parent)
    local panel = UI.CreateBehaviourPanel(parent, {
        scope = function()
            return UI.SelectedBot() or 0
        end,
        intro = "Values set here hold for this bot only; everything else follows the guild's settings (Settings tab, "
            .. "Bot behaviour).",
    })
    panel:SetAllPoints(parent)
    return panel
end

function UI.RefreshBotBehaviour(panel, bot)
    UI.RefreshBehaviourPanel(panel)
end
