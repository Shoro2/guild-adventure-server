-- Minimal WoW 3.3.5a client API stub for offline addon tests (Lua 5.1 / 5.2 interpreter).
--
-- Widget methods are whitelisted per object type. Calling a method that does not exist in
-- the 3.3.5a client (SetShown, SetColorTexture, EditBox:Disable, ...) raises an error, as does
-- an unknown template, font object, anchor point or script name. The whitelists were checked
-- against the FrameXML shipped with the 3.3.5a (12340) client.
--
-- Not covered: rendering, real layout rules, secure code, the real dropdown list frames.

local Stub = {}
_G.WowStub = Stub

Stub.now = 1000            -- GetTime()
Stub.epoch = 1794229200    -- time() at now = 0
Stub.errors = {}
Stub.chat = {}
Stub.sentAddon = {}        -- SendAddonMessage calls
Stub.popups = {}
Stub.player = { name = "Tester", guid = "0x0000000000000001", faction = "Alliance", level = 70, zone = "Stormwind City" }

-------------------------------------------------------------------------------
-- Lua 5.1 environment (the client runs Lua 5.1)
-------------------------------------------------------------------------------

if not unpack then
    unpack = table.unpack
end
table.unpack = nil
table.pack = nil
rawlen = nil
bit32 = nil
loadstring = loadstring or load

-------------------------------------------------------------------------------
-- Method whitelists
-------------------------------------------------------------------------------

local function Set(...)
    local s = {}
    for _, list in ipairs({ ... }) do
        for _, name in ipairs(list) do
            s[name] = true
        end
    end
    return s
end

local REGION = { "GetName", "GetObjectType", "IsObjectType", "GetParent", "SetParent", "Show", "Hide",
    "IsShown", "IsVisible", "SetPoint", "ClearAllPoints", "SetAllPoints", "GetPoint", "GetNumPoints",
    "SetWidth", "SetHeight", "SetSize", "GetWidth", "GetHeight", "GetSize", "GetCenter", "GetLeft",
    "GetRight", "GetTop", "GetBottom", "SetAlpha", "GetAlpha", "IsMouseOver" }
local LAYERED = { "SetDrawLayer", "GetDrawLayer", "SetVertexColor" }
local FONT_INSTANCE = { "SetFontObject", "GetFontObject", "SetFont", "GetFont", "SetTextColor",
    "GetTextColor", "SetJustifyH", "GetJustifyH", "SetJustifyV", "GetJustifyV", "SetShadowColor",
    "SetShadowOffset", "SetSpacing" }
local TEXTURE = { "SetTexture", "GetTexture", "SetTexCoord", "GetTexCoord", "SetDesaturated",
    "IsDesaturated", "SetBlendMode", "GetBlendMode", "SetGradient", "SetGradientAlpha" }
local FONT_STRING = { "SetText", "GetText", "SetFormattedText", "GetStringWidth", "GetStringHeight",
    "SetNonSpaceWrap", "SetTextHeight" }
local FRAME = { "SetScript", "GetScript", "HookScript", "HasScript", "RegisterEvent", "UnregisterEvent",
    "UnregisterAllEvents", "IsEventRegistered", "EnableMouse", "IsMouseEnabled", "EnableMouseWheel",
    "IsMouseWheelEnabled", "EnableKeyboard", "SetMovable", "IsMovable", "SetResizable", "IsResizable",
    "SetMinResize", "SetMaxResize", "StartMoving", "StartSizing", "StopMovingOrSizing", "SetUserPlaced",
    "RegisterForDrag", "SetFrameStrata", "GetFrameStrata", "SetFrameLevel", "GetFrameLevel",
    "SetToplevel", "IsToplevel", "SetClampedToScreen", "SetBackdrop", "SetBackdropColor",
    "SetBackdropBorderColor", "CreateTexture", "CreateFontString", "SetScale", "GetScale",
    "GetEffectiveScale", "SetID", "GetID", "GetChildren", "GetRegions", "SetHitRectInsets", "Raise",
    "Lower", "SetAttribute", "GetAttribute" }
local BUTTON = { "SetText", "GetText", "SetFormattedText", "GetFontString", "SetFontString",
    "SetNormalFontObject", "SetHighlightFontObject", "SetDisabledFontObject", "SetNormalTexture",
    "SetPushedTexture", "SetHighlightTexture", "SetDisabledTexture", "GetNormalTexture",
    "GetHighlightTexture", "GetPushedTexture", "GetDisabledTexture", "Enable", "Disable", "IsEnabled",
    "Click", "LockHighlight", "UnlockHighlight", "RegisterForClicks", "GetTextWidth", "GetTextHeight",
    "SetButtonState", "GetButtonState" }
local CHECK_BUTTON = { "SetChecked", "GetChecked", "SetCheckedTexture", "GetCheckedTexture",
    "SetDisabledCheckedTexture" }
local EDIT_BOX = { "SetText", "GetText", "SetNumber", "GetNumber", "SetNumeric", "IsNumeric",
    "SetMaxLetters", "GetMaxLetters", "SetAutoFocus", "IsAutoFocus", "SetFocus", "ClearFocus",
    "HighlightText", "SetCursorPosition", "GetCursorPosition", "Insert", "SetMultiLine", "IsMultiLine",
    "SetTextInsets", "GetNumLetters", "AddHistoryLine", "SetPassword" }
