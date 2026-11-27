/*
 * GADV wire format. See GadvCodec.h and docs/protocol.md sections 2 and 3.
 */

#include "GadvCodec.h"
#include <charconv>

namespace GuildAdventure::Gadv
{
    namespace
    {
        bool ParseInt64(std::string_view text, int64_t& out)
        {
            if (text.empty())
                return false;
            char const* begin = text.data();
            char const* end = text.data() + text.size();
            auto [ptr, ec] = std::from_chars(begin, end, out);
            return ec == std::errc() && ptr == end;
        }

        bool ParseUInt32(std::string_view text, uint32_t& out)
        {
            if (text.empty() || text.size() > 9)
                return false;
            for (char c : text)
                if (c < '0' || c > '9')
                    return false;
            auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
            return ec == std::errc() && ptr == text.data() + text.size();
        }

        bool IsOpcode(std::string_view text)
        {
            if (text.empty())
                return false;
            for (char c : text)
                if (!((c >= 'A' && c <= 'Z') || c == '_'))
                    return false;
            return true;
        }
    }

    std::string const* Message::Get(std::string const& key) const
    {
        auto itr = _fields.find(key);
        return itr != _fields.end() ? &itr->second : nullptr;
    }

    std::optional<int64_t> Message::GetInt(std::string const& key) const
    {
        std::string const* value = Get(key);
        int64_t result = 0;
        if (!value || !ParseInt64(*value, result))
            return std::nullopt;
        return result;
    }

    std::optional<bool> Message::GetBool(std::string const& key) const
    {
        std::string const* value = Get(key);
        if (!value)
            return std::nullopt;
        if (*value == "1")
            return true;
        if (*value == "0")
            return false;
        return std::nullopt;
    }

    std::string Message::GetString(std::string const& key, std::string const& fallback) const
    {
        std::string const* value = Get(key);
        return value ? *value : fallback;
    }

    std::optional<std::vector<int64_t>> Message::GetIntList(std::string const& key) const
    {
        std::string const* value = Get(key);
        if (!value)
            return std::nullopt;

        std::vector<int64_t> result;
        if (value->empty())
            return result;

        std::size_t start = 0;
        while (start <= value->size())
        {
            std::size_t const comma = value->find(',', start);
            std::size_t const end = comma == std::string::npos ? value->size() : comma;
            int64_t item = 0;
            if (!ParseInt64(std::string_view(*value).substr(start, end - start), item))
                return std::nullopt;
            result.push_back(item);
            if (comma == std::string::npos)
                break;
            start = comma + 1;
        }
        return result;
    }

    std::string Escape(std::string_view raw)
    {
        std::string out;
        out.reserve(raw.size());
        for (char c : raw)
        {
            switch (c)
            {
                case '~':  out += "~~"; break;
                case '^':  out += "~c"; break;
                case '|':  out += "~p"; break;
                case '\n': out += "~n"; break;
                case '\r': out += "~r"; break;
                case '\t': out += "~t"; break;
                case '\0': out += "~0"; break;
                default:   out += c;    break;
            }
        }
        return out;
    }

    std::string Unescape(std::string_view escaped)
    {
        std::string out;
        out.reserve(escaped.size());
        for (std::size_t i = 0; i < escaped.size(); ++i)
        {
            char const c = escaped[i];
            if (c != '~' || i + 1 >= escaped.size())
            {
                out += c;
                continue;
            }

            char const next = escaped[++i];
            switch (next)
            {
                case '~': out += '~';  break;
                case 'c': out += '^';  break;
                case 'p': out += '|';  break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case '0': out += '\0'; break;
                default:
                    // Unknown sequences map to themselves (docs/protocol.md section 3).
                    out += '~';
                    out += next;
                    break;
            }
        }
        return out;
    }

    std::string Encode(Message const& message)
    {
        std::string out = message.Opcode();
        for (auto const& [key, value] : message.Fields())
        {
            out += '^';
            out += Escape(key);
            out += '=';
            out += Escape(value);
        }
        return out;
    }

