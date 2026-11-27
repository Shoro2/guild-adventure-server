-- Main window (movable, resizable), tabs, page registry, connection overlay and the
-- minimap button. Pages register themselves with UI.RegisterPage and are created lazily.

local GA = GuildAdventure
local UI = GA.UI
local W = UI.W
local State = GA.State

UI.MIN_WIDTH, UI.MIN_HEIGHT = 720, 480
UI.MAX_WIDTH, UI.MAX_HEIGHT = 1200, 900

-- Tabs in display order. `page` is the default page of the tab; `feature` greys the tab
-- label while that feature is locked (the page then explains the requirement).
UI.TABS = {
    { key = "overview", label = "Overview", page = "overview" },
    { key = "roster", label = "Roster", page = "roster" },
    { key = "recruitment", label = "Recruitment", page = "recruitment", feature = "recruit" },
    { key = "progression", label = "Progression", page = "progression" },
    { key = "achievements", label = "Achievements", page = "achievements" },
    { key = "perks", label = "Perks", page = "perks" },
    { key = "storage", label = "Storage", page = "storage" },
    { key = "groups", label = "Dungeons", page = "groups" },
    { key = "map", label = "Map", page = "map", feature = "map" },
    { key = "settings", label = "Settings", page = "settings" },
}

local pages = {}
local main, content, overlay, statusLine
local currentPage = nil
local refreshScheduled = false

-- def: { tab = tabKey, create = function(parent) -> frame, refresh = function(frame),
--        offline = true if the page works without a server connection }
function UI.RegisterPage(key, def)
    pages[key] = def
end

local function TabIndex(tabKey)
    for i, tab in ipairs(UI.TABS) do
        if tab.key == tabKey then
            return i
        end
    end
    return 1
end

local function PageFrame(key)
    local def = pages[key]
    if not def then
        return nil
    end
    if not def.frame then
        def.frame = def.create(content)
        def.frame:SetAllPoints(content)
        def.frame:Hide()
    end
    return def.frame
end

-------------------------------------------------------------------------------
-- Connection overlay (covers pages that need a server)
-------------------------------------------------------------------------------

local CONNECTION_TEXT = {
    idle = { "Not connected", "Waiting for the world to load." },
    connecting = { "Connecting...", "Looking for the Guild Adventure server." },
    failed = { "No server found", "The server did not answer. Is mod-guild-adventure running on this realm?" },
    missing = { "Server module missing", "This realm does not run mod-guild-adventure: your addon messages came back unanswered." },
    badversion = { "Addon out of date", "The server needs a newer GuildAdventure addon." },
}

local function CreateOverlay(parent)
    local f = CreateFrame("Frame", nil, parent)
    f:SetAllPoints(parent)
    f:SetFrameLevel(parent:GetFrameLevel() + 20)
    f:EnableMouse(true)
    local bg = f:CreateTexture(nil, "BACKGROUND")
    bg:SetAllPoints(f)
    bg:SetTexture("Interface\\Buttons\\WHITE8X8")
    bg:SetVertexColor(0, 0, 0, 0.8)
    f.title = W.Label(f, "", "GameFontNormalLarge", "CENTER")
    f.title:SetPoint("CENTER", f, "CENTER", 0, 50)
    f.text = W.Label(f, "", "GameFontHighlight", "CENTER")
    f.text:SetPoint("TOP", f.title, "BOTTOM", 0, -10)
    f.text:SetWidth(420)
    f.retry = W.Button(f, "Retry", 120, 24, function()
        GA.Protocol.Connect(true)
    end)
    f.retry:SetPoint("TOPRIGHT", f.text, "BOTTOM", -6, -20)
    f.demo = W.Button(f, "Start demo mode", 140, 24, function()
        GA.MockServer.Enable()
    end)
    f.demo:SetPoint("TOPLEFT", f.text, "BOTTOM", 6, -20)
    W.SetTooltip(f.demo, "Demo mode", {
        "Runs a simulated Guild Adventure server inside your client,",
        "so you can try the UI without mod-guild-adventure.",
        "Leave it with /ga mock off.",
    })
    f.hint = W.Label(f, "Demo mode can be left any time with /ga mock off.", "GameFontDisableSmall", "CENTER")
    f.hint:SetPoint("TOP", f.text, "BOTTOM", 0, -56)
    f:Hide()
    return f
end

