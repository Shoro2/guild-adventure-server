-- Progression tab: renown track and the unlock list (slots, offline window, features).
-- Locked steps stay visible with their requirement so the player sees what comes next.

local GA = GuildAdventure
local UI = GA.UI
local W = UI.W
local Data = GA.Data
local State = GA.State

local KIND_ICONS = {
    slots = "Interface\\Icons\\INV_Misc_GroupNeedMore",
    offline = "Interface\\Icons\\INV_Misc_PocketWatch_01",
    feature = "Interface\\Icons\\INV_Misc_Key_03",
}

-- What pays for a gold unlock (STATE^ulfrom, bot setting Unlocks.PayFrom) and what it can pay now (STATE^ulgold;
-- an older server sends neither: the guild bank).
local PAY_FROM = {
    bank = "from the guild bank",
    bankleader = "from the guild bank (what it lacks from your guild leader's gold)",
    leader = "from your guild leader's gold",
}

local function PayFromText()
    return PAY_FROM[State.guild.ulfrom or "bank"] or PAY_FROM.bank
end

local function Funds()
    local g = State.guild
    return g.ulgold or g.bank or 0
end

local function CoveredText(u)
    local g = State.guild
    if u.kind == "slots" then
        return "Your guild has " .. (g.slots or 0) .. " bot slots already: this tier adds nothing."
    end
    return "Your offline window is " .. Data.FormatMinutes(g.ow or 0) .. " already: this tier adds nothing."
end

local function BuyUnlock(u)
    UI.Confirm("Spend " .. Data.FormatMoney(u.gold or 0) .. " " .. PayFromText() .. " on: " .. (u.desc or "this unlock") .. "?", function()
        GA.Protocol.BuyUnlock(u.id, UI.RequestCallback("Unlocked: " .. (u.desc or "") .. "."))
    end)
end

