-- Crafting from the guild storage (docs/storage.md "Crafting", docs/protocol.md §7.13).
--
-- 1. The profession window of the character you play: reagent counts include the guild storage (gold when only the
--    storage makes them enough), the recipe list shows how often each recipe can be made with it, and when the bags
--    alone are not enough the Create / Create All buttons become "Create (storage)": the client would refuse that cast
--    (it checks its bags), so the server starts it (CRAFT); the reagents then come from the storage (core patch CP-7).
-- 2. The bot detail's "Crafting" sub-tab: the bot's recipes with how often its bags and the storage allow them, and a
--    Craft button; the bot stands still while it crafts and its roster line shows the progress.
-- (Idea and the profession window overlay from ForgottenLand's mod-endless-storage, the owner's own module.)

local GA = GuildAdventure
local UI = GA.UI
local W = UI.W
local State = GA.State
local Protocol = GA.Protocol

local function LinkToId(link, kind)
    if not link then
        return nil
    end
    return tonumber(string.match(link, (kind or "item") .. ":(%d+)"))
end

local function SpellOfRecipe(index)
    return LinkToId(GetTradeSkillRecipeLink(index), "enchant")
end

-- How often the recipe can be made with the bags and the guild storage together.
local function CraftableWithStorage(index)
    local max = 999
    local numReagents = GetTradeSkillNumReagents(index)
    for i = 1, numReagents do
        local _, _, needed, inBags = GetTradeSkillReagentInfo(index, i)
        local itemId = LinkToId(GetTradeSkillReagentItemLink(index, i))
        local total = (inBags or 0) + (itemId and State.StorageCount(itemId) or 0)
        if needed and needed > 0 then
            max = math.min(max, math.floor(total / needed))
        end
    end
    return numReagents > 0 and max or 0
end

local function StorageUsable()
    return State.IsConnected() and State.storage.loaded and State.loot.settings.reag ~= false
end

-------------------------------------------------------------------------------
-- Profession window overlay
-------------------------------------------------------------------------------

local createOne, createAll
local hooked = false

local function CreateButtons()
    if createOne then
        return
    end
    createOne = CreateFrame("Button", "GuildAdventureCraftStorageButton", TradeSkillFrame, "UIPanelButtonTemplate")
    createOne:SetText("Create")
    createOne:Hide()
    createOne:SetScript("OnClick", function()
        local index = GetTradeSkillSelectionIndex()
        local spellId = index and index > 0 and SpellOfRecipe(index)
        if spellId then
            local count = TradeSkillInputBox and tonumber(TradeSkillInputBox:GetText()) or 1
            Protocol.Craft(spellId, math.max(1, math.min(count or 1, 100)), nil, UI.RequestCallback(nil))
        end
    end)
    W.SetTooltip(createOne, "Create from the guild storage", {
        "Your bags lack some materials; the guild storage has them. The server starts the craft and takes them from there.",
    })
    createAll = CreateFrame("Button", "GuildAdventureCraftAllStorageButton", TradeSkillFrame, "UIPanelButtonTemplate")
    createAll:SetText("Create All")
    createAll:Hide()
    createAll:SetScript("OnClick", function()
        local index = GetTradeSkillSelectionIndex()
        local spellId = index and index > 0 and SpellOfRecipe(index)
        if spellId then
            local max = CraftableWithStorage(index)
            if max > 0 then
                Protocol.Craft(spellId, math.min(max, 100), nil, UI.RequestCallback(nil))
            end
        end
    end)
    for _, b in ipairs({ createOne, createAll }) do
        b:GetFontString():SetTextColor(0.25, 0.75, 1)
    end
end

local function RestoreButtons()
    if createOne then
        createOne:Hide()
        createAll:Hide()
    end
    if TradeSkillCreateButton then
        TradeSkillCreateButton:Show()
    end
    if TradeSkillCreateAllButton then
        TradeSkillCreateAllButton:Show()
    end
end

