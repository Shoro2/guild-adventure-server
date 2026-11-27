-- Recruitment tab: pick race, class, gender and name for a free slot (decision D7: free
-- choice for MVP; a candidate board comes later).

local GA = GuildAdventure
local UI = GA.UI
local W = UI.W
local Data = GA.Data
local State = GA.State

local CLASS_ROLES = {
    [1] = "Tank, melee damage",
    [2] = "Tank, healer, melee damage",
    [3] = "Ranged damage with a pet",
    [4] = "Melee damage",
    [5] = "Healer, ranged damage",
    [6] = "Tank, melee damage (starts at level 55)",
    [7] = "Healer, ranged or melee damage",
    [8] = "Ranged damage",
    [9] = "Ranged damage with a pet",
    [11] = "Tank, healer, ranged or melee damage",
}

local PREFIXES = { "Al", "Bel", "Cor", "Dar", "El", "Fen", "Gal", "Hal", "Ith", "Jor", "Kel", "Lor",
    "Mor", "Nor", "Or", "Pel", "Quin", "Ral", "Sel", "Tor", "Ul", "Val", "Wyn", "Yor", "Zan", "Bra", "Thal" }
local SUFFIXES = { "dric", "wen", "ius", "ara", "eth", "ion", "mir", "ric", "wyn", "dor", "ra", "lin",
    "thas", "gar", "mund", "nor", "iel", "ash", "ok", "ul", "ana", "ek", "ia", "orn" }

local draft = { race = nil, class = nil, gender = 0 }

local function Faction()
    if State.guild.fac then
        return State.guild.fac
    end
    return UnitFactionGroup("player") == "Horde" and "H" or "A"
end

local function NameInUse(name)
    local lower = string.lower(name)
    if State.control.ln and string.lower(State.control.ln) == lower then
        return true
    end
    for _, bot in pairs(State.bots) do
        if string.lower(bot.n or "") == lower then
            return true
        end
    end
    return false
end