    std::optional<Message> Decode(std::string_view text)
    {
        std::size_t const firstSep = text.find('^');
        std::string_view const opcode = text.substr(0, firstSep);
        if (!IsOpcode(opcode))
            return std::nullopt;

        Message message{std::string(opcode)};
        if (firstSep == std::string_view::npos)
            return message;

        std::size_t start = firstSep + 1;
        while (start <= text.size())
        {
            std::size_t const sep = text.find('^', start);
            std::string_view const field = text.substr(start, sep == std::string_view::npos ? std::string_view::npos : sep - start);
            if (!field.empty())
            {
                // A field is split at its first '='; a field without '=' is a key with an empty value.
                std::size_t const eq = field.find('=');
                std::string key = Unescape(field.substr(0, eq));
                std::string value = eq == std::string_view::npos ? std::string() : Unescape(field.substr(eq + 1));
                if (!key.empty())
                    message.Set(key, std::move(value));
            }
            if (sep == std::string_view::npos)
                break;
            start = sep + 1;
        }
        return message;
    }

    std::optional<Chunk> ParseChunk(std::string_view text)
    {
        // <dir><id>,<seq>,<total>:<body>
        if (text.size() < 7 || (text[0] != 'C' && text[0] != 'S'))
            return std::nullopt;

        std::size_t const colon = text.find(':');
        if (colon == std::string_view::npos || colon > 12)
            return std::nullopt;

        std::string_view const header = text.substr(1, colon - 1);
        std::size_t const c1 = header.find(',');
        std::size_t const c2 = c1 == std::string_view::npos ? std::string_view::npos : header.find(',', c1 + 1);
        if (c1 == std::string_view::npos || c2 == std::string_view::npos)
            return std::nullopt;

        Chunk chunk;
        chunk.direction = text[0];
        if (!ParseUInt32(header.substr(0, c1), chunk.id) ||
            !ParseUInt32(header.substr(c1 + 1, c2 - c1 - 1), chunk.seq) ||
            !ParseUInt32(header.substr(c2 + 1), chunk.total))
            return std::nullopt;

        if (chunk.id < 1 || chunk.id > MaxMessageId || chunk.total < 1 || chunk.total > MaxChunks ||
            chunk.seq < 1 || chunk.seq > chunk.total)
            return std::nullopt;

        chunk.body = std::string(text.substr(colon + 1));
        return chunk;
    }

    std::vector<std::string> SplitIntoChunks(std::string const& encoded, char direction, uint32_t id)
    {
        // Cut the body into slices of at most MaxBodyBytes, never in front of a UTF-8 continuation byte.
        std::vector<std::string_view> slices;
        std::string_view rest(encoded);
        do
        {
            std::size_t cut = rest.size() <= MaxBodyBytes ? rest.size() : MaxBodyBytes;
            if (cut < rest.size())
                while (cut > 0 && (static_cast<unsigned char>(rest[cut]) & 0xC0) == 0x80)
                    --cut;
            if (cut == 0)
                cut = rest.size() <= MaxBodyBytes ? rest.size() : MaxBodyBytes; // not UTF-8; cut hard
            slices.push_back(rest.substr(0, cut));
            rest.remove_prefix(cut);
        } while (!rest.empty());

        if (slices.size() > MaxChunks)
            return {};

        std::vector<std::string> chunks;
        chunks.reserve(slices.size());
        std::string const prefix = std::string(1, direction) + std::to_string(id) + ",";
        std::string const total = "," + std::to_string(slices.size()) + ":";
        for (std::size_t i = 0; i < slices.size(); ++i)
        {
            std::string chunk = prefix + std::to_string(i + 1) + total;
            chunk.append(slices[i]);
            chunks.push_back(std::move(chunk));
        }
        return chunks;
    }

    std::string JoinInts(std::vector<int64_t> const& values)
    {
        std::string out;
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            if (i)
                out += ',';
            out += std::to_string(values[i]);
        }
        return out;
    }
}
