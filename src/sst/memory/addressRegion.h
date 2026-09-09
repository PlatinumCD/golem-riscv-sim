#pragma once
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <mittens/MemoryMap.h>

namespace SST::Mittens
{

// A byte-addressed half-open region. No method forms an unchecked end address.
// Empty ranges at the end are allowed here; clients decide whether zero-length
// operations are meaningful. Endpoints must be representable in uint64_t.
struct AddressRegion
{
    std::uint64_t base;
    std::uint64_t bytes;

    static constexpr bool representable(std::uint64_t address, std::uint64_t length) noexcept
    {
        return mittens_memory_span_representable(address, length);
    }
    constexpr bool contains(std::uint64_t address) const noexcept
    {
        return mittens_memory_contains(base, bytes, address);
    }
    constexpr bool containsRange(std::uint64_t address, std::uint64_t length) const noexcept
    {
        return mittens_memory_contains_range(base, bytes, address, length);
    }
    constexpr bool crossesStart(std::uint64_t address, std::uint64_t length) const noexcept
    {
        return address < base && length > base - address;
    }
    std::uint64_t offset(std::uint64_t address, std::uint64_t length = 0) const
    {
        if (!containsRange(address, length))
            throw std::out_of_range("address range is outside memory region");
        return address - base;
    }
};
} // namespace SST::Mittens
