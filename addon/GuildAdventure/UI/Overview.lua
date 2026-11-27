-- Overview tab: guild name, renown, slots, offline window, money, next unlocks, events.

local GA = GuildAdventure
local UI = GA.UI
local W = UI.W
local Data = GA.Data
local State = GA.State

-- A label/value line. It is a frame (not a font string) so it can carry a tooltip.
local function StatRow(parent, label, anchor, yOffset)
    local row = CreateFrame("Frame", nil, parent)
    row:SetHeight(14)
    row:SetWidth(336)
    row:SetPoint("TOPLEFT", anchor, "BOTTOMLEFT", 0, yOffset or -8)
    local l = W.Label(row, label, "GameFontNormalSmall")
    l:SetPoint("LEFT", row, "LEFT", 0, 0)
    l:SetWidth(120)
    local v = W.Label(row, "", "GameFontHighlightSmall")
    v:SetPoint("LEFT", l, "RIGHT", 6, 0)
    v:SetWidth(210)
    row.value = v
    return row
end

local function CreateEventRow(list, index)
    local row = CreateFrame("Frame", nil, list)
    row.time = W.Label(row, "", "GameFontDisableSmall")
    row.time:SetPoint("LEFT", row, "LEFT", 4, 0)
    row.time:SetWidth(70)
    row.text = W.Label(row, "", "GameFontHighlightSmall")
    row.text:SetPoint("LEFT", row.time, "RIGHT", 4, 0)
    row.text:SetPoint("RIGHT", row, "RIGHT", -2, 0)
    row:EnableMouse(true)
    W.SetTooltip(row, function(self)
        return self.event and Data.FormatAgo(State.ServerNow() - (self.event.t or 0)) or nil
    end, function(self)
        return self.event and { self.event.msg or "" } or nil
    end)
    return row
end

local function UpdateEventRow(row, event)
    row.event = event
    row.time:SetText(Data.FormatAgo(State.ServerNow() - (event.t or 0)))
    row.text:SetText((Data.EVENT_COLORS[event.type] or "|cffcccccc") .. (event.msg or "") .. "|r")
end

local function Create(parent)
    local f = CreateFrame("Frame", nil, parent)

    -- left column
    local left = CreateFrame("Frame", nil, f)
    left:SetPoint("TOPLEFT", f, "TOPLEFT", 4, -4)
    left:SetPoint("BOTTOMLEFT", f, "BOTTOMLEFT", 4, 4)
    left:SetWidth(350)

    f.guild = W.Label(left, "", "GameFontNormalLarge")
    f.guild:SetPoint("TOPLEFT", left, "TOPLEFT", 4, -4)
    f.guild:SetWidth(340)
    f.subtitle = W.Label(left, "", "GameFontHighlightSmall")
    f.subtitle:SetPoint("TOPLEFT", f.guild, "BOTTOMLEFT", 0, -4)

    f.renownLabel = W.Label(left, "", "GameFontNormal")
    f.renownLabel:SetPoint("TOPLEFT", f.subtitle, "BOTTOMLEFT", 0, -14)
    f.renownBar = W.Bar(left, 330, 16, 1, 0.5, 0)
    f.renownBar:SetPoint("TOPLEFT", f.renownLabel, "BOTTOMLEFT", 0, -6)
    W.SetTooltip(f.renownBar, "Guild Renown", {
        "Earned by your bots: level-ups, quests, profession milestones and more.",
        "Renown levels unlock bot slots, a longer offline window and new features.",
    })

    f.slots = StatRow(left, "Bot slots", f.renownBar, -14)
    f.offline = StatRow(left, "Offline window", f.slots)
    f.bank = StatRow(left, "Guild bank", f.offline)
    f.botGold = StatRow(left, "Bots' gold", f.bank)
    f.states = StatRow(left, "Bots", f.botGold)
    f.playing = StatRow(left, "You are playing", f.states)
    f.achievements = StatRow(left, "Achievements", f.playing)
    W.SetTooltip(f.offline, "Offline window", {
        "How long your guild keeps adventuring after you log out.",
        "Starts at 1 hour; Renown unlocks up to 8 hours.",
    })
    W.SetTooltip(f.achievements, "Guild achievements", {
        "Achievements are guild-wide: when one of your characters earns one, the whole guild has it",
        "and every member gets it in its own achievement list.",
        "Each new guild achievement gives renown (its achievement points, at least 5).",
    })

    f.nextHeader = W.Header(left, "Next unlocks")
    f.nextHeader:SetPoint("TOPLEFT", f.achievements, "BOTTOMLEFT", 0, -16)
    f.nextLines = {}
    local anchor = f.nextHeader
    for i = 1, 4 do
        local line = W.Label(left, "", "GameFontHighlightSmall")
        line:SetPoint("TOPLEFT", anchor, "BOTTOMLEFT", 0, i == 1 and -8 or -3)
        line:SetWidth(340)
        f.nextLines[i] = line
        anchor = line
    end

    f.rosterButton = W.Button(left, "Roster", 100, 22, function()
        UI.ShowPage("roster")
    end)
    f.rosterButton:SetPoint("BOTTOMLEFT", left, "BOTTOMLEFT", 0, 0)
    f.recruitButton = W.Button(left, "Recruit", 100, 22, function()
        UI.ShowPage("recruitment")
    end)
    f.recruitButton:SetPoint("LEFT", f.rosterButton, "RIGHT", 6, 0)
    f.awayButton = W.Button(left, "While you were away", 140, 22, function()
        UI.ShowAwaySummary()
    end)
    f.awayButton:SetPoint("LEFT", f.recruitButton, "RIGHT", 6, 0)

    -- right column: the leader (its order, UI/LeaderPanel.lua), then the event feed
    f.leader = UI.CreateLeaderPanel(f)
    f.leader:SetPoint("TOPLEFT", left, "TOPRIGHT", 10, 0)
    f.leader:SetPoint("RIGHT", f, "RIGHT", -4, 0)
    local feed = W.Panel(f)
    feed:SetPoint("TOPLEFT", f.leader, "BOTTOMLEFT", 0, -8)
    feed:SetPoint("BOTTOMRIGHT", f, "BOTTOMRIGHT", -4, 4)
    local feedTitle = W.Label(feed, "Recent events", "GameFontNormal")
    feedTitle:SetPoint("TOPLEFT", feed, "TOPLEFT", 10, -8)
    f.events = W.ScrollList(feed, { rowHeight = 18, createRow = CreateEventRow, updateRow = UpdateEventRow })
    f.events:SetPoint("TOPLEFT", feed, "TOPLEFT", 6, -26)
    f.events:SetPoint("BOTTOMRIGHT", feed, "BOTTOMRIGHT", -6, 6)
    f.noEvents = W.Label(feed, "Nothing happened yet.", "GameFontDisableSmall", "CENTER")
    f.noEvents:SetPoint("CENTER", feed, "CENTER", 0, 0)

    return f
