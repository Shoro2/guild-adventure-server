-- Dungeons (the dungeon run window), Map (locked until its unlock; content arrives in M9) and Settings.

local GA = GuildAdventure
local UI = GA.UI
local W = UI.W
local Data = GA.Data
local State = GA.State

-------------------------------------------------------------------------------
-- Locked / upcoming feature pages
-------------------------------------------------------------------------------

local function FeaturePage(feature, title, detail)
    local function Create(parent)
        local f = CreateFrame("Frame", nil, parent)
        f.lock = W.LockedOverlay(f)
        return f
    end
    local function Refresh(f)
        if State.HasFeature(feature) then
            f.lock:SetLock(title, "Unlocked - this screen arrives in a later milestone.", detail)
        else
            f.lock:SetLock(title, State.FeatureRequirement(feature), detail)
        end
    end
    return { create = Create, refresh = Refresh }
end

-------------------------------------------------------------------------------
-- Dungeons: the dungeon run window (docs/orders.md "The dungeon run window"). Three steps: the dungeon, the bots of
-- the group, how often and go; beside them the runs that are going. The page key stays "groups".
-------------------------------------------------------------------------------

local RUN_CHOICES = { 1, 2, 3, 5, 10, 0 } -- 0 = until it is stopped
local ROLE_TEXT = { tank = "|cff6fa8ffTank|r", heal = "|cff40ff40Healer|r", dps = "|cffd0d0d0Damage|r" }
local ENDLESS = 255

