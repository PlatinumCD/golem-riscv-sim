#ifndef SST_MITTENS_QEMU_PROCESS_H
#define SST_MITTENS_QEMU_PROCESS_H

#include <cstdint>
#include <mittens/MemoryMap.h>
#include <optional>
#include <string>

#include <sys/types.h>

namespace SST {
namespace Mittens {

struct QemuConfiguration {
    std::uint32_t tileId;
    std::string executable;
    std::string elfPath;
    std::string memory;
    bool riscvVectorEnabled = true;
    std::uint32_t riscvVectorLengthBits = 256;
    std::uint32_t riscvVectorElementBits = 64;
    std::uint64_t scratchpadBase = MITTENS_SCRATCHPAD_BASE;
    std::uint64_t scratchpadBytes = UINT64_C(256) * 1024;
    int syncBridgeFileDescriptor = -1;
    int bridgeFileDescriptor = -1;
    int analogBridgeFileDescriptor = -1;
    int globalRAMFileDescriptor = -1;
    std::uint64_t globalRAMBytes = UINT64_C(34359738368);
    std::string serialOutputPath;
};

struct QemuExitStatus {
    bool exitedNormally;
    int exitCode;
    int signalNumber;

    bool success() const;
    std::string describe() const;
};

class QemuProcess final
{
  public:
    QemuProcess();
    ~QemuProcess();

    QemuProcess(const QemuProcess&) = delete;
    QemuProcess& operator=(const QemuProcess&) = delete;
    QemuProcess(QemuProcess&&) = delete;
    QemuProcess& operator=(QemuProcess&&) = delete;

    void start(const QemuConfiguration& config);
    std::optional<QemuExitStatus> pollExit();
    QemuExitStatus waitForExit();
    void terminate() noexcept;
    static void terminateAll() noexcept;

    bool running() const noexcept { return pid_ > 0; }
    pid_t pid() const noexcept { return pid_; }

  private:
    static QemuExitStatus decodeWaitStatus(int status);

    pid_t pid_ = -1;
};

} // namespace Mittens
} // namespace SST

#endif
