#include "performanceProfile.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <stdlib.h>

using SST::Mittens::PerformanceProfile;

namespace {

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream input(path);
    assert(input.is_open());
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

} // namespace

int main()
{
    char directoryTemplate[] =
        "/tmp/mittens-performance-profile-XXXXXX";
    char* const rawDirectory = mkdtemp(directoryTemplate);
    assert(rawDirectory != nullptr);
    const std::filesystem::path directory(rawDirectory);

    {
        PerformanceProfile profile;
        profile.configure(3, directory.string(), true);
        assert(profile.enabled());
        assert(profile.traceEnabled());

        profile.recordWait(100, 125, "nic-receive-wait", 7);
        profile.recordNetwork(
            "arrive",
            9,
            0,
            3,
            4,
            2,
            "frame-payload",
            16,
            0,
            16,
            3,
            80,
            90,
            130);
        profile.recordReceiveDMA(
            "complete",
            0,
            4,
            2,
            11,
            16,
            130,
            140,
            150,
            150,
            10);
        profile.recordAnalog(
            "compute-start", 12, 3, 1, 40, 160);
        profile.recordMemory(
            "response", 13, 0x80001000, 0x1000, 0x80000200, 0x80000400,
            8, false,
            5, 2, "task", 170, 190);
        profile.recordTransmitBlocked(
            14, 4, 2, 5, "frame-payload", 16, 180, 195, 3, 5);

        const std::uint64_t waits[] = {0, 1, 2, 3};
        const char* const waitNames[] = {
            "none",
            "quantum-end",
            "nic-transmit",
            "nic-receive-wait",
        };
        profile.writeSummary(
            200,
            1000,
            20,
            500,
            2,
            21,
            63,
            40,
            10,
            30,
            12,
            10,
            15,
            1,
            3,
            5,
            waits,
            waitNames,
            4);
    }

    const std::string waitText =
        readFile(directory / "tile-3-waits.csv");
    assert(waitText.find("nic-receive-wait,100,125,25") !=
           std::string::npos);

    const std::string networkText =
        readFile(directory / "tile-3-network.csv");
    assert(networkText.find(
               "frame-payload,16,0,16,3,48,80,90,130,10,40") !=
           std::string::npos);

    const std::string summaryText =
        readFile(directory / "tile-3-summary.csv");
    assert(summaryText.find("network_word_hops,63") !=
           std::string::npos);
    assert(summaryText.find("wait_nic-receive-wait_ticks,3") !=
           std::string::npos);
    assert(summaryText.find("transmit_blocked_ticks,15") !=
           std::string::npos);

    const std::string transmitText =
        readFile(directory / "tile-3-transmit-blocked.csv");
    assert(transmitText.find(
               "3,14,4,2,5,frame-payload,16,180,195,15,3,5") !=
           std::string::npos);

    std::error_code error;
    std::filesystem::remove_all(directory, error);
    assert(!error);
    return 0;
}