local function CreateRow(list, index)
    local row = CreateFrame("Frame", nil, list)
    row.shade = row:CreateTexture(nil, "BACKGROUND")
    row.shade:SetAllPoints(row)
    row.shade:SetTexture("Interface\\Buttons\\WHITE8X8")
    row.level = W.Label(row, "", "GameFontNormalSmall")
    row.level:SetPoint("LEFT", row, "LEFT", 6, 0)
    row.level:SetWidth(70)
    row.icon = W.Icon(row, 20, KIND_ICONS.feature)
    row.icon:SetPoint("LEFT", row.level, "RIGHT", 4, 0)
    row.buy = W.Button(row, "Buy", 110, 20, function(self)
        local u = self:GetParent().unlock
        if u then
            BuyUnlock(u)
        end
    end)
    row.buy:SetPoint("RIGHT", row, "RIGHT", -4, 0)
    row.status = W.Label(row, "", "GameFontHighlightSmall", "RIGHT")
    row.status:SetPoint("RIGHT", row, "RIGHT", -8, 0)
    row.status:SetWidth(150)
    row.desc = W.Label(row, "", "GameFontHighlightSmall")
    row.desc:SetPoint("LEFT", row.icon, "RIGHT", 8, 0)
    row.desc:SetPoint("RIGHT", row.status, "LEFT", -8, 0)
    W.SetTooltip(row, function(self)
        return self.unlock and self.unlock.desc or nil
    end, function(self)
        local u = self.unlock
        if not u then
            return nil
        end
        local lines = {}
        if u.kind == "feature" then
            local f = Data.FEATURES[u.val]
            lines[#lines + 1] = f and f.desc or ""
        end
        if u.st == "owned" then
            lines[#lines + 1] = "|cff40ff40Unlocked.|r"
        elseif u.st == "covered" then
            lines[#lines + 1] = "|cff40ff40Included.|r " .. CoveredText(u)
        elseif u.st == "available" then
            lines[#lines + 1] = "|cff40ff40Available:|r costs " .. Data.FormatMoney(u.gold or 0) .. " " .. PayFromText() .. "."
            if (u.gold or 0) > Funds() then
                lines[#lines + 1] = "|cffff8040Not enough gold:|r " .. Data.FormatMoney(Funds()) .. " can be paid now."
            end
        else
            lines[#lines + 1] = "|cffff8040Requires Renown level " .. (u.lvl or "?") .. ".|r"
            if (u.gold or 0) > 0 then
                lines[#lines + 1] = "Then costs " .. Data.FormatMoney(u.gold) .. "."
            end
        end
        return lines
    end)
    return row
end

local function UpdateRow(row, u)
    row.unlock = u
    local current = State.guild.rl or 1
    local shadeAlpha = (u.lvl or 0) % 2 == 0 and 0.05 or 0.0
    if u.lvl == current then
        row.shade:SetVertexColor(1, 0.82, 0, 0.08)
    else
        row.shade:SetVertexColor(1, 1, 1, shadeAlpha)
    end
    row.level:SetText("Renown " .. (u.lvl or "?"))
    row.icon:SetTexture(KIND_ICONS[u.kind] or KIND_ICONS.feature)
    local color = u.st == "locked" and "|cff9d9d9d" or "|cffffffff"
    row.desc:SetText(color .. (u.desc or "") .. "|r")
    if u.st == "available" and (u.gold or 0) > 0 then
        row.status:Hide()
        row.buy:Show()
        row.buy:SetText("Buy " .. math.floor((u.gold or 0) / 10000) .. "g")
        W.SetEnabled(row.buy, State.IsConnected() and Funds() >= (u.gold or 0))
        row.icon:SetDesaturated(false)
    else
        row.buy:Hide()
        row.status:Show()
        if u.st == "owned" then
            row.status:SetText("|cff40ff40Unlocked|r")
            row.icon:SetDesaturated(false)
        elseif u.st == "covered" then
            row.status:SetText("|cff40ff40Included|r")
            row.icon:SetDesaturated(false)
        elseif u.st == "available" then
            row.status:SetText("|cff40ff40Free|r")
            row.icon:SetDesaturated(false)
        else
            local cost = (u.gold or 0) > 0 and (", " .. math.floor(u.gold / 10000) .. "g") or ""
            row.status:SetText("|cff9d9d9dLocked" .. cost .. "|r")
            row.icon:SetDesaturated(true)
        end
    end
end

local function Create(parent)
    local f = CreateFrame("Frame", nil, parent)

    f.level = W.Label(f, "", "GameFontNormalLarge")
    f.level:SetPoint("TOPLEFT", f, "TOPLEFT", 8, -8)
    f.bar = W.Bar(f, 360, 16, 1, 0.5, 0)
    f.bar:SetPoint("TOPLEFT", f.level, "BOTTOMLEFT", 0, -8)
    f.summary = W.Label(f, "", "GameFontHighlightSmall")
    f.summary:SetPoint("TOPLEFT", f.bar, "BOTTOMLEFT", 0, -8)
    f.summary:SetPoint("RIGHT", f, "RIGHT", -8, 0)
    f.features = W.Label(f, "", "GameFontHighlightSmall")
    f.features:SetPoint("TOPLEFT", f.summary, "BOTTOMLEFT", 0, -4)
    f.features:SetPoint("RIGHT", f, "RIGHT", -8, 0)
    f.features:SetHeight(28)
    f.features:SetJustifyV("TOP")
    local sources = W.Label(f, "Bots earn renown with level-ups, quests, dungeon bosses, profession milestones and achievements.",
        "GameFontDisableSmall")
    sources:SetPoint("TOPLEFT", f.level, "TOPRIGHT", 20, 0)
    sources:SetPoint("RIGHT", f, "RIGHT", -8, 0)
    sources:SetJustifyV("TOP")
    f.bank = W.Label(f, "", "GameFontHighlightSmall", "RIGHT")
    f.bank:SetPoint("BOTTOMRIGHT", f.summary, "TOPRIGHT", 0, 6)

    local panel = W.Panel(f)
    panel:SetPoint("TOPLEFT", f, "TOPLEFT", 0, -92)
    panel:SetPoint("BOTTOMRIGHT", f, "BOTTOMRIGHT", 0, 0)
    f.list = W.ScrollList(panel, { rowHeight = 24, createRow = CreateRow, updateRow = UpdateRow })
    f.list:SetPoint("TOPLEFT", panel, "TOPLEFT", 6, -6)
    f.list:SetPoint("BOTTOMRIGHT", panel, "BOTTOMRIGHT", -6, 6)
    return f
end

local function Refresh(f)
    local g = State.guild
    f.level:SetText("Renown level " .. (g.rl or 1))
    if g.rn and g.rn > 0 then
        f.bar:SetProgress(g.rp or 0, g.rn, string.format("%d / %d to level %d", g.rp or 0, g.rn, (g.rl or 1) + 1))
    else
        f.bar:SetProgress(1, 1, "Maximum renown level")
    end
    f.summary:SetText(string.format("Total renown %d (%d from achievements)   -   Bot slots %d / %d   -   Offline window %s / %s",
        g.rt or 0, (g.achr or 0) + (g.msr or 0), g.slots or 0, g.max or 40, Data.FormatMinutes(g.ow or 0), Data.FormatMinutes(g.owmax or 480)))
    local names = {}
    for _, key in ipairs(g.feat or {}) do
        names[#names + 1] = Data.FeatureLabel(key)
    end
    f.features:SetText("Unlocked features: " .. (#names > 0 and table.concat(names, ", ") or "none"))
    local bank = "Guild bank: " .. Data.FormatMoney(g.bank or 0)
    if g.ulfrom and g.ulfrom ~= "bank" then
        bank = bank .. "   -   unlocks can pay " .. Data.FormatMoney(Funds())
    end
    f.bank:SetText(bank)
    f.list:SetItems(State.unlocks)
end

UI.RegisterPage("progression", { tab = "progression", create = Create, refresh = Refresh })
