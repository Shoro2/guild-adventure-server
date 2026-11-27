-- Storage tab: the guild storage (docs/storage.md), its loot filter and the guild's recent loot.
--
--   Items        categories, search, quality, item level and sort, Take (Shift: the amount you type, Ctrl: everything),
--                Deposit materials; Alt+click an item
--                in your bags while this tab is open puts that stack into the storage; AH: keep X of an item and let
--                the guild leader auction the rest (docs/storage.md "Auction house").
--   Loot filter  the guild's options (filter on, what happens to loot no rule decides, auto loot) and its rules (one
--                action plus up to four conditions; checked top to bottom, the first matching rule decides).
--   Recent loot  what the guild characters looted lately and what the filter did with it.

local GA = GuildAdventure
local UI = GA.UI
local W = UI.W
local State = GA.State
local Protocol = GA.Protocol

local QUESTION = "Interface\\Icons\\INV_Misc_QuestionMark"

-- Categories (the server's StorageCategory, docs/protocol.md §7.10). The server names a category the class does not
-- say in the storage tuple's tenth field: "darkmoon" for the Darkmoon Faire cards and decks, "reputation" for the
-- reputation turn-ins (quest items to the game, both).
local CATEGORIES = {
    { key = "all", label = "All" },
    { key = "cloth", label = "Cloth", cls = 7, sub = 5 },
    { key = "leather", label = "Leather", cls = 7, sub = 6 },
    { key = "metal", label = "Metal & Stone", cls = 7, sub = 7 },
    { key = "herb", label = "Herbs", cls = 7, sub = 9 },
    { key = "meat", label = "Meat", cls = 7, sub = 8 },
    { key = "elemental", label = "Elemental", cls = 7, sub = 10 },
    { key = "enchanting", label = "Enchanting", cls = 7, sub = 12 },
    { key = "gems", label = "Gems & Jewelcrafting" },
    { key = "trade", label = "Other trade goods" },
    { key = "recipes", label = "Recipes", cls = 9 },
    { key = "consumables", label = "Consumables" },
    { key = "weapons", label = "Weapons", cls = 2 },
    { key = "armor", label = "Armor", cls = 4 },
    { key = "bags", label = "Bags" },
    { key = "reputation", label = "Reputation turn-ins" },
    { key = "darkmoon", label = "Darkmoon cards" },
    { key = "other", label = "Other" },
}

local function CategoryOf(cls, sub, cat)
    if cat and cat ~= "" then
        return cat
    end
    if cls == 7 then
        if sub == 4 then
            return "gems"
        end
        for _, c in ipairs(CATEGORIES) do
            if c.cls == 7 and c.sub == sub then
                return c.key
            end
        end
        return "trade"
    end
    if cls == 3 then
        return "gems"
    end
    if cls == 9 then
        return "recipes"
    end
    if cls == 0 or cls == 5 or cls == 6 then
        return "consumables"
    end
    if cls == 2 then
        return "weapons"
    end
    if cls == 4 then
        return "armor"
    end
    if cls == 1 or cls == 11 then
        return "bags"
    end
    return "other"
end
UI.StorageCategoryOf = CategoryOf

-- Types and slots inside a category (the owner, 2026-11-13: [quote left out]). A type is an item class and subclass (cls * 100 + sub): for armour
-- the armour kind, for weapons the weapon kind, for the rest what the subclass says. A slot is where it is worn.
local CLASS_LABELS = {
    [0] = "Consumable", [1] = "Bag", [2] = "Weapon", [3] = "Gem", [4] = "Armor", [5] = "Reagent", [6] = "Projectile",
    [7] = "Trade goods", [9] = "Recipe", [11] = "Quiver", [12] = "Quest", [13] = "Key", [15] = "Miscellaneous",
    [16] = "Glyph",
}
local SUBTYPE_LABELS = {
    [0] = { [0] = "Consumables", [1] = "Potions", [2] = "Elixirs", [3] = "Flasks", [4] = "Scrolls", [5] = "Food & drink",
        [6] = "Item enhancements", [7] = "Bandages", [8] = "Other" },
    [1] = { [0] = "Bags", [1] = "Soul bags", [2] = "Herb bags", [3] = "Enchanting bags", [4] = "Engineering bags",
        [5] = "Gem bags", [6] = "Mining bags", [7] = "Leatherworking bags", [8] = "Inscription bags" },
    [2] = { [0] = "One-handed axes", [1] = "Two-handed axes", [2] = "Bows", [3] = "Guns", [4] = "One-handed maces",
        [5] = "Two-handed maces", [6] = "Polearms", [7] = "One-handed swords", [8] = "Two-handed swords", [10] = "Staves",
        [13] = "Fist weapons", [14] = "Miscellaneous", [15] = "Daggers", [16] = "Thrown", [17] = "Spears",
        [18] = "Crossbows", [19] = "Wands", [20] = "Fishing poles" },
    [3] = { [0] = "Red", [1] = "Blue", [2] = "Yellow", [3] = "Purple", [4] = "Green", [5] = "Orange", [6] = "Meta",
        [7] = "Simple", [8] = "Prismatic" },
    [4] = { [0] = "Jewelry & misc", [1] = "Cloth", [2] = "Leather", [3] = "Mail", [4] = "Plate", [6] = "Shields",
        [7] = "Librams", [8] = "Idols", [9] = "Totems", [10] = "Sigils" },
    [5] = { [0] = "Reagents" },
    [6] = { [2] = "Arrows", [3] = "Bullets" },
    [7] = { [0] = "Trade goods", [1] = "Parts", [2] = "Explosives", [3] = "Devices", [4] = "Jewelcrafting", [5] = "Cloth",
        [6] = "Leather", [7] = "Metal & stone", [8] = "Meat", [9] = "Herbs", [10] = "Elemental", [11] = "Other",
        [12] = "Enchanting", [13] = "Materials", [14] = "Armor enchantments", [15] = "Weapon enchantments" },
    [9] = { [0] = "Books", [1] = "Leatherworking", [2] = "Tailoring", [3] = "Engineering", [4] = "Blacksmithing",
        [5] = "Cooking", [6] = "Alchemy", [7] = "First Aid", [8] = "Enchanting", [9] = "Fishing", [10] = "Jewelcrafting",
        [11] = "Inscription" },
    [11] = { [2] = "Quivers", [3] = "Ammo pouches" },
    [12] = { [0] = "Quest items" },
    [13] = { [0] = "Keys", [1] = "Lockpicks" },
    [15] = { [0] = "Junk", [1] = "Reagents", [2] = "Pets", [3] = "Holiday", [4] = "Other", [5] = "Mounts" },
}
-- Inventory types (item_template.InventoryType) as the slot filter shows them; robes count as chest, the guns' and
-- wands' "ranged right" as ranged.
local SLOT_ORDER = { 1, 2, 3, 16, 5, 4, 19, 9, 10, 6, 7, 8, 11, 12, 13, 17, 21, 22, 14, 23, 15, 25, 24, 28, 18 }
local SLOT_LABELS = {
    [1] = "Head", [2] = "Neck", [3] = "Shoulder", [4] = "Shirt", [5] = "Chest", [6] = "Waist", [7] = "Legs", [8] = "Feet",
    [9] = "Wrist", [10] = "Hands", [11] = "Finger", [12] = "Trinket", [13] = "One-hand", [14] = "Shield",
    [15] = "Ranged", [16] = "Back", [17] = "Two-hand", [18] = "Bag", [19] = "Tabard", [21] = "Main hand",
    [22] = "Off hand", [23] = "Held in off-hand", [24] = "Ammunition", [25] = "Thrown", [28] = "Relic",
}
local EQUIP_LOC_TYPES = {
    INVTYPE_HEAD = 1, INVTYPE_NECK = 2, INVTYPE_SHOULDER = 3, INVTYPE_BODY = 4, INVTYPE_CHEST = 5, INVTYPE_ROBE = 20,
    INVTYPE_WAIST = 6, INVTYPE_LEGS = 7, INVTYPE_FEET = 8, INVTYPE_WRIST = 9, INVTYPE_HAND = 10, INVTYPE_FINGER = 11,
    INVTYPE_TRINKET = 12, INVTYPE_WEAPON = 13, INVTYPE_SHIELD = 14, INVTYPE_RANGED = 15, INVTYPE_CLOAK = 16,
    INVTYPE_2HWEAPON = 17, INVTYPE_BAG = 18, INVTYPE_TABARD = 19, INVTYPE_WEAPONMAINHAND = 21,
    INVTYPE_WEAPONOFFHAND = 22, INVTYPE_HOLDABLE = 23, INVTYPE_AMMO = 24, INVTYPE_THROWN = 25,
    INVTYPE_RANGEDRIGHT = 26, INVTYPE_QUIVER = 27, INVTYPE_RELIC = 28,
}

function UI.StorageTypeKey(item)
    return (item.cls or 0) * 100 + (item.sub or 0)
end

-- The type's label; in the category "All" with its class in front ("Armor: Plate").
function UI.StorageTypeLabel(key, withClass)
    local cls, sub = math.floor(key / 100), key % 100
    local label = SUBTYPE_LABELS[cls] and SUBTYPE_LABELS[cls][sub] or "Other"
    if withClass then
        return (CLASS_LABELS[cls] or "Other") .. ": " .. label
    end
    return label
end

-- The slot an item goes in (0: none): from the server's inventory type, else from the client's item cache.
function UI.StorageSlotOf(item)
    local inv = item.inv
    if not inv then
        local equipLoc
        if GetItemInfo then
            equipLoc = select(9, GetItemInfo(item.e))
        end
        inv = equipLoc and EQUIP_LOC_TYPES[equipLoc] or 0
    end
    if inv == 20 then
        return 5
    elseif inv == 26 then
        return 15
    elseif inv == 27 then
        return 18
    end
    return SLOT_LABELS[inv] and inv or 0
end

function UI.StorageSlotLabel(slot)
    return SLOT_LABELS[slot] or "None"
end

-- The types and slots the items of a category have (only what is there), in the order the dropdowns show them.
function UI.StorageFilterChoices(items, category, subtype)
    local types, slots, seenType, seenSlot = {}, {}, {}, {}
    for _, item in pairs(items) do
        if category == "all" or CategoryOf(item.cls, item.sub, item.cat) == category then
            local key = UI.StorageTypeKey(item)
            if not seenType[key] then
                seenType[key] = true
                types[#types + 1] = key
            end
            if subtype == "all" or key == subtype then
                local slot = UI.StorageSlotOf(item)
                if slot ~= 0 then
                    seenSlot[slot] = true
                end
            end
        end
    end
    table.sort(types, function(a, b)
        local la, lb = UI.StorageTypeLabel(a, category == "all"), UI.StorageTypeLabel(b, category == "all")
        if la ~= lb then
            return la < lb
        end
        return a < b
    end)
    for _, slot in ipairs(SLOT_ORDER) do
        if seenSlot[slot] then
            slots[#slots + 1] = slot
        end
    end
    return types, slots
end

-- Loot filter vocabulary (docs/protocol.md §7.11)
local ACTIONS = {
    [0] = { label = "Keep", color = "|cffffffff", desc = "stays in the looter's bags" },
    [1] = { label = "Sell", color = "|cffffd100", desc = "vendor price to the looter" },
    [2] = { label = "Disenchant", color = "|cffa335ee", desc = "materials into the guild storage" },
    [3] = { label = "Delete", color = "|cffff4040", desc = "destroyed" },
    [4] = { label = "To storage", color = "|cff40c0ff", desc = "into the guild storage" },
}
-- Outcomes of the storage's log only (docs/protocol.md section 8.8), not loot rule actions.
local LOG_OUTCOMES = {
    [5] = { color = "|cffff9933", text = "Taken out" },
    [6] = { color = "|cff40c0ff", text = "Put in" },
    [7] = { color = "|cff1eff00", text = "Opened" },
    [8] = { color = "|cffffd100", text = "Bought" },
}
local COND_TYPES = {
    { value = 0, label = "Quality", hint = "0 poor, 1 common, 2 uncommon, 3 rare, 4 epic" },
    { value = 1, label = "Item level" },
    { value = 2, label = "Sell price", hint = "copper (1 gold = 10000)" },
    { value = 3, label = "Item type", hint = "subclass: empty = any" },
    { value = 6, label = "Item id" },
    { value = 7, label = "Name contains", hint = "letters, digits, space ' - . ," },
}
local OPS = { [0] = "is", [1] = "at least", [2] = "at most" }
local ITEM_CLASSES = {
    { value = 0, label = "Consumable" }, { value = 1, label = "Container" }, { value = 2, label = "Weapon" },
    { value = 3, label = "Gem" }, { value = 4, label = "Armor" }, { value = 5, label = "Reagent" },
    { value = 6, label = "Projectile" }, { value = 7, label = "Trade goods" }, { value = 9, label = "Recipe" },
    { value = 11, label = "Quiver" }, { value = 15, label = "Miscellaneous" },
}
local QUALITY_NAMES = { [0] = "Poor", [1] = "Common", [2] = "Uncommon", [3] = "Rare", [4] = "Epic", [5] = "Legendary",
    [6] = "Artifact", [7] = "Heirloom" }
local MAX_EDIT_CONDITIONS = 4

local function CondTypeLabel(t)
    for _, c in ipairs(COND_TYPES) do
        if c.value == t then
            return c.label
        end
    end
    return "?"
end

local function ClassLabel(v)
    for _, c in ipairs(ITEM_CLASSES) do
        if c.value == v then
            return c.label
        end
    end
    return "class " .. tostring(v)
end

-- Condition codec "type:op:value:value2:text[:group];..." <-> list of { type, op, value, value2, text, group }
local function DecodeConditions(c)
    local list = {}
    for part in string.gmatch(c or "", "[^;]+") do
        local f = {}
        for field in string.gmatch(part .. ":", "([^:]*):") do
            f[#f + 1] = field
        end
        list[#list + 1] = { type = tonumber(f[1]) or 0, op = tonumber(f[2]) or 0, value = tonumber(f[3]) or 0,
            value2 = tonumber(f[4]) or 0, text = f[5] or "", group = tonumber(f[6]) or 0 }
    end
    return list
end

local function EncodeConditions(list)
    local parts = {}
    for _, c in ipairs(list) do
        local s = string.format("%d:%d:%d:%d:%s", c.type, c.op, c.value, c.value2, c.text or "")
        if (c.group or 0) > 0 then
            s = s .. ":" .. c.group
        end
        parts[#parts + 1] = s
    end
    return table.concat(parts, ";")
end

local function DescribeCondition(c)
    if c.type == 0 then
        return "quality " .. OPS[c.op] .. " " .. (QUALITY_NAMES[c.value] or c.value)
    elseif c.type == 1 then
        return "item level " .. OPS[c.op] .. " " .. c.value
    elseif c.type == 2 then
        return "sell price " .. OPS[c.op] .. " " .. GA.Data.FormatMoney(c.value)
    elseif c.type == 3 then
        return ClassLabel(c.value) .. (c.value2 ~= 255 and (" / subclass " .. c.value2) or "")
    elseif c.type == 6 then
        local name = GetItemInfo(c.value)
        return "item " .. (name or ("#" .. c.value))
    elseif c.type == 7 then
        return "name contains \"" .. (c.text or "") .. "\""
    end
    return "?"
end

local function DescribeRule(rule)
    local conds = DecodeConditions(rule.c)
    local parts = {}
    for _, c in ipairs(conds) do
        parts[#parts + 1] = DescribeCondition(c)
    end
    local join = (rule.lg or 0) % 2 == 1 and " or " or " and "
    return table.concat(parts, join)
end
UI.DescribeLootRule = DescribeRule

-------------------------------------------------------------------------------
-- Item names: the client knows only items it has seen; ask the server for the others (a hidden tooltip), a few per
-- second, and refresh when they arrive.
-------------------------------------------------------------------------------

local scanTip
local asked = {}
local askQueue = {}
local function AskItem(entry)
    if asked[entry] then
        return
    end
    asked[entry] = true
    askQueue[#askQueue + 1] = entry
end

local askTimer
local function PumpItemQueries()
    askTimer = nil
    if #askQueue == 0 then
        return
    end
    if not scanTip then
        scanTip = CreateFrame("GameTooltip", "GuildAdventureScanTooltip", UIParent, "GameTooltipTemplate")
    end
    for _ = 1, 5 do
        local entry = table.remove(askQueue, 1)
        if not entry then
            break
        end
        scanTip:SetOwner(UIParent, "ANCHOR_NONE")
        scanTip:SetHyperlink("item:" .. entry .. ":0:0:0:0:0:0:0")
        scanTip:Hide()
    end
    UI.RequestRefresh()
    askTimer = GA.After(1, PumpItemQueries)
end

local function ItemInfo(entry, rp)
    local name, link, quality, _, _, _, _, stack, _, texture = GetItemInfo(entry)
    if not name then
        AskItem(entry)
        if not askTimer then
            askTimer = GA.After(0.2, PumpItemQueries)
        end
        return "Item #" .. entry, nil, 1, 1, QUESTION
    end
    if rp and rp ~= 0 then
        -- a random property ("of the Bear"): the link with it names the suffix
        local _, rpLink = GetItemInfo("item:" .. entry .. ":0:0:0:0:0:" .. rp .. ":0")
        link = rpLink or link
        name = rpLink and string.match(rpLink, "%[(.-)%]") or name
    end
    return name, link, quality or 1, stack or 1, texture or QUESTION
end
UI.StorageItemInfo = ItemInfo

local function QualityColor(quality)
    local c = ITEM_QUALITY_COLORS and ITEM_QUALITY_COLORS[quality or 1]
    return c and c.hex or "|cffffffff"
end

-------------------------------------------------------------------------------
-- Items view
-------------------------------------------------------------------------------

-- Take: one stack; Ctrl: everything; Shift: a box for the amount (the owner, 2026-11-25: [quote left out]). nil: the box decides.
local function TakeAmount(item)
    local _, _, _, stack = ItemInfo(item.e, item.rp)
    stack = math.max(1, stack or 1)
    if IsControlKeyDown() then
        return item.n
    elseif IsShiftKeyDown() then
        return nil
    end
    return math.min(item.n, stack)
end

function UI.TakeFromStorage(item, n)
    n = math.floor(tonumber(n) or 0)
    if n <= 0 then
        return
    end
    Protocol.StorageTake(item.e, item.rp, math.min(n, item.n), UI.RequestCallback(nil))
end

function UI.AskStorageTake(item)
    local name = ItemInfo(item.e, item.rp)
    local _, _, _, stack = ItemInfo(item.e, item.rp)
    StaticPopup_Show("GUILDADVENTURE_STORAGE_AMOUNT",
        string.format("Take how many %s?\nThe guild storage holds %d (one stack is %d).", name or ("item " .. item.e), item.n,
            math.max(1, stack or 1)),
        nil, { current = math.min(item.n, math.max(1, stack or 1)), onAccept = function(n)
            UI.TakeFromStorage(item, n)
        end })
end

local function CreateItemRow(list)
    local row = CreateFrame("Button", nil, list)
    row:RegisterForClicks("LeftButtonUp")
    row.shade = row:CreateTexture(nil, "BACKGROUND")
    row.shade:SetAllPoints(row)
    row.shade:SetTexture("Interface\\Buttons\\WHITE8X8")
    row.icon = W.Icon(row, 18, QUESTION)
    row.icon:SetPoint("LEFT", row, "LEFT", 4, 0)
    row.take = W.Button(row, "Take", 56, 18, function(self)
        local item = self:GetParent().item
        if item then
            local n = TakeAmount(item)
            if n then
                UI.TakeFromStorage(item, n)
            else
                UI.AskStorageTake(item)
            end
        end
    end)
    row.take:SetPoint("RIGHT", row, "RIGHT", -4, 0)
    W.SetTooltip(row.take, "Take", { "Click: one stack into the bags of the character you play.",
        "Shift-click: a box opens where you type how many.", "Ctrl-click: everything." })
    row.ah = W.Button(row, "AH", 36, 18, function(self)
        local item = self:GetParent().item
        if item then
            UI.OpenAuctionRuleEditor(item)
        end
    end)
    row.ah:SetPoint("RIGHT", row.take, "LEFT", -4, 0)
    -- The reserve (the owner, 2026-11-13): how many of it the bots leave in the storage.
    row.res = W.Button(row, "Res", 36, 18, function(self)
        local item = self:GetParent().item
        if item then
            UI.AskStorageReserve(item)
        end
    end)
    row.res:SetPoint("RIGHT", row.ah, "LEFT", -4, 0)
    W.SetTooltip(row.res, "Reserve", function(owner)
        return UI.ReserveHint(owner:GetParent().item)
    end)
    -- Sell, Sell all, Disenchant (docs/storage.md "Selling and disenchanting"): the owner's own orders, each with a line
    -- in Recent loot.
    row.de = W.Button(row, "DE", 30, 18, function(self)
        local item = self:GetParent().item
        if item then
            UI.DisenchantStorageItem(item)
        end
    end)
    row.de:SetPoint("RIGHT", row.res, "LEFT", -4, 0)
    W.SetTooltip(row.de, "Disenchant", function(owner)
        return UI.DisenchantHint(owner:GetParent().item)
    end)
    row.sellAll = W.Button(row, "All", 32, 18, function(self)
        local item = self:GetParent().item
        if item and (item.sell or 0) <= 0 then
            UI.DeleteStorageItem(item, true)
        elseif item then
            UI.SellStorageItem(item, true)
        end
    end)
    row.sellAll:SetPoint("RIGHT", row.de, "LEFT", -4, 0)
    W.SetTooltip(row.sellAll, "Sell all", function(owner)
        local item = owner:GetParent().item
        if item and (item.sell or 0) <= 0 then
            return UI.DeleteHint(item, true)
        end
        return UI.SellHint(item, true)
    end)
    row.sell = W.Button(row, "Sell", 38, 18, function(self)
        local item = self:GetParent().item
        if item and (item.sell or 0) <= 0 then
            UI.DeleteStorageItem(item, false)
        elseif item then
            UI.SellStorageItem(item, false)
        end
    end)
    row.sell:SetPoint("RIGHT", row.sellAll, "LEFT", -4, 0)
    W.SetTooltip(row.sell, "Sell one", function(owner)
        local item = owner:GetParent().item
        if item and (item.sell or 0) <= 0 then
            return UI.DeleteHint(item, false)
        end
        return UI.SellHint(item, false)
    end)
    row.count = W.Label(row, "", "GameFontHighlightSmall", "RIGHT")
    row.count:SetPoint("RIGHT", row.sell, "LEFT", -8, 0)
    row.count:SetWidth(96)
    row.name = W.Label(row, "", "GameFontHighlightSmall")
    row.name:SetPoint("LEFT", row.icon, "RIGHT", 6, 0)
    row.name:SetPoint("RIGHT", row.count, "LEFT", -6, 0)
    row:SetScript("OnEnter", function(self)
        if not self.item then
            return
        end
        GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
        GameTooltip:SetHyperlink("item:" .. self.item.e .. ":0:0:0:0:0:" .. (self.item.rp or 0) .. ":0")
        GameTooltip:AddLine(" ")
        GameTooltip:AddLine(string.format("In the guild storage: %d", self.item.n), 0.25, 0.75, 1)
        if (self.item.res or 0) > 0 then
            GameTooltip:AddLine(string.format("Reserve: the bots leave %d of it here (they may take %d).", self.item.res,
                math.max(0, self.item.n - self.item.res)), 0.25, 0.75, 1, 1)
        end
        local rule, fromCategory = State.EffectiveAuctionRule(self.item, CategoryOf)
        if rule and not fromCategory then
            GameTooltip:AddLine(string.format("Auction house: keep %d, the rest in stacks of %d at %s each%s; %d listed now",
                rule.keep or 0, rule.stack or 1, GA.Data.FormatMoney((rule.price or 0) > 0 and rule.price or (rule.auto or 0)),
                (rule.price or 0) > 0 and "" or " (default)", State.AuctionListedCount(self.item.e, self.item.rp)), 1, 0.82, 0, 1)
        elseif rule then
            GameTooltip:AddLine(string.format("Auction house (rule of the category %s): keep %d of it, the rest at the default "
                .. "price; %d listed now", UI.StorageCategoryLabel(fromCategory), rule.keep or 0,
                State.AuctionListedCount(self.item.e, self.item.rp)), 1, 0.82, 0, 1)
        end
        local _, _, _, stackSize = ItemInfo(self.item.e, self.item.rp)
        local surplus = State.AuctionSurplus(self.item, rule, stackSize)
        if surplus > 0 then
            GameTooltip:AddLine(string.format("%d of these go into the auction house at the next pass (every few minutes); the "
                .. "gold goes into the guild bank.", surplus), 1, 0.82, 0, 1)
        end
        GameTooltip:AddLine("Take: one stack. Shift: how many you type. Ctrl: everything. AH: auction what is above a quantity.",
            0.7, 0.7, 0.7, 1)
        GameTooltip:Show()
    end)
    row:SetScript("OnLeave", function()
        GameTooltip:Hide()
    end)
    row:SetScript("OnClick", function(self)
        -- Shift+click links the item into the chat box, as in the bags.
        if self.item and IsModifiedClick("CHATLINK") then
            local _, link = ItemInfo(self.item.e, self.item.rp)
            if link then
                ChatEdit_InsertLink(link)
            end
        end
    end)
    return row
end

local function UpdateItemRow(row, item, index)
    row.item = item
    row.shade:SetVertexColor(1, 1, 1, index % 2 == 0 and 0.05 or 0)
    local name, _, quality, stackSize, texture = ItemInfo(item.e, item.rp)
    row.icon:SetTexture(texture)
    local rule, fromCategory = State.EffectiveAuctionRule(item, CategoryOf)
    local listed = State.AuctionListedCount(item.e, item.rp)
    -- What an auction rule will sell is marked before it is listed (the owner, 2026-11-13): a gold [AH] before the name and
    -- a gold row; what is listed already stays "(+n AH)" after the count.
    local surplus = State.AuctionSurplus(item, rule, stackSize)
    row.name:SetText((surplus > 0 and "|cffffd100[AH " .. surplus .. "]|r " or "") .. QualityColor(quality) .. name .. "|r")
    if surplus > 0 then
        row.shade:SetVertexColor(1, 0.82, 0, 0.12)
    end
    row.count:SetText(tostring(item.n) .. ((item.res or 0) > 0 and (" |cff40c0ffres " .. item.res .. "|r") or "")
        .. (listed > 0 and (" |cffffd100(+" .. listed .. " AH)|r") or ""))
    row.res:SetText((item.res or 0) > 0 and "|cff40c0ffRes|r" or "Res")
    W.SetEnabled(row.res, State.IsConnected())
    local own = State.AuctionRule(item.e, item.rp)
    row.ah:SetText(own and (own.on ~= false and "|cffffd100AH|r" or "|cff808080AH|r")
        or (fromCategory and (rule.on ~= false and "|cffd0a000AH|r" or "|cff808080AH|r")) or "AH")
    W.SetEnabled(row.take, State.IsConnected())
    W.SetEnabled(row.ah, State.IsConnected() and State.auctions.srv ~= false)
    -- No vendor buys it: Delete in place of Sell (the owner, 2026-11-13), always after asking.
    local noPrice = (item.sell or 0) <= 0
    row.sell:SetText(noPrice and "|cffff6060Del|r" or "Sell")
    row.sellAll:SetText(noPrice and "|cffff6060All|r" or "All")
    W.SetEnabled(row.sell, State.IsConnected())
    W.SetEnabled(row.sellAll, State.IsConnected())
    -- Disenchant only for green and blue weapons and armour; greyed while no enchanter of the guild has the skill.
    if (item.ds or -1) >= 0 then
        row.de:Show()
        W.SetEnabled(row.de, State.IsConnected() and UI.CanDisenchant(item))
    else
        row.de:Hide()
    end
end

-- The reserve of a storage item (the owner, 2026-11-13: [quote left out]): the server keeps it from every automatic take.
function UI.ReserveHint(item)
    if not item then
        return {}
    end
    local lines = {}
    if (item.res or 0) > 0 then
        lines[#lines + 1] = string.format("The bots leave %d in the guild storage (they may take %d).", item.res,
            math.max(0, (item.n or 0) - item.res))
    else
        lines[#lines + 1] = "No reserve: the bots may take all of it."
    end
    lines[#lines + 1] = "A reserve is kept from everything the bots do by themselves: supplies while they level, crafting,"
    lines[#lines + 1] = "orders, the auction rules and Sell junk. Your own Take, Sell and DE are not held back by it."
    return lines
end

StaticPopupDialogs["GUILDADVENTURE_STORAGE_RESERVE"] = {
    text = "%s",
    button1 = OKAY,
    button2 = CANCEL,
    hasEditBox = 1,
    maxLetters = 7,
    OnShow = function(self, data)
        local box = self.editBox or _G[self:GetName() .. "EditBox"]
        if box then
            box:SetNumeric(true)
            box:SetText(tostring(data and data.current or 0))
            box:HighlightText()
            box:SetFocus()
        end
    end,
    OnAccept = function(self, data)
        local box = self.editBox or _G[self:GetName() .. "EditBox"]
        if data and data.onAccept and box then
            data.onAccept(tonumber(box:GetText()) or 0)
        end
    end,
    EditBoxOnEnterPressed = function(self)
        local parent = self:GetParent()
        local data = parent.data
        if data and data.onAccept then
            data.onAccept(tonumber(self:GetText()) or 0)
        end
        parent:Hide()
    end,
    EditBoxOnEscapePressed = function(self)
        self:GetParent():Hide()
    end,
    timeout = 0,
    whileDead = 1,
    hideOnEscape = 1,
}
-- The same box for the amount a Shift-click on Take asks for (UI.AskStorageTake).
StaticPopupDialogs["GUILDADVENTURE_STORAGE_AMOUNT"] = StaticPopupDialogs["GUILDADVENTURE_STORAGE_RESERVE"]

function UI.AskStorageReserve(item)
    local name = ItemInfo(item.e, item.rp)
    StaticPopup_Show("GUILDADVENTURE_STORAGE_RESERVE",
        string.format("Reserve for %s:\nthe bots leave this many in the guild storage (0 = no reserve).", name or ("item " .. item.e)),
        nil, { current = item.res or 0, onAccept = function(n)
            UI.SetStorageReserve(item, n)
        end })
end

function UI.SetStorageReserve(item, n)
    Protocol.SetStorageReserve(item.e, item.rp, math.max(0, math.floor(n or 0)), UI.RequestCallback(nil))
end

-- Whether the guild's best enchanter in the world can disenchant this storage item.
function UI.CanDisenchant(item)
    return item and (item.ds or -1) >= 0 and (State.storage.ench or -1) >= item.ds
end

function UI.DisenchantHint(item)
    if not item then
        return {}
    end
    local lines = { "Has the guild's best enchanter in the world disenchant one of these; the materials go into the guild storage." }
    if (State.storage.ench or -1) < 0 then
        lines[#lines + 1] = "|cffff4040No character of the guild in the world knows Enchanting.|r"
    elseif (State.storage.ench or 0) < (item.ds or 0) then
        lines[#lines + 1] = string.format("|cffff4040It needs Enchanting %d; %s has %d.|r", item.ds, State.storage.enchn or "?",
            State.storage.ench)
    else
        lines[#lines + 1] = string.format("%s (Enchanting %d) does it.", State.storage.enchn or "?", State.storage.ench)
    end
    return lines
end

function UI.SellHint(item, all)
    if not item then
        return {}
    end
    if (item.sell or 0) <= 0 then
        return { "|cffff4040No vendor buys this.|r" }
    end
    local n = all and item.n or 1
    return { string.format("Sells %d at the vendor price for %s. The gold goes into the guild bank.", n,
        GA.Data.FormatMoney(n * item.sell)) }
end

function UI.DeleteHint(item, all)
    if not item then
        return {}
    end
    local n = all and item.n or 1
    return { string.format("No vendor buys this: deletes %d from the guild storage (asks first; it cannot be undone).", n),
        "A line in Recent loot says who deleted what." }
end

-- Delete one (or all) of a storage item no vendor buys; always asks first.
function UI.DeleteStorageItem(item, all)
    local name = ItemInfo(item.e, item.rp)
    local n = all and item.n or 1
    UI.Confirm(string.format("Delete %d x %s from the guild storage?\nNo vendor buys it. This cannot be undone.", n,
        name or ("item " .. item.e)), function()
        Protocol.StorageDelete(item.e, item.rp, all and 0 or 1, UI.RequestCallback(nil))
    end)
end

-- Sell one (or all) of a storage item; asks first for all, and for green and better items.
function UI.SellStorageItem(item, all)
    local name, _, quality = ItemInfo(item.e, item.rp)
    local n = all and item.n or 1
    local send = function()
        Protocol.StorageSell(item.e, item.rp, all and 0 or 1, UI.RequestCallback(nil))
    end
    if all or (quality or 0) >= 2 then
        UI.Confirm(string.format("Sell %d x %s from the guild storage for %s?", n, name or ("item " .. item.e),
            GA.Data.FormatMoney(n * (item.sell or 0))), send)
    else
        send()
    end
end

function UI.DisenchantStorageItem(item)
    local name = ItemInfo(item.e, item.rp)
    UI.Confirm(string.format("Have %s disenchant 1 x %s from the guild storage?\nThe materials go into the guild storage.",
        State.storage.enchn ~= "" and State.storage.enchn or "the guild's enchanter", name or ("item " .. item.e)), function()
        Protocol.StorageDisenchant(item.e, item.rp, UI.RequestCallback(nil))
    end)
end

local function CreateCategoryRow(list)
    local row = CreateFrame("Button", nil, list)
    row.highlight = row:CreateTexture(nil, "BACKGROUND")
    row.highlight:SetAllPoints(row)
    row.highlight:SetTexture("Interface\\Buttons\\WHITE8X8")
    row.label = W.Label(row, "", "GameFontHighlightSmall")
    row.label:SetPoint("LEFT", row, "LEFT", 6, 0)
    row.count = W.Label(row, "", "GameFontDisableSmall", "RIGHT")
    row.count:SetPoint("RIGHT", row, "RIGHT", -4, 0)
    row:SetScript("OnClick", function(self)
        local view = list.view
        if view.category ~= self.category.key then
            -- a new category: its types and slots are others
            view.subtype, view.slot = "all", "all"
            view.typeDD:SetSelected("all", "All types")
            view.slotDD:SetSelected("all", "All slots")
        end
        view.category = self.category.key
        UI.RequestRefresh()
    end)
    return row
end

local function UpdateCategoryRow(row, category)
    row.category = category
    local selected = row:GetParent().view.category == category.key
    row.highlight:SetVertexColor(1, 0.82, 0, selected and 0.18 or 0)
    local rule = category.key ~= "all" and State.AuctionCategoryRule(category.key)
    row.label:SetText((selected and "|cffffd100" or "|cffffffff") .. category.label .. "|r"
        .. (rule and (rule.on ~= false and " |cffffd100AH|r" or " |cff808080AH|r") or ""))
    row.count:SetText(category.count and category.count > 0 and tostring(category.count) or "")
end

local function CreateItemsView(parent)
    local v = CreateFrame("Frame", nil, parent)
    v.category = "all"
    v.subtype = "all"
    v.slot = "all"
    v.quality = "all"
    v.sort = "name"

    local catPanel = W.Panel(v)
    catPanel:SetPoint("TOPLEFT", v, "TOPLEFT", 0, 0)
    catPanel:SetPoint("BOTTOMLEFT", v, "BOTTOMLEFT", 0, 34)
    catPanel:SetWidth(170)
    v.categories = W.ScrollList(catPanel, { rowHeight = 18, createRow = CreateCategoryRow, updateRow = UpdateCategoryRow })
    v.categories.view = v
    v.categories:SetPoint("TOPLEFT", catPanel, "TOPLEFT", 4, -4)
    v.categories:SetPoint("BOTTOMRIGHT", catPanel, "BOTTOMRIGHT", -4, 4)

    -- The label sits left of the box: above it, it was hidden behind the view buttons.
    local searchLabel = W.Label(v, "Search:", "GameFontNormalSmall")
    searchLabel:SetPoint("TOPLEFT", catPanel, "TOPRIGHT", 8, -6)
    v.search = W.EditBox(v, 115, false, 40)
    v.search:SetPoint("LEFT", searchLabel, "RIGHT", 10, 0)
    v.search:SetScript("OnTextChanged", function()
        UI.RequestRefresh()
    end)

    -- Type and slot inside the chosen category: only what the storage holds there.
    v.typeChoices, v.slotChoices = {}, {}
    v.typeDD = W.DropDown(v, 115, function()
        local items = { { text = "All types", value = "all" } }
        for _, key in ipairs(v.typeChoices) do
            items[#items + 1] = { text = UI.StorageTypeLabel(key, v.category == "all"), value = key }
        end
        return items
    end)
    v.typeDD:SetPoint("LEFT", v.search, "RIGHT", -6, -2)
    v.typeDD:SetSelected("all", "All types")
    v.typeDD.onSelect = function(value)
        v.subtype = value
        v.slot = "all"
        v.slotDD:SetSelected("all", "All slots")
        UI.RequestRefresh()
    end
    v.slotDD = W.DropDown(v, 95, function()
        local items = { { text = "All slots", value = "all" } }
        for _, slot in ipairs(v.slotChoices) do
            items[#items + 1] = { text = UI.StorageSlotLabel(slot), value = slot }
        end
        return items
    end)
    v.slotDD:SetPoint("LEFT", v.typeDD, "RIGHT", -28, 0)
    v.slotDD:SetSelected("all", "All slots")
    v.slotDD.onSelect = function(value)
        v.slot = value
        UI.RequestRefresh()
    end
    v.totals = W.Label(v, "", "GameFontHighlightSmall", "RIGHT")
    v.totals:SetPoint("RIGHT", v, "RIGHT", -8, 0)
    v.totals:SetPoint("TOP", v.search, "TOP", 0, -4)

    -- Second row (the owner, 2026-11-25: [quote left out]): quality, the lowest item level of weapons and armour, and the order of the list.
    local qualityLabel = W.Label(v, "Quality:", "GameFontNormalSmall")
    qualityLabel:SetPoint("TOPLEFT", catPanel, "TOPRIGHT", 8, -32)
    v.qualityDD = W.DropDown(v, 105, function()
        local items = { { text = "All qualities", value = "all" } }
        for q = 0, 5 do
            items[#items + 1] = { text = QualityColor(q) .. QUALITY_NAMES[q] .. "|r", value = q }
        end
        for q = 2, 4 do
            items[#items + 1] = { text = QualityColor(q) .. QUALITY_NAMES[q] .. " or better|r", value = q + 100 }
        end
        return items
    end)
    v.qualityDD:SetPoint("LEFT", qualityLabel, "RIGHT", -12, -2)
    v.qualityDD:SetSelected("all", "All qualities")
    v.qualityDD.onSelect = function(value)
        v.quality = value
        UI.RequestRefresh()
    end
    local levelLabel = W.Label(v, "iLvl from:", "GameFontNormalSmall")
    levelLabel:SetPoint("LEFT", v.qualityDD, "RIGHT", -8, 2)
    v.minLevel = W.EditBox(v, 34, true, 3)
    v.minLevel:SetPoint("LEFT", levelLabel, "RIGHT", 8, 0)
    v.minLevel:SetScript("OnTextChanged", function()
        UI.RequestRefresh()
    end)
    W.SetTooltip(v.minLevel, "Item level", { "Shows only weapons and armour of this item level or higher.",
        "Empty: no limit. Sort by item level below to see the best first." })
    local sortLabel = W.Label(v, "Sort:", "GameFontNormalSmall")
    sortLabel:SetPoint("LEFT", v.minLevel, "RIGHT", 10, 0)
    v.sortDD = W.DropDown(v, 95, function()
        return {
            { text = "Name", value = "name" },
            { text = "Quality", value = "quality" },
            { text = "Item level", value = "ilvl" },
            { text = "Amount", value = "count" },
        }
    end)
    v.sortDD:SetPoint("LEFT", sortLabel, "RIGHT", -12, -2)
    v.sortDD:SetSelected("name", "Name")
    v.sortDD.onSelect = function(value)
        v.sort = value
        UI.RequestRefresh()
    end

    local listPanel = W.Panel(v)
    listPanel:SetPoint("TOPLEFT", catPanel, "TOPRIGHT", 6, -52)
    listPanel:SetPoint("BOTTOMRIGHT", v, "BOTTOMRIGHT", 0, 34)
    v.list = W.ScrollList(listPanel, { rowHeight = 22, createRow = CreateItemRow, updateRow = UpdateItemRow })
    v.list:SetPoint("TOPLEFT", listPanel, "TOPLEFT", 6, -6)
    v.list:SetPoint("BOTTOMRIGHT", listPanel, "BOTTOMRIGHT", -6, 6)
    v.empty = W.Label(listPanel, "", "GameFontDisable", "CENTER")
    v.empty:SetPoint("CENTER", listPanel, "CENTER", 0, 0)
    v.empty:SetWidth(380)

    v.deposit = W.Button(v, "Deposit materials", 150, 22, function()
        Protocol.StorageDeposit(nil, nil, UI.RequestCallback(nil))
    end)
    v.deposit:SetPoint("BOTTOMLEFT", v, "BOTTOMLEFT", 0, 4)
    W.SetTooltip(v.deposit, "Deposit materials", {
        "Puts the trade goods, gems, recipes and food in the bags of the character you play into the guild storage.",
        "Alt+click any other item in your bags while this tab is open to put just that stack in.",
    })
    v.sellJunk = W.Button(v, "Sell junk", 100, 22, function()
        UI.SellStorageJunk()
    end)
    v.sellJunk:SetPoint("LEFT", v.deposit, "RIGHT", 6, 0)
    W.SetTooltip(v.sellJunk, "Sell junk", {
        "Sells what lies in the guild storage that the loot filter now sells at once: grey items, and white weapons and armour",
        "no guild character would wear. Crafting materials, quest items, consumables, bags and ammunition stay.",
        "You see the amount and the price first; the gold goes into the guild bank.",
    })
    v.catAuction = W.Button(v, "Category AH", 110, 22, function()
        if v.category ~= "all" then
            UI.OpenAuctionCategoryEditor(v.category)
        end
    end)
    v.catAuction:SetPoint("LEFT", v.sellJunk, "RIGHT", 6, 0)
    W.SetTooltip(v.catAuction, "Auction rule for the category", {
        "From which amount on the items of the chosen category go into the auction house: every kind of it that has no rule",
        "of its own keeps that many in the storage, the rest is auctioned by the guild leader at the default price.",
        "The gold goes into the guild bank. Choose a category on the left first.",
    })
    v.hint = W.Label(v, "Alt+click an item in your bags to store it.  Everything the bots loot lands here (Loot filter).",
        "GameFontDisableSmall")
    v.hint:SetPoint("LEFT", v.catAuction, "RIGHT", 10, 0)
    v.hint:SetPoint("RIGHT", v, "RIGHT", -8, 0)
    return v
end

-- What the item list says when it shows nothing (also for the offline tests).
function UI.StorageEmptyText(storage, shown, search, category, filtered)
    if not State.IsConnected() then
        return "Not connected."
    elseif not storage.loaded then
        return "Loading the guild storage..."
    elseif shown > 0 then
        return ""
    elseif (storage.kinds or 0) == 0 and next(storage.items) == nil then
        local how = (#State.perks > 0 and not State.HasPerk("materials_storage"))
            and "The bots' loot lands here once the guild has the perk Material Haulers (Perks tab)."
            or "What the bots loot lands here (see Loot filter)."
        return "The guild storage is empty.\n\n" .. how
            .. "\nDeposit materials or Alt+click an item in your bags to put your own things in."
    elseif search ~= "" then
        return "Nothing matches \"" .. search .. "\"."
    elseif filtered then
        return "Nothing of this type, slot, quality or item level here."
    elseif category ~= "all" then
        return "Nothing in this category."
    end
    return "Nothing to show."
end

local function RefreshItemsView(v)
    local storage = State.storage
    local search = string.lower(v.search:GetText() or "")
    local counts = { all = 0 }
    for _, item in pairs(storage.items) do
        local key = CategoryOf(item.cls, item.sub, item.cat)
        counts[key] = (counts[key] or 0) + 1
        counts.all = counts.all + 1
    end
    local cats = {}
    for _, c in ipairs(CATEGORIES) do
        if c.key == "all" or (counts[c.key] or 0) > 0 or v.category == c.key then
            cats[#cats + 1] = { key = c.key, label = c.label, count = counts[c.key] or 0 }
        end
    end
    v.categories:SetItems(cats)

    v.typeChoices, v.slotChoices = UI.StorageFilterChoices(storage.items, v.category, v.subtype)
    v.slotDD:SetEnabled(#v.slotChoices > 0 or v.slot ~= "all")
    local minLevel = tonumber(v.minLevel:GetText() or "")
    local list = State.StorageList(function(item)
        if v.category ~= "all" and CategoryOf(item.cls, item.sub, item.cat) ~= v.category then
            return false
        end
        if v.subtype ~= "all" and UI.StorageTypeKey(item) ~= v.subtype then
            return false
        end
        if v.slot ~= "all" and UI.StorageSlotOf(item) ~= v.slot then
            return false
        end
        if v.quality ~= "all" then
            local q = State.StorageQuality(item)
            if (v.quality >= 100 and q < v.quality - 100) or (v.quality < 100 and q ~= v.quality) then
                return false
            end
        end
        if minLevel and minLevel > 0 and ((item.cls ~= 2 and item.cls ~= 4) or State.StorageItemLevel(item) < minLevel) then
            return false
        end
        if search ~= "" then
            local name = ItemInfo(item.e, item.rp)
            return string.find(string.lower(name), search, 1, true) ~= nil
        end
        return true
    end, v.sort)
    v.list:SetItems(list)
    v.totals:SetText(string.format("%d kinds, %d items", storage.kinds or 0, storage.total or 0))
    v.empty:SetText(UI.StorageEmptyText(storage, #list, search, v.category, v.subtype ~= "all" or v.slot ~= "all"
        or v.quality ~= "all" or (minLevel or 0) > 0))
    W.SetEnabled(v.deposit, State.IsConnected())
    W.SetEnabled(v.sellJunk, State.IsConnected() and UI.JunkQuality() >= 0)
    W.SetEnabled(v.catAuction, State.IsConnected() and v.category ~= "all" and State.auctions.srv ~= false)
end

function UI.StorageCategoryLabel(key)
    for _, c in ipairs(CATEGORIES) do
        if c.key == key then
            return c.label
        end
    end
    return key or "?"
end

-- The highest quality the loot filter sells as junk for the guild, -1 when it sells none (the bot settings
-- LootFilter.SellJunk / .SellJunkQuality when they are loaded, else what the server said in LOOT_SETTINGS).
function UI.JunkQuality()
    local s = State.loot.settings
    if s.junkok == false then
        return -1
    end
    local on = State.SettingDef("LootFilter.SellJunk")
    local quality = State.SettingDef("LootFilter.SellJunkQuality")
    if on and quality then
        if (State.SettingValue(on) or 0) == 0 then
            return -1
        end
        return math.min(State.SettingValue(quality) or 1, 1)
    end
    return s.junk or -1
end

-- Whether grey and white gear a guild character would wear stays out of the junk (bot setting LootFilter.KeepWornGear).
function UI.KeepWornGear()
    local def = State.SettingDef("LootFilter.KeepWornGear")
    if def then
        return (State.SettingValue(def) or 0) ~= 0
    end
    return State.loot.settings.kwg == true
end

-- Asks the server what of the storage is junk, then sells it after a confirmation.
function UI.SellStorageJunk()
    Protocol.SellStorageJunk(false, function(ok, code, msg, reason)
        if not ok then
            UI.Notify(Protocol.ErrorText(code, msg, reason), true)
            return
        end
        local j = State.storage.junk or {}
        if (j.items or 0) == 0 then
            UI.Notify("No junk in the guild storage.")
            return
        end
        UI.Confirm(string.format("Sell %d junk items (%d kinds) from the guild storage for %s?\nThe gold goes to %s.",
            j.items, j.kinds or 0, GA.Data.FormatMoney(j.money or 0), j.to or "you"), function()
            Protocol.SellStorageJunk(true, UI.RequestCallback(nil))
        end)
    end)
end

-------------------------------------------------------------------------------
-- Auction house surplus: per item "keep X, auction the rest" (docs/storage.md "Auction house")
-------------------------------------------------------------------------------

local auctionEditor

local function MoneyFromBoxes(e)
    local g = tonumber(e.gold:GetText()) or 0
    local s = tonumber(e.silver:GetText()) or 0
    local c = tonumber(e.copper:GetText()) or 0
    return math.max(0, math.floor(g) * 10000 + math.floor(s) * 100 + math.floor(c))
end

local function SetMoneyBoxes(e, copper)
    copper = copper or 0
    W.SetEditText(e.gold, copper >= 10000 and tostring(math.floor(copper / 10000)) or "")
    W.SetEditText(e.silver, copper >= 100 and tostring(math.floor(copper / 100) % 100) or "")
    W.SetEditText(e.copper, copper > 0 and tostring(copper % 100) or "")
end

local function CreateAuctionEditor()
    local e = CreateFrame("Frame", "GuildAdventureAuctionRuleEditor", UIParent)
    e:SetWidth(420)
    e:SetHeight(238)
    e:SetPoint("CENTER", UIParent, "CENTER", 0, 80)
    e:SetFrameStrata("DIALOG")
    e:SetBackdrop(W.BACKDROP_DIALOG)
    W.DarkenBackground(e, 0.7)
    e:EnableMouse(true)
    e:SetMovable(true)
    e:RegisterForDrag("LeftButton")
    e:SetScript("OnDragStart", e.StartMoving)
    e:SetScript("OnDragStop", e.StopMovingOrSizing)
    e:Hide()
    tinsert(UISpecialFrames, "GuildAdventureAuctionRuleEditor")

    e.title = W.Label(e, "Auction house", "GameFontNormalLarge")
    e.title:SetPoint("TOP", e, "TOP", 0, -18)
    e.info = W.Label(e, "", "GameFontHighlightSmall", "CENTER")
    e.info:SetPoint("TOP", e.title, "BOTTOM", 0, -6)
    e.info:SetWidth(380)

    local keepLabel = W.Label(e, "Keep in the storage:", "GameFontNormalSmall")
    keepLabel:SetPoint("TOPLEFT", e, "TOPLEFT", 26, -70)
    e.keep = W.EditBox(e, 70, true, 7)
    e.keep:SetPoint("LEFT", keepLabel, "RIGHT", 12, 0)
    local stackLabel = W.Label(e, "Items per auction:", "GameFontNormalSmall")
    stackLabel:SetPoint("TOPLEFT", keepLabel, "BOTTOMLEFT", 0, -16)
    e.stack = W.EditBox(e, 70, true, 4)
    e.stack:SetPoint("LEFT", stackLabel, "RIGHT", 22, 0)
    local priceLabel = W.Label(e, "Price per item:", "GameFontNormalSmall")
    priceLabel:SetPoint("TOPLEFT", stackLabel, "BOTTOMLEFT", 0, -16)
    e.gold = W.EditBox(e, 46, true, 6)
    e.gold:SetPoint("LEFT", priceLabel, "RIGHT", 36, 0)
    local gl = W.Label(e, "g", "GameFontHighlightSmall")
    gl:SetPoint("LEFT", e.gold, "RIGHT", 2, 0)
    e.silver = W.EditBox(e, 30, true, 2)
    e.silver:SetPoint("LEFT", gl, "RIGHT", 10, 0)
    local sl = W.Label(e, "s", "GameFontHighlightSmall")
    sl:SetPoint("LEFT", e.silver, "RIGHT", 2, 0)
    e.copper = W.EditBox(e, 30, true, 2)
    e.copper:SetPoint("LEFT", sl, "RIGHT", 10, 0)
    local cl = W.Label(e, "c", "GameFontHighlightSmall")
    cl:SetPoint("LEFT", e.copper, "RIGHT", 2, 0)
    e.priceHint = W.Label(e, "", "GameFontDisableSmall")
    e.priceHint:SetPoint("TOPLEFT", priceLabel, "BOTTOMLEFT", 0, -8)
    e.priceHint:SetWidth(370)
    e.on = W.CheckBox(e, "On (the leader posts the surplus every few minutes)")
    e.on:SetPoint("TOPLEFT", e.priceHint, "BOTTOMLEFT", -4, -6)
    e.priceWidgets = { priceLabel, e.gold, gl, e.silver, sl, e.copper, cl }

    e.save = W.Button(e, "Save", 96, 22, function()
        if e.category then
            Protocol.SetAuctionCategoryRule({ cat = e.category, keep = tonumber(e.keep:GetText()) or 0,
                stack = tonumber(e.stack:GetText()) or 0, on = W.IsChecked(e.on) },
                UI.RequestCallback("Auction rule for the category saved.", function(ok)
                    if ok then
                        e:Hide()
                    end
                end))
            return
        end
        local item = e.item
        if not item then
            return
        end
        Protocol.SetAuctionRule({
            e = item.e, rp = item.rp or 0, keep = tonumber(e.keep:GetText()) or 0, stack = tonumber(e.stack:GetText()) or 0,
            price = MoneyFromBoxes(e), on = W.IsChecked(e.on),
        }, UI.RequestCallback("Auction rule saved.", function(ok)
            if ok then
                e:Hide()
            end
        end))
    end)
    e.save:SetPoint("BOTTOMLEFT", e, "BOTTOMLEFT", 30, 18)
    e.delete = W.Button(e, "Delete rule", 110, 22, function()
        if e.category then
            Protocol.DeleteAuctionCategoryRule(e.category, UI.RequestCallback("Auction rule for the category deleted.", function(ok)
                if ok then
                    e:Hide()
                end
            end))
            return
        end
        local item = e.item
        if item then
            Protocol.DeleteAuctionRule(item.e, item.rp or 0, UI.RequestCallback("Auction rule deleted.", function(ok)
                if ok then
                    e:Hide()
                end
            end))
        end
    end)
    e.delete:SetPoint("BOTTOM", e, "BOTTOM", 0, 18)
    e.cancel = W.Button(e, "Cancel", 96, 22, function()
        e:Hide()
    end)
    e.cancel:SetPoint("BOTTOMRIGHT", e, "BOTTOMRIGHT", -30, 18)
    return e
end

local function ShowPriceWidgets(e, show)
    for _, w in ipairs(e.priceWidgets or {}) do
        if show then
            w:Show()
        else
            w:Hide()
        end
    end
end

function UI.OpenAuctionRuleEditor(item)
    if not auctionEditor then
        auctionEditor = CreateAuctionEditor()
    end
    local e = auctionEditor
    e.item = item
    e.category = nil
    ShowPriceWidgets(e, true)
    local name, _, quality, stackSize = ItemInfo(item.e, item.rp)
    e.title:SetText(QualityColor(quality) .. name .. "|r")
    local rule = State.AuctionRule(item.e, item.rp)
    local hours = State.auctions.hours or 24
    e.info:SetText(string.format("%d in the storage. The guild leader auctions what is above the amount you keep, in %d h "
        .. "auctions; the gold goes into the guild bank, unsold items come back into the storage.", item.n or 0, hours))
    W.SetEditText(e.keep, tostring(rule and rule.keep or math.max(0, math.min(item.n or 0, (stackSize or 1)))))
    W.SetEditText(e.stack, tostring(rule and rule.stack or math.min(stackSize or 1, 20)))
    SetMoneyBoxes(e, rule and rule.price or 0)
    local auto = rule and rule.auto
    e.priceHint:SetText("Empty = the default price" .. (auto and auto > 0 and (" (" .. GA.Data.FormatMoney(auto) .. ")") or
        " (vendor price based, what the auction house buyer accepts)") .. ".")
    e.on:SetChecked(not rule or rule.on ~= false)
    W.SetEnabled(e.delete, rule ~= nil)
    e:Show()
end

-- The rule for a whole category of the storage (AUCTION_CAT_SET).
function UI.OpenAuctionCategoryEditor(category)
    if not auctionEditor then
        auctionEditor = CreateAuctionEditor()
    end
    local e = auctionEditor
    e.item = nil
    e.category = category
    ShowPriceWidgets(e, false)
    local rule = State.AuctionCategoryRule(category)
    e.title:SetText("|cffffd100" .. UI.StorageCategoryLabel(category) .. "|r: auction house")
    e.info:SetText(string.format("Every kind of %s without a rule of its own: the guild leader keeps this many of each kind in "
        .. "the storage and auctions the rest at the default price in %d h auctions; the gold goes into the guild bank.",
        UI.StorageCategoryLabel(category), State.auctions.hours or 24))
    W.SetEditText(e.keep, tostring(rule and rule.keep or 20))
    W.SetEditText(e.stack, tostring(rule and (rule.stack or 0) > 0 and rule.stack or 20))
    e.priceHint:SetText("The price is the default price of each kind (what the auction house buyer accepts). A rule of "
        .. "an item's own (AH button on its row) comes first.")
    e.on:SetChecked(not rule or rule.on ~= false)
    W.SetEnabled(e.delete, rule ~= nil)
    e:Show()
end

-------------------------------------------------------------------------------
-- Loot filter view
-------------------------------------------------------------------------------

local editor -- the rule editor panel (one, shared)

local function ActionItems(allowedOnly)
    local allow = {}
    for _, a in ipairs(State.loot.settings.allow or { 0, 1, 2, 3, 4 }) do
        allow[a] = true
    end
    local items = {}
    for a = 0, 4 do
        items[#items + 1] = { text = ACTIONS[a].label, value = a, disabled = allowedOnly and not allow[a],
            tooltip = ACTIONS[a].desc }
    end
    return items
end

local function SendOptions(v, changes)
    local s = State.loot.settings
    local opts = { on = s.on ~= false, dai = s.dai or 4, dpl = s.dpl or 0, al = s.al ~= false }
    for k, val in pairs(changes) do
        opts[k] = val
    end
    Protocol.SetLootOptions(opts, UI.RequestCallback("Loot settings saved."))
end

local function CreateRuleRow(list)
    local row = CreateFrame("Frame", nil, list)
    row.shade = row:CreateTexture(nil, "BACKGROUND")
    row.shade:SetAllPoints(row)
    row.shade:SetTexture("Interface\\Buttons\\WHITE8X8")
    row.enabled = W.CheckBox(row, "", function(self)
        local rule = self:GetParent().rule
        if rule then
            Protocol.EnableLootRule(rule.id, W.IsChecked(self), UI.RequestCallback(nil))
        end
    end)
    row.enabled:SetPoint("LEFT", row, "LEFT", 2, 0)
    row.pos = W.Label(row, "", "GameFontNormalSmall")
    row.pos:SetPoint("LEFT", row.enabled, "RIGHT", 2, 0)
    row.pos:SetWidth(24)
    row.del = W.Button(row, "X", 24, 18, function(self)
        local rule = self:GetParent().rule
        if rule then
            UI.Confirm("Delete loot rule " .. rule.pos .. "?", function()
                Protocol.DeleteLootRule(rule.id, UI.RequestCallback("Rule deleted."))
            end)
        end
    end)
    row.del:SetPoint("RIGHT", row, "RIGHT", -2, 0)
    row.edit = W.Button(row, "Edit", 44, 18, function(self)
        local rule = self:GetParent().rule
        if rule then
            UI.OpenLootRuleEditor(rule)
        end
    end)
    row.edit:SetPoint("RIGHT", row.del, "LEFT", -2, 0)
    row.down = W.Button(row, "v", 22, 18, function(self)
        local rule = self:GetParent().rule
        if rule then
            Protocol.MoveLootRule(rule.id, rule.pos + 1, UI.RequestCallback(nil))
        end
    end)
    row.down:SetPoint("RIGHT", row.edit, "LEFT", -2, 0)
    row.up = W.Button(row, "^", 22, 18, function(self)
        local rule = self:GetParent().rule
        if rule and rule.pos > 1 then
            Protocol.MoveLootRule(rule.id, rule.pos - 1, UI.RequestCallback(nil))
        end
    end)
    row.up:SetPoint("RIGHT", row.down, "LEFT", -2, 0)
    row.action = W.Label(row, "", "GameFontHighlightSmall")
    row.action:SetPoint("LEFT", row.pos, "RIGHT", 4, 0)
    row.action:SetWidth(78)
    row.desc = W.Label(row, "", "GameFontHighlightSmall")
    row.desc:SetPoint("LEFT", row.action, "RIGHT", 4, 0)
    row.desc:SetPoint("RIGHT", row.up, "LEFT", -6, 0)
    return row
end

local function UpdateRuleRow(row, rule, index)
    row.rule = rule
    row.shade:SetVertexColor(1, 1, 1, index % 2 == 0 and 0.05 or 0)
    -- The built-in rules (UI.BuiltinLootRules): shown, not edited here (Settings: Loot).
    local builtin = rule.builtin == true
    for _, b in ipairs({ row.enabled, row.up, row.down, row.edit, row.del }) do
        if builtin then
            b:Hide()
        else
            b:Show()
        end
    end
    if builtin then
        local a = ACTIONS[rule.act or -1]
        row.pos:SetText("|cff808080--|r")
        row.action:SetText(a and (a.color .. a.label .. "|r") or "|cff808080Default|r")
        row.desc:SetText("|cffc0c0c0" .. rule.text .. "|r")
        return
    end
    row.enabled:SetChecked(rule.en ~= false)
    row.pos:SetText(tostring(rule.pos or index))
    local a = ACTIONS[rule.act or 0] or ACTIONS[0]
    row.action:SetText(a.color .. a.label .. "|r")
    row.desc:SetText((rule.en == false and "|cff808080" or "|cffffffff") .. DescribeRule(rule) .. "|r")
    local connected = State.IsConnected()
    for _, b in ipairs({ row.up, row.down, row.edit, row.del }) do
        W.SetEnabled(b, connected)
    end
    W.SetEnabled(row.up, connected and (rule.pos or 1) > 1)
    W.SetEnabled(row.down, connected and (rule.pos or 1) < #State.loot.rules)
end

local function CreateLootView(parent)
    local v = CreateFrame("Frame", nil, parent)

    v.on = W.CheckBox(v, "Loot filter on", function(self)
        SendOptions(v, { on = W.IsChecked(self) })
    end)
    v.on:SetPoint("TOPLEFT", v, "TOPLEFT", 0, 0)
    v.autoLoot = W.CheckBox(v, "Auto loot (own kills, skinning, herbs and ore within 10 yards)", function(self)
        SendOptions(v, { al = W.IsChecked(self) })
    end)
    v.autoLoot:SetPoint("LEFT", v.on, "RIGHT", 110, 0)

    local aiLabel = W.Label(v, "Bots' loot that no rule decides:", "GameFontNormalSmall")
    aiLabel:SetPoint("TOPLEFT", v.on, "BOTTOMLEFT", 4, -10)
    v.defaultAi = W.DropDown(v, 110, function()
        return ActionItems(true)
    end)
    v.defaultAi:SetPoint("LEFT", aiLabel, "RIGHT", -8, -2)
    v.defaultAi.onSelect = function(value)
        SendOptions(v, { dai = value })
    end
    local ownLabel = W.Label(v, "Your own:", "GameFontNormalSmall")
    ownLabel:SetPoint("LEFT", v.defaultAi, "RIGHT", 0, 2)
    v.defaultOwn = W.DropDown(v, 110, function()
        return ActionItems(true)
    end)
    v.defaultOwn:SetPoint("LEFT", ownLabel, "RIGHT", -8, -2)
    v.defaultOwn.onSelect = function(value)
        SendOptions(v, { dpl = value })
    end
    W.SetTooltip(aiLabel, "Loot no rule decides", {
        "For a character the AI plays (the bots, and the leader while you play a bot): by default it goes into the guild storage,",
        "once the guild has the perk Material Haulers (Perks tab); until then it stays in the bags.",
        "Quest items are never touched. What a bot will use itself (an equipment upgrade, its food, drink, potions, ammunition) stays with it.",
    })

    v.totals = W.Label(v, "", "GameFontHighlightSmall")
    v.totals:SetPoint("TOPLEFT", aiLabel, "BOTTOMLEFT", 0, -14)
    v.totals:SetPoint("RIGHT", v, "RIGHT", -8, 0)

    local panel = W.Panel(v)
    panel:SetPoint("TOPLEFT", v, "TOPLEFT", 0, -86)
    panel:SetPoint("BOTTOMRIGHT", v, "BOTTOMRIGHT", 0, 34)
    v.list = W.ScrollList(panel, { rowHeight = 24, createRow = CreateRuleRow, updateRow = UpdateRuleRow })
    v.list:SetPoint("TOPLEFT", panel, "TOPLEFT", 6, -6)
    v.list:SetPoint("BOTTOMRIGHT", panel, "BOTTOMRIGHT", -6, 6)
    v.empty = W.Label(panel, "No rules: loot goes the default way above.", "GameFontDisable", "CENTER")
    v.empty:SetPoint("CENTER", panel, "CENTER", 0, 0)

    v.add = W.Button(v, "New rule", 110, 22, function()
        UI.OpenLootRuleEditor(nil)
    end)
    v.add:SetPoint("BOTTOMLEFT", v, "BOTTOMLEFT", 0, 4)
    v.hint = W.Label(v, "Rules are checked from the top; the first matching rule whose action fits the item decides.",
        "GameFontDisableSmall")
    v.hint:SetPoint("LEFT", v.add, "RIGHT", 10, 0)
    v.hint:SetPoint("RIGHT", v, "RIGHT", -8, 0)
    return v
end

-- The rules the server applies after the guild's own ones, as rows of the list: junk is sold, the rest goes the
-- default way above.
function UI.BuiltinLootRules()
    local rows = {}
    local quality = UI.JunkQuality()
    if quality >= 0 then
        local keep = UI.KeepWornGear()
        local text
        if quality >= 1 and keep then
            text = "Built in: grey items and white weapons and armour (junk), except gear a guild character would wear"
        elseif quality >= 1 then
            text = "Built in: grey items and white weapons and armour (junk), except what the looter puts on"
        elseif keep then
            text = "Built in: grey items (junk), except gear a guild character would wear"
        else
            text = "Built in: grey items (junk), except what the looter puts on"
        end
        rows[#rows + 1] = { builtin = true, act = 1, text = text }
    end
    rows[#rows + 1] = { builtin = true, act = -1, text = "Built in: crafting materials, quest items, consumables, bags, "
        .. "ammunition and what a bot uses itself go the default way above" }
    return rows
end

local function RefreshLootView(v)
    local s = State.loot.settings
    local connected = State.IsConnected()
    v.on:SetChecked(s.on ~= false)
    v.autoLoot:SetChecked(s.al ~= false)
    v.defaultAi:SetSelected(s.dai or 4, ACTIONS[s.dai or 4].label)
    v.defaultOwn:SetSelected(s.dpl or 0, ACTIONS[s.dpl or 0].label)
    W.SetEnabled(v.on, connected)
    W.SetEnabled(v.autoLoot, connected and s.salr ~= false)
    v.defaultAi:SetEnabled(connected)
    v.defaultOwn:SetEnabled(connected)
    v.totals:SetText(string.format("So far: %s stored, sold for %s, %s disenchanted, %s deleted.%s%s",
        s.st or 0, GA.Data.FormatMoney(s.sold or 0), s.de or 0, s.del or 0,
        s.srv == false and "  |cffff8040The loot filter is off on this server.|r" or "",
        s.reag == false and "  |cffff8040Crafting from the storage is off on this server.|r" or "")
        .. ((#State.perks > 0 and not State.HasPerk("materials_storage"))
            and "  |cffff8040Loot goes into the storage by itself once the guild has the perk Material Haulers (Perks tab).|r" or ""))
    local rows = {}
    for _, rule in ipairs(State.loot.rules) do
        rows[#rows + 1] = rule
    end
    if State.loot.loaded then
        for _, rule in ipairs(UI.BuiltinLootRules()) do
            rows[#rows + 1] = rule
        end
    end
    v.list:SetItems(rows)
    if State.loot.loaded then
        v.empty:SetText("")
    else
        v.empty:SetText(connected and "Loading the loot filter..." or "Not connected.")
    end
    W.SetEnabled(v.add, connected and #State.loot.rules < (s.max or 30))
end

-- Rule editor -----------------------------------------------------------------

local function CreateConditionRow(parent, index)
    local r = CreateFrame("Frame", nil, parent)
    r:SetHeight(28)
    r.type = W.DropDown(r, 110, function()
        local items = { { text = "(none)", value = -1 } }
        for _, c in ipairs(COND_TYPES) do
            items[#items + 1] = { text = c.label, value = c.value, tooltip = c.hint }
        end
        return items
    end)
    r.type:SetPoint("LEFT", r, "LEFT", -14, 0)
    r.op = W.DropDown(r, 70, function()
        return { { text = OPS[0], value = 0 }, { text = OPS[1], value = 1 }, { text = OPS[2], value = 2 } }
    end)
    r.op:SetPoint("LEFT", r.type, "RIGHT", -26, 0)
    r.class = W.DropDown(r, 100, function()
        local items = {}
        for _, c in ipairs(ITEM_CLASSES) do
            items[#items + 1] = { text = c.label, value = c.value }
        end
        return items
    end)
    r.class:SetPoint("LEFT", r.type, "RIGHT", -26, 0)
    r.value = W.EditBox(r, 120, false, 40)
    r.value:SetPoint("LEFT", r.op, "RIGHT", -2, 2)
    r.hint = W.Label(r, "", "GameFontDisableSmall")
    r.hint:SetPoint("LEFT", r.value, "RIGHT", 8, 0)
    r.hint:SetPoint("RIGHT", parent, "RIGHT", -10, 0)

    function r:Apply(cond)
        self.cond = cond
        local t = cond and cond.type or -1
        self.type:SetSelected(t, t >= 0 and CondTypeLabel(t) or "(none)")
        self.op:SetSelected(cond and cond.op or 0, OPS[cond and cond.op or 0])
        if t == 3 then
            self.class:Show()
            self.op:Hide()
            self.class:SetSelected(cond.value, ClassLabel(cond.value))
            self.value:ClearAllPoints()
            self.value:SetPoint("LEFT", self.class, "RIGHT", -2, 2)
            W.SetEditText(self.value, (cond.value2 and cond.value2 ~= 255) and tostring(cond.value2) or "")
        else
            self.class:Hide()
            self.value:ClearAllPoints()
            self.value:SetPoint("LEFT", self.op, "RIGHT", -2, 2)
            if t == 7 then
                self.op:Hide()
                W.SetEditText(self.value, cond.text or "")
            else
                if t == 6 then
                    self.op:Hide()
                else
                    self.op:Show()
                end
                W.SetEditText(self.value, (cond and t >= 0) and tostring(cond.value or 0) or "")
            end
        end
        if t < 0 then
            self.op:Hide()
            self.value:Hide()
        else
            self.value:Show()
        end
        local hint = ""
        for _, c in ipairs(COND_TYPES) do
            if c.value == t then
                hint = c.hint or ""
            end
        end
        self.hint:SetText(hint)
    end

    function r:Read()
        local c = self.cond
        if not c or c.type < 0 then
            return nil
        end
        local text = self.value:GetText() or ""
        local out = { type = c.type, op = 0, value = 0, value2 = 0, text = "", group = 0 }
        if c.type == 7 then
            out.text = text
        elseif c.type == 3 then
            out.value = c.value or 0
            out.value2 = tonumber(text) or 255
        else
            out.value = math.floor(tonumber(text) or 0)
            if c.type ~= 6 then
                out.op = c.op or 0
            end
        end
        return out
    end

    r.type.onSelect = function(value)
        if value < 0 then
            r:Apply(nil)
        else
            r:Apply({ type = value, op = value == 0 and 1 or 0, value = value == 3 and 7 or 0, value2 = 255, text = "" })
        end
    end
    r.op.onSelect = function(value)
        if r.cond then
            r.cond.op = value
        end
    end
    r.class.onSelect = function(value)
        if r.cond then
            r.cond.value = value
        end
    end
    return r
end

local function CreateEditor()
    local e = CreateFrame("Frame", "GuildAdventureLootRuleEditor", UIParent)
    e:SetWidth(560)
    e:SetHeight(258)
    e:SetPoint("CENTER", UIParent, "CENTER", 0, 60)
    e:SetFrameStrata("DIALOG")
    e:SetBackdrop(W.BACKDROP_DIALOG)
    W.DarkenBackground(e, 0.7)
    e:EnableMouse(true)
    e:SetMovable(true)
    e:RegisterForDrag("LeftButton")
    e:SetScript("OnDragStart", e.StartMoving)
    e:SetScript("OnDragStop", e.StopMovingOrSizing)
    e:Hide()
    tinsert(UISpecialFrames, "GuildAdventureLootRuleEditor")

    e.title = W.Label(e, "Loot rule", "GameFontNormalLarge")
    e.title:SetPoint("TOP", e, "TOP", 0, -18)
    local actionLabel = W.Label(e, "Action:", "GameFontNormalSmall")
    actionLabel:SetPoint("TOPLEFT", e, "TOPLEFT", 24, -48)
    e.action = W.DropDown(e, 120, function()
        return ActionItems(true)
    end)
    e.action:SetPoint("LEFT", actionLabel, "RIGHT", -8, -2)
    e.action.onSelect = function(value)
        e.actionValue = value
    end
    e.any = W.CheckBox(e, "Match when ANY condition fits (else all)")
    e.any:SetPoint("LEFT", e.action, "RIGHT", 10, 2)

    e.rows = {}
    for i = 1, MAX_EDIT_CONDITIONS do
        local r = CreateConditionRow(e, i)
        r:SetPoint("TOPLEFT", e, "TOPLEFT", 24, -78 - (i - 1) * 30)
        r:SetPoint("RIGHT", e, "RIGHT", -20, 0)
        e.rows[i] = r
    end

    e.save = W.Button(e, "Save", 100, 22, function()
        local conds = {}
        for _, r in ipairs(e.rows) do
            local c = r:Read()
            if c then
                conds[#conds + 1] = c
            end
        end
        if #conds == 0 then
            UI.Notify("A rule needs at least one condition.", true)
            return
        end
        local rule = e.rule or {}
        Protocol.SetLootRule({
            id = rule.id or 0, pos = rule.pos or 0, act = e.actionValue or 4, en = rule.en ~= false,
            lg = W.IsChecked(e.any) and 1 or 0, c = EncodeConditions(conds),
        }, UI.RequestCallback("Loot rule saved.", function(ok)
            if ok then
                e:Hide()
            end
        end))
    end)
    e.save:SetPoint("BOTTOMRIGHT", e, "BOTTOM", -6, 18)
    e.cancel = W.Button(e, "Cancel", 100, 22, function()
        e:Hide()
    end)
    e.cancel:SetPoint("BOTTOMLEFT", e, "BOTTOM", 6, 18)
    return e
end

function UI.OpenLootRuleEditor(rule)
    if not editor then
        editor = CreateEditor()
    end
    editor.rule = rule
    editor.title:SetText(rule and ("Loot rule " .. (rule.pos or "")) or "New loot rule")
    editor.actionValue = rule and rule.act or 4
    editor.action:SetSelected(editor.actionValue, ACTIONS[editor.actionValue].label)
    editor.any:SetChecked(rule and ((rule.lg or 0) % 2 == 1) or false)
    local conds = rule and DecodeConditions(rule.c) or {}
    for i, r in ipairs(editor.rows) do
        r:Apply(conds[i])
    end
    if not rule then
        editor.rows[1]:Apply({ type = 0, op = 2, value = 0, value2 = 0, text = "" })
    end
    editor:Show()
end

-------------------------------------------------------------------------------
-- Recent loot view
-------------------------------------------------------------------------------

local function CharacterName(g)
    if not g then
        return "?"
    end
    local bot = State.GetBot(g)
    if bot then
        return bot.n or "?"
    end
    if State.control.leader == g then
        return State.control.ln or "Leader"
    end
    return "?"
end

local function CreateLogRow(list)
    local row = CreateFrame("Frame", nil, list)
    row.shade = row:CreateTexture(nil, "BACKGROUND")
    row.shade:SetAllPoints(row)
    row.shade:SetTexture("Interface\\Buttons\\WHITE8X8")
    row.time = W.Label(row, "", "GameFontDisableSmall")
    row.time:SetPoint("LEFT", row, "LEFT", 4, 0)
    row.time:SetWidth(60)
    row.who = W.Label(row, "", "GameFontNormalSmall")
    row.who:SetPoint("LEFT", row.time, "RIGHT", 4, 0)
    row.who:SetWidth(90)
    row.icon = W.Icon(row, 16, QUESTION)
    row.icon:SetPoint("LEFT", row.who, "RIGHT", 4, 0)
    row.outcome = W.Label(row, "", "GameFontHighlightSmall", "RIGHT")
    row.outcome:SetPoint("RIGHT", row, "RIGHT", -6, 0)
    row.outcome:SetWidth(230)
    row.item = W.Label(row, "", "GameFontHighlightSmall")
    row.item:SetPoint("LEFT", row.icon, "RIGHT", 6, 0)
    row.item:SetPoint("RIGHT", row.outcome, "LEFT", -6, 0)
    return row
end

local function UpdateLogRow(row, e, index)
    row.shade:SetVertexColor(1, 1, 1, index % 2 == 0 and 0.05 or 0)
    row.time:SetText(GA.Data.FormatAgo and GA.Data.FormatAgo(State.ServerNow() - (e.t or 0)) or "")
    row.who:SetText(CharacterName(e.g))
    local name, _, quality, _, texture = ItemInfo(e.e or 0, e.rp)
    row.icon:SetTexture(texture)
    row.item:SetText(QualityColor(quality) .. name .. "|r" .. ((e.n or 1) > 1 and (" x" .. e.n) or ""))
    local a = ACTIONS[e.o or 0] or ACTIONS[0]
    local text = a.color .. (e.o == 4 and "Stored" or e.o == 1 and ("Sold " .. GA.Data.FormatMoney(e.m or 0)) or
        e.o == 2 and "Disenchanted" or e.o == 3 and "Deleted" or "Kept") .. "|r"
    local move = LOG_OUTCOMES[e.o or -1]
    if move then
        text = move.color .. move.text .. (e.o == 8 and (" " .. GA.Data.FormatMoney(e.m or 0)) or "") .. "|r"
    end
    if e.why and e.why ~= "" then
        text = text .. " |cff9d9d9d(" .. e.why .. ")|r"
    end
    row.outcome:SetText(text)
end

local function CreateLogView(parent)
    local v = CreateFrame("Frame", nil, parent)
    local panel = W.Panel(v)
    panel:SetPoint("TOPLEFT", v, "TOPLEFT", 0, 0)
    panel:SetPoint("BOTTOMRIGHT", v, "BOTTOMRIGHT", 0, 34)
    v.list = W.ScrollList(panel, { rowHeight = 20, createRow = CreateLogRow, updateRow = UpdateLogRow })
    v.list:SetPoint("TOPLEFT", panel, "TOPLEFT", 6, -6)
    v.list:SetPoint("BOTTOMRIGHT", panel, "BOTTOMRIGHT", -6, 6)
    v.empty = W.Label(panel, "", "GameFontDisable", "CENTER")
    v.empty:SetPoint("CENTER", panel, "CENTER", 0, 0)
    v.empty:SetWidth(420)
    v.reload = W.Button(v, "Refresh", 100, 22, function()
        Protocol.Get("lootlog")
    end)
    v.reload:SetPoint("BOTTOMLEFT", v, "BOTTOMLEFT", 0, 4)
    local hint = W.Label(v, "The last loot of every guild character, newest first. The full rules are under Loot filter.",
        "GameFontDisableSmall")
    hint:SetPoint("LEFT", v.reload, "RIGHT", 10, 0)
    hint:SetPoint("RIGHT", v, "RIGHT", -8, 0)
    return v
end

local function RefreshLogView(v)
    local log = State.loot.log
    local items = {}
    for i = #log, 1, -1 do
        items[#items + 1] = log[i]
    end
    v.list:SetItems(items)
    if not State.IsConnected() then
        v.empty:SetText("Not connected.")
    elseif #items > 0 then
        v.empty:SetText("")
    elseif not State.loot.logLoaded then
        v.empty:SetText("Loading the recent loot...")
    else
        v.empty:SetText("Nothing looted yet.\n\nThe guild characters' loot shows up here as they loot it "
            .. "(the server keeps the newest entries since its last start).")
    end
    W.SetEnabled(v.reload, State.IsConnected())
end

-------------------------------------------------------------------------------
-- Craft orders: "make N x item" (docs/orders.md "Craft orders")
-------------------------------------------------------------------------------

local function CreateOrderRow(list)
    local row = CreateFrame("Frame", nil, list)
    row.shade = row:CreateTexture(nil, "BACKGROUND")
    row.shade:SetAllPoints(row)
    row.shade:SetTexture("Interface\\Buttons\\WHITE8X8")
    row.count = W.Label(row, "", "GameFontHighlightSmall")
    row.count:SetPoint("LEFT", row, "LEFT", 4, 0)
    row.count:SetWidth(60)
    row.item = W.Label(row, "", "GameFontNormalSmall")
    row.item:SetPoint("LEFT", row.count, "RIGHT", 4, 0)
    row.item:SetWidth(170)
    row.remove = W.Button(row, "Remove", 64, 18, function()
        if row.order then
            Protocol.DeleteCraftOrder(row.order.id, UI.RequestCallback(nil))
        end
    end)
    row.remove:SetPoint("RIGHT", row, "RIGHT", -4, 0)
    row.note = W.Label(row, "", "GameFontDisableSmall")
    row.note:SetPoint("LEFT", row.item, "RIGHT", 6, 0)
    row.note:SetPoint("RIGHT", row.remove, "LEFT", -6, 0)
    return row
end

local function UpdateOrderRow(row, order, index)
    row.order = order
    row.shade:SetVertexColor(1, 1, 1, index % 2 == 0 and 0.05 or 0)
    row.count:SetText((order.made or 0) .. " / " .. (order.amt or 0))
    row.item:SetText(order.n or ("item " .. (order.item or 0)))
    row.note:SetText(order.note or "")
    W.SetEnabled(row.remove, State.IsConnected())
end

local function CreateCraftableRow(list)
    local row = CreateFrame("Button", nil, list)
    row.highlight = row:CreateTexture(nil, "BACKGROUND")
    row.highlight:SetAllPoints(row)
    row.highlight:SetTexture("Interface\\Buttons\\WHITE8X8")
    row.label = W.Label(row, "", "GameFontHighlightSmall")
    row.label:SetPoint("LEFT", row, "LEFT", 6, 0)
    row.label:SetWidth(220)
    row.by = W.Label(row, "", "GameFontDisableSmall", "RIGHT")
    row.by:SetPoint("RIGHT", row, "RIGHT", -4, 0)
    row.by:SetPoint("LEFT", row.label, "RIGHT", 6, 0)
    row:SetScript("OnClick", function(self)
        list.view.selected = self.entry and self.entry.item or nil
        UI.RequestRefresh()
    end)
    return row
end

local function UpdateCraftableRow(row, entry)
    row.entry = entry
    local selected = row:GetParent().view.selected == entry.item
    row.highlight:SetVertexColor(1, 0.82, 0, selected and 0.18 or 0)
    row.label:SetText((selected and "|cffffd100" or "|cffffffff") .. (entry.n or "?") .. "|r")
    row.by:SetText(entry.by or "")
end

-- What the guild can make, filtered by the search text (also for the offline tests).
function UI.CraftableChoices(search)
    local needle = string.lower(search or "")
    local list = {}
    for _, entry in ipairs(State.craftables) do
        if needle == "" or string.find(string.lower(entry.n or ""), needle, 1, true)
            or string.find(string.lower(entry.by or ""), needle, 1, true) then
            list[#list + 1] = entry
        end
    end
    return list
end

local function CreateOrdersView(parent)
    local v = CreateFrame("Frame", nil, parent)

    local ordersTitle = W.Label(v, "The guild's craft orders", "GameFontNormalSmall")
    ordersTitle:SetPoint("TOPLEFT", v, "TOPLEFT", 4, -2)
    local ordersPanel = W.Panel(v)
    ordersPanel:SetPoint("TOPLEFT", v, "TOPLEFT", 0, -18)
    ordersPanel:SetPoint("TOPRIGHT", v, "TOPRIGHT", 0, -18)
    ordersPanel:SetHeight(150)
    v.orders = W.ScrollList(ordersPanel, { rowHeight = 22, createRow = CreateOrderRow, updateRow = UpdateOrderRow })
    v.orders:SetPoint("TOPLEFT", ordersPanel, "TOPLEFT", 6, -6)
    v.orders:SetPoint("BOTTOMRIGHT", ordersPanel, "BOTTOMRIGHT", -6, 6)
    v.ordersEmpty = W.Label(ordersPanel, "", "GameFontDisable", "CENTER")
    v.ordersEmpty:SetPoint("CENTER", ordersPanel, "CENTER", 0, 0)
    v.ordersEmpty:SetWidth(520)

    local makeTitle = W.Label(v, "What the guild can make", "GameFontNormalSmall")
    makeTitle:SetPoint("TOPLEFT", ordersPanel, "BOTTOMLEFT", 4, -10)
    v.search = W.EditBox(v, 160, false, 40)
    v.search:SetPoint("LEFT", makeTitle, "RIGHT", 16, 0)
    v.search:SetScript("OnTextChanged", function()
        UI.RequestRefresh()
    end)
    local makePanel = W.Panel(v)
    makePanel:SetPoint("TOPLEFT", ordersPanel, "BOTTOMLEFT", 0, -30)
    makePanel:SetPoint("BOTTOMRIGHT", v, "BOTTOMRIGHT", 0, 34)
    v.craftables = W.ScrollList(makePanel, { rowHeight = 18, createRow = CreateCraftableRow, updateRow = UpdateCraftableRow })
    v.craftables.view = v
    v.craftables:SetPoint("TOPLEFT", makePanel, "TOPLEFT", 6, -6)
    v.craftables:SetPoint("BOTTOMRIGHT", makePanel, "BOTTOMRIGHT", -6, 6)
    v.makeEmpty = W.Label(makePanel, "", "GameFontDisable", "CENTER")
    v.makeEmpty:SetPoint("CENTER", makePanel, "CENTER", 0, 0)
    v.makeEmpty:SetWidth(520)

    v.reload = W.Button(v, "Refresh", 80, 22, function()
        Protocol.Get("craftorders")
        Protocol.Get("craftables")
    end)
    v.reload:SetPoint("BOTTOMLEFT", v, "BOTTOMLEFT", 0, 4)
    local countLabel = W.Label(v, "Count:", "GameFontNormalSmall")
    countLabel:SetPoint("LEFT", v.reload, "RIGHT", 14, 0)
    v.count = W.EditBox(v, 40, true, 3)
    v.count:SetPoint("LEFT", countLabel, "RIGHT", 8, 0)
    v.count:SetText("1")
    v.order = W.Button(v, "Order", 90, 22, function()
        UI.OrderCraft(v.selected, tonumber(v.count:GetText()) or 1)
    end)
    v.order:SetPoint("LEFT", v.count, "RIGHT", 8, 0)
    W.SetTooltip(v.order, "Order", {
        "A bot that knows a recipe for the item makes it on its next town trip (a vendor visit, a hand-in at a hub),",
        "from its bags and the guild storage. What it makes goes into the guild storage.",
        "Materials a vendor sells are bought from the guild's budget; the rest has to be in the storage.",
    })
    v.hint = W.Label(v, "", "GameFontDisableSmall")
    v.hint:SetPoint("LEFT", v.order, "RIGHT", 10, 0)
    v.hint:SetPoint("RIGHT", v, "RIGHT", -8, 0)
    return v
end

-- Orders `count` of an item (the view's button; also for the offline tests).
function UI.OrderCraft(item, count)
    if not item then
        UI.Notify("Pick an item from the list first.", true)
        return false
    end
    Protocol.AddCraftOrder(item, math.max(1, count or 1), UI.RequestCallback(nil))
    return true
end

local function RefreshOrdersView(v)
    if State.IsConnected() and not v.asked then
        v.asked = true
        Protocol.Get("craftorders")
        Protocol.Get("craftables")
    elseif not State.IsConnected() then
        v.asked = false
    end
    v.orders:SetItems(State.craftOrders)
    if not State.IsConnected() then
        v.ordersEmpty:SetText("Not connected.")
    elseif #State.craftOrders > 0 then
        v.ordersEmpty:SetText("")
    elseif not State.craftOrdersLoaded then
        v.ordersEmpty:SetText("Loading the craft orders...")
    else
        v.ordersEmpty:SetText("No craft orders. Pick an item below, set a count and press Order.")
    end

    local choices = UI.CraftableChoices(v.search:GetText())
    v.craftables:SetItems(choices)
    if not State.IsConnected() then
        v.makeEmpty:SetText("")
    elseif #choices > 0 then
        v.makeEmpty:SetText("")
    elseif not State.craftablesLoaded then
        v.makeEmpty:SetText("Loading what the guild can make...")
    elseif #State.craftables == 0 then
        v.makeEmpty:SetText("Nobody of the guild who is in the world knows a recipe yet.\n\n"
            .. "Give a bot a profession in its Professions sub-tab; it learns the recipes its skill allows.")
    else
        v.makeEmpty:SetText("Nothing matches the search.")
    end
    local selected
    for _, entry in ipairs(State.craftables) do
        if entry.item == v.selected then
            selected = entry
        end
    end
    v.hint:SetText(selected and ("Selected: " .. (selected.n or "?") .. "  (made by " .. (selected.by or "?") .. ")")
        or "Select an item of the list, set how many, and order.")
    W.SetEnabled(v.order, State.IsConnected() and selected ~= nil)
    W.SetEnabled(v.reload, State.IsConnected())
end

-------------------------------------------------------------------------------
-- Shopping list: a minimum stock per item vendors sell (docs/storage.md "Shopping list")
-------------------------------------------------------------------------------

-- Quick entries: the vendor drinks and breads by the level they need (the owner, 2026-11-14: [quote left out]).
UI.SHOPPING_TIERS = {
    drink = { { e = 159, lvl = 1 }, { e = 1179, lvl = 5 }, { e = 1205, lvl = 15 }, { e = 1708, lvl = 25 },
        { e = 1645, lvl = 35 }, { e = 8766, lvl = 45 } },
    food = { { e = 4540, lvl = 1 }, { e = 4541, lvl = 5 }, { e = 4542, lvl = 15 }, { e = 4544, lvl = 25 },
        { e = 4601, lvl = 35 }, { e = 8950, lvl = 45 } },
}

local function CreateShoppingRow(list)
    local row = CreateFrame("Frame", nil, list)
    row.shade = row:CreateTexture(nil, "BACKGROUND")
    row.shade:SetAllPoints(row)
    row.shade:SetTexture("Interface\\Buttons\\WHITE8X8")
    row.icon = W.Icon(row, 16, QUESTION)
    row.icon:SetPoint("LEFT", row, "LEFT", 4, 0)
    row.item = W.Label(row, "", "GameFontHighlightSmall")
    row.item:SetPoint("LEFT", row.icon, "RIGHT", 6, 0)
    row.item:SetWidth(190)
    row.stock = W.Label(row, "", "GameFontHighlightSmall")
    row.stock:SetPoint("LEFT", row.item, "RIGHT", 6, 0)
    row.stock:SetWidth(110)
    row.remove = W.Button(row, "Remove", 60, 18, function()
        if row.entry then
            Protocol.DeleteShoppingItem(row.entry.e, UI.RequestCallback(nil))
        end
    end)
    row.remove:SetPoint("RIGHT", row, "RIGHT", -4, 0)
    row.pause = W.Button(row, "Pause", 60, 18, function()
        if row.entry then
            Protocol.SetShoppingItem(row.entry.e, row.entry.min, not row.entry.on, UI.RequestCallback(nil))
        end
    end)
    row.pause:SetPoint("RIGHT", row.remove, "LEFT", -4, 0)
    row.min = W.Button(row, "Min", 44, 18, function()
        if row.entry then
            UI.AskShoppingMinimum(row.entry)
        end
    end)
    row.min:SetPoint("RIGHT", row.pause, "LEFT", -4, 0)
    W.SetTooltip(row.min, "Minimum", { "The guild storage is filled up to this many.",
        "A bot at a vendor it visits anyway buys what is missing, paid from the guild bank." })
    row.price = W.Label(row, "", "GameFontDisableSmall", "RIGHT")
    row.price:SetPoint("RIGHT", row.min, "LEFT", -6, 0)
    row.price:SetPoint("LEFT", row.stock, "RIGHT", 4, 0)
    return row
end

-- Name, quality and icon of a list entry: the client's item cache, else the name the server sent.
local function ShoppingItemInfo(entry)
    local name, quality, texture = entry.n or ("Item #" .. (entry.e or 0)), 1, QUESTION
    if GetItemInfo then
        local cached, _, q, _, t = ItemInfo(entry.e or 0)
        if string.sub(cached, 1, 6) ~= "Item #" then
            name = cached
        end
        quality, texture = q, t
    end
    return name, quality, texture
end

local function UpdateShoppingRow(row, entry, index)
    row.entry = entry
    row.shade:SetVertexColor(1, 1, 1, index % 2 == 0 and 0.05 or 0)
    local name, quality, texture = ShoppingItemInfo(entry)
    row.icon:SetTexture(texture)
    row.item:SetText(QualityColor(quality) .. name .. "|r" .. (entry.on == false and " |cff9d9d9d(paused)|r" or ""))
    local have = State.ShoppingStock(entry.e)
    local low = have < (entry.min or 0)
    row.stock:SetText((low and "|cffff6060" or "|cff40ff40") .. have .. "|r / " .. (entry.min or 0))
    row.price:SetText(entry.price and entry.price > 0 and (GA.Data.FormatMoney(entry.price) .. " for " .. (entry.bc or 1)) or "")
    row.pause:SetText(entry.on == false and "Resume" or "Pause")
    local connected = State.IsConnected()
    W.SetEnabled(row.remove, connected)
    W.SetEnabled(row.pause, connected)
    W.SetEnabled(row.min, connected)
end

local function CreateShopItemRow(list)
    local row = CreateFrame("Button", nil, list)
    row.highlight = row:CreateTexture(nil, "BACKGROUND")
    row.highlight:SetAllPoints(row)
    row.highlight:SetTexture("Interface\\Buttons\\WHITE8X8")
    row.label = W.Label(row, "", "GameFontHighlightSmall")
    row.label:SetPoint("LEFT", row, "LEFT", 6, 0)
    row.label:SetWidth(260)
    row.info = W.Label(row, "", "GameFontDisableSmall", "RIGHT")
    row.info:SetPoint("RIGHT", row, "RIGHT", -4, 0)
    row.info:SetPoint("LEFT", row.label, "RIGHT", 6, 0)
    row:SetScript("OnClick", function(self)
        list.view.selected = self.entry and self.entry.e or nil
        UI.RequestRefresh()
    end)
    return row
end

local function UpdateShopItemRow(row, entry)
    row.entry = entry
    local selected = row:GetParent().view.selected == entry.e
    row.highlight:SetVertexColor(1, 0.82, 0, selected and 0.18 or 0)
    row.label:SetText((selected and "|cffffd100" or QualityColor(entry.q)) .. (entry.n or "?") .. "|r")
    row.info:SetText(string.format("level %d, %s for %d", entry.lvl or 0, GA.Data.FormatMoney(entry.price or 0), entry.bc or 1))
end

function UI.AskShoppingMinimum(entry)
    local name = ShoppingItemInfo(entry)
    StaticPopup_Show("GUILDADVENTURE_STORAGE_RESERVE",
        string.format("Minimum for %s:\nthe bots fill the guild storage up to this many at vendors they visit.", name),
        nil, { current = entry.min or 0, onAccept = function(n)
            Protocol.SetShoppingItem(entry.e, n, entry.on ~= false, UI.RequestCallback(nil))
        end })
end

-- Puts an item on the list with a minimum (the Add button; also for the offline tests).
function UI.AddShoppingItem(entry, minimum)
    if not entry then
        UI.Notify("Search for an item and pick it from the results first.", true)
        return false
    end
    Protocol.SetShoppingItem(entry, math.max(0, minimum or 0), true, UI.RequestCallback(nil))
    return true
end

-- Puts every drink or bread tier on the list with one minimum; tiers already on it keep theirs.
function UI.AddShoppingTiers(kind, minimum)
    local added = 0
    for _, tier in ipairs(UI.SHOPPING_TIERS[kind] or {}) do
        if not State.ShoppingEntry(tier.e) then
            Protocol.SetShoppingItem(tier.e, math.max(0, minimum or 0), true, UI.RequestCallback(nil))
            added = added + 1
        end
    end
    if added == 0 then
        UI.Notify("Every tier is on the shopping list already.")
    end
    return added
end

local function CreateShoppingView(parent)
    local v = CreateFrame("Frame", nil, parent)

    v.title = W.Label(v, "", "GameFontNormalSmall")
    v.title:SetPoint("TOPLEFT", v, "TOPLEFT", 4, -2)
    v.title:SetPoint("RIGHT", v, "RIGHT", -4, 0)
    local listPanel = W.Panel(v)
    listPanel:SetPoint("TOPLEFT", v, "TOPLEFT", 0, -18)
    listPanel:SetPoint("TOPRIGHT", v, "TOPRIGHT", 0, -18)
    listPanel:SetHeight(170)
    v.list = W.ScrollList(listPanel, { rowHeight = 22, createRow = CreateShoppingRow, updateRow = UpdateShoppingRow })
    v.list:SetPoint("TOPLEFT", listPanel, "TOPLEFT", 6, -6)
    v.list:SetPoint("BOTTOMRIGHT", listPanel, "BOTTOMRIGHT", -6, 6)
    v.empty = W.Label(listPanel, "", "GameFontDisable", "CENTER")
    v.empty:SetPoint("CENTER", listPanel, "CENTER", 0, 0)
    v.empty:SetWidth(520)

    local addTitle = W.Label(v, "Add an item vendors sell:", "GameFontNormalSmall")
    addTitle:SetPoint("TOPLEFT", listPanel, "BOTTOMLEFT", 4, -10)
    v.search = W.EditBox(v, 150, false, 40)
    v.search:SetPoint("LEFT", addTitle, "RIGHT", 10, 0)
    v.search:SetScript("OnEnterPressed", function(self)
        self:ClearFocus()
        UI.SearchShopItems(self:GetText())
    end)
    v.find = W.Button(v, "Search", 70, 20, function()
        UI.SearchShopItems(v.search:GetText())
    end)
    v.find:SetPoint("LEFT", v.search, "RIGHT", 6, 0)
    v.drinks = W.Button(v, "Drink tiers", 86, 20, function()
        UI.AddShoppingTiers("drink", tonumber(v.min:GetText()) or 0)
    end)
    v.drinks:SetPoint("LEFT", v.find, "RIGHT", 12, 0)
    W.SetTooltip(v.drinks, "Drink tiers", { "Puts the vendor waters of level 1, 5, 15, 25, 35 and 45 on the list,",
        "each with the minimum below. Change a tier's minimum on its row; 0 or Pause stops it." })
    v.breads = W.Button(v, "Food tiers", 80, 20, function()
        UI.AddShoppingTiers("food", tonumber(v.min:GetText()) or 0)
    end)
    v.breads:SetPoint("LEFT", v.drinks, "RIGHT", 4, 0)
    W.SetTooltip(v.breads, "Food tiers", { "Puts the vendor breads of level 1, 5, 15, 25, 35 and 45 on the list,",
        "each with the minimum below." })

    local resultPanel = W.Panel(v)
    resultPanel:SetPoint("TOPLEFT", listPanel, "BOTTOMLEFT", 0, -32)
    resultPanel:SetPoint("BOTTOMRIGHT", v, "BOTTOMRIGHT", 0, 34)
    v.results = W.ScrollList(resultPanel, { rowHeight = 18, createRow = CreateShopItemRow, updateRow = UpdateShopItemRow })
    v.results.view = v
    v.results:SetPoint("TOPLEFT", resultPanel, "TOPLEFT", 6, -6)
    v.results:SetPoint("BOTTOMRIGHT", resultPanel, "BOTTOMRIGHT", -6, 6)
    v.resultEmpty = W.Label(resultPanel, "", "GameFontDisable", "CENTER")
    v.resultEmpty:SetPoint("CENTER", resultPanel, "CENTER", 0, 0)
    v.resultEmpty:SetWidth(520)

    v.reload = W.Button(v, "Refresh", 80, 22, function()
        Protocol.Get("shopping")
    end)
    v.reload:SetPoint("BOTTOMLEFT", v, "BOTTOMLEFT", 0, 4)
    local minLabel = W.Label(v, "Minimum:", "GameFontNormalSmall")
    minLabel:SetPoint("LEFT", v.reload, "RIGHT", 14, 0)
    v.min = W.EditBox(v, 60, true, 6)
    v.min:SetPoint("LEFT", minLabel, "RIGHT", 8, 0)
    v.min:SetText("100")
    v.add = W.Button(v, "Add", 80, 22, function()
        UI.AddShoppingItem(v.selected, tonumber(v.min:GetText()) or 0)
    end)
    v.add:SetPoint("LEFT", v.min, "RIGHT", 8, 0)
    W.SetTooltip(v.add, "Add", {
        "The guild storage is filled up to the minimum: a bot at a vendor it visits anyway (selling, repairing,",
        "restocking) buys what is missing of an item this vendor sells, paid from the guild bank. No extra trips.",
    })
    v.hint = W.Label(v, "", "GameFontDisableSmall")
    v.hint:SetPoint("LEFT", v.add, "RIGHT", 10, 0)
    v.hint:SetPoint("RIGHT", v, "RIGHT", -8, 0)
    return v
end

function UI.SearchShopItems(text)
    text = text or ""
    if string.len(text) < 2 and not tonumber(text) then
        UI.Notify("Type at least two letters of the item's name, or its id.", true)
        return false
    end
    Protocol.ShopSearch(text, UI.RequestCallback(nil))
    return true
end

local function RefreshShoppingView(v)
    if State.IsConnected() and not v.asked then
        v.asked = true
        Protocol.Get("shopping")
    elseif not State.IsConnected() then
        v.asked = false
    end
    local s = State.shopping
    v.title:SetText(string.format("Shopping list: spent today %s of %s; at most %s per vendor visit, from the guild bank%s",
        GA.Data.FormatMoney(s.day or 0), GA.Data.FormatMoney(s.cap or 0), GA.Data.FormatMoney(s.visit or 0),
        s.srv == false and "  |cffff6060(off: bot setting Shopping.Enable)|r" or ""))
    v.list:SetItems(s.items)
    if not State.IsConnected() then
        v.empty:SetText("Not connected.")
    elseif #s.items > 0 then
        v.empty:SetText("")
    elseif not s.loaded then
        v.empty:SetText("Loading the shopping list...")
    else
        v.empty:SetText("The shopping list is empty.\n\nSearch for an item below, or put the drink and food tiers on it;\n"
            .. "bots fill the guild storage up to each minimum at vendors they visit anyway.")
    end

    local results = State.shopSearch.items
    v.results:SetItems(results)
    if not State.IsConnected() then
        v.resultEmpty:SetText("")
    elseif #results > 0 then
        v.resultEmpty:SetText("")
    elseif State.shopSearch.loaded then
        v.resultEmpty:SetText("No vendor of your side sells an item by that name for gold\n(ammunition and items that bind are left out).")
    else
        v.resultEmpty:SetText("Type a name and press Search.")
    end
    local selected
    for _, entry in ipairs(results) do
        if entry.e == v.selected then
            selected = entry
        end
    end
    v.hint:SetText(selected and ("Selected: " .. (selected.n or "?")) or "Pick an item of the results, set the minimum, Add.")
    local connected = State.IsConnected()
    W.SetEnabled(v.add, connected and selected ~= nil)
    W.SetEnabled(v.find, connected)
    W.SetEnabled(v.drinks, connected)
    W.SetEnabled(v.breads, connected)
    W.SetEnabled(v.reload, connected)
end

-------------------------------------------------------------------------------
-- Page
-------------------------------------------------------------------------------

local VIEWS = {
    { key = "items", label = "Items", create = CreateItemsView, refresh = RefreshItemsView },
    { key = "loot", label = "Loot filter", create = CreateLootView, refresh = RefreshLootView },
    { key = "log", label = "Recent loot", create = CreateLogView, refresh = RefreshLogView },
    { key = "orders", label = "Craft orders", create = CreateOrdersView, refresh = RefreshOrdersView },
    { key = "shopping", label = "Shopping list", create = CreateShoppingView, refresh = RefreshShoppingView },
}

local page

-- Stands in for a view whose creation raised a Lua error, so the tab says so instead of staying black (a half-built
-- view frame never gets its anchors and shows nothing at all).
local function CreateBrokenView(parent, err)
    local v = CreateFrame("Frame", nil, parent)
    v.gaBroken = true
    local text = W.Label(v, "|cffff4040This view could not be built.|r\n\n" .. tostring(err), "GameFontHighlightSmall",
        "CENTER")
    text:SetPoint("CENTER", v, "CENTER", 0, 0)
    text:SetWidth(460)
    return v
end

local function ViewFrame(def)
    local frame = page.frames[def.key]
    if not frame then
        local ok, result = pcall(def.create, page.body)
        if ok then
            frame = result
        else
            geterrorhandler()(result)
            frame = CreateBrokenView(page.body, result)
        end
        frame:SetAllPoints(page.body)
        page.frames[def.key] = frame
    end
    return frame
end

local function ShowView(key)
    page.view = key
    -- Build and show the chosen view first: a failure must not leave the tab without any view.
    for _, def in ipairs(VIEWS) do
        if def.key == key then
            ViewFrame(def):Show()
        end
    end
    for _, def in ipairs(VIEWS) do
        if def.key ~= key and page.frames[def.key] then
            page.frames[def.key]:Hide()
        end
    end
    if key == "log" and #State.loot.log == 0 then
        Protocol.Get("lootlog")
    end
    UI.RequestRefresh()
end

-- The frame of a sub-view once it was opened (ga-probe and the offline tests check it is anchored and visible).
function UI.StorageViewFrame(key)
    return page and page.frames[key]
end

local function Create(parent)
    local f = CreateFrame("Frame", nil, parent)
    page = f
    f.frames = {}
    f.buttons = {}
    local x = 0
    for i, def in ipairs(VIEWS) do
        local b = W.Button(f, def.label, 110, 22, function()
            ShowView(def.key)
        end)
        b:SetPoint("TOPLEFT", f, "TOPLEFT", x, -4)
        x = x + 114
        f.buttons[i] = b
    end
    f.body = CreateFrame("Frame", nil, f)
    f.body:SetPoint("TOPLEFT", f, "TOPLEFT", 0, -32)
    f.body:SetPoint("BOTTOMRIGHT", f, "BOTTOMRIGHT", 0, 0)
    ShowView("items")
    return f
end

local function Refresh(f)
    -- The buttons first, so they follow the view even when a view's refresh fails.
    for i, def in ipairs(VIEWS) do
        if def.key == f.view then
            f.buttons[i]:LockHighlight()
        else
            f.buttons[i]:UnlockHighlight()
        end
    end
    for _, def in ipairs(VIEWS) do
        local frame = f.frames[def.key]
        if frame and frame:IsShown() and not frame.gaBroken then
            def.refresh(frame)
        end
    end
end

function UI.ShowStorageView(key)
    UI.ShowPage("storage")
    if page then
        ShowView(key)
    end
end

UI.RegisterPage("storage", { tab = "storage", create = Create, refresh = Refresh })

-------------------------------------------------------------------------------
-- Alt+click in the bags: that stack into the storage (while the Storage tab is open)
-------------------------------------------------------------------------------

GA.OnInit(function()
    if not ContainerFrameItemButton_OnModifiedClick then
        return
    end
    hooksecurefunc("ContainerFrameItemButton_OnModifiedClick", function(self, button)
        if button ~= "LeftButton" or not IsAltKeyDown() or not UI.IsShown() or UI.CurrentPage() ~= "storage" then
            return
        end
        local bag = self:GetParent():GetID()
        local slot = self:GetID()
        if GetContainerItemInfo(bag, slot) then
            Protocol.StorageDeposit(bag, slot, UI.RequestCallback(nil))
        end
    end)
end)