local STATUS_BAR = { "SetStatusBarTexture", "GetStatusBarTexture", "SetStatusBarColor",
    "SetMinMaxValues", "GetMinMaxValues", "SetValue", "GetValue", "SetOrientation" }
local SLIDER = { "SetMinMaxValues", "GetMinMaxValues", "SetValue", "GetValue", "SetValueStep",
    "GetValueStep", "SetOrientation", "SetThumbTexture", "GetThumbTexture", "Enable", "Disable",
    "IsEnabled" }
local SCROLL_FRAME = { "SetScrollChild", "GetScrollChild", "SetVerticalScroll", "GetVerticalScroll",
    "SetHorizontalScroll", "GetVerticalScrollRange", "UpdateScrollChildRect" }
local TOOLTIP = { "SetOwner", "SetText", "AddLine", "AddDoubleLine", "ClearLines", "IsOwned", "NumLines" }
local MESSAGE = { "AddMessage", "Clear" }

local METHODS = {
    Texture = Set(REGION, LAYERED, TEXTURE),
    FontString = Set(REGION, LAYERED, FONT_INSTANCE, FONT_STRING),
    Frame = Set(REGION, FRAME),
    Button = Set(REGION, FRAME, BUTTON),
    CheckButton = Set(REGION, FRAME, BUTTON, CHECK_BUTTON),
    EditBox = Set(REGION, FRAME, FONT_INSTANCE, EDIT_BOX),
    StatusBar = Set(REGION, FRAME, STATUS_BAR),
    Slider = Set(REGION, FRAME, SLIDER),
    ScrollFrame = Set(REGION, FRAME, SCROLL_FRAME),
    GameTooltip = Set(REGION, FRAME, TOOLTIP),
    MessageFrame = Set(REGION, FRAME, MESSAGE),
}

local COMMON_SCRIPTS = { "OnShow", "OnHide", "OnEvent", "OnUpdate", "OnEnter", "OnLeave", "OnMouseDown",
    "OnMouseUp", "OnMouseWheel", "OnDragStart", "OnDragStop", "OnReceiveDrag", "OnSizeChanged",
    "OnLoad", "OnKeyDown", "OnKeyUp", "OnAttributeChanged" }
local SCRIPTS = {
    Frame = Set(COMMON_SCRIPTS),
    Button = Set(COMMON_SCRIPTS, { "OnClick", "OnDoubleClick", "PreClick", "PostClick" }),
    CheckButton = Set(COMMON_SCRIPTS, { "OnClick", "OnDoubleClick", "PreClick", "PostClick" }),
    EditBox = Set(COMMON_SCRIPTS, { "OnTextChanged", "OnEnterPressed", "OnEscapePressed", "OnTabPressed",
        "OnSpacePressed", "OnEditFocusGained", "OnEditFocusLost", "OnChar", "OnTextSet", "OnCursorChanged" }),
    StatusBar = Set(COMMON_SCRIPTS, { "OnValueChanged", "OnMinMaxChanged" }),
    Slider = Set(COMMON_SCRIPTS, { "OnValueChanged", "OnMinMaxChanged" }),
    ScrollFrame = Set(COMMON_SCRIPTS, { "OnVerticalScroll", "OnHorizontalScroll", "OnScrollRangeChanged" }),
    GameTooltip = Set(COMMON_SCRIPTS, { "OnTooltipSetUnit", "OnTooltipCleared" }),
    MessageFrame = Set(COMMON_SCRIPTS),
}

local POINTS = Set({ "TOPLEFT", "TOP", "TOPRIGHT", "LEFT", "CENTER", "RIGHT", "BOTTOMLEFT", "BOTTOM", "BOTTOMRIGHT" })
local STRATA = Set({ "BACKGROUND", "LOW", "MEDIUM", "HIGH", "DIALOG", "FULLSCREEN", "FULLSCREEN_DIALOG", "TOOLTIP" })
local LAYERS = Set({ "BACKGROUND", "BORDER", "ARTWORK", "OVERLAY", "HIGHLIGHT" })

-- Font objects defined by the 3.3.5a FrameXML (Fonts.xml / FontStyles.xml).
local FONT_NAMES = { "GameFontNormal", "GameFontNormalSmall", "GameFontNormalLarge", "GameFontNormalHuge",
    "GameFontHighlight", "GameFontHighlightSmall", "GameFontHighlightLarge", "GameFontHighlightMedium",
    "GameFontHighlightExtraSmall", "GameFontDisable", "GameFontDisableSmall", "GameFontDisableLarge",
    "GameFontGreen", "GameFontGreenSmall", "GameFontRed", "GameFontRedSmall", "GameFontWhite",
    "GameFontBlack", "ChatFontNormal", "ChatFontSmall", "NumberFontNormal", "SystemFont_Small" }

-------------------------------------------------------------------------------
-- Objects
-------------------------------------------------------------------------------

local Impl = {}
local allObjects = {}
local UIParentObj

local function TypeOf(o)
    return rawget(o, "__type")
end

