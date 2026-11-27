-- Achievements tab: the tiered guild achievements (docs/protocol.md §7.18, docs/guild-management.md "Tiered guild
-- achievements"). One row per series with its progress to the next tier; the tooltip lists every tier. Tiers above
-- the level cap stay visible, greyed, so the player sees what later content brings.

local GA = GuildAdventure
local UI = GA.UI
local W = UI.W
local Data = GA.Data
local State = GA.State
local Protocol = GA.Protocol

local REFRESH_SECONDS = 30 -- progress values (kills, quests) while the page is open
local FALLBACK_ICON = "Interface\\Icons\\INV_Misc_QuestionMark"

local GROUPS = {
    { key = "all", label = "All" },
    { key = "class", label = "Classes" },
    { key = "race", label = "Races" },
    { key = "prof", label = "Professions" },
    { key = "kills", label = "Kills" },
    { key = "guild", label = "Guild" },
}

local KIND_ICONS = {
    storage_items = "Interface\\Icons\\INV_Misc_Bag_10",
    guild_gold = "Interface\\Icons\\INV_Misc_Coin_01",
    quests = "Interface\\Icons\\INV_Misc_Book_09",
    dungeons = "Interface\\Icons\\INV_Misc_Key_03",
    raids = "Interface\\Icons\\INV_Misc_Head_Dragon_01",
}

local RACE_ICONS = {
    [1] = "INV_Misc_Head_Human_01", [3] = "INV_Misc_Head_Dwarf_01", [4] = "INV_Misc_Head_Elf_02",
    [7] = "INV_Misc_Head_Gnome_01", [11] = "INV_Misc_Head_Elf_01", [2] = "INV_Misc_Head_Orc_01",
    [5] = "INV_Misc_Head_Undead_01", [6] = "INV_Misc_Head_Tauren_01", [8] = "INV_Misc_Head_Troll_01",
    [10] = "INV_Misc_Head_Elf_01",
}

local KILL_ICONS = {
    [1] = "Ability_Hunter_Pet_Wolf", [2] = "INV_Misc_Head_Dragon_01", [3] = "Spell_Shadow_SummonFelHunter",
    [4] = "Spell_Fire_Fire", [5] = "Ability_Warrior_Rampage", [6] = "Spell_Shadow_RaiseDead",
    [7] = "Ability_Warrior_Challange", [9] = "INV_Gizmo_01",
}

local GUILD_NAMES = {
    storage_items = "Guild storage", guild_gold = "Guild gold", quests = "Quests", dungeons = "Dungeon runs",
    raids = "Raid runs",
}

local page

-- 1234567 -> "1,234,567"
local function FormatCount(n)
    local s = tostring(math.floor(n or 0))
    local out = s:reverse():gsub("(%d%d%d)", "%1,"):reverse()
    return (out:gsub("^,", ""))
end

local function SeriesName(s)
    if s.k == "kills" then
        return (s.n ~= "" and s.n or "Creatures") .. " slain"
    end
    if s.n and s.n ~= "" then
        return s.n
    end
    return GUILD_NAMES[s.k] or s.s
end

-- What the value of the series counts, e.g. "highest level 24", "1,250 items".
local function ValueText(s)
    local v = s.v or 0
    if s.k == "level_class" or s.k == "level_race" then
        return v > 0 and ("highest level " .. v) or "nobody yet"
    elseif s.k == "skill" then
        return v > 0 and ("highest skill " .. v) or "nobody has learned it"
    elseif s.k == "storage_items" then
        return FormatCount(v) .. " items in the storage"
    elseif s.k == "guild_gold" then
        return FormatCount(v) .. " gold (guild bank and members)"
    elseif s.k == "kills" then
        return FormatCount(v) .. " slain"
    elseif s.k == "quests" then
        return FormatCount(v) .. " quests completed"
    end
    return FormatCount(v) .. (v == 1 and " run" or " runs")
end

-- One tier as text: "level 30", "skill 225", "10,000".
local function TierText(s, threshold)
    if s.k == "level_class" or s.k == "level_race" then
        return "level " .. threshold
    elseif s.k == "skill" then
        return "skill " .. threshold
    elseif s.k == "guild_gold" then
        return FormatCount(threshold) .. " gold"
    end
    return FormatCount(threshold)
end

local function SetIcon(tex, path)
    if not path or not tex:SetTexture(path) then
        tex:SetTexture(FALLBACK_ICON)
    end
end

local function ApplyIcon(row, s)
    row.icon:SetTexCoord(0, 1, 0, 1)
    if s.grp == "class" then
        row.icon:SetTexture(Data.CLASS_ICON_TEXTURE)
        row.icon:SetTexCoord(Data.ClassTexCoords(s.sub))
    elseif s.grp == "race" then
        SetIcon(row.icon, RACE_ICONS[s.sub] and ("Interface\\Icons\\" .. RACE_ICONS[s.sub]))
    elseif s.grp == "prof" then
        SetIcon(row.icon, Data.ProfessionIcon(s.sub))
    elseif s.grp == "kills" then
        SetIcon(row.icon, KILL_ICONS[s.sub] and ("Interface\\Icons\\" .. KILL_ICONS[s.sub]))
    else
        SetIcon(row.icon, KIND_ICONS[s.k])
    end
end

local function TooltipLines(s)
    local lines = { ValueText(s) }
    local th, pt = s.th or {}, s.pt or {}
    for i = 1, #th do
        local text = TierText(s, th[i]) .. "   +" .. (pt[i] or 0) .. " points"
        if i <= (s.got or 0) then
            lines[#lines + 1] = "|cff40ff40" .. text .. "  - reached|r"
        elseif i > (s.act or 0) then
            lines[#lines + 1] = "|cff808080" .. text .. "  - needs a higher level cap|r"
        else
            lines[#lines + 1] = "|cffffffff" .. text .. "|r"
        end
    end
    if (s.act or 0) > 0 then
        lines[#lines + 1] = "|cff9d9d9dThe last tier the level cap allows gives the bonus points.|r"
    end
    return lines
end

local function CreateRow(list, index)
    local row = CreateFrame("Frame", nil, list)
    row.shade = row:CreateTexture(nil, "BACKGROUND")
    row.shade:SetAllPoints(row)
    row.shade:SetTexture("Interface\\Buttons\\WHITE8X8")
    row.icon = W.Icon(row, 28, FALLBACK_ICON)
    row.icon:SetPoint("LEFT", row, "LEFT", 4, 0)
    row.tiers = W.Label(row, "", "GameFontHighlightSmall", "RIGHT")
    row.tiers:SetPoint("RIGHT", row, "RIGHT", -8, 0)
    row.tiers:SetWidth(70)
    row.bar = W.Bar(row, 190, 14, 0.9, 0.7, 0.1)
    row.bar:SetPoint("RIGHT", row.tiers, "LEFT", -8, 0)
    row.name = W.Label(row, "", "GameFontNormal")
    row.name:SetPoint("TOPLEFT", row.icon, "TOPRIGHT", 8, -1)
    row.name:SetPoint("RIGHT", row.bar, "LEFT", -8, 0)
    row.detail = W.Label(row, "", "GameFontHighlightSmall")
    row.detail:SetPoint("BOTTOMLEFT", row.icon, "BOTTOMRIGHT", 8, 1)
    row.detail:SetPoint("RIGHT", row.bar, "LEFT", -8, 0)
    W.SetTooltip(row, function(self)
        return self.series and SeriesName(self.series) or nil
    end, function(self)
        return self.series and TooltipLines(self.series) or nil
    end)
    return row
end

local function UpdateRow(row, s)
    row.series = s
    row.shade:SetVertexColor(1, 1, 1, ((s.got or 0) >= (s.act or 0) and (s.act or 0) > 0) and 0.06 or 0.0)
    ApplyIcon(row, s)
    row.name:SetText(SeriesName(s))

    local th, pt = s.th or {}, s.pt or {}
    local nextTier = State.NextMilestoneTier(s)
    local done = not nextTier and (s.act or 0) > 0
    row.icon:SetDesaturated((s.got or 0) == 0)
    if nextTier then
        local goal = th[nextTier] or 0
        local from = nextTier > 1 and (th[nextTier - 1] or 0) or 0
        row.detail:SetText(ValueText(s) .. "  |cff9d9d9d- next: " .. TierText(s, goal) .. " (+" .. (pt[nextTier] or 0) .. ")|r")
        row.bar:SetProgress((s.v or 0) - from, goal - from, FormatCount(s.v or 0) .. " / " .. FormatCount(goal))
        row.bar:SetStatusBarColor(0.9, 0.7, 0.1)
    elseif done then
        row.detail:SetText(ValueText(s) .. "  |cff40ff40- every tier reached|r")
        row.bar:SetProgress(1, 1, "Complete")
        row.bar:SetStatusBarColor(0.25, 0.75, 0.25)
    else
        row.detail:SetText(ValueText(s) .. "  |cff808080- needs a higher level cap|r")
        row.bar:SetProgress(0, 1, "")
    end
    local extra = #th > (s.act or 0) and ("|cff808080 +" .. (#th - (s.act or 0)) .. "|r") or ""
    row.tiers:SetText((s.got or 0) .. " / " .. (s.act or 0) .. extra)
end

-- The series of the selected group; races of the other faction are left out (D9: one faction per guild).
local function VisibleSeries(group)
    local faction = State.guild.fac
    local items = {}
    for _, s in ipairs(State.milestones) do
        local race = s.grp == "race" and Data.RACES[s.sub]
        local otherFaction = race and faction and race.faction ~= faction
        if not otherFaction and (group == "all" or s.grp == group) then
            items[#items + 1] = s
        end
    end
    return items
end

local function Create(parent)
    local f = CreateFrame("Frame", nil, parent)
    page = f
    f.group = "all"
    f.lastGet = 0

    f.title = W.Label(f, "Guild achievements", "GameFontNormalLarge")
    f.title:SetPoint("TOPLEFT", f, "TOPLEFT", 8, -8)
    f.summary = W.Label(f, "", "GameFontHighlightSmall", "RIGHT")
    f.summary:SetPoint("TOPRIGHT", f, "TOPRIGHT", -8, -10)
    f.summary:SetPoint("LEFT", f.title, "RIGHT", 12, 0)
    local hint = W.Label(f, "Every tier the guild reaches gives guild points (Perks tab) and renown; the last tier of a "
        .. "series gives bonus points. Greyed tiers wait for a higher level cap.", "GameFontDisableSmall")
    hint:SetPoint("TOPLEFT", f.title, "BOTTOMLEFT", 0, -6)
    hint:SetPoint("RIGHT", f, "RIGHT", -8, 0)
    hint:SetJustifyV("TOP")

    f.buttons = {}
    local x = 8
    for i, group in ipairs(GROUPS) do
        local b = W.Button(f, group.label, 92, 20, function()
            f.group = group.key
            UI.RequestRefresh()
        end)
        b:SetPoint("TOPLEFT", f, "TOPLEFT", x, -50)
        x = x + 96
        f.buttons[i] = b
    end

    local panel = W.Panel(f)
    panel:SetPoint("TOPLEFT", f, "TOPLEFT", 0, -76)
    panel:SetPoint("BOTTOMRIGHT", f, "BOTTOMRIGHT", 0, 0)
    f.list = W.ScrollList(panel, { rowHeight = 36, createRow = CreateRow, updateRow = UpdateRow })
    f.list:SetPoint("TOPLEFT", panel, "TOPLEFT", 6, -6)
    f.list:SetPoint("BOTTOMRIGHT", panel, "BOTTOMRIGHT", -6, 6)
    f.empty = W.Label(panel, "", "GameFontDisableSmall", "CENTER")
    f.empty:SetPoint("CENTER", panel, "CENTER", 0, 0)
    return f
end

local function Refresh(f)
    local g = State.guild
    f.summary:SetText(string.format("|cffffe060%d of %d tiers|r  |cff9d9d9d(%d guild points, +%d renown)|r", g.ms or 0, g.msa or 0,
        g.msp or 0, g.msr or 0))
    for i, group in ipairs(GROUPS) do
        if group.key == f.group then
            f.buttons[i]:LockHighlight()
        else
            f.buttons[i]:UnlockHighlight()
        end
    end

    -- Fresh progress values while the page is open (the server pushes the block itself when a tier is reached).
    local now = GetTime()
    if State.IsConnected() and now - f.lastGet >= REFRESH_SECONDS then
        f.lastGet = now
        Protocol.Get("milestones")
    end

    local items = VisibleSeries(f.group)
    f.list:SetItems(items)
    if #items == 0 then
        f.empty:SetText(State.IsConnected() and "No tiered achievements known yet." or "Not connected.")
        f.empty:Show()
    else
        f.empty:Hide()
    end
end

UI.RegisterPage("achievements", { tab = "achievements", create = Create, refresh = Refresh })

-- Test and probe access: the rows the page would show for a group.
function UI.AchievementSeries(group)
    return VisibleSeries(group or "all")
end