local function UpdateOverlay()
    if not overlay then
        return
    end
    local def = currentPage and pages[currentPage]
    if State.IsConnected() or (def and def.offline) then
        overlay:Hide()
        return
    end
    local text = CONNECTION_TEXT[State.connection] or CONNECTION_TEXT.idle
    overlay.title:SetText(text[1])
    overlay.text:SetText(text[2])
    W.SetEnabled(overlay.retry, State.connection ~= "connecting")
    if State.mode == "mock" then
        overlay.demo:Hide()
        overlay.hint:Hide()
    else
        overlay.demo:Show()
        overlay.hint:Show()
    end
    overlay:Show()
end

-------------------------------------------------------------------------------
-- Refresh
-------------------------------------------------------------------------------

local function UpdateTabs()
    for i, tab in ipairs(UI.TABS) do
        local button = _G[main:GetName() .. "Tab" .. i]
        local locked = tab.feature and State.IsConnected() and not State.HasFeature(tab.feature)
        button:SetText(locked and ("|cff808080" .. tab.label .. "|r") or tab.label)
        PanelTemplates_TabResize(button, 0)
    end
end

local function UpdateStatusLine()
    local text
    if State.mode == "mock" then
        text = "|cffffd100Demo mode|r (simulated server)"
    elseif State.IsConnected() then
        text = "|cff40ff40Connected|r" .. (State.server.srv and (" - server " .. State.server.srv) or "")
    else
        text = "|cff9d9d9d" .. ((CONNECTION_TEXT[State.connection] or CONNECTION_TEXT.idle)[1]) .. "|r"
    end
    main.connection:SetText(text)
end

function UI.RefreshNow()
    refreshScheduled = false
    if not main or not main:IsShown() then
        return
    end
    -- A refresh rebuilds and hides rows of the page, which can close the menu the player has open; while one of
    -- the addon's dropdown menus is open the refresh waits for it to close.
    if W.MenuOpen() then
        refreshScheduled = true
        GA.After(0.3, UI.RefreshNow)
        return
    end
    UpdateStatusLine()
    UpdateTabs()
    UpdateOverlay()
    local def = currentPage and pages[currentPage]
    if def and def.frame and def.frame:IsShown() and def.refresh then
        def.refresh(def.frame)
    end
end

-- Coalesces bursts (a sync is dozens of messages) into one refresh.
function UI.RequestRefresh()
    if refreshScheduled then
        return
    end
    refreshScheduled = true
    GA.After(0.05, UI.RefreshNow)
end

-------------------------------------------------------------------------------
-- Status messages
-------------------------------------------------------------------------------

local statusTimer
function UI.Notify(text, isError)
    if isError then
        UIErrorsFrame:AddMessage(text, 1.0, 0.1, 0.1, 1.0)
    end
    if statusLine then
        statusLine:SetText((isError and "|cffff4040" or "|cffffd100") .. text .. "|r")
        if statusTimer then
            statusTimer:Cancel()
        end
        statusTimer = GA.After(8, function()
            statusLine:SetText("")
        end)
    end
end

-- Standard request callback: shows success/failure text, then calls after(ok).
function UI.RequestCallback(successText, after)
    return function(ok, code, msg, reason)
        if ok then
            if successText then
                UI.Notify(successText)
            end
        else
            UI.Notify(GA.Protocol.ErrorText(code, msg, reason), true)
        end
        if after then
            after(ok, code)
        end
    end
end

-------------------------------------------------------------------------------
-- Window
-------------------------------------------------------------------------------

local function SavePosition(frame)
    local point, _, relativePoint, x, y = frame:GetPoint()
    GA.db.ui.point = { point, relativePoint, x, y }
end

