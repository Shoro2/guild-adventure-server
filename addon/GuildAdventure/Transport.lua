-- GADV chunk layer and transport (docs/protocol.md §1-2).
-- Splits encoded messages into <dir><id>,<seq>,<total>:<body> chunks, throttles sending and
-- reassembles incoming chunks. Two backends: "live" (whisper-to-self addon messages) and
-- "mock" (the in-client MockServer).

local GA = GuildAdventure
local Transport = {}
GA.Transport = Transport

Transport.MAX_CHUNK = 240          -- header + body, so "GADV\t" .. chunk <= 255
Transport.MAX_HEADER = 12          -- "C9999,64,64:"
Transport.MAX_BODY = Transport.MAX_CHUNK - Transport.MAX_HEADER
Transport.MAX_CHUNKS = 64
Transport.MAX_ID = 9999
Transport.REASSEMBLY_TIMEOUT = 15  -- seconds
Transport.SEND_RATE = 10           -- chunks per second (client side throttle)
Transport.MAX_PARTIALS = 8

-------------------------------------------------------------------------------
-- Pure helpers (also used by MockServer and tests)
-------------------------------------------------------------------------------

-- Returns the last byte index of a slice that starts at startPos and holds at most maxLen
-- bytes without cutting a UTF-8 sequence (never cut in front of a 0x80..0xBF byte).
local function SafeCut(s, startPos, maxLen)
    local endPos = startPos + maxLen - 1
    local len = string.len(s)
    if endPos >= len then
        return len
    end
    while endPos > startPos do
        local nextByte = string.byte(s, endPos + 1)
        if nextByte < 0x80 or nextByte >= 0xC0 then
            break
        end
        endPos = endPos - 1
    end
    return endPos
end
Transport.SafeCut = SafeCut

