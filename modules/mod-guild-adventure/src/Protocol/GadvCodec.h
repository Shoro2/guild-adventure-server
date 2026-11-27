/*
 * GADV wire format (docs/protocol.md sections 2 and 3): message layer (OP^key=value^...) with "~" escaping and
 * the chunk layer (<dir><id>,<seq>,<total>:<body>). Pure string code without game dependencies, so it can be
 * tested outside the worldserver (tools\test-gadv-codec.ps1).
 */

#ifndef MOD_GUILD_ADVENTURE_GADV_CODEC_H
#define MOD_GUILD_ADVENTURE_GADV_CODEC_H

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace GuildAdventure::Gadv
{
    constexpr int ProtocolVersion = 1;
    constexpr int MinClientVersion = 1;
    constexpr std::string_view Prefix = "GADV";
    constexpr std::size_t MaxChunkBytes = 240;   // header + body; "GADV\t" + chunk stays <= 255
    constexpr std::size_t MaxBodyBytes = 228;    // MaxChunkBytes minus the longest header "C9999,64,64:"
    constexpr uint32_t MaxChunks = 64;
    constexpr uint32_t MaxMessageId = 9999;

    // One logical message. Keys are unique; field order on the wire is irrelevant (encoded sorted).
    class Message
    {
    public:
        Message() = default;
        explicit Message(std::string opcode) : _opcode(std::move(opcode)) { }

        std::string const& Opcode() const { return _opcode; }
        std::map<std::string, std::string> const& Fields() const { return _fields; }

        Message& Set(std::string const& key, std::string value) { _fields[key] = std::move(value); return *this; }
        Message& Set(std::string const& key, char const* value) { _fields[key] = value ? value : ""; return *this; }
        Message& Set(std::string const& key, bool value) { _fields[key] = value ? "1" : "0"; return *this; }

        template <typename T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
        Message& Set(std::string const& key, T value)
        {
            // Widen first so 8-bit types are written as numbers, not as characters.
            if constexpr (std::is_signed_v<T>)
                _fields[key] = std::to_string(static_cast<long long>(value));
            else
                _fields[key] = std::to_string(static_cast<unsigned long long>(value));
            return *this;
        }

        bool Has(std::string const& key) const { return _fields.find(key) != _fields.end(); }
        // Raw (decoded) value or nullptr.
        std::string const* Get(std::string const& key) const;
        // Typed reads: std::nullopt when the key is missing or malformed.
        std::optional<int64_t> GetInt(std::string const& key) const;
        std::optional<bool> GetBool(std::string const& key) const;
        std::string GetString(std::string const& key, std::string const& fallback = {}) const;
        // "list" values: comma-separated integers. Returns std::nullopt if any item is not an integer.
        std::optional<std::vector<int64_t>> GetIntList(std::string const& key) const;

    private:
        std::string _opcode;
        std::map<std::string, std::string> _fields;
    };

    std::string Escape(std::string_view raw);
    std::string Unescape(std::string_view escaped);

    std::string Encode(Message const& message);
    // std::nullopt if the opcode is not [A-Z_]+.
    std::optional<Message> Decode(std::string_view text);

    struct Chunk
    {
        char direction = 0; // 'C' client -> server, 'S' server -> client
        uint32_t id = 0;
        uint32_t seq = 0;
        uint32_t total = 0;
        std::string body;
    };

    std::optional<Chunk> ParseChunk(std::string_view text);
    // Splits an encoded message into chunk texts (without the "GADV\t" prefix). Body cuts never fall in front
    // of a UTF-8 continuation byte. Returns an empty vector if the message needs more than MaxChunks chunks.
    std::vector<std::string> SplitIntoChunks(std::string const& encoded, char direction, uint32_t id);

    // Helpers for list / tuples values.
    std::string JoinInts(std::vector<int64_t> const& values);
}

#endif
