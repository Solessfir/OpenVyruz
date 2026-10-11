#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <type_traits>

// On-disk layouts are declared as plain structs and copied out byte-for-byte; the original data is little-endian.
static_assert(std::endian::native == std::endian::little, "On-disk structs are read without byte swapping");

// Copies a T out of Data at Offset, or nullopt when it does not fit.
template <typename T>
    requires std::is_trivially_copyable_v<T>
std::optional<T> ReadStruct(const std::span<const uint8_t> Data, const size_t Offset = 0)
{
    if (Offset > Data.size() || Data.size() - Offset < sizeof(T))
    {
        return std::nullopt;
    }
    T Value;
    std::memcpy(&Value, Data.data() + Offset, sizeof(T));
    return Value;
}