-- itemsOff: the kinds of items of the guild storage the leader took out (by entry): every other kind a run can use
-- is used, each bot what fits it (it stays for the next runs of the session); rules: the fight rules the leader
-- changed (nil: the server's).
local wizard = { step = 1, dungeon = nil, picked = {}, runs = 1, itemsOff = {}, rules = {} }
local KIND_TEXT = { [1] = "elixir", [2] = "scroll", [3] = "for a weapon", [4] = "scope" }
local LEAD_CHOICES = { 0, 2, 4, 6, 8 }
local EMERGENCY_CHOICES = { 30, 40, 50, 60, 70 }
local PACK_CHOICES = { 0, 2, 3, 4, 5 } -- 0 = no careful pulls
-- How much the group fights on its way (the owner, 2026-11-19): 0 the bosses, 1 the rooms, 2 the rooms in big pulls.
local MODE_TEXT = {
    [0] = "Bosses: only what is in the way",
    [1] = "Clear: every room on the way",
    [2] = "Clear in big pulls",
    [3] = "Tribute run: the guards live",
}
-- The modes the dropdown offers: the tribute run only for Dire Maul North (the owner, 2026-11-21).
local TRIBUTE_DUNGEON = 38
local MODE_CHOICES = { 0, 1, 2 }
local GATHER_YARDS = { 15, 20, 25, 30, 40 }
local GATHER_COUNT = { 4, 6, 8, 10, 12 }

-- The fight rule that counts: what the leader chose, else the server's value.
local function Rule(key, fallback)
    local value = wizard.rules[key]
    if value == nil then
        value = State.dungeonPrep.rules[key]
    end
    if value == nil then
        value = fallback
    end
    return value
end

local function RunOptions()
    local out = {}
    for e in pairs(wizard.itemsOff) do
        out[#out + 1] = e
    end
    table.sort(out)
    local options = { noitems = out }
    if next(wizard.rules) then
        options.tl, options.em = Rule("tl", 4), Rule("em", 50)
        options.cp, options.cpon, options.ff = Rule("cp", 3), Rule("cpon", true), Rule("ff", true)
        -- The mode, and a big pull's yards and creatures.
        options.md, options.gr, options.gc = Rule("md", 0), Rule("gr", 25), Rule("gc", 6)
    end
    return options
end

local function RunsText(n)
    return n == 0 and "Until I stop it" or (n == 1 and "1 run" or (n .. " runs"))
end

local function DungeonList()
    local list = {}
    for _, d in pairs(State.dungeons) do
        list[#list + 1] = d
    end
    -- The guild's order of the dungeons first (ord, from the server), then what cannot be run yet, by level; the raids
    -- after every dungeon (no order: the guild leader sends a raid when he wants, docs/orders.md "Raid runs").
    table.sort(list, function(a, b)
        local oa = a.raid and 2000 or ((a.ord or 0) > 0 and a.ord or 1000)
        local ob = b.raid and 2000 or ((b.ord or 0) > 0 and b.ord or 1000)
        if oa ~= ob then
            return oa < ob
        end
        if (a.min or 0) ~= (b.min or 0) then
            return (a.min or 0) < (b.min or 0)
        end
        return (a.id or 0) < (b.id or 0)
    end)
    return list
end

-- The guild leader's own character as a row of the group step: it can come along on a run, played by hand or by
-- the AI (Watch); it never leads the group (docs/orders.md "The guild leader in a dungeon run").
local function OwnRow()
    local l = State.leader
    if not l or not l.g then
        return nil
    end
    return { g = l.g, n = l.n, class = l.class, lvl = l.lvl, s = l.s, own = true }
end

-- Why a bot cannot be picked for a group right now (nil: it can).
local function BotBlocked(bot)
    if bot.own then
        if bot.s == "offline" then
            return "offline"
        elseif State.BotDungeonRun(bot.g) then
            return "on a run"
        end
        return nil
    end
    if bot.s == "benched" then
        return "benched"
    elseif bot.s == "offline" then
        return "offline"
    elseif bot.s == "controlled" then
        return "you play it"
    elseif State.BotDungeonRun(bot.g) then
        return "on a run"
    elseif not bot.gl then
        return "not in the world"
    end
end

local function PickedBots()
    local list = {}
    for _, bot in ipairs(State.SortedBots("lvl", true)) do
        if wizard.picked[bot.g] then
            list[#list + 1] = bot
        end
    end
    local own = OwnRow()
    if own and wizard.picked[own.g] then
        list[#list + 1] = own
    end
    return list
end

-- Who can be clicked in the group step: the leader's own character first, then the bots.
local function GroupCandidates()
    local list = {}
    local own = OwnRow()
    if own then
        list[1] = own
    end
    for _, bot in ipairs(State.SortedBots("lvl", true)) do
        list[#list + 1] = bot
    end
    return list
end
UI.DungeonGroupCandidates = GroupCandidates

-- The raid's "Whole guild": every candidate that can be picked now, in the list's order (the leader's own character
-- first, then the bots by level), up to the raid's size. Returns how many are picked afterwards.
function UI.PickWholeGuild()
    local dungeon = wizard.dungeon
    if not dungeon or not dungeon.raid then
        return 0
    end
    local size = dungeon.size or 40
    local count = 0
    for g in pairs(wizard.picked) do
        count = count + 1
    end
    for _, bot in ipairs(GroupCandidates()) do
        if count >= size then
            break
        end
        if not wizard.picked[bot.g] and not BotBlocked(bot) then
            wizard.picked[bot.g] = true
            count = count + 1
        end
    end
    UI.RequestRefresh()
    return count
end

local function GroupSummary(dungeon)
    local tanks, heals, damage, low = {}, {}, 0, {}
    local picked = PickedBots()
    for _, bot in ipairs(picked) do
        local role = State.BotGroupRole(bot)
        if role == "tank" then
            tanks[#tanks + 1] = bot.n
        elseif role == "heal" then
            heals[#heals + 1] = bot.n
        else
            damage = damage + 1
        end
        local pct = dungeon and not bot.own and State.BotDungeonReadiness(bot, dungeon.id)
        if pct and pct < 50 then
            low[#low + 1] = bot.n .. " " .. pct .. "%"
        end
    end
    -- (A raid's group is big: the tanks and healers by their number, not their names.)
    local function Names(list, one, many)
        if #list == 0 then
            return "|cffff8040no " .. one .. "|r"
        end
        if #picked > 5 then
            return #list .. " " .. (#list == 1 and one or many)
        end
        return one .. " " .. table.concat(list, ", ")
    end
    local text = string.format("%d of %d: %s, %s, %d damage", #picked, dungeon and dungeon.size or 5,
        Names(tanks, "tank", "tanks"), Names(heals, "healer", "healers"), damage)
    if #low > 0 then
        text = text .. "  |cffff4040not ready: " .. table.concat(low, ", ") .. "|r"
    end
    return text, #picked, #low
end

local function StartRun()
    local d = wizard.dungeon
    local picked = PickedBots()
    if not d or #picked == 0 then
        return
    end
    local guids = {}
    for i, bot in ipairs(picked) do
        guids[i] = bot.g
    end
    GA.Protocol.StartDungeon(d.id, guids, wizard.runs, nil, UI.RequestCallback(
        "The group goes into " .. (d.n or "the dungeon") .. ".", function(ok)
            if ok then
                wizard.step, wizard.picked = 1, {}
                UI.RequestRefresh()
            end
        end), RunOptions())
end

local function CreateDungeonRow(list)
    local row = CreateFrame("Button", nil, list)
    row.shade = row:CreateTexture(nil, "BACKGROUND")
    row.shade:SetAllPoints(row)
    row.shade:SetTexture("Interface\\Buttons\\WHITE8X8")
    row.name = W.Label(row, "", "GameFontNormal")
    row.name:SetPoint("LEFT", row, "LEFT", 6, 0)
    row.name:SetWidth(190)
    row.level = W.Label(row, "", "GameFontHighlightSmall")
    row.level:SetPoint("LEFT", row.name, "RIGHT", 4, 0)
    row.level:SetWidth(70)
    row.info = W.Label(row, "", "GameFontHighlightSmall")
    row.info:SetPoint("LEFT", row.level, "RIGHT", 4, 0)
    row.info:SetPoint("RIGHT", row, "RIGHT", -6, 0)
    row:SetScript("OnClick", function(self)
        local d = self.dungeon
        if d and d.run ~= false and not d.lock then
            wizard.dungeon, wizard.step = d, 2
            UI.RequestRefresh()
        elseif d and d.lock then
            UI.Notify((d.n or "That dungeon") .. " is not open yet: finish " .. ((d.need or "") ~= "" and d.need or "the one before it")
                .. " first.", true)
        end
    end)
    return row
end

local function UpdateDungeonRow(row, d)
    row.dungeon = d
    local ready = 0
    for _, bot in pairs(State.bots) do
        local pct = State.BotDungeonReadiness(bot, d.id)
        if pct and pct >= 50 and not BotBlocked(bot) then
            ready = ready + 1
        end
    end
    local can = d.run ~= false and not d.lock
    row.shade:SetVertexColor(1, 1, 1, wizard.dungeon == d and 0.12 or 0.03)
    row.name:SetText((can and "|cffffd100" or "|cff808080") .. (d.n or "?") .. "|r")
    row.level:SetText(string.format("%d-%d", d.min or 0, d.max or 0))
    if d.run == false then
        row.info:SetText("|cff808080cannot be run yet|r")
    elseif d.lock then
        row.info:SetText("|cffff8040locked:|r |cff808080finish " .. ((d.need or "") ~= "" and d.need or "the one before it") .. " first|r")
    else
        row.info:SetText(string.format("%d of your bots ready%s", ready,
            (d.fin or 0) > 0 and string.format("  |cff40ff40finished %dx|r", d.fin) or "  |cff9d9d9dnot finished yet|r"))
    end
end

local function CreateBotRow(list)
    local row = CreateFrame("Button", nil, list)
    row.shade = row:CreateTexture(nil, "BACKGROUND")
    row.shade:SetAllPoints(row)
    row.shade:SetTexture("Interface\\Buttons\\WHITE8X8")
    row.mark = W.Label(row, "", "GameFontNormal")
    row.mark:SetPoint("LEFT", row, "LEFT", 6, 0)
    row.mark:SetWidth(18)
    row.icon = W.ClassIcon(row, 16)
    row.icon:SetPoint("LEFT", row.mark, "RIGHT", 2, 0)
    row.name = W.Label(row, "", "GameFontNormal")
    row.name:SetPoint("LEFT", row.icon, "RIGHT", 6, 0)
    row.name:SetWidth(120)
    row.level = W.Label(row, "", "GameFontHighlightSmall")
    row.level:SetPoint("LEFT", row.name, "RIGHT", 2, 0)
    row.level:SetWidth(30)
    row.role = W.Label(row, "", "GameFontHighlightSmall")
    row.role:SetPoint("LEFT", row.level, "RIGHT", 2, 0)
    row.role:SetWidth(60)
    row.info = W.Label(row, "", "GameFontHighlightSmall")
    row.info:SetPoint("LEFT", row.role, "RIGHT", 2, 0)
    row.info:SetPoint("RIGHT", row, "RIGHT", -6, 0)
    row:SetScript("OnClick", function(self)
        local bot = self.bot
        if not bot then
            return
        end
        if wizard.picked[bot.g] then
            wizard.picked[bot.g] = nil
        elseif not BotBlocked(bot) then
            local _, count = GroupSummary(wizard.dungeon)
            if count < (wizard.dungeon and wizard.dungeon.size or 5) then
                wizard.picked[bot.g] = true
            else
                UI.Notify("The group is full.", true)
            end
        end
        UI.RequestRefresh()
    end)
    return row
end

local function UpdateBotRow(row, bot)
    row.bot = bot
    local picked = wizard.picked[bot.g]
    local blocked = BotBlocked(bot)
    local d = wizard.dungeon
    row.shade:SetVertexColor(1, 1, 1, picked and 0.14 or 0.03)
    row.mark:SetText(picked and "|cff40ff40X|r" or "")
    row.icon:SetClass(bot.class)
    row.name:SetText((blocked and "|cff808080" or Data.ClassColorCode(bot.class)) .. (bot.n or "?") .. "|r"
        .. (bot.own and " |cffffd100(you)|r" or ""))
    row.level:SetText(tostring(bot.lvl or 0))
    row.role:SetText(ROLE_TEXT[State.BotGroupRole(bot)] or ROLE_TEXT.dps)
    if blocked then
        row.info:SetText("|cff808080" .. blocked .. "|r")
    elseif bot.own then
        row.info:SetText("|cff40ff40your own character|r: play it yourself or let it play (Watch)")
    else
        local pct = d and State.BotDungeonReadiness(bot, d.id)
        local inSet = d and State.BotRoleReadiness(bot, d.id)
        local text = pct and ((pct >= 50 and "|cff40ff40" or "|cffff4040") .. pct .. "% ready|r") or "|cff808080out of its reach|r"
        if inSet then
            text = text .. string.format(" |cff9d9d9d(in its dungeon set %d%%)|r", inSet)
        end
        row.info:SetText(text)
    end
end

local function CreateItemRow(list)
    local row = CreateFrame("Button", nil, list)
    row.shade = row:CreateTexture(nil, "BACKGROUND")
    row.shade:SetAllPoints(row)
    row.shade:SetTexture("Interface\\Buttons\\WHITE8X8")
    row.mark = W.Label(row, "", "GameFontNormal")
    row.mark:SetPoint("LEFT", row, "LEFT", 6, 0)
    row.mark:SetWidth(18)
    row.name = W.Label(row, "", "GameFontHighlightSmall")
    row.name:SetPoint("LEFT", row.mark, "RIGHT", 2, 0)
    row.name:SetWidth(210)
    row.info = W.Label(row, "", "GameFontHighlightSmall")
    row.info:SetPoint("LEFT", row.name, "RIGHT", 4, 0)
    row.info:SetPoint("RIGHT", row, "RIGHT", -6, 0)
    row:SetScript("OnClick", function(self)
        local item = self.item
        if item then
            wizard.itemsOff[item.e] = (not wizard.itemsOff[item.e]) or nil
            UI.RequestRefresh()
        end
    end)
    W.SetTooltip(row, function(self)
        return self.item and self.item.n or nil
    end, function(self)
        return { "The group uses this out of the guild storage. Click to take it out (and to put it back).",
            "Each bot uses only what fits it best: one battle and one guardian elixir, one scroll,",
            "what its weapons take; once at the start of a run and once after a wipe." }
    end)
    return row
end

local function UpdateItemRow(row, item)
    row.item = item
    local on = not wizard.itemsOff[item.e]
    row.shade:SetVertexColor(1, 1, 1, on and 0.14 or 0.03)
    row.mark:SetText(on and "|cff40ff40X|r" or "")
    row.name:SetText((on and "|cffffffff" or "|cffb0b0b0") .. (item.n or "?") .. "|r")
    row.info:SetText(string.format("%d in the storage, %s%s", item.amt or 0, KIND_TEXT[item.k] or "?",
        (item.lvl or 0) > 1 and (", from level " .. item.lvl) or ""))
end

-- One line of the preparation step's talents: a bot of the group and what it goes as.
local function CreateTalentLine(parent, index)
    local line = CreateFrame("Frame", nil, parent)
    line:SetHeight(24)
    line.name = W.Label(line, "", "GameFontNormalSmall")
    line.name:SetPoint("LEFT", line, "LEFT", 4, 0)
    line.name:SetWidth(110)
    line.dd = W.DropDown(line, 250, function()
        local items = {}
        for i, choice in ipairs(line.bot and not line.bot.own and UI.RunTalentChoices(line.bot) or {}) do
            items[i] = { text = choice.text, value = i, choice = choice }
        end
        return items
    end)
    line.dd:SetPoint("LEFT", line.name, "RIGHT", -10, -2)
    line.dd.onSelect = function(_, item)
        local bot, choice = line.bot, item and item.choice
        if bot and choice and not bot.own then
            GA.Protocol.SetRole(bot.g, choice.role, choice.t, UI.RequestCallback(
                (bot.n or "The bot") .. " goes " .. (choice.role == "none" and "as it levels" or ("as " .. choice.text)) .. "."))
        end
    end
    return line
end

local function TalentText(bot)
    if bot.own then
        return "As you play it (your own character)"
    end
    if not bot.dr or bot.dr == "none" or bot.dr == "" then
        return "As it levels"
    end
    return (bot.dr == "tank" and "Tank" or "Healer") .. ((bot.drn and bot.drn ~= "") and (": " .. bot.drn) or "")
end

local function CreateRunRow(list)
    local row = CreateFrame("Frame", nil, list)
    row.shade = row:CreateTexture(nil, "BACKGROUND")
    row.shade:SetAllPoints(row)
    row.shade:SetTexture("Interface\\Buttons\\WHITE8X8")
    row.shade:SetVertexColor(1, 1, 1, 0.04)
    row.stop = W.Button(row, "Stop", 56, 20, function(self)
        local run = self:GetParent().run
        if run then
            UI.Confirm("End the run in " .. (run.dn or "the dungeon") .. " now? The group leaves it at once.", function()
                GA.Protocol.StopDungeon(run.g, true, UI.RequestCallback("The run ends."))
            end)
        end
    end)
    row.stop:SetPoint("TOPRIGHT", row, "TOPRIGHT", -4, -4)
    row.last = W.Button(row, "Last run", 70, 20, function(self)
        local run = self:GetParent().run
        if run then
            GA.Protocol.StopDungeon(run.g, false, UI.RequestCallback("The run that is going is the last."))
        end
    end)
    row.last:SetPoint("RIGHT", row.stop, "LEFT", -4, 0)
    row.title = W.Label(row, "", "GameFontNormal")
    row.title:SetPoint("TOPLEFT", row, "TOPLEFT", 6, -5)
    row.title:SetPoint("RIGHT", row.last, "LEFT", -6, 0)
    row.detail = W.Label(row, "", "GameFontHighlightSmall")
    row.detail:SetPoint("TOPLEFT", row.title, "BOTTOMLEFT", 0, -3)
    row.detail:SetPoint("RIGHT", row, "RIGHT", -6, 0)
    row.detail:SetJustifyV("TOP")
    return row
end

local function UpdateRunRow(row, run)
    row.run = run
    local leader = State.GetBot(run.g)
    row.title:SetText(string.format("|cffffd100%s|r  %s +%d", run.dn or "?", leader and leader.n or "?", #(run.m or {})))
    local where
    if run.ph == 2 then
        where = string.format("inside, %d of %d bosses%s", run.b or 0, run.of or 0,
            (run.nb and run.nb ~= "") and (", next " .. run.nb) or "")
    elseif run.ph == 1 then
        where = "getting ready"
    else
        where = "not started (its leader is not in the world)"
    end
    local runs
    if (run.left or 1) >= ENDLESS then
        runs = string.format("run %d, until you stop it", (run.done or 0) + 1)
    else
        runs = string.format("run %d of %d", (run.done or 0) + 1, (run.done or 0) + (run.left or 1))
    end
    row.detail:SetText(string.format("%s; %s%s%s", where, runs, (run.w or 0) > 0 and (", " .. run.w .. " wipe") or "",
        (run.min or 0) > 0 and (", " .. run.min .. " min") or ""))
    if (run.left or 1) > 1 then
        row.last:Show()
    else
        row.last:Hide()
    end
end

local function CreateDungeons(parent)
    local f = CreateFrame("Frame", nil, parent)
    -- (Each time the tab is opened the dungeons and the runs are asked for anew: what is open changes with every
    -- finished run, and a block the server pushed during a loading screen never arrived.)
    f:SetScript("OnShow", function(self)
        self.askAgain = true
    end)
    f.title = W.Label(f, "Dungeon runs", "GameFontNormalLarge")
    f.title:SetPoint("TOPLEFT", f, "TOPLEFT", 8, -8)
    f.steps = W.Label(f, "", "GameFontHighlight")
    f.steps:SetPoint("LEFT", f.title, "RIGHT", 16, 0)
    f.hint = W.Label(f, "", "GameFontDisableSmall")
    f.hint:SetPoint("TOPLEFT", f.title, "BOTTOMLEFT", 0, -6)
    f.hint:SetPoint("RIGHT", f, "RIGHT", -8, 0)
    f.hint:SetJustifyV("TOP")

    -- Left: the three steps. Right: the runs that are going.
    local left = W.Panel(f)
    left:SetPoint("TOPLEFT", f, "TOPLEFT", 0, -56)
    left:SetPoint("BOTTOMRIGHT", f, "BOTTOM", 60, 0)
    local right = W.Panel(f)
    right:SetPoint("TOPLEFT", left, "TOPRIGHT", 6, 0)
    right:SetPoint("BOTTOMRIGHT", f, "BOTTOMRIGHT", 0, 0)
    f.left, f.right = left, right

    f.dungeons = W.ScrollList(left, { rowHeight = 22, createRow = CreateDungeonRow, updateRow = UpdateDungeonRow })
    f.dungeons:SetPoint("TOPLEFT", left, "TOPLEFT", 6, -6)
    f.dungeons:SetPoint("BOTTOMRIGHT", left, "BOTTOMRIGHT", -6, 6)

    f.bots = W.ScrollList(left, { rowHeight = 22, createRow = CreateBotRow, updateRow = UpdateBotRow })
    f.bots:SetPoint("TOPLEFT", left, "TOPLEFT", 6, -6)
    f.bots:SetPoint("BOTTOMRIGHT", left, "BOTTOMRIGHT", -6, 58)

    f.summary = W.Label(left, "", "GameFontHighlightSmall")
    f.summary:SetPoint("BOTTOMLEFT", left, "BOTTOMLEFT", 8, 34)
    f.summary:SetPoint("RIGHT", left, "RIGHT", -8, 0)
    f.back = W.Button(left, "Back", 70, 22, function()
        wizard.step = math.max(1, wizard.step - 1)
        UI.RequestRefresh()
    end)
    f.back:SetPoint("BOTTOMLEFT", left, "BOTTOMLEFT", 6, 6)
    f.next = W.Button(left, "Next", 90, 22, function()
        if wizard.step == 2 or wizard.step == 3 then
            wizard.step = wizard.step + 1
            UI.RequestRefresh()
        elseif wizard.step == 4 then
            StartRun()
        end
    end)
    f.next:SetPoint("BOTTOMRIGHT", left, "BOTTOMRIGHT", -6, 6)
    -- Step 2 of a raid: the whole guild joins (the owner, 2026-11-27: [quote left out]): the leader's own character and every bot that can be picked now, up to
    -- the raid's size.
    f.wholeGuild = W.Button(left, "Whole guild", 100, 22, function()
        UI.PickWholeGuild()
    end)
    f.wholeGuild:SetPoint("RIGHT", f.next, "LEFT", -6, 0)
    W.SetTooltip(f.wholeGuild, "Whole guild", function()
        return { "Puts the whole guild into the raid: your own character and every bot",
            "that is in the world and not on another run, up to the raid's size.",
            "Click a row afterwards to take it out again." }
    end)
    f.wholeGuild:Hide()

    -- Step 3: the preparation. Talents per bot, what the group may use out of the guild storage, how it fights.
    f.prep = CreateFrame("Frame", nil, left)
    f.prep:SetPoint("TOPLEFT", left, "TOPLEFT", 6, -6)
    f.prep:SetPoint("BOTTOMRIGHT", left, "BOTTOMRIGHT", -6, 58)
    f.prepTalents = W.Label(f.prep, "Talents for the run", "GameFontNormalSmall")
    f.prepTalents:SetPoint("TOPLEFT", f.prep, "TOPLEFT", 4, -2)
    f.talentLines = {}
    for i = 1, 5 do
        local line = CreateTalentLine(f.prep, i)
        line:SetPoint("TOPLEFT", f.prep, "TOPLEFT", 0, -16 - (i - 1) * 24)
        line:SetPoint("RIGHT", f.prep, "RIGHT", 0, 0)
        f.talentLines[i] = line
    end
    f.prepItems = W.Label(f.prep, "", "GameFontNormalSmall")
    f.prepItems:SetPoint("TOPLEFT", f.prep, "TOPLEFT", 4, -142)
    f.items = W.ScrollList(f.prep, { rowHeight = 18, createRow = CreateItemRow, updateRow = UpdateItemRow })
    f.items:SetPoint("TOPLEFT", f.prep, "TOPLEFT", 0, -158)
    f.items:SetPoint("BOTTOMRIGHT", f.prep, "BOTTOMRIGHT", 0, 62)
    f.noItems = W.Label(f.prep, "The guild storage holds nothing a run can use (elixirs, scrolls, stones, oils, scopes).",
        "GameFontDisableSmall")
    f.noItems:SetPoint("TOPLEFT", f.prep, "TOPLEFT", 6, -162)
    f.noItems:SetPoint("RIGHT", f.prep, "RIGHT", -6, 0)
    f.noItems:SetJustifyV("TOP")
    -- How the group fights: two lines of two choices.
    local function RuleDropDown(label, width, choices, textOf, onPick)
        local text = W.Label(f.prep, label, "GameFontNormalSmall")
        local dd = W.DropDown(f.prep, width, function()
            local items = {}
            for _, value in ipairs(choices) do
                items[#items + 1] = { text = textOf(value), value = value }
            end
            return items
        end)
        dd:SetPoint("LEFT", text, "RIGHT", -8, -2)
        dd.onSelect = function(value)
            onPick(value)
            UI.RequestRefresh()
        end
        dd.textOf = textOf
        return text, dd
    end
    f.leadLabel, f.leadDD = RuleDropDown("Wait for the tank", 70, LEAD_CHOICES, function(v)
        return v == 0 and "off" or (v .. " s")
    end, function(v)
        wizard.rules.tl = v
    end)
    f.leadLabel:SetPoint("BOTTOMLEFT", f.prep, "BOTTOMLEFT", 4, 34)
    f.emergencyLabel, f.emergencyDD = RuleDropDown("Healer heals below", 70, EMERGENCY_CHOICES, function(v)
        return v .. " %"
    end, function(v)
        wizard.rules.em = v
    end)
    f.emergencyLabel:SetPoint("LEFT", f.leadLabel, "LEFT", 210, 0)
    f.packLabel, f.packDD = RuleDropDown("Careful pulls", 110, PACK_CHOICES, function(v)
        return v == 0 and "off" or ("from " .. v .. " mobs")
    end, function(v)
        wizard.rules.cpon = v ~= 0
        if v ~= 0 then
            wizard.rules.cp = v
        end
    end)
    f.packLabel:SetPoint("BOTTOMLEFT", f.prep, "BOTTOMLEFT", 4, 6)
    f.fleeBox = W.CheckBox(f.prep, "Focus mobs that run away", function(self)
        wizard.rules.ff = W.IsChecked(self) and true or false
        UI.RequestRefresh()
    end)
    f.fleeBox:SetPoint("LEFT", f.packLabel, "LEFT", 206, 0)

    -- Step 4: how often, and what happens.
    f.start = CreateFrame("Frame", nil, left)
    f.start:SetPoint("TOPLEFT", left, "TOPLEFT", 6, -6)
    f.start:SetPoint("BOTTOMRIGHT", left, "BOTTOMRIGHT", -6, 58)
    f.startText = W.Label(f.start, "", "GameFontHighlight")
    f.startText:SetPoint("TOPLEFT", f.start, "TOPLEFT", 4, -4)
    f.startText:SetPoint("RIGHT", f.start, "RIGHT", -4, 0)
    f.startText:SetJustifyV("TOP")
    f.runsLabel = W.Label(f.start, "How often", "GameFontNormalSmall")
    f.runsLabel:SetPoint("TOPLEFT", f.startText, "BOTTOMLEFT", 0, -16)
    f.runsDD = W.DropDown(f.start, 130, function()
        local items = {}
        for _, n in ipairs(RUN_CHOICES) do
            items[#items + 1] = { text = RunsText(n), value = n }
        end
        return items
    end)
    f.runsDD:SetPoint("LEFT", f.runsLabel, "RIGHT", -6, -2)
    f.runsDD.onSelect = function(value)
        wizard.runs = value
        UI.RequestRefresh()
    end
    local function StartDropDown(label, width, choices, textOf, key)
        local text = W.Label(f.start, label, "GameFontNormalSmall")
        local dd = W.DropDown(f.start, width, function()
            local items = {}
            for _, value in ipairs(choices) do
                items[#items + 1] = { text = textOf(value), value = value }
            end
            return items
        end)
        dd:SetPoint("LEFT", text, "RIGHT", -6, -2)
        dd.onSelect = function(value)
            wizard.rules[key] = value
            UI.RequestRefresh()
        end
        dd.textOf = textOf
        return text, dd
    end
    f.modeLabel, f.modeDD = StartDropDown("How it fights", 200, MODE_CHOICES, function(v)
        return MODE_TEXT[v] or "?"
    end, "md")
    f.modeLabel:SetPoint("TOPLEFT", f.runsLabel, "BOTTOMLEFT", 0, -16)
    f.gatherLabel, f.gatherDD = StartDropDown("Big pull: within", 70, GATHER_YARDS, function(v)
        return v .. " yd"
    end, "gr")
    f.gatherLabel:SetPoint("TOPLEFT", f.modeLabel, "BOTTOMLEFT", 0, -16)
    f.countLabel, f.countDD = StartDropDown("up to", 90, GATHER_COUNT, function(v)
        return v .. " mobs"
    end, "gc")
    f.countLabel:SetPoint("LEFT", f.gatherLabel, "LEFT", 170, 0)
    f.modeHelp = W.Label(f.start, "", "GameFontDisableSmall")
    f.modeHelp:SetPoint("TOPLEFT", f.gatherLabel, "BOTTOMLEFT", 0, -12)
    f.modeHelp:SetPoint("RIGHT", f.start, "RIGHT", -4, 0)
    f.modeHelp:SetJustifyV("TOP")
    f.flow = W.Label(f.start, "The group is taken into the dungeon from wherever its bots are; tanks and healers change to "
        .. "their dungeon talents and sets. 30 seconds after the last boss everybody goes to their hearthstone place. With "
        .. "more runs the dungeon is reset and the group goes in again.", "GameFontDisableSmall")
    f.flow:SetPoint("TOPLEFT", f.modeHelp, "BOTTOMLEFT", 0, -12)
    f.flow:SetPoint("RIGHT", f.start, "RIGHT", -4, 0)
    f.flow:SetJustifyV("TOP")

    f.runsTitle = W.Label(right, "Runs that are going", "GameFontNormal")
    f.runsTitle:SetPoint("TOPLEFT", right, "TOPLEFT", 8, -8)
    f.runs = W.ScrollList(right, { rowHeight = 46, createRow = CreateRunRow, updateRow = UpdateRunRow })
    f.runs:SetPoint("TOPLEFT", right, "TOPLEFT", 6, -28)
    f.runs:SetPoint("BOTTOMRIGHT", right, "BOTTOMRIGHT", -6, 6)
    f.noRuns = W.Label(right, "No group is on a dungeon run.", "GameFontDisableSmall", "CENTER")
    f.noRuns:SetPoint("CENTER", right, "CENTER", 0, 0)
    return f
end

local function RefreshDungeons(f)
    if State.IsConnected() and ((not State.dungeonsLoaded and not f.asked) or f.askAgain) then
        f.asked, f.askAgain = true, nil
        GA.Protocol.Get("dungeons")
        GA.Protocol.Get("dungeonruns")
    end
    -- (What the storage holds changes: asked for anew whenever the preparation step is opened.)
    if wizard.step == 3 and f.prepStep ~= 3 and State.IsConnected() then
        GA.Protocol.Get("dungeonprep")
    end
    f.prepStep = wizard.step
    local d = wizard.dungeon
    -- (A dungeon list that came anew: the chosen record is the new one.)
    if d and State.dungeons[d.id] then
        d = State.dungeons[d.id]
        wizard.dungeon = d
    end
    if wizard.step > 1 and not d then
        wizard.step = 1
    end
    local own = OwnRow()
    for g in pairs(wizard.picked) do
        local bot = State.GetBot(g) or (own and own.g == g and own) or nil
        if not bot or BotBlocked(bot) then
            wizard.picked[g] = nil
        end
    end
    local step = wizard.step
    local names = { "1 Dungeon", "2 Group", "3 Preparation", "4 Start" }
    for i = 1, 4 do
        names[i] = (i == step and "|cffffd100" or "|cff808080") .. names[i] .. "|r"
    end
    f.steps:SetText(table.concat(names, "  >  "))

    f.dungeons:Hide()
    f.bots:Hide()
    f.prep:Hide()
    f.start:Hide()
    f.summary:SetText("")
    f.back:Hide()
    f.next:Hide()
    f.wholeGuild:Hide()
    if step == 1 then
        f.hint:SetText("Choose the dungeon or raid. The guild plays the dungeons in their order: a run that brings the end boss "
            .. "down opens the next one. The raids (last) are open: a raid group of up to 40 bots goes in once a week.")
        f.dungeons:Show()
        f.dungeons:SetItems(DungeonList())
    else
        local text, count, low = GroupSummary(d)
        f.summary:SetText(text)
        f.back:Show()
        f.next:Show()
        if step == 2 then
            f.hint:SetText(d.raid and string.format("%s (raid, level %d-%d): click the bots of the raid, up to %d. A raid wants "
                .. "several tanks (the first leads) and a healer for every four or five others, by their dungeon role. Your own "
                .. "character (first row) can come along.", d.n or "?", d.min or 0, d.max or 0, d.size or 40)
                or string.format("%s (level %d-%d): click the bots of the group, up to %d. A tank and a healer by their "
                .. "dungeon role (bot detail, Dungeon role) or their talents make the run safe. Your own character (first row) "
                .. "can come along.", d.n or "?", d.min or 0, d.max or 0, d.size or 5))
            f.bots:Show()
            f.bots:SetItems(GroupCandidates())
            if d.raid then
                f.wholeGuild:Show()
            end
            f.next:SetText("Next")
            -- (A bot leads the group: the leader's own character alone is no group.)
            W.SetEnabled(f.next, count - ((own and wizard.picked[own.g]) and 1 or 0) > 0)
        elseif step == 3 then
            f.hint:SetText("Preparation: what each bot goes as, what the group may use out of the guild storage, and how it "
                .. "fights. Nothing here has to be changed.")
            f.prep:Show()
            local picked = PickedBots()
            for i, line in ipairs(f.talentLines) do
                local bot = picked[i]
                line.bot = bot
                if bot then
                    line.name:SetText(Data.ClassColorCode(bot.class) .. (bot.n or "?") .. "|r")
                    line.dd:SetSelected(nil, TalentText(bot))
                    line:Show()
                else
                    line:Hide()
                end
            end
            local items = State.dungeonPrep.items
            local chosen = 0
            for _, item in ipairs(items) do
                chosen = chosen + (wizard.itemsOff[item.e] and 0 or 1)
            end
            f.prepItems:SetText(string.format("From the guild storage (%d of %d kinds are used; click to take one out)", chosen, #items))
            f.items:SetItems(items)
            if #items == 0 then
                f.noItems:Show()
            else
                f.noItems:Hide()
            end
            local lead, emergency = Rule("tl", 4), Rule("em", 50)
            local pack = Rule("cpon", true) and Rule("cp", 3) or 0
            f.leadDD:SetSelected(lead, f.leadDD.textOf(lead))
            f.emergencyDD:SetSelected(emergency, f.emergencyDD.textOf(emergency))
            f.packDD:SetSelected(pack, f.packDD.textOf(pack))
            f.fleeBox:SetChecked(Rule("ff", true) and true or false)
            f.next:SetText("Next")
            W.SetEnabled(f.next, count > 0)
        else
            f.hint:SetText("How often the group runs the dungeon, then go.")
            f.start:Show()
            local tribute = d.id == TRIBUTE_DUNGEON
            MODE_CHOICES[4] = tribute and 3 or nil
            if not tribute and wizard.rules.md == 3 then
                wizard.rules.md = nil
            end
            local mode, yards, count = Rule("md", 0), Rule("gr", 25), Rule("gc", 6)
            if mode == 3 and not tribute then
                mode = 0
            end
            f.modeDD:SetSelected(mode, f.modeDD.textOf(mode))
            f.gatherDD:SetSelected(yards, f.gatherDD.textOf(yards))
            f.countDD:SetSelected(count, f.countDD.textOf(count))
            if mode == 2 then
                f.gatherLabel:Show()
                f.gatherDD:Show()
                f.countLabel:Show()
                f.countDD:Show()
            else
                f.gatherLabel:Hide()
                f.gatherDD:Hide()
                f.countLabel:Hide()
                f.countDD:Hide()
            end
            f.modeHelp:SetText(mode == 3 and ("Keeps the guards and the other named ogres alive and kills only King Gordok: "
                    .. "the group takes Fengus's key, repairs the trap for Slip'kik, makes the ogre suit for Kromcrush and leaves "
                    .. "Cho'Rush alone. Needs from the guild storage: 1 Frost Oil, 1 Thorium Widget, 4 Bolts of Runecloth, "
                    .. "8 Rugged Leather, 2 Rune Thread.")
                or mode == 0 and "Fights only what would attack the group on its way, one pack at a time."
                or mode == 1 and "Clears every room it walks through, wide around its way in the open, one pack at a time."
                or string.format("Clears every room it walks through; the tank gathers what stands within %d yd and brings up to "
                    .. "%d mobs to the group at once.", yards, count))
            local list = {}
            for _, bot in ipairs(PickedBots()) do
                list[#list + 1] = Data.ClassColorCode(bot.class) .. (bot.n or "?") .. "|r"
            end
            f.startText:SetText(string.format("|cffffd100%s|r with %s", d.n or "?", table.concat(list, ", ")))
            f.runsDD:SetSelected(wizard.runs, RunsText(wizard.runs))
            f.next:SetText("Start")
            W.SetEnabled(f.next, count > 0 and low == 0 and State.IsConnected())
        end
    end

    f.runs:SetItems(State.dungeonRuns)
    if #State.dungeonRuns == 0 then
        f.noRuns:Show()
    else
        f.noRuns:Hide()
    end
end

-- For tests and other screens: the wizard's state, and a start with a dungeon chosen.
UI.DungeonWizard = wizard
function UI.OpenDungeonRun(dungeonId)
    wizard.dungeon = dungeonId and State.dungeons[dungeonId] or nil
    wizard.step = wizard.dungeon and 2 or 1
    wizard.picked = {}
    UI.RequestRefresh()
end

UI.RegisterPage("groups", { tab = "groups", create = CreateDungeons, refresh = RefreshDungeons })

local map = FeaturePage("map", "World map",
    "See where every bot is and which zone it is assigned to, right on the world map.")
map.tab = "map"
UI.RegisterPage("map", map)

-------------------------------------------------------------------------------
-- Settings: two views, "Bot behaviour" (the guild's bot settings, UI/Behaviour.lua; needs the server) and "Addon"
-- (this addon's own options; works without a server connection)
-------------------------------------------------------------------------------

local SETTINGS_VIEWS = { { key = "behaviour", label = "Bot behaviour" }, { key = "addon", label = "Addon" } }

local function CreateAddonOptions(parent)
    local f = CreateFrame("Frame", nil, parent)
    local db = GA.db.ui

    local uiHeader = W.Header(f, "Interface")
    uiHeader:SetPoint("TOPLEFT", f, "TOPLEFT", 8, -8)

    local sliderName = W.Name("Slider")
    local slider = CreateFrame("Slider", sliderName, f, "OptionsSliderTemplate")
    slider:SetWidth(200)
    slider:SetHeight(16)
    slider:SetPoint("TOPLEFT", uiHeader, "BOTTOMLEFT", 6, -26)
    slider:SetMinMaxValues(0.7, 1.3)
    slider:SetValueStep(0.05)
    _G[sliderName .. "Low"]:SetText("70%")
    _G[sliderName .. "High"]:SetText("130%")
    slider.label = _G[sliderName .. "Text"]
    local function ScaleLabel(value)
        slider.label:SetText("Window scale: " .. math.floor(value * 100 + 0.5) .. "%")
    end
    slider:SetValue(db.scale or 1)
    ScaleLabel(db.scale or 1)
    slider:SetScript("OnValueChanged", function(self, value)
        ScaleLabel(value)
    end)
    -- apply on release, so the window does not move under the cursor while dragging
    slider:SetScript("OnMouseUp", function(self)
        local value = math.floor(self:GetValue() * 20 + 0.5) / 20
        UI.ApplyScale(value)
    end)
    f.slider = slider

    f.minimap = W.CheckBox(f, "Show the minimap button", function(self)
        UI.SetMinimapButtonShown(W.IsChecked(self))
    end)
    f.minimap:SetPoint("TOPLEFT", slider, "BOTTOMLEFT", -6, -24)
    f.chat = W.CheckBox(f, "Print guild events to the chat window", function(self)
        GA.db.ui.chatEvents = W.IsChecked(self)
    end)
    f.chat:SetPoint("TOPLEFT", f.minimap, "BOTTOMLEFT", 0, -4)
    f.away = W.CheckBox(f, "Show \"While you were away\" after logging in", function(self)
        GA.db.ui.awayPopup = W.IsChecked(self)
    end)
    f.away:SetPoint("TOPLEFT", f.chat, "BOTTOMLEFT", 0, -4)

    local connHeader = W.Header(f, "Connection")
    connHeader:SetPoint("TOPLEFT", f.away, "BOTTOMLEFT", 6, -20)
    f.mode = W.Label(f, "", "GameFontHighlight")
    f.mode:SetPoint("TOPLEFT", connHeader, "BOTTOMLEFT", 0, -10)
    f.mode:SetPoint("RIGHT", f, "RIGHT", -10, 0)
    f.version = W.Label(f, "", "GameFontDisableSmall")
    f.version:SetPoint("TOPLEFT", f.mode, "BOTTOMLEFT", 0, -4)
    f.version:SetPoint("RIGHT", f, "RIGHT", -10, 0)

    f.reconnect = W.Button(f, "Reconnect", 110, 22, function()
        GA.Protocol.Connect(true)
    end)
    f.reconnect:SetPoint("TOPLEFT", f.version, "BOTTOMLEFT", 0, -12)
    f.demo = W.Button(f, "Start demo mode", 140, 22, function()
        if GA.MockServer.IsEnabled() then
            GA.MockServer.Disable()
        else
            GA.MockServer.Enable()
        end
    end)
    f.demo:SetPoint("LEFT", f.reconnect, "RIGHT", 6, 0)
    f.resetDemo = W.Button(f, "Reset demo guild", 130, 22, function()
        UI.Confirm("Reset the demo guild? All simulated progress is lost.", function()
            GA.MockServer.Reset()
        end)
    end)
    f.resetDemo:SetPoint("LEFT", f.demo, "RIGHT", 6, 0)
    f.demoHint = W.Label(f, "Demo commands: /ga mock renown <n> adds renown, /ga mock away simulates an offline window.",
        "GameFontDisableSmall")
    f.demoHint:SetPoint("TOPLEFT", f.reconnect, "BOTTOMLEFT", 0, -10)
    f.demoHint:SetPoint("RIGHT", f, "RIGHT", -10, 0)
    f.demoHint:SetHeight(28)
    f.demoHint:SetJustifyV("TOP")

    f.debug = W.CheckBox(f, "Protocol debug output in chat", function(self)
        GA.db.debug = W.IsChecked(self)
    end)
    f.debug:SetPoint("TOPLEFT", f.demoHint, "BOTTOMLEFT", -4, -12)
    return f
end

local function CreateSettings(parent)
    local page = CreateFrame("Frame", nil, parent)
    page.viewButtons = {}
    for i, view in ipairs(SETTINGS_VIEWS) do
        local button = W.Button(page, view.label, 120, 22, function()
            GA.db.ui.settingsView = view.key
            UI.RequestRefresh()
        end)
        if i == 1 then
            button:SetPoint("TOPLEFT", page, "TOPLEFT", 6, -4)
        else
            button:SetPoint("LEFT", page.viewButtons[i - 1], "RIGHT", 4, 0)
        end
        button.viewKey = view.key
        page.viewButtons[i] = button
    end
    page.behaviour = UI.CreateBehaviourPanel(page, {
        scope = function()
            return 0
        end,
        intro = "How your guild's bots play. A value set here holds for every bot of the guild; a bot can have its own "
            .. "in its Behaviour tab (Roster, the bot). Hover a setting for what it does.",
    })
    page.behaviour:SetPoint("TOPLEFT", page, "TOPLEFT", 0, -30)
    page.behaviour:SetPoint("BOTTOMRIGHT", page, "BOTTOMRIGHT", 0, 0)
    page.addon = CreateAddonOptions(page)
    page.addon:SetPoint("TOPLEFT", page, "TOPLEFT", 0, -30)
    page.addon:SetPoint("BOTTOMRIGHT", page, "BOTTOMRIGHT", 0, 0)
    return page
end

local MODE_TEXT = {
    connected = "|cff40ff40connected|r",
    connecting = "|cffffd100connecting...|r",
    failed = "|cffff4040no answer|r",
    missing = "|cffff4040server module not installed|r",
    badversion = "|cffff4040addon too old|r",
    idle = "|cff9d9d9dwaiting|r",
}

local function RefreshAddonOptions(f)
    local db = GA.db.ui
    f.minimap:SetChecked(not db.minimapHide)
    f.chat:SetChecked(db.chatEvents)
    f.away:SetChecked(db.awayPopup)
    f.debug:SetChecked(GA.db.debug)
    local source = State.mode == "mock" and "Demo mode (simulated server)" or "Live server"
    f.mode:SetText(source .. ": " .. (MODE_TEXT[State.connection] or State.connection))
    f.version:SetText(string.format("Addon %s, protocol v%d%s", GA.VERSION, GA.PROTOCOL_VERSION,
        State.server.srv and (", server " .. State.server.srv) or ""))
    f.demo:SetText(GA.MockServer.IsEnabled() and "Leave demo mode" or "Start demo mode")
    W.SetEnabled(f.resetDemo, GA.MockServer.IsEnabled())
end

local function RefreshSettings(page)
    local view = GA.db.ui.settingsView == "addon" and "addon" or "behaviour"
    for _, button in ipairs(page.viewButtons) do
        if button.viewKey == view then
            button:LockHighlight()
        else
            button:UnlockHighlight()
        end
    end
    if view == "addon" then
        page.behaviour:Hide()
        page.addon:Show()
        RefreshAddonOptions(page.addon)
    else
        page.addon:Hide()
        page.behaviour:Show()
        UI.RefreshBehaviourPanel(page.behaviour)
    end
end

-- The view of the Settings tab ("behaviour" or "addon"), for /ga and the tests.
function UI.ShowSettingsView(view)
    GA.db.ui.settingsView = view
    UI.ShowPage("settings")
    UI.RequestRefresh()
end

UI.RegisterPage("settings", { tab = "settings", create = CreateSettings, refresh = RefreshSettings, offline = true })

-------------------------------------------------------------------------------
-- Event notifications in chat
-------------------------------------------------------------------------------

-- Only milestones go to chat; quests, orders and info stay in the event feed.
local CHAT_EVENTS = {
    LEVEL_UP = true, DEATH = true, ZONE_CHANGE = true, ZONE_DONE = true, PROF_UP = true,
    RENOWN_LEVEL = true, UNLOCK = true, RECRUIT = true, DISMISS = true, CONTROL = true, ACHIEVEMENT = true, MILESTONE = true, PERK = true, GEAR = true,
}

GA.On("EVENT", function(event)
    if GA.db and GA.db.ui.chatEvents and event.msg and CHAT_EVENTS[event.type] then
        DEFAULT_CHAT_FRAME:AddMessage("|cff33ff99[GA]|r " .. (Data.EVENT_COLORS[event.type] or "") .. event.msg .. "|r")
    end
end)