end

local function NextUnlockLines(g)
    local lines = {}
    if not g.rn or g.rn == 0 then
        lines[1] = "|cff40ff40All renown levels reached.|r"
        return lines
    end
    local nextLevel = (g.rl or 1) + 1
    for _, u in ipairs(State.UnlocksAtLevel(nextLevel)) do
        local cost = (u.gold or 0) > 0 and (" (" .. Data.FormatMoney(u.gold) .. ")") or ""
        lines[#lines + 1] = "|cffffd100Renown " .. nextLevel .. ":|r " .. (u.desc or "") .. cost
    end
    for _, u in ipairs(State.unlocks) do
        if u.st == "available" and #lines < 4 then
            lines[#lines + 1] = "|cff40ff40Ready to buy:|r " .. (u.desc or "") .. " (" .. Data.FormatMoney(u.gold or 0) .. ")"
        end
    end
    return lines
end

local function Refresh(f)
    local g = State.guild
    f.guild:SetText(g.guild or "Your guild")
    local faction = g.fac == "H" and "|cffff4040Horde|r" or (g.fac == "A" and "|cff4080ffAlliance|r" or "")
    f.subtitle:SetText(faction .. (State.control.ln and (" - Guild leader " .. State.control.ln) or ""))

    f.renownLabel:SetText("Renown level " .. (g.rl or 1))
    if g.rn and g.rn > 0 then
        f.renownBar:SetProgress(g.rp or 0, g.rn, (g.rp or 0) .. " / " .. g.rn .. "  (total " .. (g.rt or 0) .. ")")
    else
        f.renownBar:SetProgress(1, 1, "Maximum renown level (total " .. (g.rt or 0) .. ")")
    end

    local bots = State.bots
    local counts = { active = 0, benched = 0, controlled = 0, offline = 0 }
    local gold = 0
    for _, bot in pairs(bots) do
        counts[bot.s or "active"] = (counts[bot.s or "active"] or 0) + 1
        gold = gold + (bot.gold or 0)
    end
    local used = g.used or State.BotCount()
    f.slots.value:SetText(used .. " / " .. (g.slots or 0) .. " used  |cff9d9d9d(max " .. (g.max or 39) .. ")|r")
    f.offline.value:SetText(Data.FormatMinutes(g.ow or 0) .. "  |cff9d9d9d(max " .. Data.FormatMinutes(g.owmax or 480) .. ")|r")
    f.bank.value:SetText(Data.FormatMoney(g.bank or 0) .. ((g.tax or 0) > 0 and (" |cff808080(tax " .. g.tax .. "% of the bots' income)|r") or ""))
    f.botGold.value:SetText(Data.FormatMoney(gold))
    f.states.value:SetText(string.format("%d active, %d benched, %d played", counts.active, counts.benched, counts.controlled))
    if State.IsControllingBot() then
        f.playing.value:SetText("|cff40c0ff" .. (State.control.cn or "?") .. "|r (a bot)")
    else
        f.playing.value:SetText(State.control.ln or UnitName("player"))
    end

    f.achievements.value:SetText(string.format("%d + %d / %d tiers  |cff9d9d9d(%d points, +%d renown)|r", g.ach or 0, g.ms or 0,
        g.msa or 0, (g.achp or 0) + (g.msp or 0), (g.achr or 0) + (g.msr or 0)))

    local lines = NextUnlockLines(g)
    for i = 1, #f.nextLines do
        f.nextLines[i]:SetText(lines[i] or "")
    end

    UI.RefreshLeaderPanel(f.leader)

    W.SetEnabled(f.recruitButton, State.HasFeature("recruit") and State.FreeSlots() > 0)
    W.SetEnabled(f.awayButton, State.away ~= nil or UI.lastAway ~= nil)

    f.events:SetItems(State.events)
    if #State.events == 0 then
        f.noEvents:Show()
    else
        f.noEvents:Hide()
    end
end

UI.RegisterPage("overview", { tab = "overview", create = Create, refresh = Refresh })
