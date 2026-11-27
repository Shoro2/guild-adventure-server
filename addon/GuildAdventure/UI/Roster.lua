-- Roster tab: sortable, filterable table of guild bots with a Play button per row, plus
-- free and locked slots so progression stays visible. Columns: name, level, gear level, dungeon role, the primary
-- professions with their skill, zone, order, state, XP.

local GA = GuildAdventure
local UI = GA.UI
local W = UI.W
local Data = GA.Data
local State = GA.State

local ROW_HEIGHT = 22

-- Column layout: name to professions from the left, order to the Play button from the right, so the zone column
-- between them absorbs extra width.
local COLUMNS = {
    { key = "name", label = "Name", sort = "name" },
    { key = "lvl", label = "Lvl", sort = "level" },
    { key = "ilvl", label = "iLvl", sort = "gear level",
        tip = "The average item level of its armour and weapons. Red: far below its level, yellow: a little below." },
    { key = "role", label = "Role", sort = "dungeon role",
        tip = "What it goes as in a dungeon run: the dungeon role you gave it (bright), else what its levelling talents make it (grey)." },
    { key = "prof", label = "Professions", sort = "highest profession skill",
        tip = "Its primary professions with their skill; the tooltip of a row lists them all." },
    { key = "zone", label = "Zone", sort = "zone" },
    { key = "order", label = "Order", sort = "order" },
    { key = "state", label = "State", sort = "state" },
    { key = "xp", label = "XP", sort = "XP" },
}
local WIDTH = { icon = 18, name = 92, lvl = 22, ilvl = 26, role = 30, prof = 74, order = 96, state = 62, xp = 52, play = 54 }
-- Columns that sort the highest first on the first click.
local SORT_DESC = { lvl = true, xp = true, ilvl = true, prof = true }

local ROLE_TEXT = { tank = "Tank", heal = "Heal", dps = "DPS" }
local ROLE_NAME = { tank = "tank", heal = "healer", dps = "damage" }
local ROLE_COLOR = { tank = "|cff60a0ff", heal = "|cff40ff40", dps = "|cffff8060" }

local filterText = ""
local filterState = "all"

local function SortSettings()
    local ui = GA.db.ui
    ui.rosterSort = ui.rosterSort or { key = "slot", desc = false }
    return ui.rosterSort
end

-- Lays out the cells of a row or the header with the same anchors.
local function LayoutCells(parent, cells)
    cells.play:SetPoint("RIGHT", parent, "RIGHT", -2, 0)
    cells.xp:SetPoint("RIGHT", cells.play, "LEFT", -6, 0)
    cells.state:SetPoint("RIGHT", cells.xp, "LEFT", -6, 0)
    cells.order:SetPoint("RIGHT", cells.state, "LEFT", -6, 0)
    cells.name:SetPoint("LEFT", parent, "LEFT", WIDTH.icon + 8, 0)
    cells.lvl:SetPoint("LEFT", cells.name, "RIGHT", 4, 0)
    cells.ilvl:SetPoint("LEFT", cells.lvl, "RIGHT", 4, 0)
    cells.role:SetPoint("LEFT", cells.ilvl, "RIGHT", 4, 0)
    cells.prof:SetPoint("LEFT", cells.role, "RIGHT", 4, 0)
    cells.zone:SetPoint("LEFT", cells.prof, "RIGHT", 6, 0)
    cells.zone:SetPoint("RIGHT", cells.order, "LEFT", -6, 0)
    -- one line per cell: a fixed height makes the client truncate instead of wrapping
    for _, key in ipairs({ "name", "lvl", "ilvl", "role", "prof", "zone", "order", "state" }) do
        cells[key]:SetHeight(14)
    end
    for _, key in ipairs({ "name", "lvl", "ilvl", "role", "prof", "order", "state", "xp", "play" }) do
        cells[key]:SetWidth(WIDTH[key])
    end
end

