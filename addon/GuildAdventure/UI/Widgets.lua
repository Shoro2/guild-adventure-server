-- Small widget helpers on top of the stock 3.3.5a FrameXML templates.
-- Template-based frames get unique global names ("GuildAdventure<Kind><n>") because the
-- templates look their children up as _G[name .. "Text"] etc.

local GA = GuildAdventure
GA.UI = GA.UI or {}
local UI = GA.UI
local W = {}
UI.W = W

local counter = 0
function W.Name(kind)
    counter = counter + 1
    return "GuildAdventure" .. kind .. counter
end

-- The dark dialog background: the normal one lets world text (names, nameplates) shine
-- through and hurts readability (seen in the first in-client screenshots).
W.BACKDROP_DIALOG = {
    bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background-Dark",
    edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
    tile = true, tileSize = 32, edgeSize = 32,
    insets = { left = 11, right = 12, top = 12, bottom = 11 },
}
W.BACKDROP_PANEL = {
    bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
    edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
    tile = true, tileSize = 16, edgeSize = 16,
    insets = { left = 4, right = 4, top = 4, bottom = 4 },
}

W.COLORS = {
    gold = "|cffffd100",
    white = "|cffffffff",
    grey = "|cff9d9d9d",
    green = "|cff40ff40",
    red = "|cffff4040",
    yellow = "|cffffff00",
    blue = "|cff40c0ff",
    orange = "|cffff8000",
}

-- Extra opaque layer inside a dialog backdrop's insets.
function W.DarkenBackground(frame, alpha)
    local tex = frame:CreateTexture(nil, "BACKGROUND")
    tex:SetTexture("Interface\\Buttons\\WHITE8X8")
    tex:SetVertexColor(0, 0, 0, alpha or 0.6)
    tex:SetPoint("TOPLEFT", frame, "TOPLEFT", 11, -12)
    tex:SetPoint("BOTTOMRIGHT", frame, "BOTTOMRIGHT", -12, 11)
    return tex
end

function W.Panel(parent, alpha)
    local f = CreateFrame("Frame", nil, parent)
    f:SetBackdrop(W.BACKDROP_PANEL)
    f:SetBackdropColor(0, 0, 0, alpha or 0.45)
    f:SetBackdropBorderColor(0.5, 0.5, 0.5, 1)
    return f
end

function W.Label(parent, text, font, justify)
    local fs = parent:CreateFontString(nil, "OVERLAY", font or "GameFontHighlight")
    fs:SetText(text or "")
    fs:SetJustifyH(justify or "LEFT")
    return fs
end

-- Section header: gold text with a thin line under it.
function W.Header(parent, text)
    local fs = W.Label(parent, text, "GameFontNormal")
    local line = parent:CreateTexture(nil, "ARTWORK")
    line:SetTexture("Interface\\Buttons\\WHITE8X8")
    line:SetVertexColor(1, 0.82, 0, 0.25)
    line:SetHeight(1)
    line:SetPoint("TOPLEFT", fs, "BOTTOMLEFT", 0, -2)
    line:SetPoint("RIGHT", parent, "RIGHT", -10, 0)
    fs.line = line
    return fs
end

function W.Button(parent, text, width, height, onClick)
    local b = CreateFrame("Button", W.Name("Button"), parent, "UIPanelButtonTemplate")
    b:SetWidth(width or 100)
    b:SetHeight(height or 22)
    b:SetText(text or "")
    if onClick then
        b:SetScript("OnClick", onClick)
    end
    return b
end

function W.SetEnabled(widget, enabled)
    if enabled then
        widget:Enable()
    else
        widget:Disable()
    end
end

-------------------------------------------------------------------------------
-- Tooltips: title/lines may be strings, tables or functions (evaluated on hover).
-------------------------------------------------------------------------------

local function Resolve(v, owner)
    if type(v) == "function" then
        return v(owner)
    end
    return v
end

local function ShowTooltip(self)
    local title = Resolve(self.gaTooltipTitle, self)
    if not title then
        return
    end
    GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
    GameTooltip:SetText(title, 1, 1, 1)
    local lines = Resolve(self.gaTooltipLines, self)
    if type(lines) == "string" then
        lines = { lines }
    end
    if lines then
        for i = 1, #lines do
            GameTooltip:AddLine(lines[i], 0.9, 0.9, 0.9, 1)
        end
    end
    GameTooltip:Show()
