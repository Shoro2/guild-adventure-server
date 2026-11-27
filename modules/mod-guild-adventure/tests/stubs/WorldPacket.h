// Test stub of AzerothCore's WorldPacket/ByteBuffer: little-endian appends and the const positional read, which
// throws on a read past the end like ByteBufferPositionException.
#ifndef GA_TEST_STUB_WORLD_PACKET_H
#define GA_TEST_STUB_WORLD_PACKET_H

#include "Define.h"
#include <cstddef>
#include <stdexcept>
#include <vector>

class WorldPacket
{
public:
    explicit WorldPacket(uint16 opcode, std::size_t reserve = 200) : _opcode(opcode) { _storage.reserve(reserve); }

    [[nodiscard]] uint16 GetOpcode() const { return _opcode; }
    [[nodiscard]] std::size_t size() const { return _storage.size(); }

    template <typename T>
    [[nodiscard]] T read(std::size_t pos) const
    {
        if (pos + sizeof(T) > _storage.size())
            throw std::out_of_range("WorldPacket::read past the end");

        T value = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i)
            value = T(value | (T(_storage[pos + i]) << (8 * i)));
        return value;
    }

    WorldPacket& operator<<(uint8 value)
    {
        _storage.push_back(value);
        return *this;
    }

    WorldPacket& operator<<(uint32 value)
    {
        for (int i = 0; i < 4; ++i)
            _storage.push_back(uint8(value >> (8 * i)));
        return *this;
    }

private:
    uint16 _opcode;
    std::vector<uint8> _storage;
};

#endif
