#ifndef SST_MITTENS_QEMU_PROCESS_H
#define SST_MITTENS_QEMU_PROCESS_H

#include <cstdint>
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
    int bridgeFileDescriptor = -1;
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
    QemuProcess() = default;
    ~QemuProcess();

    QemuProcess(const QemuProcess&) = delete;
    QemuProcess& operator=(const QemuProcess&) = delete;
    QemuProcess(QemuProcess&&) = delete;
    QemuProcess& operator=(QemuProcess&&) = delete;

    void start(const QemuConfiguration& config);
    std::optional<QemuExitStatus> pollExit();
    void terminate() noexcept;

    bool running() const noexcept { return pid_ > 0; }
    pid_t pid() const noexcept { return pid_; }

  private:
    static QemuExitStatus decodeWaitStatus(int status);
    QemuExitStatus waitForExit();

    pid_t pid_ = -1;
};

} // namespace Mittens
} // namespace SST

#endif