-- Gear level text, coloured by how far it lies below the bot's level (as the Character sub-tab does); grey for a
-- bot that is not in the world (the value of when it was last seen).
local function GearText(bot)
    local gl = State.BotGearLevel(bot)
    if not gl then
        return "|cff9d9d9d-|r"
    end
    local level = bot.lvl or 1
    local color = (gl + 8 <= level) and "|cffff4040" or ((gl + 3 <= level) and "|cffffd100" or "|cffffffff")
    if bot.s == "benched" or bot.s == "offline" then
        color = "|cff9d9d9d"
    end
    return color .. gl .. "|r"
end

-- The role it goes as in a dungeon run (a bot without a role of its talents counts as damage, as in the run
-- window).
local function GoesAs(bot)
    local role = State.BotGroupRole(bot)
    return ROLE_TEXT[role] and role or "dps"
end

-- The role cell: bright when the owner set the dungeon role, grey when it comes from its talents.
local function RoleText(bot)
    local role = GoesAs(bot)
    local text = ROLE_TEXT[role]
    if bot.dr == "tank" or bot.dr == "heal" then
        return ROLE_COLOR[role] .. text .. "|r"
    end
    return "|cff9d9d9d" .. text .. "|r"
end

local function ProfIcon(id)
    return "|T" .. Data.ProfessionIcon(id) .. ":12:12|t"
end