end

local function HideTooltip()
    GameTooltip:Hide()
end

-- A FontString or Texture has no mouse handling of its own: the tooltip goes on a transparent frame laid over it.
-- (W.SetTooltip on a label used to raise "attempt to call method 'EnableMouse'" and left the loot filter view black.)
local function TooltipTarget(region)
    local kind = region:GetObjectType()
    if kind ~= "FontString" and kind ~= "Texture" then
        return region
    end
    if not region.gaTooltipFrame then
        local hit = CreateFrame("Frame", nil, region:GetParent())
        hit:SetAllPoints(region)
        region.gaTooltipFrame = hit
    end
    return region.gaTooltipFrame
end

function W.SetTooltip(frame, title, lines)
    frame = TooltipTarget(frame)
    frame.gaTooltipTitle = title
    frame.gaTooltipLines = lines
    if not frame.gaTooltipHooked then
        frame.gaTooltipHooked = true
        frame:EnableMouse(true)
        frame:HookScript("OnEnter", ShowTooltip)
        frame:HookScript("OnLeave", HideTooltip)
    end
end

-------------------------------------------------------------------------------
-- Inputs
-------------------------------------------------------------------------------

function W.CheckBox(parent, label, onClick)
    local name = W.Name("Check")
    local cb = CreateFrame("CheckButton", name, parent, "UICheckButtonTemplate")
    cb:SetWidth(24)
    cb:SetHeight(24)
    cb.label = _G[name .. "Text"]
    cb.label:SetText(label or "")
    cb.label:SetFontObject(GameFontHighlightSmall)
    if onClick then
        cb:SetScript("OnClick", onClick)
    end
    return cb
end

-- 3.3.5a GetChecked() returns 1 or nil.
function W.IsChecked(cb)
    return cb:GetChecked() and true or false
end

function W.EditBox(parent, width, numeric, maxLetters)
    local eb = CreateFrame("EditBox", W.Name("Edit"), parent, "InputBoxTemplate")
    eb:SetWidth(width or 80)
    eb:SetHeight(20)
    eb:SetAutoFocus(false)
    if numeric then
        eb:SetNumeric(true)
    end
    if maxLetters then
        eb:SetMaxLetters(maxLetters)
    end
    eb:SetScript("OnEnterPressed", function(self)
        self:ClearFocus()
    end)
    eb:HookScript("OnEditFocusGained", function(self)
        self.gaFocused = true
    end)
    eb:HookScript("OnEditFocusLost", function(self)
        self.gaFocused = false
    end)
    return eb
end

-- Updates the text unless the player is typing in the box right now.
function W.SetEditText(eb, text)
    text = tostring(text or "")
    if not eb.gaFocused and eb:GetText() ~= text then
        eb:SetText(text)
    end
end

-- 3.3.5a EditBoxes have no Enable/Disable: block mouse focus and grey the text instead.
function W.SetEditEnabled(eb, enabled)
    if enabled then
        eb:EnableMouse(true)
        eb:SetTextColor(1, 1, 1)
    else
        eb:ClearFocus()
        eb:EnableMouse(false)
        eb:SetTextColor(0.5, 0.5, 0.5)
    end
end

-- True while the menu of one of the addon's dropdowns is open (the stock DropDownList1 shows it).
function W.MenuOpen()
    local open = UIDROPDOWNMENU_OPEN_MENU
    return DropDownList1 ~= nil and DropDownList1:IsShown() and type(open) == "table" and open.isGuildAdventure == true
end

