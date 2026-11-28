-- Bot detail: Talents sub-tab (docs/specs.md), below it the Dungeon role sub-tab (docs/dungeon-roles.md) and the Combat
-- sub-tab (docs/fight-profiles.md).
-- Talents sub-tab. The talent template a bot plays by: the default of its class, one of
-- the built-in templates, one the guild saved, or none ("Manual"). The server spends the bot's talent points along the
-- template; assigning one resets its talents for free.

local GA = GuildAdventure
local UI = GA.UI
local W = UI.W
local Data = GA.Data
local State = GA.State
local Protocol = GA.Protocol

local PURPOSES = { [0] = "Leveling", [1] = "Dungeon and raid", [2] = "Raid", [3] = "PvP" }
local PURPOSE_ORDER = { 0, 1, 2, 3 }
local ROLES = { [0] = "damage", [1] = "tank", [2] = "healer" }

local function DefaultOf(class)
    for _, spec in ipairs(State.SpecsForClass(class)) do
        if spec.d then
            return spec
        end
    end
end

-- The choices of a bot: the class default, manual, then the templates of its class.
local function BuildRows(bot)
    local default = DefaultOf(bot.class)
    local rows = {
        { id = 0, n = "Class default", detail = default and ("\"" .. default.n .. "\"") or "none for this class" },
        { id = 1, n = "Manual", detail = "the server spends no talent points for this bot" },
    }
    for _, spec in ipairs(State.SpecsForClass(bot.class)) do
        rows[#rows + 1] = {
            id = spec.id, n = spec.n, own = spec.own,
            detail = (ROLES[spec.ro or 0] or "damage") .. ", " .. (spec.sum or "") .. " at level 60"
                .. (spec.own and ", saved by your guild" or ""),
        }
    end
    return rows
end

local function Assign(bot, row)
    local function send()
        Protocol.SetSpec(bot.g, row.id, UI.RequestCallback("Talent template of " .. (bot.n or "the bot") .. ": " .. row.n .. "."))
    end
    if row.id == 1 then
        send()
    else
        UI.Confirm(string.format("Let %s play by \"%s\"?\nIts talents are reset (free) and learned again in the template's order.",
            bot.n or "the bot", row.n), send)
    end
end

local function CreateRow(list)
    local row = CreateFrame("Frame", nil, list)
    row.shade = row:CreateTexture(nil, "BACKGROUND")
    row.shade:SetAllPoints(row)
    row.shade:SetTexture("Interface\\Buttons\\WHITE8X8")
    row.assign = W.Button(row, "Assign", 70, 20, function()
        local bot = State.GetBot(UI.SelectedBot())
        if bot and row.item then
            Assign(bot, row.item)
        end
    end)
    row.assign:SetPoint("RIGHT", row, "RIGHT", -4, 0)
    row.delete = W.Button(row, "Delete", 60, 20, function()
        local item = row.item
        if item and item.own then
            UI.Confirm("Delete the template \"" .. item.n .. "\"?\nBots that play by it go back to the default of their class.", function()
                Protocol.DeleteSpec(item.id, UI.RequestCallback("Template deleted."))
            end)
        end
    end)
    row.delete:SetPoint("RIGHT", row.assign, "LEFT", -4, 0)
    row.name = W.Label(row, "", "GameFontHighlightSmall")
    row.name:SetPoint("LEFT", row, "LEFT", 6, 0)
    row.name:SetWidth(210)
    row.detail = W.Label(row, "", "GameFontDisableSmall")
    row.detail:SetPoint("LEFT", row.name, "RIGHT", 6, 0)
    row.detail:SetPoint("RIGHT", row.delete, "LEFT", -6, 0)
    return row
end

local function UpdateRow(row, item, index)
    row.item = item
    local view = row:GetParent().view
    local active = view.assigned == item.id
    row.shade:SetVertexColor(1, 0.82, 0, active and 0.18 or (index % 2 == 0 and 0.04 or 0))
    row.name:SetText((active and "|cffffd100" or "|cffffffff") .. item.n .. "|r")
    row.detail:SetText(item.detail or "")
    row.assign:SetText(active and "Active" or "Assign")
    W.SetEnabled(row.assign, State.IsConnected() and not active)
    if item.own then
        row.delete:Show()
        W.SetEnabled(row.delete, State.IsConnected())
    else
        row.delete:Hide()
    end
end

