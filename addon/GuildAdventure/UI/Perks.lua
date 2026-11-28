-- Perks tab: guild points (the achievement points of the guild's achievements) and the permanent guild perks they buy
-- for every guild character (docs/protocol.md §7.15, docs/guild-management.md "Guild perks").

local GA = GuildAdventure
local UI = GA.UI
local W = UI.W
local State = GA.State

local ICONS = {
    move_speed = "Interface\\Icons\\Ability_Rogue_Sprint",
    experience = "Interface\\Icons\\INV_Misc_Book_09",
    gold = "Interface\\Icons\\INV_Misc_Coin_01",
    extra_material = "Interface\\Icons\\INV_Fabric_Linen_01",
    kill_regen = "Interface\\Icons\\Spell_Nature_Rejuvenation",
    no_reagents = "Interface\\Icons\\INV_Misc_Dust_01",
    instant_flight = "Interface\\Icons\\Ability_Mount_Gryphon_01",
    all_flight_paths = "Interface\\Icons\\INV_Misc_Map_01",
    shared_flight_paths = "Interface\\Icons\\INV_Letter_08",
    materials_storage = "Interface\\Icons\\INV_Misc_Bag_10",
    drop_chance = "Interface\\Icons\\INV_Misc_Gem_01",
    craft_speed = "Interface\\Icons\\Trade_BlackSmithing",
    gather_speed = "Interface\\Icons\\Trade_Mining",
    water_breathing = "Interface\\Icons\\Spell_Shadow_DemonBreath",
    no_fall_damage = "Interface\\Icons\\Spell_Magic_FeatherFall",
}
local DEFAULT_ICON = "Interface\\Icons\\INV_Misc_QuestionMark"

local function Points()
    return State.guild.gp or 0
end

local function CanBuy(p)
    return State.IsConnected() and p.avail and (p.rank or 0) < (p.max or 0) and Points() >= (p.cost or 0)
end

-- A perk bought with gold (STATE^gpgold copper per guild point, paid as an unlock: STATE^ulgold).
local function GoldPrice(p)
    return (p.cost or 0) * (State.guild.gpgold or 0)
end

local function CanBuyGold(p)
    local price = GoldPrice(p)
    return State.IsConnected() and p.avail and (p.rank or 0) < (p.max or 0) and price > 0 and
        (State.guild.ulgold or State.guild.bank or 0) >= price
end

local function BuyPerk(p, withGold)
    local nextRank = (p.rank or 0) + 1
    local what = (p.n or "this perk") .. ((p.max or 1) > 1 and (" rank " .. nextRank) or "")
    local cost = withGold and GetCoinTextureString(GoldPrice(p)) or ((p.cost or 0) .. " guild points")
    UI.Confirm("Spend " .. cost .. " on " .. what .. ": " .. (p.next or "") .. "?", function()
        GA.Protocol.BuyPerk(p.id, UI.RequestCallback("Bought " .. what .. "."), withGold)
    end)
end

local function CreateRow(list, index)
    local row = CreateFrame("Frame", nil, list)
    row.shade = row:CreateTexture(nil, "BACKGROUND")
    row.shade:SetAllPoints(row)
    row.shade:SetTexture("Interface\\Buttons\\WHITE8X8")
    row.icon = W.Icon(row, 28, DEFAULT_ICON)
    row.icon:SetPoint("LEFT", row, "LEFT", 6, 0)
    row.buy = W.Button(row, "Buy", 96, 22, function(self)
        local p = self:GetParent().perk
        if p then
            BuyPerk(p)
        end
    end)
    row.buy:SetPoint("RIGHT", row, "RIGHT", -6, 0)
    row.gold = W.Button(row, "Gold", 96, 22, function(self)
        local p = self:GetParent().perk
        if p then
            BuyPerk(p, true)
        end
    end)
    row.gold:SetPoint("RIGHT", row.buy, "LEFT", -4, 0)
    W.SetTooltip(row.gold, "Buy with gold", { "Buys the next rank with gold instead of guild points (paid like a gold unlock)." })
    row.name = W.Label(row, "", "GameFontNormal")
    row.name:SetPoint("TOPLEFT", row.icon, "TOPRIGHT", 8, 1)
    row.name:SetWidth(200)
    row.rank = W.Label(row, "", "GameFontHighlightSmall")
    row.rank:SetPoint("LEFT", row.name, "RIGHT", 4, 0)
    row.rank:SetWidth(90)
    row.effect = W.Label(row, "", "GameFontHighlightSmall")
    row.effect:SetPoint("BOTTOMLEFT", row.icon, "BOTTOMRIGHT", 8, 0)
    row.effect:SetPoint("RIGHT", row.gold, "LEFT", -8, 0)
    W.SetTooltip(row, function(self)
        return self.perk and self.perk.n or nil
    end, function(self)
        local p = self.perk
        if not p then
            return nil
        end
        local lines = {}
        if (p.rank or 0) > 0 then
            lines[#lines + 1] = "|cff40ff40Now:|r " .. (p.desc or "")
        else
            lines[#lines + 1] = "|cff9d9d9dNot bought yet.|r"
        end
        if not p.avail then
            lines[#lines + 1] = "|cffff8040Needs a newer server core.|r"
        elseif (p.rank or 0) < (p.max or 0) then
            lines[#lines + 1] = "|cffffd100Next:|r " .. (p.next or "") .. " for " .. (p.cost or 0) .. " guild points."
        else
            lines[#lines + 1] = "|cff40ff40Highest rank.|r"
        end
        lines[#lines + 1] = "Works for the guild leader and every guild bot."
        return lines
    end)
    return row
end

local function UpdateRow(row, p)
    row.perk = p
    local rank, max = p.rank or 0, p.max or 1
    row.shade:SetVertexColor(1, 1, 1, rank > 0 and 0.06 or 0.0)
    row.icon:SetTexture(ICONS[p.key] or DEFAULT_ICON)
    row.icon:SetDesaturated(rank == 0)
    row.name:SetText((rank > 0 and "|cffffd100" or "|cffcccccc") .. (p.n or "?") .. "|r")
    if max > 1 then
        row.rank:SetText("Rank " .. rank .. " / " .. max)
    else
        row.rank:SetText(rank > 0 and "|cff40ff40Owned|r" or "")
    end
    local now = rank > 0 and ("|cffffffff" .. (p.desc or "") .. "|r") or ""
    local nextText = ""
    if not p.avail then
        nextText = "|cffff8040needs a newer server core|r"
    elseif rank < max then
        nextText = "|cff9d9d9dnext: " .. (p.next or "") .. "|r"
    end
    row.effect:SetText(now .. (now ~= "" and nextText ~= "" and "   " or "") .. nextText)
    if rank >= max then
        row.buy:SetText("Max")
        W.SetEnabled(row.buy, false)
        row.gold:Hide()
    else
        row.buy:SetText("Buy (" .. (p.cost or 0) .. ")")
        W.SetEnabled(row.buy, CanBuy(p))
        local price = GoldPrice(p)
        if price > 0 then
            row.gold:SetText(math.floor(price / 10000) .. "g")
            W.SetEnabled(row.gold, CanBuyGold(p))
            row.gold:Show()
        else
            row.gold:Hide()
        end
    end
end

local function Create(parent)
    local f = CreateFrame("Frame", nil, parent)

    f.title = W.Label(f, "Guild perks", "GameFontNormalLarge")
    f.title:SetPoint("TOPLEFT", f, "TOPLEFT", 8, -8)
    f.points = W.Label(f, "", "GameFontHighlight")
    f.points:SetPoint("TOPLEFT", f.title, "BOTTOMLEFT", 0, -8)
    f.points:SetPoint("RIGHT", f, "RIGHT", -8, 0)
    local hint = W.Label(f, "Guild achievements give guild points (their achievement points). Perks are permanent and "
        .. "work for the guild leader and every guild bot; buying them does not touch renown.", "GameFontDisableSmall")
    hint:SetPoint("TOPLEFT", f.points, "BOTTOMLEFT", 0, -6)
    hint:SetPoint("RIGHT", f, "RIGHT", -8, 0)
    hint:SetJustifyV("TOP")
    f.hint = hint

    local panel = W.Panel(f)
    panel:SetPoint("TOPLEFT", f, "TOPLEFT", 0, -70)
    panel:SetPoint("BOTTOMRIGHT", f, "BOTTOMRIGHT", 0, 0)
    f.list = W.ScrollList(panel, { rowHeight = 36, createRow = CreateRow, updateRow = UpdateRow })
    f.list:SetPoint("TOPLEFT", panel, "TOPLEFT", 6, -6)
    f.list:SetPoint("BOTTOMRIGHT", panel, "BOTTOMRIGHT", -6, 6)
    f.empty = W.Label(panel, "No perks known yet (not connected?).", "GameFontDisableSmall", "CENTER")
    f.empty:SetPoint("CENTER", panel, "CENTER", 0, 0)
    return f
end

local function Refresh(f)
    local g = State.guild
    local earned = (g.achp or 0) + (g.msp or 0)
    local spent = g.gps or 0
    local granted = (g.gpg or 0) > 0 and (", " .. g.gpg .. " granted") or ""
    f.points:SetText(string.format("|cff40ffc0%d guild points|r   |cff9d9d9d(%d earned from %d achievements and %d tiers%s, %d spent)|r",
        Points(), earned, g.ach or 0, g.ms or 0, granted, spent))
    f.list:SetItems(State.perks)
    if #State.perks == 0 then
        f.empty:Show()
    else
        f.empty:Hide()
    end
end

UI.RegisterPage("perks", { tab = "perks", create = Create, refresh = Refresh })
