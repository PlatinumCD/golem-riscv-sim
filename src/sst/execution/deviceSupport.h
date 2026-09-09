#pragma once
#include "cpuActions.h"
#include "diagnosticMessage.h"
#include "../bridge/sharedSyncMemoryBridge.h"
#include "../configuration/tileConfiguration.h"
#include "../memory/addressRegion.h"
#include <mittens/MemoryMap.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
namespace SST::Mittens
{
// Owner-thread diagnostics; failures throw before reaching the Tile boundary.
struct DeviceDiagnostics
{
    std::function<void(int, const std::string&)> log;
    template <class... A> [[noreturn]] void fatal(int, const char* format, A... args) const
    {
        throw std::runtime_error(message(format, args...));
    }
    template <class... A> void verbose(int level, int, const char* format, A... args) const
    {
        if (log)
            log(level, message(format, args...));
    }
    template <class... A> static std::string message(const char* format, A... args)
    {
        return diagnosticMessage("device diagnostic formatting error", format, args...);
    }
};
} // namespace SST::Mittens