local function CreateMain()
    local db = GA.db.ui
    main = CreateFrame("Frame", "GuildAdventureMainFrame", UIParent)
    main:Hide()
    main:SetWidth(math.max(UI.MIN_WIDTH, db.width or 780))
    main:SetHeight(math.max(UI.MIN_HEIGHT, db.height or 520))
    main:SetScale(db.scale or 1)
    if db.point then
        main:SetPoint(db.point[1], UIParent, db.point[2], db.point[3], db.point[4])
    else
        main:SetPoint("CENTER", UIParent, "CENTER", 0, 40)
    end
    main:SetFrameStrata("HIGH")
    main:SetToplevel(true)
    main:SetClampedToScreen(true)
    main:SetBackdrop(W.BACKDROP_DIALOG)
    W.DarkenBackground(main, 0.55)
    main:EnableMouse(true)
    main:SetMovable(true)
    main:SetResizable(true)
    main:SetMinResize(UI.MIN_WIDTH, UI.MIN_HEIGHT)
    main:SetMaxResize(UI.MAX_WIDTH, UI.MAX_HEIGHT)
    main:RegisterForDrag("LeftButton")
    main:SetScript("OnDragStart", function(self)
        self:StartMoving()
    end)
    main:SetScript("OnDragStop", function(self)
        self:StopMovingOrSizing()
        SavePosition(self)
    end)
    main:SetScript("OnShow", function()
        PlaySound("igCharacterInfoOpen")
        if State.mode == "live" and (State.connection == "failed" or State.connection == "idle") then
            GA.Protocol.Connect(false)
        end
        UI.RefreshNow()
    end)
    main:SetScript("OnHide", function()
        PlaySound("igCharacterInfoClose")
    end)
    tinsert(UISpecialFrames, "GuildAdventureMainFrame")

    local header = main:CreateTexture(nil, "ARTWORK")
    header:SetTexture("Interface\\DialogFrame\\UI-DialogBox-Header")
    header:SetWidth(300)
    header:SetHeight(64)
    header:SetPoint("TOP", main, "TOP", 0, 12)
    local title = W.Label(main, "Guild Adventure", "GameFontNormal", "CENTER")
    title:SetPoint("TOP", header, "TOP", 0, -14)

    local close = CreateFrame("Button", nil, main, "UIPanelCloseButton")
    close:SetPoint("TOPRIGHT", main, "TOPRIGHT", -6, -6)

    main.connection = W.Label(main, "", "GameFontHighlightSmall")
    main.connection:SetPoint("TOPLEFT", main, "TOPLEFT", 20, -18)

    content = CreateFrame("Frame", nil, main)
    content:SetPoint("TOPLEFT", main, "TOPLEFT", 16, -36)
    content:SetPoint("BOTTOMRIGHT", main, "BOTTOMRIGHT", -16, 30)
    UI.content = content

    statusLine = W.Label(main, "", "GameFontHighlightSmall")
    statusLine:SetPoint("BOTTOMLEFT", main, "BOTTOMLEFT", 22, 14)
    statusLine:SetPoint("RIGHT", main, "RIGHT", -40, 0)

    overlay = CreateOverlay(content)

    -- size grip
    local grip = CreateFrame("Button", nil, main)
    grip:SetWidth(16)
    grip:SetHeight(16)
    grip:SetPoint("BOTTOMRIGHT", main, "BOTTOMRIGHT", -8, 8)
    grip:SetNormalTexture("Interface\\ChatFrame\\UI-ChatIM-SizeGrabber-Up")
    grip:SetHighlightTexture("Interface\\ChatFrame\\UI-ChatIM-SizeGrabber-Highlight")
    grip:SetPushedTexture("Interface\\ChatFrame\\UI-ChatIM-SizeGrabber-Down")
    grip:SetScript("OnMouseDown", function()
        main:StartSizing("BOTTOMRIGHT")
    end)
    grip:SetScript("OnMouseUp", function()
        main:StopMovingOrSizing()
        GA.db.ui.width = math.floor(main:GetWidth() + 0.5)
        GA.db.ui.height = math.floor(main:GetHeight() + 0.5)
        SavePosition(main)
        UI.RequestRefresh()
    end)

    -- tabs along the bottom edge
    for i, tab in ipairs(UI.TABS) do
        local button = CreateFrame("Button", "GuildAdventureMainFrameTab" .. i, main, "CharacterFrameTabButtonTemplate")
        button:SetID(i)
        button:SetText(tab.label)
        if i == 1 then
            button:SetPoint("TOPLEFT", main, "BOTTOMLEFT", 11, 2)
        else
            button:SetPoint("LEFT", _G["GuildAdventureMainFrameTab" .. (i - 1)], "RIGHT", -15, 0)
        end
        button:SetScript("OnClick", function(self)
            PlaySound("igCharacterInfoTab")
            UI.ShowPage(UI.TABS[self:GetID()].page)
        end)
        button:SetScript("OnShow", function(self)
            PanelTemplates_TabResize(self, 0)
        end)
        PanelTemplates_TabResize(button, 0)
    end
    PanelTemplates_SetNumTabs(main, #UI.TABS)
    PanelTemplates_SetTab(main, 1)
    UI.main = main
end

function UI.ShowPage(key)
    if not main then
        CreateMain()
    end
    local def = pages[key]
    if not def then
        return
    end
    for k, other in pairs(pages) do
        if k ~= key and other.frame then
            other.frame:Hide()
        end
    end
    currentPage = key
    PanelTemplates_SetTab(main, TabIndex(def.tab))
    local frame = PageFrame(key)
    frame:Show()
    if not main:IsShown() then
        main:Show()
    else
        UI.RefreshNow()
    end
end

function UI.CurrentPage()
    return currentPage
end

function UI.Show(page)
    UI.ShowPage(page or currentPage or "overview")
end

function UI.Hide()
    if main then
        main:Hide()
    end
end

function UI.IsShown()
    return main ~= nil and main:IsShown()
end

function UI.Toggle()
    if UI.IsShown() then
        UI.Hide()
    else
        UI.Show()
    end
end

function UI.ApplyScale(scale)
    GA.db.ui.scale = scale
    if main then
        main:SetScale(scale)
    end
end

-------------------------------------------------------------------------------
-- Minimap button
-------------------------------------------------------------------------------

local minimapButton

local function PlaceMinimapButton()
    local angle = math.rad(GA.db.ui.minimapAngle or 200)
    minimapButton:ClearAllPoints()
    minimapButton:SetPoint("CENTER", Minimap, "CENTER", math.cos(angle) * 80, math.sin(angle) * 80)
end

local function MinimapDragUpdate()
    local mx, my = Minimap:GetCenter()
    local px, py = GetCursorPosition()
    local scale = Minimap:GetEffectiveScale()
    px, py = px / scale, py / scale
    GA.db.ui.minimapAngle = math.deg(math.atan2(py - my, px - mx))
    PlaceMinimapButton()
end

local function CreateMinimapButton()
    local b = CreateFrame("Button", "GuildAdventureMinimapButton", Minimap)
    b:SetWidth(31)
    b:SetHeight(31)
    b:SetFrameStrata("MEDIUM")
    b:SetFrameLevel(8)
    b:SetHighlightTexture("Interface\\Minimap\\UI-Minimap-ZoomButton-Highlight")
    local bg = b:CreateTexture(nil, "BACKGROUND")
    bg:SetWidth(20)
    bg:SetHeight(20)
    bg:SetTexture("Interface\\Minimap\\UI-Minimap-Background")
    bg:SetPoint("TOPLEFT", b, "TOPLEFT", 7, -5)
    local icon = b:CreateTexture(nil, "ARTWORK")
    icon:SetWidth(18)
    icon:SetHeight(18)
    icon:SetTexture("Interface\\Icons\\INV_Shirt_GuildTabard_01")
    icon:SetPoint("TOPLEFT", b, "TOPLEFT", 7, -6)
    local border = b:CreateTexture(nil, "OVERLAY")
    border:SetWidth(53)
    border:SetHeight(53)
    border:SetTexture("Interface\\Minimap\\MiniMap-TrackingBorder")
    border:SetPoint("TOPLEFT", b, "TOPLEFT", 0, 0)
    b:RegisterForClicks("LeftButtonUp", "RightButtonUp")
    b:RegisterForDrag("LeftButton")
    b:SetScript("OnClick", function(_, button)
        if button == "RightButton" then
            UI.Show("roster")
        else
            UI.Toggle()
        end
    end)
    b:SetScript("OnDragStart", function(self)
        self:SetScript("OnUpdate", MinimapDragUpdate)
    end)
    b:SetScript("OnDragStop", function(self)
        self:SetScript("OnUpdate", nil)
    end)
    W.SetTooltip(b, "Guild Adventure", function()
        local lines = { "Left-click: open or close", "Right-click: roster", "Drag: move this button" }
        local g = State.guild
        if State.IsConnected() and g.rl then
            table.insert(lines, 1, string.format("Renown %d - %d/%d bots", g.rl, State.BotCount(), g.slots or 0))
        end
        return lines
    end)
    minimapButton = b
    PlaceMinimapButton()
    if GA.db.ui.minimapHide then
        b:Hide()
    end
end

function UI.SetMinimapButtonShown(shown)
    GA.db.ui.minimapHide = not shown
    if minimapButton then
        if shown then
            minimapButton:Show()
        else
            minimapButton:Hide()
        end
    end
end

-------------------------------------------------------------------------------
-- Wiring
-------------------------------------------------------------------------------

GA.OnInit(function()
    CreateMinimapButton()
end)

for _, event in ipairs({ "CONNECTION", "GUILD", "CONTROL", "ROSTER", "BOT", "LEADER", "ZONES", "UNLOCKS", "EVENTS", "AWAY",
    "STORAGE", "LOOT", "LOOT_LOG", "RECIPES", "AUCTIONS", "PERKS", "MILESTONES", "SETTINGS", "DUNGEONS", "CRAFT_ORDERS", "FARM_ITEMS" }) do
    GA.On(event, UI.RequestRefresh)
end

GA.On("ERROR", function(code, msg, reason, r)
    if not r then
        UI.Notify(GA.Protocol.ErrorText(code, msg, reason), true)
    end
end)