local function SuggestName()
    for _ = 1, 20 do
        local name = PREFIXES[math.random(#PREFIXES)] .. SUFFIXES[math.random(#SUFFIXES)]
        if not NameInUse(name) then
            return name
        end
    end
    return PREFIXES[1] .. SUFFIXES[1]
end

-- Client-side pre-check only; the server applies the real name rules.
local function CheckName(name)
    if not name or name == "" then
        return false, "Enter a name."
    elseif string.len(name) < 2 or string.len(name) > 12 then
        return false, "Names need 2 to 12 letters."
    elseif string.find(name, "[%s%d%p]") then
        return false, "Only letters, no spaces, digits or symbols."
    elseif NameInUse(name) then
        return false, "That name is already in your guild."
    end
    return true
end

local function FirstValidRace()
    for _, id in ipairs(Data.RACE_ORDER) do
        if Data.RACES[id].faction == Faction() then
            return id
        end
    end
    return 1
end

local function ClassAllowed(classId)
    if not Data.IsValidCombo(draft.race, classId) then
        return false, "Not available for " .. Data.RaceName(draft.race) .. "."
    end
    if classId == Data.DEATH_KNIGHT and not State.guild.dk then
        return false, "Death Knights unlock once a guild member reaches level 55."
    end
    return true
end

local function FirstValidClass()
    for _, id in ipairs(Data.CLASS_ORDER) do
        if ClassAllowed(id) then
            return id
        end
    end
    return nil
end

local function Create(parent)
    local f = CreateFrame("Frame", nil, parent)

    local left = CreateFrame("Frame", nil, f)
    left:SetPoint("TOPLEFT", f, "TOPLEFT", 6, -6)
    left:SetPoint("BOTTOMLEFT", f, "BOTTOMLEFT", 6, 6)
    left:SetWidth(380)

    local title = W.Header(left, "Recruit a new adventurer")
    title:SetPoint("TOPLEFT", left, "TOPLEFT", 4, -4)
    f.slots = W.Label(left, "", "GameFontHighlight")
    f.slots:SetPoint("TOPLEFT", title, "BOTTOMLEFT", 0, -12)
    f.slots:SetWidth(370)

    local raceLabel = W.Label(left, "Race", "GameFontNormalSmall")
    raceLabel:SetPoint("TOPLEFT", f.slots, "BOTTOMLEFT", 0, -20)
    raceLabel:SetWidth(60)
    f.race = W.DropDown(left, 140, function()
        local items = {}
        for _, id in ipairs(Data.RACE_ORDER) do
            local race = Data.RACES[id]
            if race.faction == Faction() then
                items[#items + 1] = { text = race.name, value = id }
            end
        end
        return items
    end)
    f.race:SetPoint("LEFT", raceLabel, "RIGHT", -8, -2)
    f.race.onSelect = function(value)
        draft.race = value
        if not draft.class or not ClassAllowed(draft.class) then
            draft.class = FirstValidClass()
        end
        UI.RequestRefresh()
    end

    local classLabel = W.Label(left, "Class", "GameFontNormalSmall")
    classLabel:SetPoint("TOPLEFT", raceLabel, "BOTTOMLEFT", 0, -22)
    classLabel:SetWidth(60)
    f.class = W.DropDown(left, 140, function()
        local items = {}
        for _, id in ipairs(Data.CLASS_ORDER) do
            if Data.IsValidCombo(draft.race, id) then
                local ok, why = ClassAllowed(id)
                items[#items + 1] = {
                    text = Data.ClassColorCode(id) .. Data.ClassName(id) .. "|r",
                    value = id,
                    disabled = not ok,
                    tooltip = (CLASS_ROLES[id] or "") .. (why and ("\n|cffff8040" .. why .. "|r") or ""),
                }
            end
        end
        return items
    end)
    f.class:SetPoint("LEFT", classLabel, "RIGHT", -8, -2)
    f.class.onSelect = function(value)
        draft.class = value
        UI.RequestRefresh()
    end

    local genderLabel = W.Label(left, "Gender", "GameFontNormalSmall")
    genderLabel:SetPoint("TOPLEFT", classLabel, "BOTTOMLEFT", 0, -22)
    genderLabel:SetWidth(60)
    f.male = W.CheckBox(left, "Male", function()
        draft.gender = 0
        UI.RequestRefresh()
    end)
    f.male:SetPoint("LEFT", genderLabel, "RIGHT", 4, 0)
    f.female = W.CheckBox(left, "Female", function()
        draft.gender = 1
        UI.RequestRefresh()
    end)
    f.female:SetPoint("LEFT", f.male, "RIGHT", 60, 0)

    local nameLabel = W.Label(left, "Name", "GameFontNormalSmall")
    nameLabel:SetPoint("TOPLEFT", genderLabel, "BOTTOMLEFT", 0, -22)
    nameLabel:SetWidth(60)
    f.name = W.EditBox(left, 140, false, 12)
    f.name:SetPoint("LEFT", nameLabel, "RIGHT", 10, 0)
    f.name:SetScript("OnTextChanged", function()
        UI.RequestRefresh()
    end)
    f.suggest = W.Button(left, "Suggest", 80, 22, function()
        f.name:SetText(SuggestName())
    end)
    f.suggest:SetPoint("LEFT", f.name, "RIGHT", 8, 0)
    f.nameHint = W.Label(left, "", "GameFontDisableSmall")
    f.nameHint:SetPoint("TOPLEFT", nameLabel, "BOTTOMLEFT", 0, -8)
    f.nameHint:SetWidth(370)

    f.recruit = W.Button(left, "Recruit", 140, 24, function()
        local name = f.name:GetText()
        local ok, why = CheckName(name)
        if not ok then
            UI.Notify(why, true)
            return
        end
        W.SetEnabled(f.recruit, false)
        GA.Protocol.Recruit(draft.race, draft.class, draft.gender, name,
            UI.RequestCallback(name .. " joins your guild!", function(success)
                W.SetEnabled(f.recruit, true)
                if success then
                    f.name:SetText("")
                end
                UI.RequestRefresh()
            end))
    end)
    f.recruit:SetPoint("TOPLEFT", f.nameHint, "BOTTOMLEFT", 0, -16)

    -- preview
    local preview = W.Panel(f)
    preview:SetPoint("TOPLEFT", left, "TOPRIGHT", 10, 0)
    preview:SetPoint("BOTTOMRIGHT", f, "BOTTOMRIGHT", -6, 6)
    f.icon = W.ClassIcon(preview, 56)
    f.icon:SetPoint("TOPLEFT", preview, "TOPLEFT", 16, -16)
    f.previewName = W.Label(preview, "", "GameFontNormalLarge")
    f.previewName:SetPoint("TOPLEFT", f.icon, "TOPRIGHT", 12, -4)
    f.previewInfo = W.Label(preview, "", "GameFontHighlight")
    f.previewInfo:SetPoint("TOPLEFT", f.previewName, "BOTTOMLEFT", 0, -6)
    f.previewRole = W.Label(preview, "", "GameFontHighlightSmall")
    f.previewRole:SetPoint("TOPLEFT", f.previewInfo, "BOTTOMLEFT", 0, -4)
    f.previewStart = W.Label(preview, "", "GameFontHighlightSmall")
    f.previewStart:SetPoint("TOPLEFT", f.icon, "BOTTOMLEFT", 0, -16)
    f.previewStart:SetPoint("RIGHT", preview, "RIGHT", -12, 0)
    f.previewStart:SetHeight(42)
    f.previewStart:SetJustifyV("TOP")
    f.guildMix = W.Label(preview, "", "GameFontHighlightSmall")
    f.guildMix:SetPoint("TOPLEFT", f.previewStart, "BOTTOMLEFT", 0, -16)
    f.guildMix:SetPoint("RIGHT", preview, "RIGHT", -12, 0)
    f.guildMix:SetHeight(42)
    f.guildMix:SetJustifyV("TOP")
    local note = W.Label(preview, "Recruits are real characters: they earn their XP, loot and skills. "
        .. "Death Knights start at level 55 once a guild member has reached level 55.", "GameFontDisableSmall")
    note:SetPoint("BOTTOMLEFT", preview, "BOTTOMLEFT", 12, 12)
    note:SetPoint("RIGHT", preview, "RIGHT", -12, 0)
    note:SetHeight(28)
    note:SetJustifyV("BOTTOM")

    f.lock = W.LockedOverlay(f)
    return f
end

local function GuildMix()
    local counts = {}
    for _, bot in pairs(State.bots) do
        counts[bot.class] = (counts[bot.class] or 0) + 1
    end
    local parts = {}
    for _, id in ipairs(Data.CLASS_ORDER) do
        if counts[id] then
            parts[#parts + 1] = Data.ClassColorCode(id) .. counts[id] .. " " .. Data.ClassName(id) .. "|r"
        end
    end
    return "Your guild: " .. (#parts > 0 and table.concat(parts, ", ") or "no bots yet")
end

local function Refresh(f)
    if not State.HasFeature("recruit") then
        f.lock:SetLock("Recruitment", State.FeatureRequirement("recruit"))
    else
        f.lock:Hide()
    end
    if not draft.race or Data.RACES[draft.race].faction ~= Faction() then
        draft.race = FirstValidRace()
        draft.class = nil
    end
    if not draft.class or not ClassAllowed(draft.class) then
        draft.class = FirstValidClass()
    end

    local free = State.FreeSlots()
    local g = State.guild
    if free > 0 then
        f.slots:SetText(string.format("|cff40ff40%d free slot%s|r of %d.", free, free == 1 and "" or "s", g.slots or 0))
    else
        local nextSlots = State.NextUnlock("slots")
        local hint = nextSlots and (" More slots at Renown " .. (nextSlots.lvl or "?") .. ".") or ""
        f.slots:SetText("|cffff8040All " .. (g.slots or 0) .. " slots are in use.|r" .. hint)
    end

    f.race:SetSelected(draft.race, Data.RaceName(draft.race))
    if draft.class then
        f.class:SetSelected(draft.class, Data.ClassColorCode(draft.class) .. Data.ClassName(draft.class) .. "|r")
    else
        f.class:SetSelected(nil, "-")
    end
    f.male:SetChecked(draft.gender == 0)
    f.female:SetChecked(draft.gender == 1)

    local name = f.name:GetText() or ""
    local nameOk, why = CheckName(name)
    f.nameHint:SetText(name == "" and "Pick a name or press Suggest." or (nameOk and "|cff40ff40Name looks fine.|r" or ("|cffff8040" .. why .. "|r")))
    W.SetEnabled(f.recruit, State.IsConnected() and State.HasFeature("recruit") and free > 0 and draft.class ~= nil and nameOk)

    -- preview
    if draft.class then
        f.icon:SetClass(draft.class)
        f.icon:Show()
    else
        f.icon:Hide()
    end
    f.previewName:SetText(name ~= "" and name or "New recruit")
    local isDk = draft.class == Data.DEATH_KNIGHT
    f.previewInfo:SetText(string.format("Level %d %s %s", isDk and Data.DK_START_LEVEL or 1, Data.RaceName(draft.race),
        draft.class and Data.ClassName(draft.class) or ""))
    f.previewRole:SetText(draft.class and CLASS_ROLES[draft.class] or "")
    local startZone = isDk and Data.DK_START_ZONE_NAME or nil
    if not startZone then
        local zone = State.zones[Data.RACES[draft.race].startZone]
        startZone = zone and zone.n or "the race's starting zone"
    end
    f.previewStart:SetText("Starts in |cffffd100" .. startZone .. "|r on the |cffffd100Autopilot|r order. "
        .. "You can change the order any time on the bot's Orders tab.")
    f.guildMix:SetText(GuildMix())
end

UI.RegisterPage("recruitment", { tab = "recruitment", create = Create, refresh = Refresh })