-- DropDown(parent, width, builder): builder(dd, level) returns the item list for that level:
-- { { text =, value =, disabled =, checked =, tooltip =, hasArrow =, isTitle = }, ... }.
-- Clicking an item calls dd.onSelect(value, item). The selected text is managed by us
-- (dd:SetSelected(value, text)) instead of UIDropDownMenu_SetSelectedValue.
function W.DropDown(parent, width, builder)
    local dd = CreateFrame("Frame", W.Name("DropDown"), parent, "UIDropDownMenuTemplate")
    dd.builder = builder
    UIDropDownMenu_SetWidth(dd, width or 120)
    UIDropDownMenu_JustifyText(dd, "LEFT")

    local function OnItemClick(button, value, item)
        dd.selectedValue = value
        UIDropDownMenu_SetText(dd, item.text)
        CloseDropDownMenus()
        if dd.onSelect then
            dd.onSelect(value, item)
        end
    end

    -- Two things in the 3.3.5a FrameXML close an open menu of any dropdown: UIDropDownMenu_Initialize of
    -- another dropdown hides every DropDownList, and the template's OnHide calls CloseDropDownMenus(). A
    -- refresh that made a row (with a dropdown) or hid one so closed the menu the player had open whenever
    -- a bot's update came in. So the menu's contents are only built when it opens (ToggleDropDownMenu runs
    -- UIDropDownMenu_Initialize with dd.initialize), and hiding a dropdown closes only its own menu.
    dd:SetScript("OnHide", function(self)
        if UIDROPDOWNMENU_OPEN_MENU == self then
            CloseDropDownMenus()
        end
    end)

    dd.initialize = function(self, level)
        level = level or 1
        local items = dd.builder and dd.builder(dd, level, UIDROPDOWNMENU_MENU_VALUE) or {}
        for i = 1, #items do
            local item = items[i]
            local info = UIDropDownMenu_CreateInfo()
            info.text = item.text
            info.value = item.value
            info.disabled = item.disabled
            info.isTitle = item.isTitle
            info.notCheckable = item.isTitle or item.hasArrow
            info.hasArrow = item.hasArrow
            info.checked = not item.hasArrow and not item.isTitle and dd.selectedValue ~= nil
                and item.value == dd.selectedValue
            if item.tooltip then
                info.tooltipTitle = item.text
                info.tooltipText = item.tooltip
                info.tooltipOnButton = 1
                info.tooltipWhileDisabled = 1
            end
            if not item.isTitle and not item.hasArrow then
                info.func = OnItemClick
                info.arg1 = item.value
                info.arg2 = item
            end
            UIDropDownMenu_AddButton(info, level)
        end
    end

    dd.isGuildAdventure = true

    function dd:SetSelected(value, text)
        self.selectedValue = value
        UIDropDownMenu_SetText(self, text or "")
    end

    function dd:SetEnabled(enabled)
        if enabled then
            UIDropDownMenu_EnableDropDown(self)
        else
            UIDropDownMenu_DisableDropDown(self)
        end
    end

    return dd
end

-------------------------------------------------------------------------------
-- Bars, icons, overlays
-------------------------------------------------------------------------------

function W.Bar(parent, width, height, r, g, b)
    local bar = CreateFrame("StatusBar", nil, parent)
    bar:SetWidth(width or 120)
    bar:SetHeight(height or 14)
    bar:SetStatusBarTexture("Interface\\TargetingFrame\\UI-StatusBar")
    bar:SetStatusBarColor(r or 0.58, g or 0, b or 0.55)
    bar:SetMinMaxValues(0, 1)
    bar:SetValue(0)
    local bg = bar:CreateTexture(nil, "BACKGROUND")
    bg:SetAllPoints(bar)
    bg:SetTexture("Interface\\Buttons\\WHITE8X8")
    bg:SetVertexColor(0.22, 0.22, 0.22, 0.85) -- visible track, also when the bar is empty
    bar.bg = bg
    bar.text = bar:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    bar.text:SetPoint("CENTER", bar, "CENTER", 0, 0)

    function bar:SetProgress(value, maxValue, text)
        maxValue = (maxValue and maxValue > 0) and maxValue or 1
        self:SetMinMaxValues(0, maxValue)
        self:SetValue(math.min(value or 0, maxValue))
        self.text:SetText(text or "")
    end

    return bar
end

function W.ClassIcon(parent, size)
    local tex = parent:CreateTexture(nil, "ARTWORK")
    tex:SetWidth(size or 18)
    tex:SetHeight(size or 18)
    tex:SetTexture(GA.Data.CLASS_ICON_TEXTURE)

    function tex:SetClass(classId)
        self:SetTexCoord(GA.Data.ClassTexCoords(classId))
    end

    return tex
end

function W.Icon(parent, size, path)
    local tex = parent:CreateTexture(nil, "ARTWORK")
    tex:SetWidth(size or 18)
    tex:SetHeight(size or 18)
    tex:SetTexture(path)
    return tex
