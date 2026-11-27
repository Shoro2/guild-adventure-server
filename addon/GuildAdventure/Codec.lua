-- GADV message layer (docs/protocol.md §3): OP^key=value^key=value with ~ escaping.
-- Pure functions, no WoW API: shared by the live transport, the mock server and the tests.

local GA = GuildAdventure
local Codec = {}
GA.Codec = Codec

Codec.FIELD_SEP = "^"

local ESCAPE = {
    ["~"] = "~~",
    ["^"] = "~c",
    ["|"] = "~p",
    ["\n"] = "~n",
    ["\r"] = "~r",
    ["\t"] = "~t",
    ["\0"] = "~0",
}
local UNESCAPE = {
    ["~"] = "~",
    c = "^",
    p = "|",
    n = "\n",
    r = "\r",
    t = "\t",
    ["0"] = "\0",
}

function Codec.Escape(s)
    return (string.gsub(s, "[~%^|\n\r\t%z]", ESCAPE))
end

function Codec.Unescape(s)
    return (string.gsub(s, "~(.)", function(c)
        local raw = UNESCAPE[c]
        if raw then
            return raw
        end
        return nil -- unknown sequence: keep "~x" as it is
    end))
end

-- Integers are written without exponent; "%.0f" also avoids the 32-bit overflow of "%d"
-- in the 3.3.5a client (copper amounts can exceed 2^31).
local function FormatNumber(v)
    if v == math.floor(v) then
        return string.format("%.0f", v)
    end
    return string.format("%.2f", v)
end

local function EncodeScalar(v)
    local t = type(v)
    if t == "boolean" then
        return v and "1" or "0"
    elseif t == "number" then
        return FormatNumber(v)
    end
    return Codec.Escape(tostring(v))
end

-- A table value is a list; a list item that is itself a table is a colon tuple.
local function EncodeValue(v)
    if type(v) ~= "table" then
        return EncodeScalar(v)
    end
    local items = {}
    for i = 1, #v do
        local item = v[i]
        if type(item) == "table" then
            local parts = {}
            for j = 1, #item do
                parts[j] = EncodeScalar(item[j])
            end
            items[i] = table.concat(parts, ":")
        else
            items[i] = EncodeScalar(item)
        end
    end
    return table.concat(items, ",")
end

-- Encode(op, fields) -> "OP^a=1^b=x". Keys are sorted so the output is deterministic.
-- nil values are skipped.
function Codec.Encode(op, fields)
    local out = { op }
    if fields then
        local keys = {}
        for k in pairs(fields) do
            keys[#keys + 1] = k
        end
        table.sort(keys)
        for i = 1, #keys do
            local k = keys[i]
            out[#out + 1] = Codec.Escape(k) .. "=" .. EncodeValue(fields[k])
        end
    end
    return table.concat(out, Codec.FIELD_SEP)
end

-- Decode("OP^a=1^b=x") -> "OP", { a = "1", b = "x" }. Values stay strings; use the
-- typed readers below or Codec.ApplySchema.
function Codec.Decode(s)
    local op, rest = string.match(s, "^([^%^]*)(.*)$")
    local fields = {}
    for part in string.gmatch(rest, "%^([^%^]*)") do
        local k, v = string.match(part, "^([^=]*)=(.*)$")
        if k then
            fields[Codec.Unescape(k)] = Codec.Unescape(v)
        elseif part ~= "" then
            fields[Codec.Unescape(part)] = ""
        end
    end
    return op, fields
end

-------------------------------------------------------------------------------
-- Typed readers
-------------------------------------------------------------------------------

function Codec.Int(v, default)
    local n = tonumber(v)
    if not n then
        return default
    end
    return math.floor(n)
end

function Codec.Bool(v, default)
    if v == nil or v == "" then
        return default
    end
    return v == "1" or v == "true"
end

function Codec.List(v)
    local list = {}
    if v == nil or v == "" then
        return list
    end
    for item in string.gmatch(v, "[^,]+") do
        list[#list + 1] = item
    end
    return list
end

function Codec.IntList(v)
    local list = {}
    for _, item in ipairs(Codec.List(v)) do
        local n = tonumber(item)
        if n then
            list[#list + 1] = math.floor(n)
        end
    end
    return list
end

function Codec.Tuples(v)
    local list = {}
    for _, item in ipairs(Codec.List(v)) do
        local tuple = {}
        for part in string.gmatch(item, "[^:]+") do
            tuple[#tuple + 1] = tonumber(part) or part
        end
        list[#list + 1] = tuple
    end
    return list
end

local READERS = {
    int = Codec.Int,
    bool = Codec.Bool,
    str = function(v) return v end,
    list = Codec.List,
    intlist = Codec.IntList,
    tuples = Codec.Tuples,
}

-- Converts decoded string fields with a schema { key = "int" | "bool" | "str" | "list" |
-- "intlist" | "tuples" }. Keys missing from the schema are kept as strings (forward
-- compatibility, docs/protocol.md §4).
function Codec.ApplySchema(fields, schema)
    local out = {}
    for k, v in pairs(fields) do
        local kind = schema[k]
        local reader = kind and READERS[kind]
        if reader then
            out[k] = reader(v)
        else
            out[k] = v
        end
    end
    return out
end