local function NewObject(otype, name, parent)
    local o = {
        __type = otype, __name = name, __parent = parent, __shown = true, __points = {},
        __scripts = {}, __children = {}, __width = 0, __height = 0, __events = {}, __alpha = 1,
        __enabled = true, __level = 1, __scale = 1,
    }
    setmetatable(o, {
        __index = function(t, k)
            local methods = METHODS[rawget(t, "__type")]
            if methods[k] then
                local fn = Impl[k]
                if fn then
                    return fn
                end
                return function()
                    return nil
                end
            end
            if type(k) == "string" and string.match(k, "^[A-Z]") then
                error(string.format("3.3.5a %s has no method %s", rawget(t, "__type"), k), 2)
            end
            return nil
        end,
    })
    if parent then
        table.insert(rawget(parent, "__children"), o)
    end
    if name then
        _G[name] = o
    end
    allObjects[#allObjects + 1] = o
    return o
end
Stub.NewObject = NewObject

local function Fire(o, script, ...)
    local fn = rawget(o, "__scripts")[script]
    if fn then
        return fn(o, ...)
    end
end
Stub.Fire = Fire

local function IsVisible(o)
    while o do
        if not rawget(o, "__shown") then
            return false
        end
        o = rawget(o, "__parent")
    end
    return true
end

local function CollectVisibility(o, list)
    list[#list + 1] = { o, IsVisible(o) }
    for _, child in ipairs(rawget(o, "__children")) do
        CollectVisibility(child, list)
    end
    return list
end

local function FireVisibilityChanges(before)
    for _, entry in ipairs(before) do
        local o, was = entry[1], entry[2]
        local now = IsVisible(o)
        if now ~= was and SCRIPTS[TypeOf(o)] then
            Fire(o, now and "OnShow" or "OnHide")
        end
    end
end

function Impl.GetName(self) return rawget(self, "__name") end
function Impl.GetObjectType(self) return rawget(self, "__type") end
function Impl.IsObjectType(self, t) return rawget(self, "__type") == t end
function Impl.GetParent(self) return rawget(self, "__parent") end
function Impl.SetParent(self, p) rawset(self, "__parent", p) end
function Impl.IsShown(self) return rawget(self, "__shown") and 1 or nil end
function Impl.IsVisible(self) return IsVisible(self) and 1 or nil end

function Impl.Show(self)
    if rawget(self, "__shown") then
        return
    end
    local before = CollectVisibility(self, {})
    rawset(self, "__shown", true)
    FireVisibilityChanges(before)
end

function Impl.Hide(self)
    if not rawget(self, "__shown") then
        return
    end
    local before = CollectVisibility(self, {})
    rawset(self, "__shown", false)
    FireVisibilityChanges(before)
end

local function ResolveRelative(rel)
    if type(rel) == "string" then
        local o = _G[rel]
        if not o then
            error("SetPoint: no region named " .. rel, 3)
        end
        return o
    end
    return rel
end

function Impl.SetPoint(self, point, a, b, c, d)
    if not POINTS[point] then
        error("SetPoint: invalid point " .. tostring(point), 2)
    end
    local rel, relPoint, x, y
    if type(a) == "number" or a == nil then
        x, y = a or 0, b or 0
    else
        rel = ResolveRelative(a)
        if type(b) == "string" then
            if not POINTS[b] then
                error("SetPoint: invalid relative point " .. tostring(b), 2)
            end
            relPoint, x, y = b, c or 0, d or 0
        else
            relPoint, x, y = point, b or 0, c or 0
        end
    end
    if rel == self then
        error("SetPoint: a region cannot anchor to itself", 2)
    end
    local points = rawget(self, "__points")
    for i = #points, 1, -1 do
        if points[i].point == point then
            table.remove(points, i)
        end
    end
    points[#points + 1] = { point = point, rel = rel, relPoint = relPoint or point, x = x, y = y }
end

function Impl.ClearAllPoints(self) rawset(self, "__points", {}) end

function Impl.SetAllPoints(self, rel)
    rel = ResolveRelative(rel) or rawget(self, "__parent")
    Impl.ClearAllPoints(self)
    Impl.SetPoint(self, "TOPLEFT", rel, "TOPLEFT", 0, 0)
    Impl.SetPoint(self, "BOTTOMRIGHT", rel, "BOTTOMRIGHT", 0, 0)
end

function Impl.GetNumPoints(self) return #rawget(self, "__points") end

function Impl.GetPoint(self, i)
    local p = rawget(self, "__points")[i or 1]
    if p then
        return p.point, p.rel, p.relPoint, p.x, p.y
    end
end

local function Horizontal(point)
    if string.find(point, "LEFT") then return "l" elseif string.find(point, "RIGHT") then return "r" end
    return "cx"
end

local function Vertical(point)
    if string.find(point, "TOP") then return "t" elseif string.find(point, "BOTTOM") then return "b" end
    return "cy"
end

local function Rect(o, depth)
    depth = depth or 0
    if o == UIParentObj then
        return { l = 0, r = 1024, t = 768, b = 0 }
    end
    if depth > 40 then
        return nil
    end
    local v = { }
    for _, p in ipairs(rawget(o, "__points")) do
        local rel = p.rel or rawget(o, "__parent") or UIParentObj
        local rr = Rect(rel, depth + 1)
        if rr then
            local h, vv = Horizontal(p.relPoint), Vertical(p.relPoint)
            local rx = (h == "l" and rr.l) or (h == "r" and rr.r) or (rr.l + rr.r) / 2
            local ry = (vv == "t" and rr.t) or (vv == "b" and rr.b) or (rr.t + rr.b) / 2
            v[Horizontal(p.point)] = rx + p.x
            v[Vertical(p.point)] = ry + p.y
        end
    end
    local w, h = rawget(o, "__width"), rawget(o, "__height")
    if v.l and v.r then
    elseif v.l then v.r = v.l + w
    elseif v.r then v.l = v.r - w
    elseif v.cx then v.l, v.r = v.cx - w / 2, v.cx + w / 2
    end
    if v.t and v.b then
    elseif v.t then v.b = v.t - h
    elseif v.b then v.t = v.b + h
    elseif v.cy then v.t, v.b = v.cy + h / 2, v.cy - h / 2
    end
    if not (v.l and v.r and v.t and v.b) then
        return nil
    end
    return v
end
Stub.Rect = Rect

function Impl.SetWidth(self, w) rawset(self, "__width", w) end
function Impl.SetHeight(self, h) rawset(self, "__height", h) end
function Impl.SetSize(self, w, h) rawset(self, "__width", w); rawset(self, "__height", h or w) end

function Impl.GetWidth(self)
    local r = Rect(self)
    if r then return r.r - r.l end
    return rawget(self, "__width")
end

function Impl.GetHeight(self)
    local r = Rect(self)
    if r then return r.t - r.b end
    return rawget(self, "__height")
end

function Impl.GetSize(self) return Impl.GetWidth(self), Impl.GetHeight(self) end

function Impl.GetCenter(self)
    local r = Rect(self)
    if r then return (r.l + r.r) / 2, (r.t + r.b) / 2 end
    return 512, 384
end

function Impl.GetLeft(self) local r = Rect(self); return r and r.l end
function Impl.GetRight(self) local r = Rect(self); return r and r.r end
function Impl.GetTop(self) local r = Rect(self); return r and r.t end
function Impl.GetBottom(self) local r = Rect(self); return r and r.b end
function Impl.SetAlpha(self, a) rawset(self, "__alpha", a) end
function Impl.GetAlpha(self) return rawget(self, "__alpha") end
function Impl.SetScale(self, s) rawset(self, "__scale", s) end
function Impl.GetScale(self) return rawget(self, "__scale") end
function Impl.GetEffectiveScale(self) return rawget(self, "__scale") end
function Impl.SetID(self, id) rawset(self, "__id", id) end
function Impl.GetID(self) return rawget(self, "__id") or 0 end
function Impl.SetFrameLevel(self, l) rawset(self, "__level", l) end
function Impl.GetFrameLevel(self) return rawget(self, "__level") end

function Impl.SetFrameStrata(self, s)
    if not STRATA[s] then error("invalid frame strata " .. tostring(s), 2) end
end

function Impl.SetScript(self, script, fn)
    if not SCRIPTS[TypeOf(self)][script] then
        error(string.format("3.3.5a %s has no script %s", TypeOf(self), tostring(script)), 2)
    end
    if fn ~= nil and type(fn) ~= "function" then
        error("SetScript: handler must be a function", 2)
    end
    rawget(self, "__scripts")[script] = fn
end

function Impl.GetScript(self, script) return rawget(self, "__scripts")[script] end
function Impl.HasScript(self, script) return SCRIPTS[TypeOf(self)][script] and 1 or nil end

function Impl.HookScript(self, script, fn)
    if not SCRIPTS[TypeOf(self)][script] then
        error(string.format("3.3.5a %s has no script %s", TypeOf(self), tostring(script)), 2)
    end
    local old = rawget(self, "__scripts")[script]
    if old then
        rawget(self, "__scripts")[script] = function(...)
            old(...)
            fn(...)
        end
    else
        rawget(self, "__scripts")[script] = fn
    end
end

function Impl.RegisterEvent(self, event) rawget(self, "__events")[event] = true end
function Impl.UnregisterEvent(self, event) rawget(self, "__events")[event] = nil end
function Impl.UnregisterAllEvents(self) rawset(self, "__events", {}) end
function Impl.IsEventRegistered(self, event) return rawget(self, "__events")[event] and 1 or nil end

function Impl.EnableMouse(self, on) rawset(self, "__mouse", on and true or false) end
function Impl.IsMouseEnabled(self) return rawget(self, "__mouse") and 1 or nil end

local function CheckFont(font)
    if type(font) == "string" then
        font = _G[font]
    end
    if type(font) ~= "table" or not rawget(font, "__font") then
        error("not a font object: " .. tostring(font), 3)
    end
end

function Impl.CreateTexture(self, name, layer, inherits)
    if layer and not LAYERS[layer] then error("invalid draw layer " .. tostring(layer), 2) end
    local t = NewObject("Texture", name, self)
    rawset(t, "__layer", layer)
    return t
end

function Impl.CreateFontString(self, name, layer, inherits)
    if layer and not LAYERS[layer] then error("invalid draw layer " .. tostring(layer), 2) end
    if inherits then CheckFont(inherits) end
    local fs = NewObject("FontString", name, self)
    rawset(fs, "__layer", layer)
    return fs
end

function Impl.SetBackdrop(self, backdrop)
    if backdrop then
        Stub.textures[backdrop.bgFile or ""] = true
        Stub.textures[backdrop.edgeFile or ""] = true
    end
end

function Impl.SetFontObject(self, font) CheckFont(font) end
function Impl.SetNormalFontObject(self, font) CheckFont(font) end
function Impl.SetHighlightFontObject(self, font) CheckFont(font) end
function Impl.SetDisabledFontObject(self, font) CheckFont(font) end

function Impl.SetTexture(self, path) rawset(self, "__texture", path); Stub.textures[path or ""] = true end
function Impl.GetTexture(self) return rawget(self, "__texture") end

local function SetTextureSlot(self, slot, path)
    if type(path) == "string" then
        Stub.textures[path] = true
    end
    local tex = rawget(self, slot)
    if not tex then
        tex = NewObject("Texture", nil, self)
        rawset(self, slot, tex)
    end
    return tex
end
function Impl.SetNormalTexture(self, p) SetTextureSlot(self, "__normal", p) end
function Impl.SetPushedTexture(self, p) SetTextureSlot(self, "__pushed", p) end
function Impl.SetHighlightTexture(self, p) SetTextureSlot(self, "__highlight", p) end
function Impl.SetDisabledTexture(self, p) SetTextureSlot(self, "__disabled", p) end
function Impl.GetNormalTexture(self) return rawget(self, "__normal") end
function Impl.GetHighlightTexture(self) return rawget(self, "__highlight") end

function Impl.SetText(self, text)
    if text ~= nil then
        text = tostring(text)
    end
    if TypeOf(self) == "EditBox" then
        text = text or ""
        local max = rawget(self, "__maxLetters")
        if max and max > 0 and string.len(text) > max then
            text = string.sub(text, 1, max)
        end
        rawset(self, "__text", text)
        -- like the client: OnTextChanged runs on the next frame, not inside SetText
        Stub.pendingText[self] = true
        return
    end
    rawset(self, "__text", text)
    local fs = rawget(self, "__fontString")
    if fs then
        rawset(fs, "__text", text)
    end
end

function Impl.GetText(self)
    if TypeOf(self) == "EditBox" then
        return rawget(self, "__text") or ""
    end
    return rawget(self, "__text")
end

function Impl.SetFormattedText(self, fmt, ...) Impl.SetText(self, string.format(fmt, ...)) end
function Impl.GetStringWidth(self) return string.len(rawget(self, "__text") or "") * 6 end
function Impl.GetTextWidth(self) return string.len(rawget(self, "__text") or "") * 6 end

function Impl.GetFontString(self)
    local fs = rawget(self, "__fontString")
    if not fs then
        fs = NewObject("FontString", nil, self)
        rawset(self, "__fontString", fs)
    end
    return fs
end

function Impl.Enable(self) rawset(self, "__enabled", true) end
function Impl.Disable(self) rawset(self, "__enabled", false) end
function Impl.IsEnabled(self) return rawget(self, "__enabled") and 1 or nil end

function Impl.SetChecked(self, v) rawset(self, "__checked", (v and v ~= 0) and true or false) end
function Impl.GetChecked(self) return rawget(self, "__checked") and 1 or nil end

function Impl.Click(self, button)
    if not rawget(self, "__enabled") then
        return
    end
    if TypeOf(self) == "CheckButton" then
        rawset(self, "__checked", not rawget(self, "__checked"))
    end
    Fire(self, "OnClick", button or "LeftButton", false)
end

function Impl.SetNumeric(self, on) rawset(self, "__numeric", on and true or false) end
function Impl.SetMaxLetters(self, n) rawset(self, "__maxLetters", n) end
function Impl.GetNumber(self) return tonumber(rawget(self, "__text") or "") or 0 end
function Impl.SetNumber(self, n) Impl.SetText(self, tostring(n)) end

function Impl.SetFocus(self)
    if Stub.focus and Stub.focus ~= self then
        Impl.ClearFocus(Stub.focus)
    end
    Stub.focus = self
    Fire(self, "OnEditFocusGained")
end

function Impl.ClearFocus(self)
    if Stub.focus == self then
        Stub.focus = nil
        Fire(self, "OnEditFocusLost")
    end
end

function Impl.SetMinMaxValues(self, lo, hi)
    rawset(self, "__min", lo)
    rawset(self, "__max", hi)
    local v = rawget(self, "__value") or lo
    if v < lo or v > hi then
        Impl.SetValue(self, math.max(lo, math.min(hi, v)))
    end
end

function Impl.GetMinMaxValues(self) return rawget(self, "__min") or 0, rawget(self, "__max") or 0 end

function Impl.SetValue(self, v)
    local lo, hi = rawget(self, "__min") or 0, rawget(self, "__max") or 0
    v = math.max(lo, math.min(hi, v))
    local old = rawget(self, "__value")
    rawset(self, "__value", v)
    if old ~= v then
        Fire(self, "OnValueChanged", v)
    end
end

function Impl.GetValue(self) return rawget(self, "__value") or 0 end
function Impl.SetValueStep(self, s) rawset(self, "__step", s) end
function Impl.GetValueStep(self) return rawget(self, "__step") or 1 end

function Impl.SetVerticalScroll(self, offset)
    rawset(self, "__vscroll", offset)
    Fire(self, "OnVerticalScroll", offset)
end
function Impl.GetVerticalScroll(self) return rawget(self, "__vscroll") or 0 end

-- tooltip
function Impl.SetOwner(self, owner) rawset(self, "__owner", owner); rawset(self, "__lines", {}) end
function Impl.IsOwned(self, owner) return rawget(self, "__owner") == owner and 1 or nil end
function Impl.AddLine(self, text) table.insert(rawget(self, "__lines") or {}, text) end

-- message frames
function Impl.AddMessage(self, text) table.insert(Stub.chat, { frame = rawget(self, "__name"), text = text }) end

-------------------------------------------------------------------------------
-- Templates
-------------------------------------------------------------------------------

local function Child(parent, otype, suffix)
    local name = rawget(parent, "__name")
    if not name then
        error("template needs a named frame (" .. suffix .. ")", 4)
    end
    return NewObject(otype, name .. suffix, parent)
end

local TAB_TEXTURES = { "Left", "Middle", "Right", "LeftDisabled", "MiddleDisabled", "RightDisabled", "HighlightTexture" }

local TEMPLATES = {
    UIPanelButtonTemplate = { types = { Button = true }, build = function(o)
        if rawget(o, "__name") then
            rawset(o, "__fontString", Child(o, "FontString", "Text"))
        end
    end },
    UIPanelCloseButton = { types = { Button = true }, build = function(o)
        rawget(o, "__scripts").OnClick = function(self) self:GetParent():Hide() end
    end },
    CharacterFrameTabButtonTemplate = { types = { Button = true }, build = function(o)
        for _, s in ipairs(TAB_TEXTURES) do Child(o, "Texture", s) end
        rawset(o, "__fontString", Child(o, "FontString", "Text"))
        o:SetFrameLevel(o:GetFrameLevel() + 4)
    end },
    OptionsFrameTabButtonTemplate = { types = { Button = true }, build = function(o)
        for _, s in ipairs(TAB_TEXTURES) do Child(o, "Texture", s) end
        rawset(o, "__fontString", Child(o, "FontString", "Text"))
        rawget(o, "__scripts").OnShow = function(self)
            PanelTemplates_TabResize(self, 0)
            _G[self:GetName() .. "HighlightTexture"]:SetWidth(self:GetTextWidth() + 30)
        end
    end },
    UICheckButtonTemplate = { types = { CheckButton = true }, build = function(o)
        Child(o, "FontString", "Text")
    end },
    InputBoxTemplate = { types = { EditBox = true }, build = function(o)
        Child(o, "Texture", "Left")
        Child(o, "Texture", "Right")
        Child(o, "Texture", "Middle")
        local s = rawget(o, "__scripts")
        s.OnEscapePressed = function(self) self:ClearFocus() end
        s.OnEditFocusLost = function() end
        s.OnEditFocusGained = function() end
    end },
    UIDropDownMenuTemplate = { types = { Frame = true }, build = function(o)
        Child(o, "Texture", "Left")
        Child(o, "Texture", "Middle")
        Child(o, "Texture", "Right")
        Child(o, "FontString", "Text")
        Child(o, "Texture", "Icon")
        local b = Child(o, "Button", "Button")
        rawget(b, "__scripts").OnClick = function(self) Stub.OpenDropDown(self:GetParent()) end
    end },
    FauxScrollFrameTemplate = { types = { ScrollFrame = true }, build = function(o)
        local bar = Child(o, "Slider", "ScrollBar")
        bar:SetMinMaxValues(0, 0)
        Child(bar, "Button", "ScrollUpButton")
        Child(bar, "Button", "ScrollDownButton")
        Child(bar, "Texture", "ThumbTexture")
        rawget(bar, "__scripts").OnValueChanged = function(self, value)
            self:GetParent():SetVerticalScroll(value)
        end
        Child(o, "Frame", "ScrollChildFrame")
        rawget(o, "__scripts").OnMouseWheel = function(self, delta)
            ScrollFrameTemplate_OnMouseWheel(self, delta)
        end
    end },
    OptionsSliderTemplate = { types = { Slider = true }, build = function(o)
        Child(o, "FontString", "Text")
        Child(o, "FontString", "Low")
        Child(o, "FontString", "High")
        Child(o, "Texture", "Thumb")
    end },
}

local TYPES = { frame = "Frame", button = "Button", checkbutton = "CheckButton", editbox = "EditBox",
    statusbar = "StatusBar", slider = "Slider", scrollframe = "ScrollFrame", gametooltip = "GameTooltip",
    messageframe = "MessageFrame" }

function CreateFrame(ftype, name, parent, template)
    local otype = TYPES[string.lower(ftype or "")]
    if not otype then
        error("CreateFrame: unknown frame type " .. tostring(ftype), 2)
    end
    if parent ~= nil and (type(parent) ~= "table" or not rawget(parent, "__type")) then
        error("CreateFrame: parent is not a frame", 2)
    end
    local o = NewObject(otype, name, parent)
    if template then
        for t in string.gmatch(template, "[^,%s]+") do
            local def = TEMPLATES[t]
            if not def then
                error("CreateFrame: unknown template " .. t, 2)
            end
            if not def.types[otype] then
                error("CreateFrame: template " .. t .. " does not fit " .. otype, 2)
            end
            def.build(o)
        end
    end
    return o
end

-------------------------------------------------------------------------------
-- Globals
-------------------------------------------------------------------------------

Stub.textures = {}
Stub.pendingText = {}
UIParentObj = NewObject("Frame", "UIParent", nil)
Minimap = NewObject("Frame", "Minimap", UIParentObj)
Minimap:SetWidth(140)
Minimap:SetHeight(140)
Minimap:SetPoint("TOPRIGHT", UIParentObj, "TOPRIGHT", -20, -20)
GameTooltip = NewObject("GameTooltip", "GameTooltip", UIParentObj)
DEFAULT_CHAT_FRAME = NewObject("MessageFrame", "ChatFrame1", UIParentObj)
UIErrorsFrame = NewObject("MessageFrame", "UIErrorsFrame", UIParentObj)

for _, fontName in ipairs(FONT_NAMES) do
    _G[fontName] = { __font = true, name = fontName }
end

UISpecialFrames = {}
StaticPopupDialogs = {}
SlashCmdList = {}
YES, NO, OKAY, CANCEL = "Yes", "No", "Okay", "Cancel"
UIDROPDOWNMENU_MENU_VALUE = nil

RAID_CLASS_COLORS = {
    HUNTER = { r = 0.67, g = 0.83, b = 0.45 }, WARLOCK = { r = 0.58, g = 0.51, b = 0.79 },
    PRIEST = { r = 1.0, g = 1.0, b = 1.0 }, PALADIN = { r = 0.96, g = 0.55, b = 0.73 },
    MAGE = { r = 0.41, g = 0.8, b = 0.94 }, ROGUE = { r = 1.0, g = 0.96, b = 0.41 },
    DRUID = { r = 1.0, g = 0.49, b = 0.04 }, SHAMAN = { r = 0.0, g = 0.44, b = 0.87 },
    WARRIOR = { r = 0.78, g = 0.61, b = 0.43 }, DEATHKNIGHT = { r = 0.77, g = 0.12, b = 0.23 },
}
CLASS_ICON_TCOORDS = {
    WARRIOR = { 0, 0.25, 0, 0.25 }, MAGE = { 0.25, 0.49609375, 0, 0.25 },
    ROGUE = { 0.49609375, 0.7421875, 0, 0.25 }, DRUID = { 0.7421875, 0.98828125, 0, 0.25 },
    HUNTER = { 0, 0.25, 0.25, 0.5 }, SHAMAN = { 0.25, 0.49609375, 0.25, 0.5 },
    PRIEST = { 0.49609375, 0.7421875, 0.25, 0.5 }, WARLOCK = { 0.7421875, 0.98828125, 0.25, 0.5 },
    PALADIN = { 0, 0.25, 0.5, 0.75 }, DEATHKNIGHT = { 0.25, 0.5, 0.5, 0.75 },
}

tinsert = table.insert
tremove = table.remove
-- the client exposes these math/string helpers as globals (FrameXML relies on them)
floor, ceil, abs, min, max, mod = math.floor, math.ceil, math.abs, math.min, math.max, math.fmod
format, strlen, strsub, strfind, gsub = string.format, string.len, string.sub, string.find, string.gsub

function GetTime() return Stub.now end
function time() return Stub.epoch + math.floor(Stub.now) end
date = os.date
function UnitName(unit) if unit == "player" then return Stub.player.name end end
function UnitGUID(unit) if unit == "player" then return Stub.player.guid end end
-- Spell info: the stub knows no spells.
function GetSpellInfo() return nil end
function UnitFactionGroup(unit) if unit == "player" then return Stub.player.faction, Stub.player.faction end end
function UnitLevel(unit) if unit == "player" then return Stub.player.level end end
function UnitIsDeadOrGhost() return nil end
function GetGuildInfo() return nil end
function GetRealZoneText() return Stub.player.zone end
function GetCursorPosition() return 0, 0 end
function InCombatLockdown() return nil end

Stub.cvars = {}
function RegisterCVar(name, default)
    if Stub.cvars[name] == nil then
        Stub.cvars[name] = default or ""
    end
end
function SetCVar(name, value)
    if Stub.cvars[name] == nil then
        error("SetCVar: unknown CVar " .. tostring(name) .. " (RegisterCVar it first)", 2)
    end
    Stub.cvars[name] = tostring(value)
end
function GetCVar(name) return Stub.cvars[name] end

function PlaySound(sound)
    if type(sound) ~= "string" then error("PlaySound expects a sound name in 3.3.5a", 2) end
end

function SendAddonMessage(prefix, text, distribution, target)
    if type(prefix) ~= "string" or type(text) ~= "string" then
        error("SendAddonMessage: bad arguments", 2)
    end
    if string.len(prefix) + 1 + string.len(text) > 255 then
        error("SendAddonMessage: message too long (" .. (string.len(prefix) + 1 + string.len(text)) .. ")", 2)
    end
    if string.find(prefix, "\t") then
        error("SendAddonMessage: prefix contains a tab", 2)
    end
    table.insert(Stub.sentAddon, { prefix = prefix, text = text, distribution = distribution, target = target })
    if Stub.onAddonSend then
        Stub.onAddonSend(prefix, text, distribution, target)
    end
end

function geterrorhandler()
    return function(err)
        table.insert(Stub.errors, tostring(err))
        print("LUA ERROR: " .. tostring(err))
    end
end

function StaticPopup_Show(which, a1, a2, data)
    local info = StaticPopupDialogs[which]
    if not info then
        return nil
    end
    local dialog = { which = which, text = string.format(info.text, a1 or "", a2 or ""), data = data, info = info }
    table.insert(Stub.popups, dialog)
    return dialog
end

function Stub.AcceptPopup()
    local dialog = table.remove(Stub.popups)
    assert(dialog, "no popup to accept")
    dialog.info.OnAccept(dialog, dialog.data)
    return dialog
end

-- FrameXML fallbacks (overridden by the real UIPanelTemplates.lua when GA_FRAMEXML is set)
function PanelTemplates_TabResize(tab, padding)
    local name = tab:GetName()
    local _ = _G[name .. "Left"]:GetWidth() + _G[name .. "Middle"]:GetWidth()
    tab:SetWidth(tab:GetTextWidth() + (padding or 24) + 40)
end

function PanelTemplates_SetNumTabs(frame, n) frame.numTabs = n end

function PanelTemplates_SetTab(frame, id)
    frame.selectedTab = id
    for i = 1, frame.numTabs do
        local tab = _G[frame:GetName() .. "Tab" .. i]
        if i == id then
            tab:Disable()
            tab:SetDisabledFontObject(GameFontHighlightSmall)
            _G[tab:GetName() .. "LeftDisabled"]:Show()
        else
            tab:Enable()
            _G[tab:GetName() .. "LeftDisabled"]:Hide()
        end
    end
end

function FauxScrollFrame_Update(frame, numItems, numToDisplay, valueStep)
    local bar = _G[frame:GetName() .. "ScrollBar"]
    local show = numItems > numToDisplay
    if show then
        frame:Show()
    else
        bar:SetValue(0)
        frame:Hide()
    end
    local range = math.max(0, (numItems - numToDisplay) * valueStep)
    bar:SetMinMaxValues(0, range)
    bar:SetValueStep(valueStep)
    _G[frame:GetName() .. "ScrollChildFrame"]:SetHeight(numItems * valueStep)
    return show and 1 or nil
end

function FauxScrollFrame_OnVerticalScroll(self, value, itemHeight, updateFunction)
    _G[self:GetName() .. "ScrollBar"]:SetValue(value)
    self.offset = math.floor((value / itemHeight) + 0.5)
    if updateFunction then
        updateFunction(self)
    end
end

function FauxScrollFrame_GetOffset(frame) return frame.offset end

function ScrollFrameTemplate_OnMouseWheel(self, value, scrollBar)
    scrollBar = scrollBar or _G[self:GetName() .. "ScrollBar"]
    if value > 0 then
        scrollBar:SetValue(scrollBar:GetValue() - (scrollBar:GetHeight() / 2))
    else
        scrollBar:SetValue(scrollBar:GetValue() + (scrollBar:GetHeight() / 2))
    end
end

-- Dropdown menus: items are collected instead of drawn.
local collecting = nil

local function CheckDropDown(frame)
    local name = frame:GetName()
    assert(name and _G[name .. "Text"] and _G[name .. "Button"] and _G[name .. "Middle"],
        "dropdown frame must inherit UIDropDownMenuTemplate and be named")
    return name
end

function UIDropDownMenu_CreateInfo() return {} end

function UIDropDownMenu_AddButton(info, level)
    if collecting then
        local copy = {}
        for k, v in pairs(info) do copy[k] = v end
        copy.level = level or 1
        table.insert(collecting, copy)
    end
end

function UIDropDownMenu_Initialize(frame, fn)
    CheckDropDown(frame)
    frame.initialize = fn
    collecting = {}
    fn(frame, nil)
    collecting = nil
end

function UIDropDownMenu_SetWidth(frame, width) CheckDropDown(frame); frame:SetWidth(width + 50) end
function UIDropDownMenu_SetText(frame, text) _G[CheckDropDown(frame) .. "Text"]:SetText(text) end
function UIDropDownMenu_GetText(frame) return _G[CheckDropDown(frame) .. "Text"]:GetText() end
function UIDropDownMenu_JustifyText(frame) CheckDropDown(frame) end
function UIDropDownMenu_EnableDropDown(frame) _G[CheckDropDown(frame) .. "Button"]:Enable() end
function UIDropDownMenu_DisableDropDown(frame) _G[CheckDropDown(frame) .. "Button"]:Disable() end
function CloseDropDownMenus() end

-- Test helper: the items a dropdown shows for a level (and parent value for submenus).
function Stub.OpenDropDown(frame, level, menuValue)
    UIDROPDOWNMENU_MENU_VALUE = menuValue
    collecting = {}
    frame.initialize(frame, level or 1)
    local items = collecting
    collecting = nil
    return items
end

-- Test helper: click a dropdown item like UIDropDownMenuButton_OnClick does.
function Stub.ClickItem(item)
    assert(not item.disabled, "dropdown item is disabled: " .. tostring(item.text))
    assert(item.func, "dropdown item has no func: " .. tostring(item.text))
    item.func({ value = item.value }, item.arg1, item.arg2, item.checked)
end

-------------------------------------------------------------------------------
-- Driving the client
-------------------------------------------------------------------------------

function Stub.FireEvent(event, ...)
    for _, o in ipairs(allObjects) do
        if rawget(o, "__events") and rawget(o, "__events")[event] then
            Fire(o, "OnEvent", event, ...)
        end
    end
end

-- Advances the clock and runs OnUpdate for visible frames.
function Stub.Advance(seconds, step)
    step = step or 0.05
    local t = 0
    while t < seconds do
        Stub.now = Stub.now + step
        t = t + step
        local changed = Stub.pendingText
        Stub.pendingText = {}
        for o in pairs(changed) do
            Fire(o, "OnTextChanged", false)
        end
        local snapshot = {}
        for i = 1, #allObjects do snapshot[i] = allObjects[i] end
        for _, o in ipairs(snapshot) do
            local fn = rawget(o, "__scripts")[ "OnUpdate"]
            if fn and IsVisible(o) then
                fn(o, step)
            end
        end
    end
end

function Stub.LoadToc(dir, tocName)
    local files = {}
    for line in io.lines(dir .. "/" .. tocName) do
        line = string.gsub(line, "\r", "")
        if line ~= "" and not string.match(line, "^##") then
            files[#files + 1] = string.gsub(line, "\\", "/")
        end
    end
    for _, file in ipairs(files) do
        local chunk, err = loadfile(dir .. "/" .. file)
        if not chunk then
            error("cannot load " .. file .. ": " .. tostring(err))
        end
        chunk("GuildAdventure", {})
    end
    return files
end

function Stub.Objects() return allObjects end

return Stub