-- Split(encoded, dir, id) -> list of chunk strings. Returns nil if the message would need
-- more than MAX_CHUNKS chunks.
function Transport.Split(encoded, dir, id)
    local bodies = {}
    local len = string.len(encoded)
    local pos = 1
    if len == 0 then
        bodies[1] = ""
    end
    while pos <= len do
        local cut = SafeCut(encoded, pos, Transport.MAX_BODY)
        bodies[#bodies + 1] = string.sub(encoded, pos, cut)
        pos = cut + 1
    end
    local total = #bodies
    if total > Transport.MAX_CHUNKS then
        return nil
    end
    local chunks = {}
    for seq = 1, total do
        chunks[seq] = dir .. id .. "," .. seq .. "," .. total .. ":" .. bodies[seq]
    end
    return chunks
end

-- ParseChunk(chunk) -> dir, id, seq, total, body (nil if malformed).
function Transport.ParseChunk(chunk)
    local dir, id, seq, total, body = string.match(chunk, "^([CS])(%d+),(%d+),(%d+):(.*)$")
    if not dir then
        return nil
    end
    id, seq, total = tonumber(id), tonumber(seq), tonumber(total)
    if seq < 1 or total < 1 or seq > total or total > Transport.MAX_CHUNKS then
        return nil
    end
    return dir, id, seq, total, body
end

-- NewReassembler(expectedDir, onMessage) -> object with :Feed(chunk, now) and :Expire(now).
-- onMessage(encoded) is called once per complete logical message.
-- :Feed returns "ok", "foreign" (wrong direction) or "bad" (malformed).
function Transport.NewReassembler(expectedDir, onMessage)
    local r = { partial = {}, count = 0 }

    function r:Feed(chunk, now)
        local dir, id, seq, total, body = Transport.ParseChunk(chunk)
        if not dir then
            return "bad"
        end
        if dir ~= expectedDir then
            return "foreign"
        end
        if total == 1 then
            onMessage(body)
            return "ok"
        end
        local p = self.partial[id]
        if p and p.total ~= total then
            -- id reused with a different shape: drop the stale partial
            self.partial[id] = nil
            self.count = self.count - 1
            p = nil
        end
        if not p then
            if self.count >= Transport.MAX_PARTIALS then
                self:DropOldest()
            end
            p = { total = total, parts = {}, got = 0, started = now }
            self.partial[id] = p
            self.count = self.count + 1
        end
        if not p.parts[seq] then
            p.parts[seq] = body
            p.got = p.got + 1
        end
        if p.got == p.total then
            self.partial[id] = nil
            self.count = self.count - 1
            onMessage(table.concat(p.parts, "", 1, p.total))
        end
        return "ok"
    end

    function r:DropOldest()
        local oldestId, oldest
        for id, p in pairs(self.partial) do
            if not oldest or p.started < oldest.started then
                oldestId, oldest = id, p
            end
        end
        if oldestId then
            self.partial[oldestId] = nil
            self.count = self.count - 1
        end
    end

    function r:Expire(now)
        for id, p in pairs(self.partial) do
            if now - p.started > Transport.REASSEMBLY_TIMEOUT then
                self.partial[id] = nil
                self.count = self.count - 1
            end
        end
    end

    function r:Reset()
        self.partial = {}
        self.count = 0
    end

    return r
end

-------------------------------------------------------------------------------
-- Client transport
-------------------------------------------------------------------------------

Transport.backend = "live"   -- "live" | "mock"
Transport.echoSeen = false   -- our own C chunks came back: the server module is missing

local nextId = 0
local queue = {}
local sendFrame = CreateFrame("Frame")
sendFrame:Hide()

local function NextId()
    nextId = nextId % Transport.MAX_ID + 1
    return nextId
end

local function MessageReceived(encoded)
    GA.Debug("<< " .. encoded)
    GA.Protocol.OnMessage(encoded)
end

local reassembler = Transport.NewReassembler("S", MessageReceived)

-- Low GUID of the character the client plays. After a world-reload swap (Path W) the
-- 3.3.5a client keeps the old name in UnitName("player") (it comes from the character list
-- of the login), but UnitGUID("player") is the new character's.
function Transport.PlayerGuid()
    local guid = UnitGUID("player")
    if not guid then
        return nil
    end
    return tonumber(string.sub(guid, -8), 16)
end

-- Name of the character the client really plays: the server's name for our GUID wins over
-- UnitName("player"). This is the whisper-to-self target (docs/protocol.md §1).
function Transport.SelfName()
    local low = Transport.PlayerGuid()
    if low then
        local State = GA.State
        local c = State.control
        if c.g == low and c.cn and c.cn ~= "" then
            return c.cn
        end
        if c.leader == low and c.ln and c.ln ~= "" then
            return c.ln
        end
        local bot = State.bots[low]
        if bot and bot.n and bot.n ~= "" then
            return bot.n
        end
    end
    return UnitName("player")
end
local OwnName = Transport.SelfName

-- The server only ever talks as the character we play. Accept that name, the possibly stale
-- UnitName("player") and the names of our own guild characters (leader and roster bots):
-- right after a swap the client may not know yet which of them it is.
local function IsOwnCharacter(name)
    if name == UnitName("player") or name == OwnName() then
        return true
    end
    local State = GA.State
    if State.control.ln == name then
        return true
    end
    for _, bot in pairs(State.bots) do
        if bot.n == name then
            return true
        end
    end
    return false
end

local function DeliverChunk(chunk)
    if Transport.backend == "mock" then
        GA.MockServer.ReceiveChunk(chunk)
    else
        SendAddonMessage(GA.PREFIX, chunk, "WHISPER", OwnName())
    end
end

local sendBudget = 0
local expireTimer = 0
sendFrame:SetScript("OnUpdate", function(self, elapsed)
    sendBudget = math.min(Transport.SEND_RATE, sendBudget + elapsed * Transport.SEND_RATE)
    while sendBudget >= 1 and #queue > 0 do
        sendBudget = sendBudget - 1
        DeliverChunk(table.remove(queue, 1))
    end
    expireTimer = expireTimer + elapsed
    if expireTimer > 1 then
        expireTimer = 0
        reassembler:Expire(GetTime())
    end
    if #queue == 0 and reassembler.count == 0 then
        self:Hide()
    end
end)

-- Send(encoded): queue one logical message (already encoded by GA.Codec).
function Transport.Send(encoded)
    GA.Debug(">> " .. encoded)
    local chunks = Transport.Split(encoded, "C", NextId())
    if not chunks then
        GA.Print("Message too large, not sent: " .. string.sub(encoded, 1, 40))
        return false
    end
    for i = 1, #chunks do
        queue[#queue + 1] = chunks[i]
    end
    if not sendFrame:IsShown() then
        sendBudget = 1 -- the first chunk goes out on the next frame
        sendFrame:Show()
    end
    return true
end

-- CHAT_MSG_ADDON entry point (live) and mock delivery point (MockServer calls it too).
function Transport.OnAddonMessage(prefix, message, distribution, sender)
    if prefix ~= GA.PREFIX or distribution ~= "WHISPER" or not message then
        return
    end
    sender = sender and string.match(sender, "^([^%-]+)") or sender
    if not sender or not IsOwnCharacter(sender) then
        return
    end
    local result = reassembler:Feed(message, GetTime())
    if result == "foreign" then
        if Transport.backend == "live" and not Transport.echoSeen then
            Transport.echoSeen = true
            GA.Fire("TRANSPORT_ECHO")
        end
    elseif result == "bad" then
        GA.Debug("dropped malformed chunk: " .. string.sub(message, 1, 40))
    elseif reassembler.count > 0 then
        sendFrame:Show() -- keep the expiry tick running while partials exist
    end
end

function Transport.SetBackend(backend)
    Transport.backend = backend
    Transport.echoSeen = false
    queue = {}
    reassembler:Reset()
end

function Transport.QueueLength()
    return #queue
end
