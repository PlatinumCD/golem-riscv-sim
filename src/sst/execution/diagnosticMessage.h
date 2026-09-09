#pragma once

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace SST::Mittens
{

// Shared formatting only. Each owner retains its logging/failure policy.
template <class... Args>
std::string diagnosticMessage(const char* failure, const char* format, Args... args)
{
    const int size = std::snprintf(nullptr, 0, format, args...);
    if (size < 0)
        return failure;
    std::vector<char> buffer(std::max<std::size_t>(256, static_cast<std::size_t>(size) + 1));
    std::snprintf(buffer.data(), buffer.size(), format, args...);
    return {buffer.data(), static_cast<std::size_t>(size)};
}

} // namespace SST::Mittens
