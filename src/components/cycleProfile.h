#pragma once
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace TileComponents {
// Passive observation only. No SST events, clocks, callbacks or model state
// are introduced. Timestamps use simulation ns (one cycle at the tile's 1 GHz).
// Profiling is explicitly opt-in and independent of legacy test diagnostics.
class CycleProfile {
    std::ofstream stream_;
public:
    static bool enabled() {
        const char* flag = std::getenv("TILE_CYCLE_PROFILE");
        if (!flag || std::string(flag) == "0") return false;
        if (std::string(flag) == "1") return true;
        throw std::invalid_argument("TILE_CYCLE_PROFILE must be 0 or 1");
    }
    static std::string directory() {
        if (!enabled()) return {};
        std::filesystem::path path;
        if (const char* dir = std::getenv("TILE_CYCLE_PROFILE_DIRECTORY"); dir && *dir) path = dir;
        else if (const char* dir = std::getenv("TILE_COMPONENT_OUTPUT"); dir && *dir)
            path = std::filesystem::path(dir) / "profiles";
        else throw std::invalid_argument("Profiling requires TILE_CYCLE_PROFILE_DIRECTORY or TILE_COMPONENT_OUTPUT");
        std::filesystem::create_directories(path);
        return path.string();
    }
    static std::string traceDirectory() {
        // Existing test validators retain their diagnostic filenames. Standalone
        // profiling collects the CPU, bank, array and NIU event traces as well.
        if (const char* dir = std::getenv("TILE_COMPONENT_OUTPUT"); dir && *dir) return dir;
        return directory();
    }
    void open(const std::string& component) {
        const auto output = directory();
        if (output.empty()) return;
        stream_.open(output + "/" + component + "-cycles.csv");
        if (!stream_) throw std::runtime_error("cannot open cycle profile for " + component);
        stream_ << "cycle,kind,resource,index,value,token,detail\n";
    }
    explicit operator bool() const { return stream_.is_open(); }
    void record(std::uint64_t cycle, const char* kind, const char* resource,
                std::uint64_t index, std::int64_t value,
                std::uint64_t token = 0, std::uint64_t detail = 0) {
        if (stream_.is_open()) stream_ << cycle << ',' << kind << ',' << resource << ','
            << index << ',' << value << ',' << token << ',' << detail << '\n';
    }
    void flush() { if (stream_.is_open()) stream_.flush(); }
};
}