local function UpdateSelection()
    if not TradeSkillFrame or not TradeSkillFrame:IsShown() then
        return
    end
    local index = GetTradeSkillSelectionIndex()
    local name, kind, numAvailable = GetTradeSkillInfo(index or 0)
    if not index or index == 0 or not name or kind == "header" or not StorageUsable() then
        RestoreButtons()
        return
    end

    local usesStorage = false
    for i = 1, GetTradeSkillNumReagents(index) do
        local _, _, needed, inBags = GetTradeSkillReagentInfo(index, i)
        local itemId = LinkToId(GetTradeSkillReagentItemLink(index, i))
        local stored = itemId and State.StorageCount(itemId) or 0
        if stored > 0 then
            local count = _G["TradeSkillReagent" .. i .. "Count"]
            if count then
                count:SetText(string.format(" %d/%d", (inBags or 0) + stored, needed or 0))
                if (inBags or 0) < (needed or 0) and (inBags or 0) + stored >= (needed or 0) then
                    count:SetTextColor(0.25, 0.75, 1)
                    local label = _G["TradeSkillReagent" .. i .. "Name"]
                    if label then
                        label:SetTextColor(0.25, 0.75, 1)
                    end
                    usesStorage = true
                end
            end
        end
    end

    local max = CraftableWithStorage(index)
    if usesStorage and (numAvailable or 0) == 0 and max > 0 and TradeSkillCreateButton then
        CreateButtons()
        createOne:ClearAllPoints()
        createOne:SetAllPoints(TradeSkillCreateButton)
        createOne:SetFrameLevel(TradeSkillCreateButton:GetFrameLevel() + 5)
        createOne:Show()
        TradeSkillCreateButton:Hide()
        if TradeSkillCreateAllButton then
            createAll:ClearAllPoints()
            createAll:SetAllPoints(TradeSkillCreateAllButton)
            createAll:SetFrameLevel(TradeSkillCreateAllButton:GetFrameLevel() + 5)
            createAll:SetText("Create All (" .. math.min(max, 100) .. ")")
            createAll:Show()
            TradeSkillCreateAllButton:Hide()
        end
    else
        RestoreButtons()
    end
end

local function UpdateList()
    if not TradeSkillFrame or not TradeSkillFrame:IsShown() or not StorageUsable() then
        return
    end
    local offset = FauxScrollFrame_GetOffset(TradeSkillListScrollFrame) or 0
    for i = 1, TRADE_SKILLS_DISPLAYED or 8 do
        local index = i + offset
        local name, kind, numAvailable = GetTradeSkillInfo(index)
        local button = _G["TradeSkillSkill" .. i]
        if name and kind ~= "header" and button and button:IsShown() then
            local max = CraftableWithStorage(index)
            if max > (numAvailable or 0) then
                local text = button:GetText() or name
                text = string.gsub(text, " %[%d+%]$", "")
                text = string.gsub(text, " |cff40c0ff%[%d+%]|r$", "")
                button:SetText(text .. " |cff40c0ff[" .. max .. "]|r")
            end
        end
    end
end

local function Hook()
    if hooked or not TradeSkillFrame then
        return
    end
    hooked = true
    hooksecurefunc("TradeSkillFrame_SetSelection", UpdateSelection)
    hooksecurefunc("TradeSkillFrame_Update", UpdateList)
    TradeSkillFrame:HookScript("OnHide", RestoreButtons)
end

local events = CreateFrame("Frame")
events:RegisterEvent("TRADE_SKILL_SHOW")
events:RegisterEvent("BAG_UPDATE")
events:SetScript("OnEvent", function(_, event)
    if event == "TRADE_SKILL_SHOW" then
        Hook()
        if State.IsConnected() and not State.storage.loaded then
            Protocol.Get("storage")
        end
    end
    UpdateSelection()
    UpdateList()
end)

GA.On("STORAGE", function()
    UpdateSelection()
    UpdateList()
end)

-------------------------------------------------------------------------------
-- Bot detail: Crafting sub-tab
-------------------------------------------------------------------------------

local function CreateRecipeRow(list)
    local row = CreateFrame("Button", nil, list)
    row.shade = row:CreateTexture(nil, "BACKGROUND")
    row.shade:SetAllPoints(row)
    row.shade:SetTexture("Interface\\Buttons\\WHITE8X8")
    row.icon = W.Icon(row, 18, "Interface\\Icons\\INV_Misc_QuestionMark")
    row.icon:SetPoint("LEFT", row, "LEFT", 4, 0)
    row.max = W.Label(row, "", "GameFontHighlightSmall", "RIGHT")
    row.max:SetPoint("RIGHT", row, "RIGHT", -6, 0)
    row.max:SetWidth(110)
    row.name = W.Label(row, "", "GameFontHighlightSmall")
    row.name:SetPoint("LEFT", row.icon, "RIGHT", 6, 0)
    row.name:SetPoint("RIGHT", row.max, "LEFT", -6, 0)
    row:SetScript("OnClick", function(self)
        list.view.selected = self.recipe and self.recipe.s
        UI.RequestRefresh()
    end)
    row:SetScript("OnEnter", function(self)
        if self.recipe and self.recipe.i and self.recipe.i > 0 then
            GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
            GameTooltip:SetHyperlink("item:" .. self.recipe.i .. ":0:0:0:0:0:0:0")
            GameTooltip:Show()
        end
    end)
    row:SetScript("OnLeave", function()
        GameTooltip:Hide()
    end)
    return row
end

local function UpdateRecipeRow(row, recipe, index)
    row.recipe = recipe
    local selected = row:GetParent().view.selected == recipe.s
    row.shade:SetVertexColor(1, 0.82, 0, selected and 0.18 or (index % 2 == 0 and 0.04 or 0))
    local name, _, quality, _, texture = UI.StorageItemInfo(recipe.i or 0, 0)
    if not recipe.i or recipe.i == 0 then
        name = recipe.nm or "?"
    end
    row.icon:SetTexture(texture)
    local c = ITEM_QUALITY_COLORS and ITEM_QUALITY_COLORS[quality or 1]
    row.name:SetText((c and c.hex or "|cffffffff") .. (recipe.nm ~= "" and recipe.nm or name) .. "|r")
    row.max:SetText((recipe.max or 0) > 0 and ("|cff40c0ffcan make " .. recipe.max .. "|r") or "|cff808080no materials|r")
