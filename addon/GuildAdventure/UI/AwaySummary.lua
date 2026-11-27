-- "While you were away" summary: what the guild did during the offline window.

local GA = GuildAdventure
local UI = GA.UI
local W = UI.W
local Data = GA.Data
local State = GA.State

local MAX_LINES = 12
local frame
UI.lastAway = nil

local function Create()
    frame = CreateFrame("Frame", "GuildAdventureAwayFrame", UIParent)
    frame:SetWidth(440)
    frame:SetHeight(360)
    frame:SetPoint("CENTER", UIParent, "CENTER", 0, 60)
    frame:SetFrameStrata("DIALOG")
    frame:SetToplevel(true)
    frame:SetBackdrop(W.BACKDROP_DIALOG)
    W.DarkenBackground(frame, 0.55)
    frame:EnableMouse(true)
    frame:SetMovable(true)
    frame:RegisterForDrag("LeftButton")
    frame:SetScript("OnDragStart", function(self)
        self:StartMoving()
    end)
    frame:SetScript("OnDragStop", function(self)
        self:StopMovingOrSizing()
    end)
    tinsert(UISpecialFrames, "GuildAdventureAwayFrame")

    local header = frame:CreateTexture(nil, "ARTWORK")
    header:SetTexture("Interface\\DialogFrame\\UI-DialogBox-Header")
    header:SetWidth(320)
    header:SetHeight(64)
    header:SetPoint("TOP", frame, "TOP", 0, 12)
    local title = W.Label(frame, "While you were away", "GameFontNormal", "CENTER")
    title:SetPoint("TOP", header, "TOP", 0, -14)

    frame.period = W.Label(frame, "", "GameFontHighlight", "CENTER")
    frame.period:SetPoint("TOP", frame, "TOP", 0, -36)
    frame.totals = W.Label(frame, "", "GameFontHighlightSmall", "CENTER")
    frame.totals:SetPoint("TOP", frame.period, "BOTTOM", 0, -8)
    frame.totals:SetWidth(400)

    frame.lines = {}
    local anchor = frame.totals
    for i = 1, MAX_LINES do
        local line = W.Label(frame, "", "GameFontHighlightSmall")
        line:SetPoint("TOPLEFT", anchor, i == 1 and "BOTTOMLEFT" or "BOTTOMLEFT", i == 1 and 10 or 0, i == 1 and -14 or -4)
        line:SetWidth(380)
        frame.lines[i] = line
        anchor = line
    end
    frame.more = W.Label(frame, "", "GameFontDisableSmall")
    frame.more:SetPoint("TOPLEFT", anchor, "BOTTOMLEFT", 0, -4)

    frame.ok = W.Button(frame, "Back to adventure", 150, 24, function()
        frame:Hide()
    end)
    frame.ok:SetPoint("BOTTOM", frame, "BOTTOM", -80, 18)
    frame.roster = W.Button(frame, "Open roster", 120, 24, function()
        frame:Hide()
        UI.Show("roster")
    end)
    frame.roster:SetPoint("LEFT", frame.ok, "RIGHT", 8, 0)
    frame:Hide()
    -- set after the initial Hide, or creating the frame would acknowledge the summary
    frame:SetScript("OnHide", function()
        if State.away then
            GA.Protocol.AckAway()
        end
    end)
end

local function Fill(summary)
    local minutes = math.floor(((summary.to or 0) - (summary.from or 0)) / 60)
    local period = Data.FormatMinutes(minutes)
    if summary.from and summary.from > 0 then
        period = period .. "  |cff9d9d9d(" .. date("%H:%M", summary.from) .. " - " .. date("%H:%M", summary.to or summary.from) .. ")|r"
    end
    frame.period:SetText(period)
    frame.totals:SetText(string.format("|cffffd100+%d|r levels   |cffff8000+%d|r renown   %s   |cff40ff40%d|r quests   |cffff4040%d|r deaths",
        summary.lv or 0, summary.rp or 0, Data.FormatMoney(summary.gold or 0), summary.q or 0, summary.d or 0))
    local lines = summary.lines or {}
    for i = 1, MAX_LINES do
        local entry = lines[i]
        if entry then
            local bot = State.GetBot(entry.g)
            local color = bot and Data.ClassColorCode(bot.class) or "|cffffffff"
            frame.lines[i]:SetText(color .. "-|r " .. (entry.msg or ""))
        else
            frame.lines[i]:SetText("")
        end
    end
    if #lines > MAX_LINES then
        frame.more:SetText("... and " .. (#lines - MAX_LINES) .. " more bots.")
    else
        frame.more:SetText("")
    end
end

function UI.ShowAwaySummary()
    local summary = State.away or UI.lastAway
    if not summary then
        return
    end
    if not frame then
        Create()
    end
    Fill(summary)
    frame:Show()
end

GA.On("AWAY", function()
    if State.away then
        UI.lastAway = State.away
        if GA.db.ui.awayPopup then
            UI.ShowAwaySummary()
        end
    end
end)