end

-- A panel that covers a locked feature: lock icon, title and the unlock requirement.
function W.LockedOverlay(parent)
    local f = CreateFrame("Frame", nil, parent)
    f:SetAllPoints(parent)
    f:SetFrameLevel(parent:GetFrameLevel() + 10)
    f:EnableMouse(true)
    local bg = f:CreateTexture(nil, "BACKGROUND")
    bg:SetAllPoints(f)
    bg:SetTexture("Interface\\Buttons\\WHITE8X8")
    bg:SetVertexColor(0, 0, 0, 0.85)
    f.icon = W.Icon(f, 32, "Interface\\LFGFrame\\UI-LFG-ICON-LOCK")
    f.icon:SetPoint("CENTER", f, "CENTER", 0, 30)
    f.title = W.Label(f, "", "GameFontNormalLarge", "CENTER")
    f.title:SetPoint("TOP", f.icon, "BOTTOM", 0, -8)
    f.text = W.Label(f, "", "GameFontHighlight", "CENTER")
    f.text:SetPoint("TOP", f.title, "BOTTOM", 0, -6)
    f.text:SetWidth(360)
    f.detail = W.Label(f, "", "GameFontDisableSmall", "CENTER")
    f.detail:SetPoint("TOP", f.text, "BOTTOM", 0, -6)
    f.detail:SetWidth(360)

    function f:SetLock(title, requirement, detail)
        self.title:SetText(title or "Locked")
        self.text:SetText(requirement or "")
        self.detail:SetText(detail or "")
        self:Show()
    end

    f:Hide()
    return f
end

-------------------------------------------------------------------------------
-- Scroll list: a FauxScrollFrame with a pool of rows that follows the list height.
-- opts: rowHeight, createRow(list, index) -> row, updateRow(row, item, index)
-------------------------------------------------------------------------------

function W.ScrollList(parent, opts)
    local list = CreateFrame("Frame", nil, parent)
    list.rowHeight = opts.rowHeight or 20
    list.rows = {}
    list.items = {}
    list.createRow = opts.createRow
    list.updateRow = opts.updateRow

    local scroll = CreateFrame("ScrollFrame", W.Name("Scroll"), list, "FauxScrollFrameTemplate")
    scroll:SetPoint("TOPLEFT", list, "TOPLEFT", 0, 0)
    scroll:SetPoint("BOTTOMRIGHT", list, "BOTTOMRIGHT", -24, 0)
    list.scroll = scroll

    local function OnWheel(_, delta)
        ScrollFrameTemplate_OnMouseWheel(scroll, delta)
    end
    list:EnableMouseWheel(true)
    list:SetScript("OnMouseWheel", OnWheel)

    function list:VisibleRows()
        local h = self:GetHeight() or 0
        return math.max(1, math.floor(h / self.rowHeight))
    end

    function list:EnsureRows()
        local needed = self:VisibleRows()
        for i = #self.rows + 1, needed do
            local row = self.createRow(self, i)
            row:SetHeight(self.rowHeight)
            row:SetPoint("TOPLEFT", self, "TOPLEFT", 0, -(i - 1) * self.rowHeight)
            row:SetPoint("RIGHT", self, "RIGHT", -26, 0)
            row:EnableMouseWheel(true)
            row:SetScript("OnMouseWheel", OnWheel)
            self.rows[i] = row
        end
        return needed
    end

    function list:Refresh()
        local visible = self:EnsureRows()
        local items = self.items
        FauxScrollFrame_Update(scroll, #items, visible, self.rowHeight)
        local offset = FauxScrollFrame_GetOffset(scroll) or 0
        for i = 1, #self.rows do
            local row = self.rows[i]
            local index = offset + i
            if i <= visible and items[index] then
                self.updateRow(row, items[index], index)
                row:Show()
            else
                row:Hide()
            end
        end
    end

    function list:SetItems(items)
        self.items = items or {}
        self:Refresh()
    end

    scroll:SetScript("OnVerticalScroll", function(self, offset)
        FauxScrollFrame_OnVerticalScroll(self, offset, list.rowHeight, function()
            list:Refresh()
        end)
    end)
    list:SetScript("OnSizeChanged", function(self)
        self:Refresh()
    end)

    return list
end