end

function UI.CreateBotCrafting(parent)
    local f = CreateFrame("Frame", nil, parent)
    f:SetAllPoints(parent)
    f.status = W.Label(f, "", "GameFontHighlightSmall")
    f.status:SetPoint("TOPLEFT", f, "TOPLEFT", 6, -6)
    f.status:SetPoint("RIGHT", f, "RIGHT", -8, 0)

    local panel = W.Panel(f)
    panel:SetPoint("TOPLEFT", f, "TOPLEFT", 0, -26)
    panel:SetPoint("BOTTOMRIGHT", f, "BOTTOMRIGHT", 0, 34)
    f.list = W.ScrollList(panel, { rowHeight = 22, createRow = CreateRecipeRow, updateRow = UpdateRecipeRow })
    f.list.view = f
    f.list:SetPoint("TOPLEFT", panel, "TOPLEFT", 6, -6)
    f.list:SetPoint("BOTTOMRIGHT", panel, "BOTTOMRIGHT", -6, 6)
    f.empty = W.Label(panel, "", "GameFontDisable", "CENTER")
    f.empty:SetPoint("CENTER", panel, "CENTER", 0, 0)

    local countLabel = W.Label(f, "Count", "GameFontNormalSmall")
    countLabel:SetPoint("BOTTOMLEFT", f, "BOTTOMLEFT", 4, 10)
    f.count = W.EditBox(f, 40, true, 3)
    f.count:SetPoint("LEFT", countLabel, "RIGHT", 10, 0)
    W.SetEditText(f.count, "1")
    f.craft = W.Button(f, "Craft", 90, 22, function()
        local bot = State.GetBot(UI.SelectedBot())
        if bot and f.selected then
            local n = math.max(1, math.min(tonumber(f.count:GetText()) or 1, 100))
            Protocol.Craft(f.selected, n, bot.g, UI.RequestCallback(nil, function()
                GA.After(1, function()
                    Protocol.Get("recipes", bot.g)
                end)
            end))
        end
    end)
    f.craft:SetPoint("LEFT", f.count, "RIGHT", 10, 0)
    f.stop = W.Button(f, "Stop", 70, 22, function()
        local bot = State.GetBot(UI.SelectedBot())
        if bot then
            Protocol.StopCraft(f.selected or 1, bot.g, UI.RequestCallback(nil))
        end
    end)
    f.stop:SetPoint("LEFT", f.craft, "RIGHT", 6, 0)
    f.reload = W.Button(f, "Refresh", 80, 22, function()
        local bot = State.GetBot(UI.SelectedBot())
        if bot then
            Protocol.Get("recipes", bot.g)
        end
    end)
    f.reload:SetPoint("LEFT", f.stop, "RIGHT", 6, 0)
    local hint = W.Label(f, "Materials come from the bot's bags and the guild storage.", "GameFontDisableSmall")
    hint:SetPoint("LEFT", f.reload, "RIGHT", 10, 0)
    hint:SetPoint("RIGHT", f, "RIGHT", -8, 0)
    return f
end

function UI.RefreshBotCrafting(f, bot)
    local connected = State.IsConnected()
    local recipes = State.recipes
    if recipes.g ~= bot.g and connected and f.requested ~= bot.g and bot.s ~= "benched" and bot.s ~= "offline" then
        f.requested = bot.g
        f.selected = nil
        Protocol.Get("recipes", bot.g)
    end
    local mine = recipes.g == bot.g
    local list = mine and recipes.list or {}
    f.list:SetItems(list)
    local crafting = bot.why and string.find(bot.why, "^Crafting") ~= nil
    if bot.s == "benched" or bot.s == "offline" then
        f.status:SetText("|cff9d9d9d" .. (bot.n or "The bot") .. " is not in the world.|r")
    elseif crafting then
        f.status:SetText("|cff40c0ff" .. bot.why .. "|r")
    else
        f.status:SetText(string.format("%s knows %d recipes that create an item.", bot.n or "The bot", #list))
    end
    if not mine then
        f.empty:SetText(connected and "Loading recipes..." or "Not connected.")
    elseif #list == 0 then
        f.empty:SetText("No crafting recipes (plan a profession on the Professions tab; bots learn them at trainers).")
    else
        f.empty:SetText("")
    end
    local selectedMax = 0
    for _, r in ipairs(list) do
        if r.s == f.selected then
            selectedMax = r.max or 0
        end
    end
    W.SetEnabled(f.craft, connected and f.selected ~= nil and selectedMax > 0 and not crafting and bot.s ~= "controlled")
    W.SetEnabled(f.stop, connected and crafting)
    W.SetEnabled(f.reload, connected)
end