function UI.CreateBotTalents(parent)
    local f = CreateFrame("Frame", nil, parent)
    f:SetAllPoints(parent)

    f.status = W.Label(f, "", "GameFontHighlightSmall")
    f.status:SetPoint("TOPLEFT", f, "TOPLEFT", 6, -6)
    f.status:SetPoint("RIGHT", f, "RIGHT", -8, 0)

    local panel = W.Panel(f)
    panel:SetPoint("TOPLEFT", f, "TOPLEFT", 0, -26)
    panel:SetPoint("BOTTOMRIGHT", f, "BOTTOMRIGHT", 0, 58)
    f.list = W.ScrollList(panel, { rowHeight = 24, createRow = CreateRow, updateRow = UpdateRow })
    f.list.view = f
    f.list:SetPoint("TOPLEFT", panel, "TOPLEFT", 6, -6)
    f.list:SetPoint("BOTTOMRIGHT", panel, "BOTTOMRIGHT", -6, 6)

    -- Saving the talents of the character you play as a template of the guild.
    f.saveLabel = W.Label(f, "", "GameFontNormalSmall")
    f.saveLabel:SetPoint("BOTTOMLEFT", f, "BOTTOMLEFT", 4, 36)
    f.saveLabel:SetPoint("RIGHT", f, "RIGHT", -8, 0)
    f.name = W.EditBox(f, 170, false, 40)
    f.name:SetPoint("BOTTOMLEFT", f, "BOTTOMLEFT", 10, 8)
    f.purpose = 0
    f.purposeDD = W.DropDown(f, 130, function()
        local items = {}
        for _, key in ipairs(PURPOSE_ORDER) do
            items[#items + 1] = { text = PURPOSES[key], value = key }
        end
        return items
    end)
    f.purposeDD:SetPoint("LEFT", f.name, "RIGHT", -6, -2)
    f.purposeDD.onSelect = function(value)
        f.purpose = value
    end
    UIDropDownMenu_SetText(f.purposeDD, PURPOSES[0])
    f.save = W.Button(f, "Save as template", 130, 22, function()
        local name = string.gsub(f.name:GetText() or "", "^%s*(.-)%s*$", "%1")
        if name == "" then
            UI.Notify("Give the template a name.", true)
            return
        end
        Protocol.SaveSpec(name, f.purpose or 0, UI.RequestCallback("Template \"" .. name .. "\" saved.", function(ok)
            if ok then
                W.SetEditText(f.name, "")
            end
        end))
    end)
    f.save:SetPoint("LEFT", f.purposeDD, "RIGHT", -8, 2)

    f.lock = W.LockedOverlay(f)
    return f
end

function UI.RefreshBotTalents(f, bot)
    if not State.HasFeature("talents") then
        f.lock:SetLock("Talents", State.FeatureRequirement("talents"), "Pick a talent template; the bot spends its points on level-up.")
        return
    end
    f.lock:Hide()

    local connected = State.IsConnected()
    f.assigned = bot.spt or 0
    local rows = BuildRows(bot)
    f.list:SetItems(rows)

    local class = Data.CLASSES[bot.class or 0]
    local talents = (bot.tal and bot.tal ~= "") and bot.tal or nil
    local plays = (bot.spec and bot.spec ~= "") and bot.spec or "no template"
    local how = (f.assigned == 0 and " (the default of its class)") or ""
    f.status:SetText(string.format("%s (%s, level %d)%s plays by: |cffffd100%s|r%s", bot.n or "The bot", class and class.name or "?",
        bot.lvl or 1, talents and (", talents " .. talents .. ",") or "", plays, how))

    local playing = State.control.cn or "the character you play"
    f.saveLabel:SetText("Save the talents " .. playing .. " has now as a template for its class (set them in the normal talent window first):")
    W.SetEnabled(f.save, connected)
end

GA.On("SPECS", function()
    UI.RequestRefresh()
end)

-------------------------------------------------------------------------------
-- Bot detail: Dungeon role sub-tab (docs/dungeon-roles.md). The role the bot fills in a dungeon group (tank or
-- healer, what its class can be), the talents it changes to for a run, and its role store: the dungeon set it
-- collects while it levels, kept outside its bags.
-------------------------------------------------------------------------------

local ROLE_TEXT = { none = "No role (it goes as it levels)", tank = "Tank", heal = "Healer" }
local ROLE_NUMBER = { tank = 1, heal = 2 }
local SLOT_TEXT = { [0] = "head", [1] = "neck", [2] = "shoulders", [4] = "chest", [5] = "waist", [6] = "legs", [7] = "feet",
    [8] = "wrists", [9] = "hands", [10] = "finger", [11] = "finger", [14] = "back", [15] = "main hand", [16] = "off hand",
    [17] = "ranged" }
local INVTYPE_TEXT = { [1] = "head", [2] = "neck", [3] = "shoulders", [5] = "chest", [6] = "waist", [7] = "legs", [8] = "feet",
    [9] = "wrists", [10] = "hands", [11] = "finger", [13] = "one-hand", [14] = "shield", [15] = "ranged", [16] = "back",
    [17] = "two-hand", [20] = "chest", [21] = "main hand", [23] = "off hand", [26] = "ranged", [28] = "relic" }

local function RoleQualityColor(quality)
    local c = ITEM_QUALITY_COLORS and ITEM_QUALITY_COLORS[quality or 1]
    return c and c.hex or "|cffffffff"
end

-- The roles a bot's class can fill ("tank,heal" from the server).
local function RolesOf(bot)
    local list = {}
    for token in string.gmatch(bot.drs or "", "[^,]+") do
        list[#list + 1] = token
    end
    return list
end

-- The talent templates a role can use: those of the class with that role, and the guild's own of the class.
local function RoleTemplates(bot, role)
    local list = {}
    for _, spec in ipairs(State.SpecsForClass(bot.class)) do
        if spec.own or (spec.ro or 0) == (ROLE_NUMBER[role] or -1) then
            list[#list + 1] = spec
        end
    end
    return list
end

-- For the dungeon run window's preparation step: what a bot can be given for a run, { { text, role, t }, ... }: as it
-- levels (no dungeon role), or a role its class can fill with one of the role's talent templates.
function UI.RunTalentChoices(bot)
    local items = { { text = "As it levels", role = "none", t = 0 } }
    for _, token in ipairs(RolesOf(bot)) do
        local templates = RoleTemplates(bot, token)
        if #templates == 0 then
            items[#items + 1] = { text = ROLE_TEXT[token] or token, role = token, t = 0 }
        end
        for _, spec in ipairs(templates) do
            items[#items + 1] = { text = (ROLE_TEXT[token] or token) .. ": " .. (spec.n or "?"), role = token, t = spec.id }
        end
    end
    return items
end

local function CreateRoleRow(list)
    local row = CreateFrame("Frame", nil, list)
    row.shade = row:CreateTexture(nil, "BACKGROUND")
    row.shade:SetAllPoints(row)
    row.shade:SetTexture("Interface\\Buttons\\WHITE8X8")
    row.kind = W.Label(row, "", "GameFontDisableSmall")
    row.kind:SetPoint("LEFT", row, "LEFT", 6, 0)
    row.kind:SetWidth(150)
    row.name = W.Label(row, "", "GameFontHighlightSmall")
    row.name:SetPoint("LEFT", row.kind, "RIGHT", 6, 0)
    row.name:SetWidth(260)
    row.detail = W.Label(row, "", "GameFontDisableSmall")
    row.detail:SetPoint("LEFT", row.name, "RIGHT", 6, 0)
    row.detail:SetPoint("RIGHT", row, "RIGHT", -6, 0)
    return row
end

local function UpdateRoleRow(row, item, index)
    row.shade:SetVertexColor(1, 1, 1, index % 2 == 0 and 0.04 or 0)
    row.kind:SetText(item.k == 2 and "Levelling piece (put aside)" or "Dungeon set")
    row.name:SetText(RoleQualityColor(item.q) .. (item.n or "?") .. "|r")
    local place = item.k == 2 and SLOT_TEXT[item.s or -1] or INVTYPE_TEXT[item.it or -1]
    row.detail:SetText(string.format("item level %d%s%s", item.il or 0, place and (", " .. place) or "",
        (item.k == 1 and item.s == 254) and ", never worn" or ""))
end

function UI.CreateBotRole(parent)
    local f = CreateFrame("Frame", nil, parent)
    f:SetAllPoints(parent)

    -- (Texts of two lines: a fixed width makes a font string wrap.)
    f.status = W.Label(f, "", "GameFontHighlightSmall")
    f.status:SetPoint("TOPLEFT", f, "TOPLEFT", 6, -6)
    f.status:SetWidth(720)
    f.status:SetHeight(24)
    f.status:SetJustifyV("TOP")

    f.roleLabel = W.Label(f, "Dungeon role", "GameFontNormalSmall")
    f.roleLabel:SetPoint("TOPLEFT", f, "TOPLEFT", 6, -40)
    f.roleDD = W.DropDown(f, 190, function()
        local bot = State.GetBot(UI.SelectedBot())
        local items = { { text = ROLE_TEXT.none, value = "none" } }
        for _, token in ipairs(bot and RolesOf(bot) or {}) do
            items[#items + 1] = { text = ROLE_TEXT[token] or token, value = token }
        end
        return items
    end)
    f.roleDD:SetPoint("LEFT", f.roleLabel, "RIGHT", -6, -2)
    f.roleDD.onSelect = function(value)
        f.draftRole = value
        f.draftTemplate = 0
        UI.RequestRefresh()
    end

    f.specLabel = W.Label(f, "Talents for a run", "GameFontNormalSmall")
    f.specLabel:SetPoint("LEFT", f.roleDD, "RIGHT", 0, 2)
    f.specDD = W.DropDown(f, 220, function()
        local bot = State.GetBot(UI.SelectedBot())
        local items = { { text = "The default of its class", value = 0 } }
        for _, spec in ipairs(bot and RoleTemplates(bot, f.draftRole or bot.dr) or {}) do
            items[#items + 1] = { text = spec.n, value = spec.id }
        end
        return items
    end)
    f.specDD:SetPoint("LEFT", f.specLabel, "RIGHT", -6, -2)
    f.specDD.onSelect = function(value)
        f.draftTemplate = value
        UI.RequestRefresh()
    end

    f.apply = W.Button(f, "Apply", 80, 22, function()
        local bot = State.GetBot(UI.SelectedBot())
        if not bot then
            return
        end
        local role = f.draftRole or bot.dr or "none"
        Protocol.SetRole(bot.g, role, f.draftTemplate or bot.drt or 0, UI.RequestCallback(
            "Dungeon role of " .. (bot.n or "the bot") .. ": " .. (ROLE_TEXT[role] or role) .. ".", function(ok)
                if ok then
                    f.draftRole, f.draftTemplate = nil, nil
                    Protocol.Get("rolegear", bot.g)
                end
            end))
    end)
    f.apply:SetPoint("LEFT", f.specDD, "RIGHT", -6, 2)

    f.info = W.Label(f, "", "GameFontDisableSmall")
    f.info:SetPoint("TOPLEFT", f, "TOPLEFT", 6, -68)
    f.info:SetWidth(720)
    f.info:SetHeight(36)
    f.info:SetJustifyV("TOP")

    f.setLine = W.Label(f, "", "GameFontHighlightSmall")
    f.setLine:SetPoint("TOPLEFT", f, "TOPLEFT", 6, -110)
    f.setLine:SetWidth(620)
    f.reload = W.Button(f, "Refresh", 80, 20, function()
        local bot = State.GetBot(UI.SelectedBot())
        if bot then
            Protocol.Get("rolegear", bot.g)
        end
    end)
    f.reload:SetPoint("TOPRIGHT", f, "TOPRIGHT", -6, -106)

    local panel = W.Panel(f)
    panel:SetPoint("TOPLEFT", f, "TOPLEFT", 0, -130)
    panel:SetPoint("BOTTOMRIGHT", f, "BOTTOMRIGHT", 0, 0)
    f.list = W.ScrollList(panel, { rowHeight = 20, createRow = CreateRoleRow, updateRow = UpdateRoleRow })
    f.list:SetPoint("TOPLEFT", panel, "TOPLEFT", 6, -6)
    f.list:SetPoint("BOTTOMRIGHT", panel, "BOTTOMRIGHT", -6, 6)
    f.empty = W.Label(panel, "", "GameFontDisableSmall")
    f.empty:SetPoint("TOPLEFT", panel, "TOPLEFT", 14, -12)
    f.empty:SetPoint("RIGHT", panel, "RIGHT", -14, 0)
    f.empty:SetJustifyH("LEFT")
    return f
end

function UI.RefreshBotRole(f, bot)
    local connected = State.IsConnected()
    if f.bot ~= bot.g then
        f.bot = bot.g
        f.draftRole, f.draftTemplate = nil, nil
        f.asked = nil
    end
    -- The role store comes on request: once per look at this bot, and after a change.
    if connected and f.asked ~= bot.g then
        f.asked = bot.g
        Protocol.Get("rolegear", bot.g)
    end

    local class = Data.CLASSES[bot.class or 0]
    local role = bot.dr or "none"
    local can = RolesOf(bot)
    local draftRole = f.draftRole or role
    local draftTemplate = f.draftTemplate or (f.draftRole and 0) or bot.drt or 0

    local status = string.format("%s (%s, level %d)", bot.n or "The bot", class and class.name or "?", bot.lvl or 1)
    if #can == 0 then
        status = status .. ": its class deals damage only. It goes into a dungeon as it levels."
    elseif role == "none" then
        status = status .. ": no dungeon role. It goes into a dungeon with the talents and the gear it levels with."
    else
        status = status .. string.format(": |cffffd100%s|r in a dungeon group, with the talents |cffffd100%s|r. It wears its %s set now.",
            ROLE_TEXT[role] or role, (bot.drn and bot.drn ~= "") and bot.drn or "of its role", bot.drw and "dungeon" or "levelling")
    end
    f.status:SetText(status)

    f.roleDD:SetSelected(draftRole, ROLE_TEXT[draftRole] or draftRole)
    f.roleDD:SetEnabled(connected and #can > 0)
    local specName = "The default of its class"
    for _, spec in ipairs(RoleTemplates(bot, draftRole)) do
        if spec.id == draftTemplate then
            specName = spec.n
        end
    end
    f.specDD:SetSelected(draftTemplate, specName)
    f.specDD:SetEnabled(connected and draftRole ~= "none")
    W.SetEnabled(f.apply, connected and #can > 0 and (draftRole ~= role or draftTemplate ~= (bot.drt or 0)))

    f.info:SetText("With a role the bot collects a dungeon set while it levels: loot, quest rewards and pieces of the guild storage"
        .. " that are better for the role than what it has. The set lies in its role store, not in its bags. For a dungeon run"
        .. " it changes to the talents of the role and puts the set on; afterwards it changes back. Both are free.")

    local pieces = State.roleGear[bot.g] or {}
    local own = 0
    for _, piece in ipairs(pieces) do
        if piece.k == 1 then
            own = own + 1
        end
    end
    if role == "none" then
        f.setLine:SetText(#pieces > 0 and "Role store (it empties into the bags when the bot next looks at it):" or "")
    elseif bot.drgl then
        f.setLine:SetText(string.format("Dungeon set: %d piece%s of its own, gear level |cffffd100%d|r in the set (%s in what it wears now).",
            bot.drp or own, (bot.drp or own) == 1 and "" or "s", bot.drgl, tostring(bot.gl or "?")))
    else
        f.setLine:SetText(string.format("Dungeon set: %d piece%s of its own (gear level known while the bot is in the world).",
            bot.drp or own, (bot.drp or own) == 1 and "" or "s"))
    end
    f.list:SetItems(pieces)
    if #pieces == 0 then
        f.empty:SetText(role == "none" and "The role store is empty." or
            "The role store is empty: the bot has found nothing yet that is better for its role than what it wears.")
        f.empty:Show()
    else
        f.empty:Hide()
    end
end

GA.On("ROLEGEAR", function()
    UI.RequestRefresh()
end)

-------------------------------------------------------------------------------
-- Bot detail: Combat sub-tab (docs/fight-profiles.md, package FP-2). The fight profile of the bot's two slots (solo,
-- and group: a dungeon or raid and a dungeon run), what it plays now and why, the rules of a profile read-only (the
-- template is mod-playerbots' rotation read from the bot), this bot's own values of a profile's options, a new profile
-- from its template, and "Copy to bots": its two slots and its own option values for other bots of its class.
-------------------------------------------------------------------------------

local TREES = {
    [1] = { "Arms", "Fury", "Protection" }, [2] = { "Holy", "Protection", "Retribution" },
    [3] = { "Beast Mastery", "Marksmanship", "Survival" }, [4] = { "Assassination", "Combat", "Subtlety" },
    [5] = { "Discipline", "Holy", "Shadow" }, [6] = { "Blood", "Frost", "Unholy" },
    [7] = { "Elemental", "Enhancement", "Restoration" }, [8] = { "Arcane", "Fire", "Frost" },
    [9] = { "Affliction", "Demonology", "Destruction" }, [11] = { "Balance", "Feral Combat", "Restoration" },
}
local FIGHT_TARGETS = { [0] = "its target", [1] = "itself", [2] = "its pet", [3] = "the lowest member", [4] = "the tank",
    [5] = "an enemy" }
-- mod-playerbots' relevance bands (Strategy.h), highest first: the list's divider lines (FC8).
local FIGHT_BANDS = {
    { min = 90, text = "Emergency" }, { min = 60, text = "Raid" }, { min = 50, text = "Dispel" },
    { min = 40, text = "Interrupt" }, { min = 30, text = "Critical heals, movement" }, { min = 20, text = "High" },
    { min = 10, text = "Normal" }, { min = -1000, text = "Rotation" },
}
local FIGHT_OPTION_KIND = { [0] = "switch", [1] = "number", [2] = "percent" }

local function TreeName(class, tab)
    local trees = TREES[class or 0]
    return trees and tab and trees[tab + 1] or nil
end

function UI.FightTreeName(class, tab)
    return TreeName(class, tab)
end

local function BandOf(relevance)
    for _, band in ipairs(FIGHT_BANDS) do
        if (relevance or 0) >= band.min then
            return band
        end
    end
    return FIGHT_BANDS[#FIGHT_BANDS]
end

local function TitleCase(text)
    return (string.gsub(text or "", "(%a)([%w']*)", function(first, rest)
        return string.upper(first) .. rest
    end))
end

-- What a rule does, for the list: its name and its icon (when the client knows it).
function UI.FightRuleName(rule)
    if rule.k == 2 then
        local name, _, icon = GetSpellInfo(rule.ref or 0)
        return name or ("spell " .. tostring(rule.ref)), icon
    elseif rule.k == 3 then
        local name, _, _, _, _, _, _, _, _, icon
        if GetItemInfo then
            name, _, _, _, _, _, _, _, _, icon = GetItemInfo(rule.ref or 0)
        end
        return "Use " .. (name or ("item " .. tostring(rule.ref))), icon
    elseif rule.k == 4 then
        return "Use trinket " .. tostring(rule.ref or 1), "Interface\\Icons\\INV_Misc_Gear_01"
    end
    -- A row of mod-playerbots: its action's name; the server names the spell it casts as ref (for its icon).
    local icon
    if (rule.ref or 0) > 0 then
        icon = select(3, GetSpellInfo(rule.ref))
    end
    return TitleCase(rule.a), icon
end

-- A trigger of mod-playerbots in plain language (the server's FTRIGS for the bot's class), nil when it has none.
function UI.FightTriggerText(g, name)
    local texts = State.fight.trig[g]
    local text = texts and texts[name or ""]
    if text and text ~= "" then
        return text
    end
end

-- What makes a rule fire, for the lists' grey column and tooltips: its trigger in plain language, then its conditions.
function UI.FightWhenText(g, trigger, conditions)
    local parts = {}
    if trigger and trigger ~= "" then
        parts[#parts + 1] = UI.FightTriggerText(g, trigger) or ("check \"" .. trigger .. "\"")
    end
    if conditions and conditions ~= "" then
        parts[#parts + 1] = conditions
    end
    return table.concat(parts, "; ")
end

-- The lines a rule's tooltip shows (the Combat sub-tab and the editor).
local function AddRuleTooltip(item, g, conditions)
    GameTooltip:AddLine((UI.FightRuleName(item)))
    if item.kn == false then
        GameTooltip:AddLine("Not learned yet" .. ((item.lv or 0) > 0 and (" (from level " .. item.lv .. ")") or "")
            .. ": the rule waits until the bot knows the spell.", 1, 0.5, 0.25, true)
    end
    if item.tg and item.tg ~= "" then
        local text = UI.FightTriggerText(g, item.tg)
        GameTooltip:AddLine("When: " .. (text or item.tg), 0.8, 0.8, 0.8, true)
        GameTooltip:AddLine("mod-playerbots' check \"" .. item.tg .. "\"", 0.5, 0.5, 0.5, true)
    end
    if conditions and conditions ~= "" then
        GameTooltip:AddLine("If: " .. conditions, 0.8, 0.8, 0.8, true)
    end
    if (not item.tg or item.tg == "") and (not conditions or conditions == "") then
        GameTooltip:AddLine("No check: it is tried whenever nothing more relevant is due.", 0.8, 0.8, 0.8, true)
    end
end

local function ShortName(list, id)
    for _, profile in ipairs(list and list.list or {}) do
        if profile.id == id then
            return (profile.n:gsub(" %(wowbot335%)$", ""))
        end
    end
    return "profile " .. tostring(id)
end

-- What an empty slot plays (FC12): the template of the bot's spec and role (FPROFILES_BEGIN dflt alone, dfltg in a
-- group), else mod-playerbots' rotation; an empty group slot plays the solo slot's profile when it has one.
local function DefaultName(list, slot, bot)
    if slot == 1 and bot and (bot.fp or 0) ~= 0 then
        return "As solo (" .. ShortName(list, bot.fp) .. ")"
    end
    local meta = list and list.meta or {}
    local id = (slot == 1) and meta.dfltg or meta.dflt
    if id and id > 0 then
        return "Default (" .. ShortName(list, id) .. ")"
    end
    return "Default (mod-playerbots)"
end

-- The profiles a slot can take: the default and the guild's profiles of the bot's class (those of another talent tree
-- than the bot's are grey: a bot plays a profile only in its own tree).
local function SlotChoices(list, tab, class, slot, bot)
    local items = { { text = DefaultName(list, slot, bot), value = 0 } }
    for _, profile in ipairs(list and list.list or {}) do
        local tree = TreeName(class, profile.tab) or "?"
        local other = tab and profile.tab ~= tab
        items[#items + 1] = {
            text = (other and "|cff808080" or "") .. profile.n .. " (" .. tree .. ")" .. (other and "|r" or ""),
            value = profile.id,
            tooltip = other and "A profile of another talent tree: the bot plays it only while its talents are that tree." or nil,
        }
    end
    return items
end

local function ProfileName(list, id, slot, bot)
    if (id or 0) == 0 then
        return slot and DefaultName(list, slot, bot) or "Default (mod-playerbots)"
    end
    for _, profile in ipairs(list and list.list or {}) do
        if profile.id == id then
            return profile.n
        end
    end
    return "profile " .. tostring(id)
end

-- The rows of the shown list: the rules in the engine's order (highest relevance first) with a divider row where a band
-- begins. Returns the rows and the number of rules.
function UI.FightRows(shown, listKind)
    local rules = {}
    for index, rule in ipairs(shown and shown.rules or {}) do
        if (rule.l or 0) == listKind then
            rule.index = index
            rules[#rules + 1] = rule
        end
    end
    table.sort(rules, function(a, b)
        if a.rel == b.rel then
            return a.index < b.index
        end
        return a.rel > b.rel
    end)
    local rows, band = {}, nil
    for _, rule in ipairs(rules) do
        local current = BandOf(rule.rel)
        if current ~= band then
            band = current
            rows[#rows + 1] = { divider = band.text }
        end
        rows[#rows + 1] = rule
    end
    return rows, #rules
end

local function CreateFightRow(list)
    local row = CreateFrame("Frame", nil, list)
    row.shade = row:CreateTexture(nil, "BACKGROUND")
    row.shade:SetAllPoints(row)
    row.shade:SetTexture("Interface\\Buttons\\WHITE8X8")
    row.icon = row:CreateTexture(nil, "ARTWORK")
    row.icon:SetWidth(16)
    row.icon:SetHeight(16)
    row.icon:SetPoint("LEFT", row, "LEFT", 4, 0)
    row.name = W.Label(row, "", "GameFontHighlightSmall")
    row.name:SetPoint("LEFT", row.icon, "RIGHT", 4, 0)
    row.name:SetWidth(140)
    row.name:SetHeight(12) -- one line: a long name ends in "...", the tooltip has it whole
    row.target = W.Label(row, "", "GameFontDisableSmall")
    row.target:SetPoint("LEFT", row.name, "RIGHT", 4, 0)
    row.target:SetWidth(84)
    row.rel = W.Label(row, "", "GameFontDisableSmall", "RIGHT")
    row.rel:SetPoint("RIGHT", row, "RIGHT", -4, 0)
    row.rel:SetWidth(34)
    row.cond = W.Label(row, "", "GameFontDisableSmall")
    row.cond:SetPoint("LEFT", row.target, "RIGHT", 4, 0)
    row.cond:SetPoint("RIGHT", row.rel, "LEFT", -4, 0)
    row.divider = W.Label(row, "", "GameFontNormalSmall")
    row.divider:SetPoint("LEFT", row, "LEFT", 6, 0)
    row:EnableMouse(true)
    row:SetScript("OnEnter", function(self)
        local item = self.item
        if not item or item.divider then
            return
        end
        GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
        AddRuleTooltip(item, item.g, item.d)
        GameTooltip:AddLine(string.format("Relevance %.1f, on %s", item.rel or 0, FIGHT_TARGETS[item.tgt or 0] or "?"), 0.6, 0.6, 0.6)
        if item.src and item.src ~= "" then
            GameTooltip:AddLine("From mod-playerbots' strategy \"" .. item.src .. "\"", 0.6, 0.6, 0.6)
        end
        if (item.iv or 0) > 0 then
            GameTooltip:AddLine("At most every " .. item.iv .. " s", 0.6, 0.6, 0.6)
        end
        if item.en == false then
            GameTooltip:AddLine("Switched off", 1, 0.3, 0.3)
        end
        GameTooltip:Show()
    end)
    row:SetScript("OnLeave", function()
        GameTooltip:Hide()
    end)
    return row
end

local function UpdateFightRow(row, item, index)
    row.item = item
    if item.divider then
        row.shade:SetVertexColor(1, 0.82, 0, 0.10)
        row.divider:SetText("- " .. item.divider .. " -")
        row.divider:Show()
        row.icon:Hide()
        row.name:SetText("")
        row.target:SetText("")
        row.cond:SetText("")
        row.rel:SetText("")
        return
    end
    row.divider:Hide()
    row.shade:SetVertexColor(1, 1, 1, index % 2 == 0 and 0.04 or 0)
    local name, icon = UI.FightRuleName(item)
    row.icon:SetTexture(icon or "Interface\\Icons\\INV_Misc_QuestionMark")
    row.icon:SetDesaturated(item.kn == false)
    row.icon:Show()
    local off = item.en == false or item.kn == false
    row.name:SetText((off and "|cff808080" or "|cffffffff") .. name .. "|r")
    row.target:SetText(FIGHT_TARGETS[item.tgt or 0] or "?")
    row.cond:SetText(UI.FightWhenText(item.g, item.tg, item.d))
    row.rel:SetText(string.format("%.1f", item.rel or 0))
end

local function OptionText(kind, value)
    if kind == 0 then
        return value ~= 0 and "on" or "off"
    elseif kind == 2 then
        return tostring(value) .. "%"
    end
    return tostring(value)
end

local function CreateFightOptionRow(list)
    local row = CreateFrame("Frame", nil, list)
    row.label = W.Label(row, "", "GameFontHighlightSmall")
    row.label:SetPoint("LEFT", row, "LEFT", 4, 0)
    row.label:SetWidth(96)
    -- One line (a long name is cut off with "..."; the tooltip has it whole): two lines and more ran into the next row.
    row.label:SetHeight(12)
    row.label:SetJustifyH("LEFT")
    row.value = W.EditBox(row, 40, false, 8)
    row.value:SetPoint("LEFT", row.label, "RIGHT", 6, 0)
    row.set = W.Button(row, "Set", 34, 18, function()
        local item = row.item
        local view = list.view
        local bot = State.GetBot(UI.SelectedBot())
        if not (item and bot and view) then
            return
        end
        local value
        if item.kind == 0 then
            value = (item.bv ~= 0) and 0 or 1
        else
            value = tonumber(row.value:GetText() or "")
            if not value then
                UI.Notify("Enter a number for " .. (item.n ~= "" and item.n or item.k) .. ".", true)
                return
            end
        end
        Protocol.SetFightOption(bot.g, view.shownId, item.k, value, UI.RequestCallback(string.format("%s of %s: %s.",
            item.n ~= "" and item.n or item.k, bot.n or "the bot", OptionText(item.kind, value))))
    end)
    row.set:SetPoint("LEFT", row.value, "RIGHT", 2, 0)
    row.reset = W.Button(row, "Default", 52, 18, function()
        local item = row.item
        local view = list.view
        local bot = State.GetBot(UI.SelectedBot())
        if item and bot and view then
            Protocol.SetFightOption(bot.g, view.shownId, item.k, nil, UI.RequestCallback(string.format("%s of %s: the profile's value.",
                item.n ~= "" and item.n or item.k, bot.n or "the bot")))
        end
    end)
    row.reset:SetPoint("LEFT", row.set, "RIGHT", 2, 0)
    return row
end

local function UpdateFightOptionRow(row, item)
    row.item = item
    local connected = State.IsConnected()
    local label = (item.n and item.n ~= "") and item.n or item.k
    row.label:SetText((item.own and "|cffffd100" or "|cffffffff") .. label .. "|r")
    row.set:ClearAllPoints()
    if item.kind == 0 then
        row.value:Hide()
        row.set:SetText(item.bv ~= 0 and "On" or "Off")
        row.set:SetWidth(76)
        row.set:SetPoint("LEFT", row.label, "RIGHT", 6, 0)
    else
        row.value:Show()
        W.SetEditText(row.value, tostring(item.bv))
        row.set:SetText("Set")
        row.set:SetWidth(34)
        row.set:SetPoint("LEFT", row.value, "RIGHT", 2, 0)
    end
    W.SetEnabled(row.set, connected)
    W.SetEnabled(row.reset, connected and item.own)
    W.SetTooltip(row, label, {
        "Key $" .. item.k .. " (" .. (FIGHT_OPTION_KIND[item.kind] or "?") .. ")",
        "The profile's value: " .. OptionText(item.kind, item.v),
        item.own and ("This bot's own value: " .. OptionText(item.kind, item.bv)) or "This bot uses the profile's value.",
    })
end

local function SlotDropDown(f, label, slot, anchor)
    local text = W.Label(f, label, "GameFontNormalSmall")
    if anchor then
        text:SetPoint("LEFT", anchor, "RIGHT", -4, 2)
    else
        text:SetPoint("TOPLEFT", f, "TOPLEFT", 6, -42)
    end
    local dd = W.DropDown(f, 170, function()
        local bot = State.GetBot(UI.SelectedBot())
        local list = bot and State.FightOf(bot.g)
        return SlotChoices(list, list and list.meta and list.meta.tab, bot and bot.class, slot, bot)
    end)
    dd:SetPoint("LEFT", text, "RIGHT", -6, -2)
    dd.onSelect = function(value)
        local bot = State.GetBot(UI.SelectedBot())
        if not bot then
            return
        end
        local list = State.FightOf(bot.g)
        Protocol.AssignFight(bot.g, slot, value, UI.RequestCallback(string.format("%s, %s: %s.", bot.n or "The bot",
            slot == 1 and "group" or "solo", ProfileName(list, value, slot, bot)), function(ok)
            if ok then
                f.shownId = nil
                f.askedProfile = nil
            end
        end))
    end
    return dd
end

-- The other bots of a bot's class, for "Copy to bots".
local function OthersOfClass(bot, only)
    return State.SortedBots("name", false, function(other)
        return other.class == bot.class and other.g ~= bot.g and (only == nil or only == "all" or other.g == only)
    end)
end

function UI.CreateBotFight(parent)
    local f = CreateFrame("Frame", nil, parent)
    f:SetAllPoints(parent)
    f.listKind = 0

    -- (Texts of two lines: a fixed width makes a font string wrap.)
    f.status = W.Label(f, "", "GameFontHighlightSmall")
    f.status:SetPoint("TOPLEFT", f, "TOPLEFT", 6, -6)
    f.status:SetWidth(720)
    f.status:SetHeight(26)
    f.status:SetJustifyV("TOP")

    f.soloDD = SlotDropDown(f, "Solo", 0)
    f.groupDD = SlotDropDown(f, "Group", 1, f.soloDD)

    f.copyTo = W.DropDown(f, 120, function()
        local bot = State.GetBot(UI.SelectedBot())
        if not bot then
            return {}
        end
        local others = OthersOfClass(bot)
        local items = { { text = "Copy to bots...", isTitle = true } }
        if #others > 1 then
            items[#items + 1] = { text = "All of its class (" .. #others .. ")", value = "all" }
        end
        for _, other in ipairs(others) do
            items[#items + 1] = { text = (other.n or "?") .. " (" .. tostring(other.lvl or "?") .. ")", value = other.g }
        end
        if #others == 0 then
            items[#items + 1] = { text = "No other bot of its class", disabled = true }
        end
        return items
    end)
    f.copyTo:SetPoint("LEFT", f.groupDD, "RIGHT", -12, 0)
    UIDropDownMenu_SetText(f.copyTo, "Copy to bots...")
    f.copyTo.onSelect = function(value)
        UIDropDownMenu_SetText(f.copyTo, "Copy to bots...")
        local bot = State.GetBot(UI.SelectedBot())
        if not bot then
            return
        end
        local targets, names = {}, {}
        for _, other in ipairs(OthersOfClass(bot, value)) do
            targets[#targets + 1] = other.g
            names[#names + 1] = other.n or "?"
        end
        if #targets == 0 then
            return
        end
        local list = State.FightOf(bot.g)
        UI.Confirm(string.format("Give %s the fight profiles of %s?\nSolo: %s, group: %s, with %s's own option values.",
            table.concat(names, ", "), bot.n or "the bot", ProfileName(list, bot.fp or 0, 0, bot),
            ProfileName(list, bot.fpg or 0, 1, bot),
            bot.n or "the bot"), function()
            Protocol.CopyFightTo(bot.g, targets, UI.RequestCallback("Fight profiles copied to " .. table.concat(names, ", ") .. "."))
        end)
    end

    f.showLabel = W.Label(f, "Show", "GameFontNormalSmall")
    f.showLabel:SetPoint("TOPLEFT", f, "TOPLEFT", 6, -72)
    f.showDD = W.DropDown(f, 180, function()
        local bot = State.GetBot(UI.SelectedBot())
        local list = bot and State.FightOf(bot.g)
        local items = { { text = "Template (mod-playerbots)", value = 0 } }
        for _, profile in ipairs(list and list.list or {}) do
            items[#items + 1] = { text = profile.n .. " (" .. (TreeName(bot.class, profile.tab) or "?") .. ")", value = profile.id }
        end
        return items
    end)
    f.showDD:SetPoint("LEFT", f.showLabel, "RIGHT", -6, -2)
    f.showDD.onSelect = function(value)
        f.shownId = value
        f.askedProfile = nil
        UI.RequestRefresh()
    end
    f.fightBtn = W.Button(f, "Fight", 60, 20, function()
        f.listKind = 0
        UI.RequestRefresh()
    end)
    f.fightBtn:SetPoint("LEFT", f.showDD, "RIGHT", -8, 2)
    f.buffBtn = W.Button(f, "Between fights", 100, 20, function()
        f.listKind = 1
        UI.RequestRefresh()
    end)
    f.buffBtn:SetPoint("LEFT", f.fightBtn, "RIGHT", 2, 0)
    f.copyName = W.EditBox(f, 110, false, 40)
    f.copyName:SetPoint("LEFT", f.buffBtn, "RIGHT", 14, 0)
    f.copy = W.Button(f, "Copy as new profile", 130, 20, function()
        local bot = State.GetBot(UI.SelectedBot())
        if not bot then
            return
        end
        local name = string.gsub(f.copyName:GetText() or "", "^%s*(.-)%s*$", "%1")
        Protocol.CopyFightProfile(bot.g, name, UI.RequestCallback("A new fight profile from the template of " .. (bot.n or "the bot") .. ".",
            function(ok)
                if ok then
                    W.SetEditText(f.copyName, "")
                end
            end))
    end)
    f.copy:SetPoint("LEFT", f.copyName, "RIGHT", 4, 0)
    W.SetTooltip(f.copy, "Copy as new profile", { "A fight profile of your guild from this bot's template (its class and talent tree):",
        "the rules of mod-playerbots' rotation as a list you can change. The name is optional." })
    f.edit = W.Button(f, "Edit...", 70, 20, function()
        local bot = State.GetBot(UI.SelectedBot())
        if bot then
            UI.OpenFightEditor(bot.g, f.shownId or 0)
        end
    end)
    f.edit:SetPoint("LEFT", f.copy, "RIGHT", 6, 0)
    W.SetTooltip(f.edit, "Fight profile editor", { "Opens the editor on the profile shown here: change the order of the rules,",
        "add spells and items, conditions and options, and save it (the template stays read-only)." })

    f.info = W.Label(f, "", "GameFontDisableSmall")
    f.info:SetPoint("TOPLEFT", f, "TOPLEFT", 6, -98)
    f.info:SetWidth(720)
    f.info:SetHeight(24)
    f.info:SetJustifyV("TOP")

    local panel = W.Panel(f)
    panel:SetPoint("TOPLEFT", f, "TOPLEFT", 0, -124)
    panel:SetPoint("BOTTOMRIGHT", f, "BOTTOMRIGHT", -248, 0)
    f.list = W.ScrollList(panel, { rowHeight = 20, createRow = CreateFightRow, updateRow = UpdateFightRow })
    f.list:SetPoint("TOPLEFT", panel, "TOPLEFT", 6, -6)
    f.list:SetPoint("BOTTOMRIGHT", panel, "BOTTOMRIGHT", -6, 6)
    f.empty = W.Label(panel, "", "GameFontDisableSmall")
    f.empty:SetPoint("TOPLEFT", panel, "TOPLEFT", 14, -12)
    f.empty:SetPoint("RIGHT", panel, "RIGHT", -14, 0)
    f.empty:SetJustifyH("LEFT")

    local options = W.Panel(f)
    options:SetPoint("TOPLEFT", panel, "TOPRIGHT", 4, 0)
    options:SetPoint("BOTTOMRIGHT", f, "BOTTOMRIGHT", 0, 0)
    f.optionsTitle = W.Label(options, "Options for this bot", "GameFontNormalSmall")
    f.optionsTitle:SetPoint("TOPLEFT", options, "TOPLEFT", 8, -8)
    f.options = W.ScrollList(options, { rowHeight = 22, createRow = CreateFightOptionRow, updateRow = UpdateFightOptionRow })
    f.options.view = f
    f.options:SetPoint("TOPLEFT", options, "TOPLEFT", 4, -26)
    f.options:SetPoint("BOTTOMRIGHT", options, "BOTTOMRIGHT", -4, 4)
    f.optionsEmpty = W.Label(options, "", "GameFontDisableSmall")
    f.optionsEmpty:SetPoint("TOPLEFT", options, "TOPLEFT", 8, -30)
    f.optionsEmpty:SetWidth(226)
    f.optionsEmpty:SetHeight(80)
    f.optionsEmpty:SetJustifyH("LEFT")
    f.optionsEmpty:SetJustifyV("TOP")
    return f
end

function UI.RefreshBotFight(f, bot)
    local connected = State.IsConnected()
    if f.bot ~= bot.g then
        f.bot = bot.g
        f.shownId, f.askedList, f.askedProfile = nil, nil, nil
        f.listKind = 0
    end
    -- The profiles of its class come on request, once per look at this bot; after a change the server sends them again.
    if connected and f.askedList ~= bot.g then
        f.askedList = bot.g
        Protocol.GetFightProfiles(bot.g)
        -- The catalogue holds the triggers' texts the lists show ("3 or more enemies ..." for "medium aoe").
        if not State.fight.trig[bot.g] then
            Protocol.GetFightCatalog(bot.g)
        end
    end
    local list, profiles, options = State.FightOf(bot.g)
    local tab = list and list.meta and list.meta.tab
    if f.shownId == nil then
        local playing = (bot.fpsl == 1) and bot.fpg or bot.fp
        f.shownId = ((bot.fpid or 0) > 0 and bot.fpid) or ((bot.fpn and bot.fpn ~= "") and playing) or bot.fp or 0
    end
    local shownKey = tostring(bot.g) .. ":" .. tostring(f.shownId)
    if connected and f.askedProfile ~= shownKey then
        f.askedProfile = shownKey
        Protocol.GetFightProfile(bot.g, f.shownId)
    end

    local class = Data.CLASSES[bot.class or 0]
    local tree = TreeName(bot.class, tab)
    local status = string.format("%s (%s%s, level %d) ", bot.n or "The bot", tree and (tree .. " ") or "",
        class and class.name or "?", bot.lvl or 1)
    if bot.fpn and bot.fpn ~= "" then
        if bot.fpd then
            status = status .. "fights by |cffffd100" .. bot.fpn .. "|r (by default: the template of its spec)."
        else
            status = status .. "fights by |cffffd100" .. bot.fpn .. "|r (its " .. ((bot.fpsl == 1) and "group" or "solo") .. " profile)."
        end
    else
        status = status .. "fights by mod-playerbots' default rotation of its talent tree."
    end
    if bot.fpw and bot.fpw ~= "" then
        status = status .. "\n|cffff8040" .. bot.fpw .. "|r"
    elseif (bot.fp or 0) == 0 and (bot.fpg or 0) == 0 then
        status = status .. "\nGive it a profile of your guild for solo play or for dungeons and raids, or copy its template as a new one."
    end
    f.status:SetText(status)

    f.soloDD:SetSelected(bot.fp or 0, ProfileName(list, bot.fp or 0, 0, bot))
    f.groupDD:SetSelected(bot.fpg or 0, ProfileName(list, bot.fpg or 0, 1, bot))
    f.soloDD:SetEnabled(connected and list ~= nil)
    f.groupDD:SetEnabled(connected and list ~= nil)
    f.copyTo:SetEnabled(connected)
    f.showDD:SetSelected(f.shownId, f.shownId == 0 and "Template (mod-playerbots)" or ProfileName(list, f.shownId))
    f.showDD:SetEnabled(connected)
    -- The list shown has its button lit.
    for kind, button in pairs({ [0] = f.fightBtn, [1] = f.buffBtn }) do
        if f.listKind == kind then
            button:LockHighlight()
        else
            button:UnlockHighlight()
        end
    end
    local count = list and #list.list or 0
    W.SetEnabled(f.copy, connected and bot.s ~= "benched" and count < ((list and list.meta.max) or 20))
    W.SetEnabled(f.edit, connected)

    local shown = profiles[f.shownId]
    local rows, rules = UI.FightRows(shown, f.listKind)
    f.list:SetItems(rows)
    local meta = shown and shown.meta
    if not shown then
        f.info:SetText(connected and "Asking the server ..." or "Not connected.")
        f.empty:SetText("")
    elseif f.shownId == 0 then
        if not meta.live then
            f.info:SetText("The template is read from the bot itself: it shows while the bot is in the world.")
        else
            f.info:SetText(string.format("Template of %s %s, read-only: mod-playerbots' strategies %s. %d of their rows stay as they"
                .. " are in every profile (movement, targets, what the guild's own rules do).", tree or "its", class and class.name or "",
                (meta.rep and meta.rep ~= "") and meta.rep or "?", meta.hid or 0))
        end
        f.empty:SetText(rules == 0 and (f.listKind == 1 and "No buffs between fights." or "No rules.") or "")
    else
        local note = (meta.note and meta.note ~= "") and (" " .. meta.note) or ""
        local used = 0
        for _, profile in ipairs(list and list.list or {}) do
            if profile.id == f.shownId then
                used = profile.used or 0
            end
        end
        f.info:SetText(string.format("Profile \"%s\" of your guild for %s %s (revision %d, used by %d bot%s), read-only here.%s",
            meta.pn or "?", TreeName(bot.class, meta.tab) or "?", class and class.name or "", meta.rev or 0, used,
            used == 1 and "" or "s", note))
        f.empty:SetText(rules == 0 and (f.listKind == 1 and "No rules between fights: the bot keeps only what stays of mod-playerbots."
            or "No rules in the fight: the bot keeps only what stays of mod-playerbots.") or "")
    end
    if rules == 0 then
        f.empty:Show()
    else
        f.empty:Hide()
    end

    local opts = options[f.shownId] or {}
    f.options:SetItems(f.shownId ~= 0 and opts or {})
    if f.shownId == 0 then
        f.optionsEmpty:SetText("The template has no options. A profile of your guild can have named values its rules ask for"
            .. " ($key), and each bot can have its own.")
        f.optionsEmpty:Show()
    elseif #opts == 0 then
        f.optionsEmpty:SetText("This profile has no options.")
        f.optionsEmpty:Show()
    else
        f.optionsEmpty:Hide()
    end
end

GA.On("FIGHT", function()
    UI.RequestRefresh()
end)
-------------------------------------------------------------------------------
-- Fight profile editor (docs/fight-profiles.md §4.8, package FP-3): its own window. Left the profiles of the bot's
-- class (the template locked, the guild's profiles); in the middle the two lists, the fight and between fights, in the
-- engine's order with the band dividers (FC8), with Up / Down / Remove / On-Off and Add spell / Add item; right the
-- selected rule's settings and conditions, or the profile's options; at the bottom Save / Discard / Assign and the
-- server's message. The editor works on a copy and saves the whole profile (FIGHT_SAVE); the server checks it.
-------------------------------------------------------------------------------

local Editor = { g = nil, id = 0, draft = nil, dirty = false, list = 0, sel = nil, view = "rule" }
UI.FightEditor = Editor
local ed -- the window

local MAX_RULES, MAX_CONDS, MAX_OPTIONS = 60, 6, 12
local COND_OPS = { "<", "<=", "=", ">=", ">" }
local COND_GROUPS = { "me", "target", "around", "pet", "group", "other" }
local COND_GROUP_TEXT = { me = "Me", target = "My target", around = "Around me", pet = "My pet", group = "My group",
    other = "Other" }
local PARAM_HINT = { [1] = "spell (name or id)", [2] = "item id", [3] = "yards", [4] = "number", [5] = "trigger name",
    [6] = "option key" }
local GUILD_RULES_LINES = { "These run above every profile, so a profile does not list them:",
    "the wand, potions, bandages, elixirs, scrolls, food and drink,", "healing itself in a pinch, careful pulls,",
    "leaving a lost fight, stepping back from casters,", "Consecration against one foe, a shaman's totems while it levels,",
    "a mage's armor, movement and the choice of targets." }

-- Flags of a rule: 1 once per target, 2 not twice in a row (no bit library needed).
local function HasFlag(flags, bit)
    return math.floor((flags or 0) / bit) % 2 == 1
end

local function WithFlag(flags, bit, on)
    flags = flags or 0
    if HasFlag(flags, bit) ~= on then
        flags = flags + (on and bit or -bit)
    end
    return flags
end

local function SplitConds(text)
    local list = {}
    for part in string.gmatch(text or "", "[^;]+") do
        list[#list + 1] = part
    end
    return list
end

-- One condition's text "[!]key[:param][op value]" in its parts.
function UI.FightParseCondition(text)
    local neg, rest = string.match(text or "", "^(!?)(.*)$")
    local key, tail = string.match(rest, "^([%w_]+)(.*)$")
    local param, opv = "", tail or ""
    if string.sub(opv, 1, 1) == ":" then
        param, opv = string.match(opv, "^:([^<>=]*)(.*)$")
    end
    local op, value = string.match(opv or "", "^([<>=]+)(.*)$")
    return { neg = neg == "!", key = key or rest, param = param or "", op = op, value = value }
end

local function SpellNameOf(id)
    id = tonumber(id)
    if not id then
        return nil
    end
    for _, spell in ipairs(State.fight.spells[Editor.g] or {}) do
        if spell.id == id or spell.hi == id then
            return spell.n
        end
    end
    return (GetSpellInfo(id))
end

function UI.FightDescribeCondition(text)
    local c = UI.FightParseCondition(text)
    -- A trigger of mod-playerbots as a condition reads as what it checks.
    local triggerText = c.key == "trig" and c.param ~= "" and UI.FightTriggerText(Editor.g, c.param)
    if triggerText then
        return (c.neg and "not: " or "") .. triggerText
    end
    local type = State.fight.condByKey[c.key]
    local out = (c.neg and "not: " or "") .. (type and type.n or c.key)
    if c.param ~= "" then
        out = out .. " " .. ((type and type.pk == 1 and SpellNameOf(c.param)) or c.param)
    end
    if c.op then
        out = out .. " " .. c.op .. " " .. (c.value or "")
    end
    return out
end

local function CopyRule(rule)
    local copy = {}
    for k, v in pairs(rule) do
        if k ~= "index" and k ~= "_i" then
            copy[k] = v
        end
    end
    copy.rel = tonumber(copy.rel) or 0
    copy.conds = SplitConds(rule.c)
    -- A row of mod-playerbots names its spell for the icon only; the server stores none.
    if (copy.k or 0) <= 1 then
        copy.icon = copy.ref
        copy.ref = 0
    end
    return copy
end

-- The editor's copy of a profile: { n, note, rules = { [0] = {...}, [1] = {...} }, options = {...} }.
local function LoadDraft()
    local _, profiles, options = State.FightOf(Editor.g)
    local shown = profiles[Editor.id]
    Editor.sel, Editor.dirty = nil, false
    if not shown then
        Editor.draft, Editor.loadedRev = nil, nil
        return
    end
    local draft = { n = shown.meta.pn or "", note = shown.meta.note or "", rules = { [0] = {}, [1] = {} }, options = {} }
    for _, rule in ipairs(shown.rules) do
        local copy = CopyRule(rule)
        local list = draft.rules[copy.l or 0]
        list[#list + 1] = copy
    end
    for _, option in ipairs(options[Editor.id] or {}) do
        draft.options[#draft.options + 1] = { k = option.k, n = option.n, kind = option.kind, v = option.v, min = option.min,
            max = option.max }
    end
    Editor.draft = draft
    Editor.loadedRev, Editor.loadedMeta, Editor.loadedOptions = shown.meta.rev, shown.meta, options[Editor.id]
end

-- The template of mod-playerbots (id 0) and the templates imported from wowbot335 (FP-4) cannot be changed: a copy can.
local function ReadOnly()
    if (Editor.id or 0) == 0 then
        return true
    end
    local list = State.FightOf(Editor.g)
    for _, profile in ipairs(list and list.list or {}) do
        if profile.id == Editor.id then
            return profile.tpl and true or false
        end
    end
    return false
end
UI.FightEditorReadOnly = ReadOnly

local function Dirty()
    if not ReadOnly() then
        Editor.dirty = true
    end
    UI.RefreshFightEditor()
end

local function Editable()
    return not ReadOnly() and Editor.draft ~= nil and State.IsConnected()
end

-- A list in the engine's order: highest relevance first, the array's order among equals.
local function Sorted(list)
    local order = {}
    for i, rule in ipairs(Editor.draft and Editor.draft.rules[list] or {}) do
        rule._i = i
        order[#order + 1] = rule
    end
    table.sort(order, function(a, b)
        if a.rel == b.rel then
            return a._i < b._i
        end
        return a.rel > b.rel
    end)
    return order
end

local function IndexOf(list, item)
    for i, v in ipairs(list) do
        if v == item then
            return i
        end
    end
end

-- Up (dir -1) or down (dir 1) in the shown order: past an equal relevance by the array's order, inside a band by
-- swapping the relevance, over a divider into the next band (it takes the other rule's relevance).
function UI.FightMoveRule(rule, dir)
    if not (rule and Editor.draft) then
        return
    end
    local rules = Editor.draft.rules[rule.l or 0]
    local order = Sorted(rule.l or 0)
    local other = order[(IndexOf(order, rule) or 0) + dir]
    if not other then
        return
    end
    if other.rel == rule.rel then
        local a, b = IndexOf(rules, rule), IndexOf(rules, other)
        rules[a], rules[b] = rules[b], rules[a]
    elseif BandOf(other.rel) == BandOf(rule.rel) then
        rule.rel, other.rel = other.rel, rule.rel
    else
        rule.rel = other.rel
        table.remove(rules, IndexOf(rules, rule))
        local at = IndexOf(rules, other)
        table.insert(rules, dir < 0 and at + 1 or at, rule)
    end
    Editor.revealSel = true
    Dirty()
end

local function RuleCount()
    return #Editor.draft.rules[0] + #Editor.draft.rules[1]
end

-- A new rule of the shown list, right under the selected one (its relevance), else at the top of the normal band.
function UI.FightAddRule(kind, ref)
    if not Editable() then
        return
    end
    if RuleCount() >= MAX_RULES then
        UI.Notify("A profile has at most " .. MAX_RULES .. " rules.", true)
        return
    end
    local list = Editor.list
    local rules = Editor.draft.rules[list]
    local sel = Editor.sel and Editor.sel.l == list and Editor.sel or nil
    local rule = { l = list, k = kind, ref = ref, a = "", tg = "", tgt = list == 1 and 1 or 0, rel = sel and sel.rel or 19,
        en = true, fl = 0, iv = 0, nd = 0, src = "", conds = {} }
    if sel then
        table.insert(rules, IndexOf(rules, sel) + 1, rule)
    else
        table.insert(rules, 1, rule)
    end
    Editor.sel, Editor.revealSel = rule, true
    Dirty()
end

-- What FIGHT_SAVE sends: list 0 first, then list 1, each in its array order.
local function SaveDraft()
    local draft = Editor.draft
    local out = { n = draft.n, note = draft.note, rules = {}, options = draft.options }
    for list = 0, 1 do
        for _, rule in ipairs(draft.rules[list]) do
            local copy = {}
            for k, v in pairs(rule) do
                copy[k] = v
            end
            copy.c = table.concat(rule.conds or {}, ";")
            out.rules[#out.rules + 1] = copy
        end
    end
    return out
end
Editor.SaveDraft = SaveDraft

-- The rule an error of the server names ("rule 3: ..." counts list 0 first, then list 1).
local function RuleOfError(msg)
    local n = tonumber(string.match(msg or "", "rule (%d+):"))
    if not (n and Editor.draft) then
        return nil
    end
    local first = #Editor.draft.rules[0]
    if n < first then
        return Editor.draft.rules[0][n + 1]
    end
    return Editor.draft.rules[1][n - first + 1]
end

function UI.FightSaveProfile()
    if not (Editable() and Editor.dirty) then
        return
    end
    local name = string.gsub(ed.name:GetText() or "", "^%s*(.-)%s*$", "%1")
    local note = string.gsub(ed.note:GetText() or "", "^%s*(.-)%s*$", "%1")
    Editor.draft.n, Editor.draft.note = name, note
    Editor.message, Editor.messageError = "Saving ...", false
    Protocol.SaveFightProfile(Editor.g, Editor.id, SaveDraft(), function(ok, code, msg)
        if ok then
            Editor.dirty = false
            LoadDraft()
            Editor.message, Editor.messageError = "Saved. The bots on it take it at their next quiet moment.", false
        else
            Editor.message, Editor.messageError = GA.Protocol.ErrorText(code, msg), true
            local rule = RuleOfError(msg)
            if rule then
                Editor.list, Editor.sel, Editor.view, Editor.revealSel = rule.l or 0, rule, "rule", true
            end
        end
        UI.RefreshFightEditor()
    end)
    UI.RefreshFightEditor()
end

local function SelectProfile(id)
    local function go()
        Editor.id = id
        Editor.message = nil
        Editor.asked = nil
        LoadDraft()
        UI.RefreshFightEditor()
    end
    if Editor.dirty and id ~= Editor.id then
        UI.Confirm("Discard the changes to this profile?", go)
    else
        go()
    end
end

-------------------------------------------------------------------------------
-- Rows
-------------------------------------------------------------------------------

local function CreateProfileRow(list)
    local row = CreateFrame("Button", nil, list)
    row.shade = row:CreateTexture(nil, "BACKGROUND")
    row.shade:SetAllPoints(row)
    row.shade:SetTexture("Interface\\Buttons\\WHITE8X8")
    row.name = W.Label(row, "", "GameFontHighlightSmall")
    row.name:SetPoint("TOPLEFT", row, "TOPLEFT", 4, -3)
    row.name:SetPoint("RIGHT", row, "RIGHT", -4, 0)
    row.detail = W.Label(row, "", "GameFontDisableSmall")
    row.detail:SetPoint("TOPLEFT", row.name, "BOTTOMLEFT", 0, -1)
    row:SetScript("OnClick", function(self)
        if self.item then
            SelectProfile(self.item.id)
        end
    end)
    return row
end

local function UpdateProfileRow(row, item)
    row.item = item
    local active = item.id == Editor.id
    row.shade:SetVertexColor(1, 0.82, 0, active and 0.2 or 0.03)
    row.name:SetText((active and "|cffffd100" or "|cffffffff") .. ((item.id == 0 or item.tpl) and "|TInterface\\LFGFrame\\UI-LFG-ICON-LOCK:0|t " or "")
        .. item.n .. "|r")
    row.detail:SetText(item.detail or "")
end

local function CreateRuleRow(list)
    local row = CreateFrame("Button", nil, list)
    row.shade = row:CreateTexture(nil, "BACKGROUND")
    row.shade:SetAllPoints(row)
    row.shade:SetTexture("Interface\\Buttons\\WHITE8X8")
    row.icon = row:CreateTexture(nil, "ARTWORK")
    row.icon:SetWidth(16)
    row.icon:SetHeight(16)
    row.icon:SetPoint("LEFT", row, "LEFT", 4, 0)
    row.name = W.Label(row, "", "GameFontHighlightSmall")
    row.name:SetPoint("LEFT", row.icon, "RIGHT", 4, 0)
    row.name:SetWidth(170)
    row.name:SetHeight(12) -- one line: a long name ends in "...", the tooltip has it whole
    row.cond = W.Label(row, "", "GameFontDisableSmall")
    row.cond:SetPoint("LEFT", row.name, "RIGHT", 6, 0)
    row.cond:SetHeight(12)
    row.rel = W.Label(row, "", "GameFontDisableSmall", "RIGHT")
    row.rel:SetPoint("RIGHT", row, "RIGHT", -4, 0)
    row.rel:SetWidth(34)
    row.cond:SetPoint("RIGHT", row.rel, "LEFT", -4, 0)
    row.divider = W.Label(row, "", "GameFontNormalSmall")
    row.divider:SetPoint("LEFT", row, "LEFT", 6, 0)
    row:SetScript("OnClick", function(self)
        if self.item and not self.item.divider then
            Editor.sel = self.item
            Editor.view = "rule"
            UI.RefreshFightEditor()
        end
    end)
    row:SetScript("OnEnter", function(self)
        local item = self.item
        if not item or item.divider then
            return
        end
        GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
        local conds = {}
        for _, cond in ipairs(item.conds or {}) do
            conds[#conds + 1] = UI.FightDescribeCondition(cond)
        end
        AddRuleTooltip(item, Editor.g, table.concat(conds, "; "))
        GameTooltip:AddLine(string.format("Relevance %.1f (%s)", item.rel or 0, BandOf(item.rel).text), 0.6, 0.6, 0.6)
        if item.en == false then
            GameTooltip:AddLine("Switched off", 1, 0.3, 0.3)
        end
        GameTooltip:Show()
    end)
    row:SetScript("OnLeave", function()
        GameTooltip:Hide()
    end)
    return row
end

local function UpdateRuleRow(row, item, index)
    row.item = item
    if item.divider then
        row.shade:SetVertexColor(1, 0.82, 0, 0.10)
        row.divider:SetText("- " .. item.divider .. " -")
        row.divider:Show()
        row.icon:Hide()
        row.name:SetText("")
        row.cond:SetText("")
        row.rel:SetText("")
        return
    end
    row.divider:Hide()
    local selected = item == Editor.sel
    row.shade:SetVertexColor(1, 0.82, 0, selected and 0.22 or (index % 2 == 0 and 0.04 or 0))
    local name, icon = UI.FightRuleName({ k = item.k, ref = (item.k or 0) <= 1 and item.icon or item.ref, a = item.a })
    row.icon:SetTexture(icon or "Interface\\Icons\\INV_Misc_QuestionMark")
    row.icon:SetDesaturated(item.kn == false)
    row.icon:Show()
    row.name:SetText(((item.en == false or item.kn == false) and "|cff808080" or "|cffffffff") .. name .. "|r")
    local conds = {}
    for _, cond in ipairs(item.conds or {}) do
        conds[#conds + 1] = UI.FightDescribeCondition(cond)
    end
    row.cond:SetText(UI.FightWhenText(Editor.g, item.tg, table.concat(conds, "; ")))
    row.rel:SetText(string.format("%.1f", item.rel or 0))
end

-- The conditions list of a rule: first its trigger of mod-playerbots (a row brings one from its strategy), then the
-- conditions of its own. X takes either away.
local function CreateCondRow(list)
    local row = CreateFrame("Frame", nil, list)
    row.text = W.Label(row, "", "GameFontHighlightSmall")
    row.text:SetPoint("LEFT", row, "LEFT", 4, 0)
    row.text:SetPoint("RIGHT", row, "RIGHT", -28, 0)
    row.text:SetHeight(12)
    row.remove = W.Button(row, "X", 22, 16, function()
        local rule = Editor.sel
        if not (rule and Editable()) then
            return
        end
        if row.trigger then
            rule.tg = ""
            Dirty()
        elseif row.index then
            table.remove(rule.conds, row.index)
            Dirty()
        end
    end)
    row.remove:SetPoint("RIGHT", row, "RIGHT", -2, 0)
    row:EnableMouse(true)
    row:SetScript("OnEnter", function(self)
        if not self.full then
            return
        end
        GameTooltip:SetOwner(self, "ANCHOR_LEFT")
        GameTooltip:AddLine(self.full, 1, 1, 1, true)
        if self.trigger then
            GameTooltip:AddLine("mod-playerbots' check \"" .. self.trigger .. "\", which this row brings from its strategy."
                .. " The rule fires only while it holds (and every condition below).", 0.8, 0.8, 0.8, true)
            GameTooltip:AddLine("X takes it away: then only the conditions below decide, and without any the rule is"
                .. " tried whenever nothing more relevant is due.", 0.6, 0.6, 0.6, true)
        end
        GameTooltip:Show()
    end)
    row:SetScript("OnLeave", function()
        GameTooltip:Hide()
    end)
    return row
end

local function UpdateCondRow(row, item, index)
    if type(item) == "table" and item.trigger then
        row.trigger, row.index = item.trigger, nil
        local text = UI.FightTriggerText(Editor.g, item.trigger) or ("check \"" .. item.trigger .. "\"")
        row.full = "mod-playerbots: " .. text
        row.text:SetText("|cff80c8ffmod-playerbots:|r " .. text)
    else
        local offset = Editor.sel and Editor.sel.tg and Editor.sel.tg ~= "" and 1 or 0
        row.trigger, row.index = nil, index - offset
        row.full = UI.FightDescribeCondition(item)
        row.text:SetText(row.full)
    end
    W.SetEnabled(row.remove, Editable())
end

local function OptionKindText(kind)
    return ({ [0] = "switch", [1] = "number", [2] = "percent" })[kind or 1] or "?"
end

local function CreateOptionRow(list)
    local row = CreateFrame("Button", nil, list)
    row.text = W.Label(row, "", "GameFontHighlightSmall")
    row.text:SetPoint("LEFT", row, "LEFT", 4, 0)
    row.text:SetPoint("RIGHT", row, "RIGHT", -28, 0)
    row.text:SetHeight(12)
    row.text:SetJustifyH("LEFT")
    row.remove = W.Button(row, "X", 22, 16, function()
        if row.index and Editable() then
            table.remove(Editor.draft.options, row.index)
            Dirty()
        end
    end)
    row.remove:SetPoint("RIGHT", row, "RIGHT", -2, 0)
    row:SetScript("OnClick", function(self)
        local option = self.item
        if option then
            W.SetEditText(ed.optKey, option.k)
            W.SetEditText(ed.optLabel, option.n)
            ed.optKindDD:SetSelected(option.kind, OptionKindText(option.kind))
            ed.optKind = option.kind
            W.SetEditText(ed.optValue, tostring(option.v))
            W.SetEditText(ed.optMin, tostring(option.min))
            W.SetEditText(ed.optMax, tostring(option.max))
        end
    end)
    return row
end

local function UpdateOptionRow(row, item, index)
    row.item, row.index = item, index
    row.text:SetText(string.format("|cffffd100$%s|r %s (%s) %s [%s-%s]", item.k, item.n or "", OptionKindText(item.kind),
        tostring(item.v), tostring(item.min), tostring(item.max)))
    W.SetEnabled(row.remove, Editable())
end

-------------------------------------------------------------------------------
-- The window
-------------------------------------------------------------------------------

-- The spells a bot knows by the tabs of its spellbook ("General" first, then its class's three in their order), each in
-- alphabetical order; a tab with more than a menu holds comes in parts.
local SPELLS_PER_MENU = 25

function UI.FightSpellTabs(g)
    local byTab, names = {}, {}
    for _, spell in ipairs(State.fight.spells[g] or {}) do
        local tab = (spell.tab and spell.tab ~= "") and spell.tab or "General"
        if not byTab[tab] then
            byTab[tab] = {}
            names[#names + 1] = tab
        end
        table.insert(byTab[tab], spell)
    end
    table.sort(names, function(a, b)
        if (a == "General") ~= (b == "General") then
            return a == "General"
        end
        return a < b
    end)
    local menus = {}
    for _, tab in ipairs(names) do
        local list = byTab[tab]
        table.sort(list, function(a, b) return (a.n or "") < (b.n or "") end)
        local parts = math.ceil(#list / SPELLS_PER_MENU)
        for part = 1, parts do
            local chunk = {}
            for i = (part - 1) * SPELLS_PER_MENU + 1, math.min(part * SPELLS_PER_MENU, #list) do
                chunk[#chunk + 1] = list[i]
            end
            menus[#menus + 1] = { text = tab .. (parts > 1 and string.format(" (%d/%d)", part, parts) or ""), spells = chunk }
        end
    end
    return menus
end

local function SpellMenu(dd, level, value)
    local menus = UI.FightSpellTabs(Editor.g)
    if level == 1 then
        if #menus == 0 then
            return { { text = "No spells known (the bot must be in the world)", disabled = true } }
        end
        local items = {}
        for i, menu in ipairs(menus) do
            items[#items + 1] = { text = menu.text .. " (" .. #menu.spells .. ")", value = i, hasArrow = true }
        end
        return items
    end
    local items = {}
    for _, spell in ipairs(menus[value or 1] and menus[value or 1].spells or {}) do
        local name, _, icon = GetSpellInfo(spell.hi or spell.id)
        items[#items + 1] = { text = (icon and ("|T" .. icon .. ":0|t ") or "") .. (name or spell.n), value = spell.id }
    end
    return items
end

local function ItemMenu()
    local items = { { text = "Use trinket 1", value = -1 }, { text = "Use trinket 2", value = -2 } }
    for _, item in ipairs(State.fight.items[Editor.g] or {}) do
        items[#items + 1] = { text = "Use " .. (item.n or tostring(item.e)), value = item.e }
    end
    return items
end

local function CondMenu(dd, level, value)
    local conds = State.fight.conds or {}
    local items = {}
    if level == 1 then
        for _, group in ipairs(COND_GROUPS) do
            items[#items + 1] = { text = COND_GROUP_TEXT[group], value = group, hasArrow = true }
        end
        return items
    end
    for _, cond in ipairs(conds) do
        if cond.grp == value then
            items[#items + 1] = { text = cond.n, value = cond.k }
        end
    end
    return items
end

-- The condition the form describes as text, or nil and why not.
local function ConditionFromForm()
    local type = State.fight.condByKey[ed.condKey or ""]
    if not type then
        return nil, "Pick a condition first."
    end
    local text = (W.IsChecked(ed.condNot) and "!" or "") .. type.k
    if (type.pk or 0) ~= 0 then
        local param = string.gsub(ed.condParam:GetText() or "", "^%s*(.-)%s*$", "%1")
        if param == "" then
            return nil, "This condition needs its " .. (PARAM_HINT[type.pk] or "parameter") .. "."
        end
        if type.pk == 1 and not tonumber(param) then
            local found
            for _, spell in ipairs(State.fight.spells[Editor.g] or {}) do
                if string.lower(spell.n or "") == string.lower(param) then
                    found = spell.id
                end
            end
            if not found then
                return nil, "No spell of the bot is called \"" .. param .. "\": give its id."
            end
            param = tostring(found)
        end
        text = text .. ":" .. param
    end
    if (type.vk or 0) ~= 0 then
        local value = string.gsub(ed.condValue:GetText() or "", "^%s*(.-)%s*$", "%1")
        local key = string.match(value, "^%$([%w_]+)$")
        if key then
            local known
            for _, option in ipairs(Editor.draft.options) do
                if option.k == key then
                    known = true
                end
            end
            if not known then
                return nil, "The profile has no option $" .. key .. "."
            end
        elseif not tonumber(value) then
            return nil, "Give a number or an option ($key)."
        end
        text = text .. (ed.condOp or "<") .. value
    end
    return text
end
Editor.ConditionFromForm = ConditionFromForm

local function CreateEditor()
    ed = CreateFrame("Frame", "GuildAdventureFightEditor", UIParent)
    ed:SetWidth(1000)
    ed:SetHeight(600)
    ed:SetPoint("CENTER", UIParent, "CENTER", 0, 20)
    ed:SetFrameStrata("DIALOG")
    ed:SetToplevel(true)
    ed:SetBackdrop(W.BACKDROP_DIALOG)
    W.DarkenBackground(ed, 0.94) -- opaque: the bot detail behind it must not show through
    ed:EnableMouse(true)
    ed:SetMovable(true)
    ed:RegisterForDrag("LeftButton")
    ed:SetScript("OnDragStart", function(self)
        self:StartMoving()
    end)
    ed:SetScript("OnDragStop", function(self)
        self:StopMovingOrSizing()
    end)
    if UI.main then
        ed:SetScale(UI.main:GetScale() or 1)
    end
    tinsert(UISpecialFrames, "GuildAdventureFightEditor")

    local header = ed:CreateTexture(nil, "ARTWORK")
    header:SetTexture("Interface\\DialogFrame\\UI-DialogBox-Header")
    header:SetWidth(360)
    header:SetHeight(64)
    header:SetPoint("TOP", ed, "TOP", 0, 12)
    ed.title = W.Label(ed, "Fight profile editor", "GameFontNormal", "CENTER")
    ed.title:SetPoint("TOP", header, "TOP", 0, -14)
    local close = CreateFrame("Button", nil, ed, "UIPanelCloseButton")
    close:SetPoint("TOPRIGHT", ed, "TOPRIGHT", -4, -4)
    close:SetScript("OnClick", function()
        if Editor.dirty then
            UI.Confirm("Close the editor and discard the changes?", function()
                Editor.dirty = false
                ed:Hide()
            end)
        else
            ed:Hide()
        end
    end)

    -- Top: name, note, options.
    ed.nameLabel = W.Label(ed, "Name", "GameFontNormalSmall")
    ed.nameLabel:SetPoint("TOPLEFT", ed, "TOPLEFT", 222, -32)
    ed.name = W.EditBox(ed, 170, false, 48)
    ed.name:SetPoint("LEFT", ed.nameLabel, "RIGHT", 10, 0)
    ed.name:SetScript("OnTextChanged", function(self)
        if self.gaFocused and Editor.draft and not ReadOnly() and self:GetText() ~= Editor.draft.n then
            Editor.draft.n = self:GetText()
            Dirty()
        end
    end)
    ed.noteLabel = W.Label(ed, "Note", "GameFontNormalSmall")
    ed.noteLabel:SetPoint("LEFT", ed.name, "RIGHT", 10, 0)
    ed.note = W.EditBox(ed, 350, false, 200)
    ed.note:SetPoint("LEFT", ed.noteLabel, "RIGHT", 10, 0)
    ed.note:SetScript("OnTextChanged", function(self)
        if self.gaFocused and Editor.draft and not ReadOnly() and self:GetText() ~= Editor.draft.note then
            Editor.draft.note = self:GetText()
            Dirty()
        end
    end)
    ed.optionsBtn = W.Button(ed, "Options", 80, 20, function()
        Editor.view = Editor.view == "options" and "rule" or "options"
        UI.RefreshFightEditor()
    end)
    ed.optionsBtn:SetPoint("LEFT", ed.note, "RIGHT", 10, 0)
    ed.banner = W.Label(ed, "", "GameFontNormalSmall")
    ed.banner:SetPoint("TOPLEFT", ed, "TOPLEFT", 222, -54)
    ed.banner:SetWidth(760)
    ed.banner:SetHeight(12)

    -- Left: the profiles of the class.
    local left = W.Panel(ed)
    left:SetPoint("TOPLEFT", ed, "TOPLEFT", 12, -30)
    left:SetPoint("BOTTOMLEFT", ed, "BOTTOMLEFT", 12, 74)
    left:SetWidth(200)
    ed.profiles = W.ScrollList(left, { rowHeight = 30, createRow = CreateProfileRow, updateRow = UpdateProfileRow })
    ed.profiles:SetPoint("TOPLEFT", left, "TOPLEFT", 4, -4)
    ed.profiles:SetPoint("BOTTOMRIGHT", left, "BOTTOMRIGHT", -4, 4)
    ed.newBtn = W.Button(ed, "New copy", 92, 20, function()
        local from = Editor.id ~= 0 and Editor.id or nil
        local before = {}
        local list = State.FightOf(Editor.g)
        for _, profile in ipairs(list and list.list or {}) do
            before[profile.id] = true
        end
        Protocol.CopyFightProfile(Editor.g, "", function(ok, code, msg)
            if not ok then
                Editor.message, Editor.messageError = GA.Protocol.ErrorText(code, msg), true
                UI.RefreshFightEditor()
                return
            end
            local now = State.FightOf(Editor.g)
            for _, profile in ipairs(now and now.list or {}) do
                if not before[profile.id] then
                    Editor.dirty = false
                    SelectProfile(profile.id)
                    Editor.message, Editor.messageError = "Copied as \"" .. profile.n .. "\".", false
                end
            end
            UI.RefreshFightEditor()
        end, from)
    end)
    ed.newBtn:SetPoint("TOPLEFT", left, "BOTTOMLEFT", 0, -4)
    W.SetTooltip(ed.newBtn, "New copy", { "A new profile of your guild: a copy of the profile on the left that is selected",
        "(of the template: mod-playerbots' rotation of the bot's talent tree)." })
    ed.deleteBtn = W.Button(ed, "Delete", 92, 20, function()
        if ReadOnly() then
            return
        end
        local name = Editor.draft and Editor.draft.n or "?"
        UI.Confirm("Delete the fight profile \"" .. name .. "\"?\nBots that use it fight by the default again.", function()
            Protocol.DeleteFightProfile(Editor.g, Editor.id, function(ok, code, msg)
                if ok then
                    Editor.dirty = false
                    SelectProfile(0)
                    Editor.message, Editor.messageError = "\"" .. name .. "\" deleted.", false
                else
                    Editor.message, Editor.messageError = GA.Protocol.ErrorText(code, msg), true
                end
                UI.RefreshFightEditor()
            end)
        end)
    end)
    ed.deleteBtn:SetPoint("LEFT", ed.newBtn, "RIGHT", 8, 0)

    -- Middle: the two lists.
    ed.fightTab = W.Button(ed, "Fight", 70, 20, function()
        Editor.list, Editor.sel = 0, nil
        UI.RefreshFightEditor()
    end)
    ed.fightTab:SetPoint("TOPLEFT", ed, "TOPLEFT", 222, -72)
    ed.buffTab = W.Button(ed, "Between fights", 110, 20, function()
        Editor.list, Editor.sel = 1, nil
        UI.RefreshFightEditor()
    end)
    ed.buffTab:SetPoint("LEFT", ed.fightTab, "RIGHT", 4, 0)
    ed.count = W.Label(ed, "", "GameFontDisableSmall")
    ed.count:SetPoint("LEFT", ed.buffTab, "RIGHT", 8, 0)
    local middle = W.Panel(ed)
    middle:SetPoint("TOPLEFT", ed, "TOPLEFT", 220, -96)
    middle:SetPoint("BOTTOMLEFT", ed, "BOTTOMLEFT", 220, 152)
    middle:SetWidth(460)
    ed.rules = W.ScrollList(middle, { rowHeight = 20, createRow = CreateRuleRow, updateRow = UpdateRuleRow })
    ed.rules:SetPoint("TOPLEFT", middle, "TOPLEFT", 4, -4)
    ed.rules:SetPoint("BOTTOMRIGHT", middle, "BOTTOMRIGHT", -4, 4)
    ed.up = W.Button(ed, "Up", 50, 20, function() UI.FightMoveRule(Editor.sel, -1) end)
    ed.up:SetPoint("TOPLEFT", middle, "BOTTOMLEFT", 0, -4)
    ed.down = W.Button(ed, "Down", 56, 20, function() UI.FightMoveRule(Editor.sel, 1) end)
    ed.down:SetPoint("LEFT", ed.up, "RIGHT", 4, 0)
    ed.toggle = W.Button(ed, "On / off", 70, 20, function()
        if Editor.sel and Editable() then
            Editor.sel.en = Editor.sel.en == false
            Dirty()
        end
    end)
    ed.toggle:SetPoint("LEFT", ed.down, "RIGHT", 4, 0)
    ed.remove = W.Button(ed, "Remove", 70, 20, function()
        local rule = Editor.sel
        if rule and Editable() then
            local rules = Editor.draft.rules[rule.l or 0]
            table.remove(rules, IndexOf(rules, rule))
            Editor.sel = nil
            Dirty()
        end
    end)
    ed.remove:SetPoint("LEFT", ed.toggle, "RIGHT", 4, 0)
    ed.addSpell = W.DropDown(ed, 120, SpellMenu)
    ed.addSpell:SetPoint("TOPLEFT", ed.up, "BOTTOMLEFT", -16, -4)
    UIDropDownMenu_SetText(ed.addSpell, "Add spell ...")
    ed.addSpell.onSelect = function(value)
        UIDropDownMenu_SetText(ed.addSpell, "Add spell ...")
        UI.FightAddRule(2, value)
    end
    ed.addItem = W.DropDown(ed, 120, ItemMenu)
    ed.addItem:SetPoint("LEFT", ed.addSpell, "RIGHT", -24, 0)
    UIDropDownMenu_SetText(ed.addItem, "Add item ...")
    ed.addItem.onSelect = function(value)
        UIDropDownMenu_SetText(ed.addItem, "Add item ...")
        if value < 0 then
            UI.FightAddRule(4, -value)
        else
            UI.FightAddRule(3, value)
        end
    end
    -- What the guild's own rules handle, as one line with the whole list in its tooltip.
    ed.guildRules = W.Label(ed, "|cff80c8ff(?)|r Not in this list: what the guild's own rules handle (hover for the list)",
        "GameFontDisableSmall")
    ed.guildRules:SetPoint("TOPLEFT", ed.up, "BOTTOMLEFT", 0, -34)
    ed.guildRules:SetWidth(460)
    ed.guildRules:SetHeight(12)
    W.SetTooltip(ed.guildRules, "Handled by the guild's own rules", GUILD_RULES_LINES)

    -- Right: the rule's settings and conditions.
    local right = W.Panel(ed)
    right:SetPoint("TOPLEFT", ed, "TOPLEFT", 688, -72)
    right:SetPoint("BOTTOMRIGHT", ed, "BOTTOMRIGHT", -12, 74)
    ed.right = right
    local rule = CreateFrame("Frame", nil, right)
    rule:SetAllPoints(right)
    ed.ruleView = rule
    ed.ruleTitle = W.Label(rule, "", "GameFontNormal")
    ed.ruleTitle:SetPoint("TOPLEFT", rule, "TOPLEFT", 8, -8)
    ed.ruleTitle:SetWidth(280)
    ed.ruleTitle:SetHeight(14)
    ed.targetLabel = W.Label(rule, "Target", "GameFontNormalSmall")
    ed.targetLabel:SetPoint("TOPLEFT", rule, "TOPLEFT", 8, -32)
    ed.targetDD = W.DropDown(rule, 140, function()
        local items = {}
        for i = 0, 5 do
            if not (Editor.list == 1 and i == 5) then
                items[#items + 1] = { text = FIGHT_TARGETS[i], value = i }
            end
        end
        return items
    end)
    ed.targetDD:SetPoint("LEFT", ed.targetLabel, "RIGHT", -8, -2)
    ed.targetDD.onSelect = function(value)
        if Editor.sel then
            Editor.sel.tgt = value
            Dirty()
        end
    end
    ed.relLabel = W.Label(rule, "Relevance", "GameFontNormalSmall")
    ed.relLabel:SetPoint("TOPLEFT", rule, "TOPLEFT", 8, -60)
    ed.rel = W.EditBox(rule, 50, false, 6)
    ed.rel:SetPoint("LEFT", ed.relLabel, "RIGHT", 10, 0)
    ed.ivLabel = W.Label(rule, "every", "GameFontNormalSmall")
    ed.ivLabel:SetPoint("LEFT", ed.rel, "RIGHT", 8, 0)
    ed.iv = W.EditBox(rule, 36, true, 4)
    ed.iv:SetPoint("LEFT", ed.ivLabel, "RIGHT", 8, 0)
    ed.ivUnit = W.Label(rule, "s", "GameFontNormalSmall")
    ed.ivUnit:SetPoint("LEFT", ed.iv, "RIGHT", 4, 0)
    ed.apply = W.Button(rule, "Apply", 56, 20, function()
        local sel = Editor.sel
        if not (sel and Editable()) then
            return
        end
        local rel = tonumber(ed.rel:GetText() or "")
        if not rel or rel < 0 or rel > 200 then
            UI.Notify("The relevance is a number from 0 to 200.", true)
            return
        end
        sel.rel = rel
        sel.iv = tonumber(ed.iv:GetText() or "") or 0
        Dirty()
    end)
    ed.apply:SetPoint("LEFT", ed.ivUnit, "RIGHT", 6, 0)
    ed.band = W.Label(rule, "", "GameFontDisableSmall")
    ed.band:SetPoint("TOPLEFT", rule, "TOPLEFT", 8, -82)
    ed.band:SetWidth(280)
    ed.band:SetHeight(24)
    ed.band:SetJustifyV("TOP")
    ed.once = W.CheckBox(rule, "Once per target", function(self)
        if Editor.sel then
            Editor.sel.fl = WithFlag(Editor.sel.fl, 1, W.IsChecked(self))
            Dirty()
        end
    end)
    ed.once:SetPoint("TOPLEFT", rule, "TOPLEFT", 4, -106)
    ed.notTwice = W.CheckBox(rule, "Not twice in a row", function(self)
        if Editor.sel then
            Editor.sel.fl = WithFlag(Editor.sel.fl, 2, W.IsChecked(self))
            Dirty()
        end
    end)
    ed.notTwice:SetPoint("LEFT", ed.once, "RIGHT", 110, 0)

    ed.condTitle = W.Label(rule, "Conditions (all must hold)", "GameFontNormalSmall")
    ed.condTitle:SetPoint("TOPLEFT", rule, "TOPLEFT", 8, -136)
    ed.conds = W.ScrollList(rule, { rowHeight = 18, createRow = CreateCondRow, updateRow = UpdateCondRow })
    ed.conds:SetPoint("TOPLEFT", rule, "TOPLEFT", 4, -152)
    ed.conds:SetPoint("RIGHT", rule, "RIGHT", -4, 0)
    ed.conds:SetHeight(130)
    ed.condEmpty = W.Label(rule, "No check of its own: it is tried whenever nothing more relevant is due.",
        "GameFontDisableSmall")
    ed.condEmpty:SetPoint("TOPLEFT", rule, "TOPLEFT", 10, -156)
    ed.condEmpty:SetWidth(276)
    ed.condDD = W.DropDown(rule, 150, CondMenu)
    ed.condDD:SetPoint("TOPLEFT", rule, "TOPLEFT", -10, -290)
    UIDropDownMenu_SetText(ed.condDD, "Condition ...")
    ed.condDD.onSelect = function(value)
        ed.condKey = value
        local type = State.fight.condByKey[value]
        UIDropDownMenu_SetText(ed.condDD, type and type.n or value)
        UI.RefreshFightEditor()
    end
    ed.condNot = W.CheckBox(rule, "not", nil)
    ed.condNot:SetPoint("LEFT", ed.condDD, "RIGHT", -12, 2)
    ed.condOpDD = W.DropDown(rule, 40, function()
        local items = {}
        for _, op in ipairs(COND_OPS) do
            items[#items + 1] = { text = op, value = op }
        end
        return items
    end)
    ed.condOpDD:SetPoint("TOPLEFT", ed.condDD, "BOTTOMLEFT", 0, 0)
    ed.condOp = "<"
    ed.condOpDD:SetSelected("<", "<")
    ed.condOpDD.onSelect = function(value)
        ed.condOp = value
    end
    ed.condValue = W.EditBox(rule, 60, false, 33)
    ed.condValue:SetPoint("LEFT", ed.condOpDD, "RIGHT", -6, 2)
    ed.condParam = W.EditBox(rule, 100, false, 64)
    ed.condParam:SetPoint("LEFT", ed.condValue, "RIGHT", 6, 0)
    ed.condParamHint = W.Label(rule, "", "GameFontDisableSmall")
    ed.condParamHint:SetPoint("TOPLEFT", ed.condParam, "BOTTOMLEFT", 0, -2)
    ed.condAdd = W.Button(rule, "Add condition", 110, 20, function()
        local sel = Editor.sel
        if not (sel and Editable()) then
            return
        end
        if #sel.conds >= MAX_CONDS then
            UI.Notify("A rule has at most " .. MAX_CONDS .. " conditions.", true)
            return
        end
        local text, why = ConditionFromForm()
        if not text then
            UI.Notify(why, true)
            return
        end
        sel.conds[#sel.conds + 1] = text
        Dirty()
    end)
    ed.condAdd:SetPoint("TOPLEFT", ed.condOpDD, "BOTTOMLEFT", 16, -16)

    -- Right, the other view: the profile's options.
    local options = CreateFrame("Frame", nil, right)
    options:SetAllPoints(right)
    ed.optionsView = options
    local optTitle = W.Label(options, "Options of the profile: named values its conditions ask for ($key); each bot can"
        .. " have its own value of them in its Combat sub-tab.", "GameFontNormalSmall")
    optTitle:SetPoint("TOPLEFT", options, "TOPLEFT", 8, -8)
    optTitle:SetWidth(280)
    optTitle:SetHeight(36)
    optTitle:SetJustifyV("TOP")
    ed.optList = W.ScrollList(options, { rowHeight = 18, createRow = CreateOptionRow, updateRow = UpdateOptionRow })
    ed.optList:SetPoint("TOPLEFT", options, "TOPLEFT", 4, -48)
    ed.optList:SetPoint("RIGHT", options, "RIGHT", -4, 0)
    ed.optList:SetHeight(180)
    local function field(label, width, x, y, numeric)
        local text = W.Label(options, label, "GameFontNormalSmall")
        text:SetPoint("TOPLEFT", options, "TOPLEFT", x, y)
        local box = W.EditBox(options, width, numeric, 64)
        box:SetPoint("TOPLEFT", text, "BOTTOMLEFT", 4, -2)
        return box
    end
    ed.optKey = field("Key", 70, 8, -236)
    ed.optLabel = field("Label", 150, 96, -236)
    ed.optValue = field("Value", 44, 8, -278)
    ed.optMin = field("Min", 44, 66, -278)
    ed.optMax = field("Max", 44, 124, -278)
    ed.optKind = 1
    ed.optKindDD = W.DropDown(options, 80, function()
        return { { text = "switch", value = 0 }, { text = "number", value = 1 }, { text = "percent", value = 2 } }
    end)
    ed.optKindDD:SetPoint("TOPLEFT", options, "TOPLEFT", 168, -290)
    ed.optKindDD:SetSelected(1, "number")
    ed.optKindDD.onSelect = function(value)
        ed.optKind = value
    end
    ed.optSave = W.Button(options, "Add / change", 110, 20, function()
        if not Editable() then
            return
        end
        local key = string.gsub(ed.optKey:GetText() or "", "^%s*(.-)%s*$", "%1")
        if not string.match(key, "^[%w_]+$") or string.len(key) > 32 then
            UI.Notify("An option's key is 1 to 32 letters, digits or _.", true)
            return
        end
        local value, min, max = tonumber(ed.optValue:GetText() or ""), tonumber(ed.optMin:GetText() or ""),
            tonumber(ed.optMax:GetText() or "")
        if ed.optKind == 0 then
            min, max, value = 0, 1, (value and value ~= 0) and 1 or 0
        elseif ed.optKind == 2 then
            min, max = min or 0, max or 100
        end
        if not (value and min and max) or min > max or value < min or value > max then
            UI.Notify("The value must lie between min and max.", true)
            return
        end
        local option
        for _, o in ipairs(Editor.draft.options) do
            if o.k == key then
                option = o
            end
        end
        if not option then
            if #Editor.draft.options >= MAX_OPTIONS then
                UI.Notify("A profile has at most " .. MAX_OPTIONS .. " options.", true)
                return
            end
            option = { k = key }
            Editor.draft.options[#Editor.draft.options + 1] = option
        end
        option.n, option.kind, option.v, option.min, option.max = ed.optLabel:GetText() or "", ed.optKind, value, min, max
        Dirty()
    end)
    ed.optSave:SetPoint("TOPLEFT", options, "TOPLEFT", 8, -326)

    -- Bottom.
    ed.save = W.Button(ed, "Save", 90, 22, function() UI.FightSaveProfile() end)
    ed.save:SetPoint("BOTTOMLEFT", ed, "BOTTOMLEFT", 220, 40)
    ed.discard = W.Button(ed, "Discard", 90, 22, function()
        LoadDraft()
        Editor.message = nil
        UI.RefreshFightEditor()
    end)
    ed.discard:SetPoint("LEFT", ed.save, "RIGHT", 6, 0)
    local function assign(slot)
        local bot = State.GetBot(Editor.g)
        if not bot or Editor.id == 0 then
            return
        end
        Protocol.AssignFight(Editor.g, slot, Editor.id, function(ok, code, msg)
            Editor.message = ok and string.format("%s fights by it %s.", bot.n or "The bot", slot == 1 and "in a group" or "solo")
                or GA.Protocol.ErrorText(code, msg)
            Editor.messageError = not ok
            UI.RefreshFightEditor()
        end)
    end
    ed.assignSolo = W.Button(ed, "Assign solo", 100, 22, function() assign(0) end)
    ed.assignSolo:SetPoint("LEFT", ed.discard, "RIGHT", 24, 0)
    ed.assignGroup = W.Button(ed, "Assign group", 100, 22, function() assign(1) end)
    ed.assignGroup:SetPoint("LEFT", ed.assignSolo, "RIGHT", 6, 0)
    ed.message = W.Label(ed, "", "GameFontHighlightSmall")
    ed.message:SetPoint("BOTTOMLEFT", ed, "BOTTOMLEFT", 16, 16)
    ed.message:SetWidth(960)
    ed:Hide()
end

-- Opens the editor for a bot (its class, its talent tree, its spells and items) on a profile (0 = its template).
function UI.OpenFightEditor(g, id)
    if not ed then
        CreateEditor()
    end
    if Editor.dirty and (Editor.g ~= g or Editor.id ~= (id or 0)) then
        UI.Confirm("Discard the changes in the fight profile editor?", function()
            Editor.dirty = false
            UI.OpenFightEditor(g, id)
        end)
        return
    end
    Editor.g, Editor.id, Editor.list, Editor.view = g, id or 0, 0, "rule"
    Editor.message, Editor.asked = nil, nil
    LoadDraft()
    if State.IsConnected() then
        Protocol.GetFightCatalog(g)
        Protocol.GetFightProfiles(g)
    end
    ed:Show()
    UI.RefreshFightEditor()
end

function UI.FightEditorFrame()
    return ed
end

function UI.RefreshFightEditor()
    if not (ed and ed:IsShown()) then
        return
    end
    local bot = State.GetBot(Editor.g)
    local list, profiles = State.FightOf(Editor.g)
    local class = bot and Data.CLASSES[bot.class or 0]
    local tab = list and list.meta and list.meta.tab
    ed.title:SetText("Fight profiles: " .. ((bot and bot.n) or "?") .. " (" .. (TreeName(bot and bot.class, tab) or "")
        .. " " .. (class and class.name or "") .. ")")

    -- The profile it shows must come first; afterwards a new revision of it is taken when nothing is changed.
    local key = tostring(Editor.g) .. ":" .. tostring(Editor.id)
    if State.IsConnected() and Editor.asked ~= key then
        Editor.asked = key
        Protocol.GetFightProfile(Editor.g, Editor.id)
    end
    -- A new copy of the profile (a new block from the server) or of its options is taken while nothing is changed.
    local shown = profiles[Editor.id]
    local _, _, options = State.FightOf(Editor.g)
    if shown and not Editor.dirty and (not Editor.draft or Editor.loadedMeta ~= shown.meta
        or Editor.loadedOptions ~= options[Editor.id]) then
        LoadDraft()
    end

    local rows = { { id = 0, n = "Template (mod-playerbots)", detail = "read-only" } }
    for _, profile in ipairs(list and list.list or {}) do
        local other = tab and profile.tab ~= tab
        rows[#rows + 1] = { id = profile.id, n = profile.tpl and profile.n:gsub(" %(wowbot335%)$", "") or profile.n, tpl = profile.tpl, detail = string.format("%s%s%s, %d rules, %d %s",
            other and "|cffff8040" or "", profile.tpl and "template, " or "", TreeName(bot and bot.class, profile.tab) or "?",
            (profile.rc or 0) + (profile.rn or 0), profile.used or 0, (profile.used or 0) == 1 and "bot" or "bots") }
    end
    ed.profiles:SetItems(rows)

    local editable = Editable()
    local draft = Editor.draft
    W.SetEditText(ed.name, draft and draft.n or "")
    W.SetEditText(ed.note, draft and draft.note or "")
    W.SetEditEnabled(ed.name, editable)
    W.SetEditEnabled(ed.note, editable)
    if not draft then
        ed.banner:SetText(State.IsConnected() and "Asking the server ..." or "Not connected.")
    elseif Editor.id == 0 then
        ed.banner:SetText("|cffff8040Template (mod-playerbots), read-only.|r Make a copy with \"New copy\" to change it.")
    elseif ReadOnly() then
        ed.banner:SetText("|cffff8040Template (wowbot335), read-only.|r Assign it as it is, or make a copy with \"New copy\" to change it.")
    elseif tab and shown and shown.meta.tab ~= tab then
        ed.banner:SetText("|cffff8040This profile is for the " .. (TreeName(bot and bot.class, shown.meta.tab) or "?")
            .. " tree; the bot's talents are " .. (TreeName(bot and bot.class, tab) or "?") .. ": it plays it only in that tree.|r")
    else
        ed.banner:SetText(Editor.dirty and "|cffffd100Changed: Save sends it to the server.|r" or "")
    end
    W.SetEnabled(ed.deleteBtn, editable)
    W.SetEnabled(ed.newBtn, State.IsConnected() and (Editor.id ~= 0 or (bot and bot.s ~= "benched")))
    for kind, button in pairs({ [0] = ed.fightTab, [1] = ed.buffTab }) do
        if Editor.list == kind then
            button:LockHighlight()
        else
            button:UnlockHighlight()
        end
    end

    -- The shown list with the band dividers.
    local items, count, band = {}, 0, nil
    for _, rule in ipairs(Sorted(Editor.list)) do
        local current = BandOf(rule.rel)
        if current ~= band then
            band = current
            items[#items + 1] = { divider = band.text }
        end
        items[#items + 1] = rule
        count = count + 1
    end
    ed.rules:SetItems(items)
    -- A rule just added, moved or named by the server is scrolled into sight.
    if Editor.revealSel and Editor.sel then
        Editor.revealSel = nil
        for i, item in ipairs(items) do
            local visible = ed.rules:VisibleRows()
            local offset = FauxScrollFrame_GetOffset(ed.rules.scroll) or 0
            if item == Editor.sel and (i <= offset or i > offset + visible) then
                local want = math.max(0, math.min(i - math.floor(visible / 2), #items - visible))
                FauxScrollFrame_OnVerticalScroll(ed.rules.scroll, want * ed.rules.rowHeight, ed.rules.rowHeight, function()
                    ed.rules:Refresh()
                end)
            end
        end
    end
    ed.count:SetText(draft and string.format("%d rules (%d of %d in all)", count, RuleCount(), MAX_RULES) or "")
    local sel = Editor.sel
    if sel and (sel.l or 0) ~= Editor.list then
        sel, Editor.sel = nil, nil
    end
    for _, button in ipairs({ ed.up, ed.down, ed.toggle, ed.remove }) do
        W.SetEnabled(button, editable and sel ~= nil)
    end
    ed.addSpell:SetEnabled(editable)
    ed.addItem:SetEnabled(editable)

    -- Right.
    if Editor.view == "options" then
        ed.ruleView:Hide()
        ed.optionsView:Show()
        ed.optList:SetItems(draft and draft.options or {})
        W.SetEnabled(ed.optSave, editable)
        ed.optionsBtn:LockHighlight()
    else
        ed.optionsView:Hide()
        ed.ruleView:Show()
        ed.optionsBtn:UnlockHighlight()
        local own = sel and (sel.k or 0) >= 2
        ed.ruleTitle:SetText(sel and UI.FightRuleName({ k = sel.k, ref = (sel.k or 0) <= 1 and sel.icon or sel.ref, a = sel.a })
            or "Select a rule")
        ed.targetDD:SetSelected(sel and sel.tgt or 0, sel and (own and FIGHT_TARGETS[sel.tgt or 0] or "its own") or "")
        ed.targetDD:SetEnabled(editable and own)
        W.SetEditText(ed.rel, sel and string.format("%.1f", sel.rel or 0) or "")
        W.SetEditText(ed.iv, sel and tostring(sel.iv or 0) or "")
        W.SetEditEnabled(ed.rel, editable and sel ~= nil)
        W.SetEditEnabled(ed.iv, editable and own)
        W.SetEnabled(ed.apply, editable and sel ~= nil)
        local source = sel and sel.src and sel.src ~= "" and (", from mod-playerbots' strategy \"" .. sel.src .. "\"") or ""
        local learned = sel and sel.kn == false and ("|cffff8040 Not learned yet" .. ((sel.lv or 0) > 0 and
            (" (from level " .. sel.lv .. ")") or "") .. ".|r") or ""
        ed.band:SetText(sel and ("Band: " .. BandOf(sel.rel).text .. source .. "." .. learned) or "")
        ed.once:SetChecked(sel and HasFlag(sel.fl, 1) or false)
        ed.notTwice:SetChecked(sel and HasFlag(sel.fl, 2) or false)
        W.SetEnabled(ed.once, editable and own)
        W.SetEnabled(ed.notTwice, editable and own)
        local condItems = {}
        if sel and sel.tg and sel.tg ~= "" then
            condItems[1] = { trigger = sel.tg }
        end
        for _, cond in ipairs(sel and sel.conds or {}) do
            condItems[#condItems + 1] = cond
        end
        ed.conds:SetItems(condItems)
        if sel and #condItems == 0 then
            ed.condEmpty:Show()
        else
            ed.condEmpty:Hide()
        end
        local type = State.fight.condByKey[ed.condKey or ""]
        local wantsValue = type and (type.vk or 0) ~= 0
        local wantsParam = type and (type.pk or 0) ~= 0
        if wantsValue then
            ed.condOpDD:Show()
            ed.condValue:Show()
        else
            ed.condOpDD:Hide()
            ed.condValue:Hide()
        end
        if wantsParam then
            ed.condParam:Show()
            ed.condParamHint:SetText(PARAM_HINT[type.pk] or "")
        else
            ed.condParam:Hide()
            ed.condParamHint:SetText("")
        end
        ed.condDD:SetEnabled(editable and sel ~= nil)
        W.SetEnabled(ed.condAdd, editable and sel ~= nil and type ~= nil)
    end

    W.SetEnabled(ed.save, editable and Editor.dirty)
    W.SetEnabled(ed.discard, Editor.dirty)
    W.SetEnabled(ed.assignSolo, State.IsConnected() and Editor.id ~= 0 and not Editor.dirty)
    W.SetEnabled(ed.assignGroup, State.IsConnected() and Editor.id ~= 0 and not Editor.dirty)
    ed.message:SetText(Editor.message and ((Editor.messageError and "|cffff4040" or "|cff40ff40") .. Editor.message .. "|r") or "")
end

GA.On("FIGHT", function()
    UI.RefreshFightEditor()
end)