-- The two primary professions as icon + skill (gold at the rank's cap); a bot without one shows its secondary
-- professions in grey instead.
local function ProfessionText(bot)
    local primary, secondary = State.BotProfessions(bot)
    local shown = #primary > 0 and primary or secondary
    local parts = {}
    for i = 1, math.min(2, #shown) do
        local p = shown[i]
        local color = "|cff9d9d9d"
        if #primary > 0 then
            color = (p.max > 0 and p.value >= p.max) and "|cffffd100" or "|cffffffff"
        end
        parts[#parts + 1] = ProfIcon(p.id) .. color .. p.value .. "|r"
    end
    if #parts == 0 then
        return "|cff9d9d9d-|r"
    end
    return table.concat(parts, " ")
end

-- Tooltip lines about gear level, dungeon role and professions.
local function DetailLines(bot, lines)
    local gl = State.BotGearLevel(bot)
    if gl then
        local stale = (bot.s == "benched" or bot.s == "offline") and " (when last seen)" or ""
        lines[#lines + 1] = string.format("Gear level %d%s", gl, stale)
    end
    if bot.dr == "tank" or bot.dr == "heal" then
        lines[#lines + 1] = "Dungeon role: " .. ROLE_NAME[bot.dr] .. " (set by you)"
    else
        lines[#lines + 1] = "Dungeon role: none set; goes as " .. ROLE_NAME[GoesAs(bot)] .. " by its talents"
    end
    local primary, secondary = State.BotProfessions(bot)
    local function Join(list)
        local parts = {}
        for _, p in ipairs(list) do
            parts[#parts + 1] = string.format("%s %d/%d", Data.ProfessionName(p.id), p.value, p.max)
        end
        return table.concat(parts, ", ")
    end
    if #primary > 0 then
        lines[#lines + 1] = "Professions: " .. Join(primary)
    end
    if #secondary > 0 then
        lines[#lines + 1] = "Secondary: " .. Join(secondary)
    end
    if #primary == 0 and #secondary == 0 then
        lines[#lines + 1] = "|cff9d9d9dNo professions known yet.|r"
    end
end

-------------------------------------------------------------------------------
-- Rows
-------------------------------------------------------------------------------

local function PlayButtonState(bot)
    if State.control.busy then
        return "Play", false, "A control swap is in progress."
    elseif bot.s == "controlled" then
        return "Return", true, "Hand " .. (bot.n or "this bot") .. " back to the AI and play your leader again."
    elseif bot.s == "benched" then
        return "Play", false, "Benched bots are offline. Activate the bot first."
    elseif bot.s == "offline" then
        return "Play", false, "This bot is not in the world right now."
    elseif bot.dead then
        return "Play", false, "Dead bots cannot be taken over until they are alive again."
    elseif bot.combat then
        return "Play", true, "In combat right now: the switch follows as soon as the fight is over."
    end
    return "Play", true, "Take control of " .. (bot.n or "this bot") .. " seamlessly. Your current character keeps playing as a bot."
end

local function OnPlayClick(self)
    local bot = self:GetParent().bot
    if not bot then
        return
    end
    if bot.s == "controlled" then
        UI.ReturnControl()
    else
        UI.TakeControl(bot.g)
    end
end

local function CreateRow(list, index)
    local row = CreateFrame("Button", nil, list)
    row:RegisterForClicks("LeftButtonUp")
    local hl = row:CreateTexture(nil, "HIGHLIGHT")
    hl:SetAllPoints(row)
    hl:SetTexture("Interface\\QuestFrame\\UI-QuestTitleHighlight")
    hl:SetBlendMode("ADD")
    hl:SetAlpha(0.4)
    if index % 2 == 0 then
        local stripe = row:CreateTexture(nil, "BACKGROUND")
        stripe:SetAllPoints(row)
        stripe:SetTexture("Interface\\Buttons\\WHITE8X8")
        stripe:SetVertexColor(1, 1, 1, 0.04)
    end

    row.icon = W.ClassIcon(row, WIDTH.icon)
    row.icon:SetPoint("LEFT", row, "LEFT", 4, 0)
    local cells = {
        name = W.Label(row, "", "GameFontHighlightSmall"),
        lvl = W.Label(row, "", "GameFontHighlightSmall", "CENTER"),
        ilvl = W.Label(row, "", "GameFontHighlightSmall", "CENTER"),
        role = W.Label(row, "", "GameFontHighlightSmall"),
        prof = W.Label(row, "", "GameFontHighlightSmall"),
        zone = W.Label(row, "", "GameFontHighlightSmall"),
        order = W.Label(row, "", "GameFontHighlightSmall"),
        state = W.Label(row, "", "GameFontHighlightSmall"),
        xp = W.Bar(row, WIDTH.xp, 12, 0.58, 0, 0.55),
        play = W.Button(row, "Play", WIDTH.play, 18, OnPlayClick),
    }
    LayoutCells(row, cells)
    row.cells = cells
    row.wide = W.Label(row, "", "GameFontDisableSmall")
    row.wide:SetPoint("LEFT", row, "LEFT", WIDTH.icon + 8, 0)
    row.wide:SetPoint("RIGHT", row, "RIGHT", -4, 0)

    row:SetScript("OnClick", function(self)
        if self.item.kind == "bot" then
            UI.ShowBot(self.item.bot.g)
        elseif self.item.kind == "free" then
            UI.ShowPage("recruitment")
        elseif self.item.kind == "locked" then
            UI.ShowPage("progression")
        end
    end)
    W.SetTooltip(row, function(self)
        local item = self.item
        if not item then
            return nil
        end
        if item.kind == "bot" then
            return Data.ClassColorCode(item.bot.class) .. (item.bot.n or "?") .. "|r"
        elseif item.kind == "free" then
            return "Free slot"
        end
        return "Locked slots"
    end, function(self)
        local item = self.item
        if not item then
            return nil
        end
        if item.kind == "free" then
            return { "Click to recruit a new bot." }
        elseif item.kind == "locked" then
            return { item.text, "Click to see the progression track." }
        end
        local bot = item.bot
        local lines = {
            string.format("Level %d %s %s", bot.lvl or 1, Data.RaceName(bot.race), Data.ClassName(bot.class)),
            "Order: " .. Data.OrderLabel(bot.o) .. ((bot.ozn and bot.ozn ~= "") and (" (" .. bot.ozn .. ")") or ""),
            "|cffffd100" .. (bot.why or "") .. "|r",
            string.format("Health %d%%   Gold %s", bot.hp or 100, Data.FormatMoney(bot.gold or 0)),
        }
        DetailLines(bot, lines)
        lines[#lines + 1] = "Click for details."
        return lines
    end)
    return row
end

local function UpdateRow(row, item)
    row.item = item
    local cells = row.cells
    local isBot = item.kind == "bot"
    for key, cell in pairs(cells) do
        if isBot then
            cell:Show()
        else
            cell:Hide()
        end
    end
    if not isBot then
        row.bot = nil
        row.icon:Hide()
        row.wide:Show()
        if item.kind == "free" then
            row.wide:SetText("|cff40ff40+ Free slot - click to recruit|r")
        else
            row.wide:SetText(item.text)
        end
        return
    end
    row.wide:Hide()
    local bot = item.bot
    row.bot = bot
    row.icon:Show()
    row.icon:SetClass(bot.class)
    cells.name:SetText(Data.ClassColorCode(bot.class) .. (bot.n or "?") .. "|r")
    cells.lvl:SetText(bot.lvl or "")
    cells.ilvl:SetText(GearText(bot))
    cells.role:SetText(RoleText(bot))
    cells.prof:SetText(ProfessionText(bot))
    local zone = bot.zn or ""
    if bot.dead then
        zone = "|cffff4040(dead)|r " .. zone
    elseif bot.combat then
        zone = "|cffff8040(combat)|r " .. zone
    end
    cells.zone:SetText(zone)
    local order = Data.OrderLabel(bot.o)
    if bot.o == "LEVEL_ZONE" and bot.ozn and bot.ozn ~= "" then
        order = "Level: " .. bot.ozn
    end
    cells.order:SetText(order)
    cells.state:SetText(Data.StateLabel(bot.s, true))
    if bot.xpmax and bot.xpmax > 0 then
        cells.xp:SetProgress(bot.xp or 0, bot.xpmax, math.floor(100 * (bot.xp or 0) / bot.xpmax) .. "%")
    else
        cells.xp:SetProgress(1, 1, "max")
    end
    local text, enabled, tip = PlayButtonState(bot)
    cells.play:SetText(text)
    W.SetEnabled(cells.play, enabled and State.IsConnected())
    W.SetTooltip(cells.play, text == "Return" and "Return to leader" or "Take control", tip)
end

-------------------------------------------------------------------------------
-- Page
-------------------------------------------------------------------------------

local function Matches(bot)
    if filterState ~= "all" and bot.s ~= filterState then
        return false
    end
    if filterText ~= "" then
        local needle = string.lower(filterText)
        local hay = (bot.n or "") .. " " .. (bot.zn or "") .. " " .. Data.ClassName(bot.class)
            .. " " .. Data.RaceName(bot.race) .. " " .. Data.OrderLabel(bot.o)
        -- also the professions it has and the role it goes as ("mining", "tank", "healer")
        local primary, secondary = State.BotProfessions(bot)
        for _, list in ipairs({ primary, secondary }) do
            for _, p in ipairs(list) do
                hay = hay .. " " .. Data.ProfessionName(p.id)
            end
        end
        local role = GoesAs(bot)
        hay = string.lower(hay .. " " .. ROLE_TEXT[role] .. " " .. ROLE_NAME[role])
        if not string.find(hay, needle, 1, true) then
            return false
        end
    end
    return true
end

local function BuildItems()
    local sort = SortSettings()
    local items = {}
    for _, bot in ipairs(State.SortedBots(sort.key, sort.desc, Matches)) do
        items[#items + 1] = { kind = "bot", bot = bot }
    end
    if filterState == "all" and filterText == "" then
        for _ = 1, State.FreeSlots() do
            items[#items + 1] = { kind = "free" }
        end
        local g = State.guild
        local locked = (g.max or 39) - (g.slots or 0)
        if locked > 0 then
            local nextSlots = State.NextUnlock("slots")
            local text = "|cff9d9d9d" .. locked .. " more slots locked|r"
            if nextSlots then
                text = text .. "  |cff808080(next: " .. (nextSlots.val or "?") .. " slots at Renown " .. (nextSlots.lvl or "?")
                    .. ((nextSlots.gold or 0) > 0 and (", " .. Data.FormatMoney(nextSlots.gold)) or "") .. ")|r"
            end
            items[#items + 1] = { kind = "locked", text = text }
        end
    end
    return items
end

local function CreateHeader(parent)
    local header = CreateFrame("Frame", nil, parent)
    header:SetHeight(20)
    local cells = {}
    for _, col in ipairs(COLUMNS) do
        local b = CreateFrame("Button", nil, header)
        b:SetHeight(18)
        local label = W.Label(b, col.label, "GameFontNormalSmall")
        label:SetPoint("LEFT", b, "LEFT", 0, 0)
        b.label = label
        b.key = col.key
        b:SetScript("OnClick", function(self)
            local sort = SortSettings()
            if sort.key == self.key then
                sort.desc = not sort.desc
            else
                sort.key = self.key
                sort.desc = SORT_DESC[self.key] or false
            end
            UI.RequestRefresh()
        end)
        local tip = { "Click again to reverse the order." }
        if col.tip then
            table.insert(tip, 1, col.tip)
        end
        W.SetTooltip(b, "Sort by " .. col.sort, tip)
        cells[col.key] = b
    end
    cells.play = CreateFrame("Frame", nil, header)
    cells.play:SetHeight(18)
    LayoutCells(header, cells)
    header.cells = cells
    return header
end

local function UpdateHeader(header)
    local sort = SortSettings()
    for _, col in ipairs(COLUMNS) do
        local b = header.cells[col.key]
        local arrow = ""
        if sort.key == col.key then
            arrow = sort.desc and " v" or " ^"
        end
        b.label:SetText(col.label .. arrow)
    end
end

local function Create(parent)
    local f = CreateFrame("Frame", nil, parent)

    local searchLabel = W.Label(f, "Search", "GameFontNormalSmall")
    searchLabel:SetPoint("TOPLEFT", f, "TOPLEFT", 6, -10)
    f.search = W.EditBox(f, 140, false, 30)
    f.search:SetPoint("LEFT", searchLabel, "RIGHT", 10, 0)
    f.search:SetScript("OnTextChanged", function(self)
        filterText = self:GetText() or ""
        UI.RequestRefresh()
    end)
    f.search:SetScript("OnEscapePressed", function(self)
        self:SetText("")
        self:ClearFocus()
    end)

    f.stateFilter = W.DropDown(f, 110, function()
        local items = { { text = "All bots", value = "all" } }
        for _, key in ipairs({ "active", "benched", "controlled", "offline" }) do
            items[#items + 1] = { text = Data.StateLabel(key), value = key }
        end
        return items
    end)
    f.stateFilter:SetPoint("LEFT", f.search, "RIGHT", 0, -2)
    f.stateFilter:SetSelected("all", "All bots")
    f.stateFilter.onSelect = function(value)
        filterState = value
        UI.RequestRefresh()
    end

    f.recruit = W.Button(f, "Recruit...", 100, 22, function()
        UI.ShowPage("recruitment")
    end)
    f.recruit:SetPoint("TOPRIGHT", f, "TOPRIGHT", -4, -6)
    f.count = W.Label(f, "", "GameFontHighlightSmall", "RIGHT")
    f.count:SetPoint("RIGHT", f.recruit, "LEFT", -10, 0)

    local panel = W.Panel(f)
    panel:SetPoint("TOPLEFT", f, "TOPLEFT", 0, -36)
    panel:SetPoint("BOTTOMRIGHT", f, "BOTTOMRIGHT", 0, 0)

    f.header = CreateHeader(panel)
    f.header:SetPoint("TOPLEFT", panel, "TOPLEFT", 6, -6)
    f.header:SetPoint("RIGHT", panel, "RIGHT", -32, 0)

    f.list = W.ScrollList(panel, { rowHeight = ROW_HEIGHT, createRow = CreateRow, updateRow = UpdateRow })
    f.list:SetPoint("TOPLEFT", panel, "TOPLEFT", 6, -28)
    f.list:SetPoint("BOTTOMRIGHT", panel, "BOTTOMRIGHT", -6, 6)

    f.empty = W.Label(panel, "No bots match the filter.", "GameFontDisableSmall", "CENTER")
    f.empty:SetPoint("CENTER", panel, "CENTER", 0, 0)
    return f
end

local function Refresh(f)
    local g = State.guild
    f.count:SetText((g.used or State.BotCount()) .. " / " .. (g.slots or 0) .. " slots")
    W.SetEnabled(f.recruit, State.HasFeature("recruit") and State.FreeSlots() > 0)
    UpdateHeader(f.header)
    local items = BuildItems()
    f.list:SetItems(items)
    if #items == 0 then
        f.empty:Show()
    else
        f.empty:Hide()
    end
end

UI.RegisterPage("roster", { tab = "roster", create = Create, refresh = Refresh })
