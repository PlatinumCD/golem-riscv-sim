#include "sst_config.h"

#include "qemuProcess.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <spawn.h>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace SST {
namespace Mittens {

namespace {

constexpr auto kTerminateGracePeriod = std::chrono::seconds(1);
constexpr auto kTerminatePollInterval = std::chrono::milliseconds(10);
constexpr int kChildSyncBridgeFileDescriptor = 41;
constexpr int kChildBridgeFileDescriptor = 42;
constexpr int kChildAnalogBridgeFileDescriptor = 43;
constexpr int kFirstStagingFileDescriptor = 64;

struct BridgeFileDescriptorMapping {
    int source;
    int target;
    int staging;
};

std::vector<std::string> buildArguments(const QemuConfiguration& config)
{
    std::vector<std::string> arguments = {
        config.executable,
        "-machine",
        "virt",
        "-smp",
        "1",
        "-m",
        config.memory,
        "-bios",
        "none",
        "-kernel",
        config.elfPath,
        "-display",
        "none",
        "-monitor",
        "none",
        "-serial",
        "stdio",
        "-no-reboot",
    };

    if (config.syncBridgeFileDescriptor >= 0) {
        arguments.push_back("-icount");
        arguments.push_back("shift=0,sleep=off");
        arguments.push_back("-global");
        arguments.push_back(
            "mittens-sync.bridge-fd=" +
            std::to_string(kChildSyncBridgeFileDescriptor));
    }
    if (config.bridgeFileDescriptor >= 0) {
        arguments.push_back("-global");
        arguments.push_back(
            "mittens-nic.bridge-fd=" +
            std::to_string(kChildBridgeFileDescriptor));
    }
    if (config.analogBridgeFileDescriptor >= 0) {
        arguments.push_back("-global");
        arguments.push_back(
            "mittens-analog.bridge-fd=" +
            std::to_string(kChildAnalogBridgeFileDescriptor));
    }

    return arguments;
}

std::vector<char*> makeArgumentPointers(std::vector<std::string>& arguments)
{
    std::vector<char*> pointers;
    pointers.reserve(arguments.size() + 1);

    for (std::string& argument : arguments) {
        pointers.push_back(argument.data());
    }
    pointers.push_back(nullptr);
    return pointers;
}

} // namespace

bool QemuExitStatus::success() const
{
    return exitedNormally && exitCode == 0;
}

std::string QemuExitStatus::describe() const
{
    if (exitedNormally) {
        return "exit status " + std::to_string(exitCode);
    }
    return "signal " + std::to_string(signalNumber) + " (" +
           std::string(::strsignal(signalNumber)) + ")";
}

QemuProcess::~QemuProcess()
{
    terminate();
}

void QemuProcess::start(const QemuConfiguration& config)
{
    if (running()) {
        throw std::logic_error("QEMU process is already running");
    }

    if (config.executable.empty()) {
        throw std::invalid_argument("QEMU executable path is empty");
    }

    if (config.elfPath.empty()) {
        throw std::invalid_argument("QEMU ELF path is empty");
    }

    const std::filesystem::path elf(config.elfPath);
    if (!std::filesystem::is_regular_file(elf)) {
        throw std::invalid_argument("QEMU ELF is not a regular file: " + config.elfPath);
    }

    std::vector<std::string> arguments = buildArguments(config);
    std::vector<char*> argumentPointers = makeArgumentPointers(arguments);

    posix_spawn_file_actions_t fileActions;
    posix_spawn_file_actions_t* fileActionsPointer = nullptr;
    std::vector<BridgeFileDescriptorMapping> bridgeMappings;

    const auto addBridgeMapping =
        [&bridgeMappings](int source, int target) {
            if (source >= 0) {
                bridgeMappings.push_back({source, target, -1});
            }
        };
    addBridgeMapping(
        config.syncBridgeFileDescriptor,
        kChildSyncBridgeFileDescriptor);
    addBridgeMapping(
        config.bridgeFileDescriptor,
        kChildBridgeFileDescriptor);
    addBridgeMapping(
        config.analogBridgeFileDescriptor,
        kChildAnalogBridgeFileDescriptor);

    const auto closeStagingFileDescriptors =
        [&bridgeMappings]() noexcept {
            for (const BridgeFileDescriptorMapping& mapping :
                 bridgeMappings) {
                if (mapping.staging >= 0) {
                    (void)::close(mapping.staging);
                }
            }
        };

    if (!bridgeMappings.empty()) {
        for (BridgeFileDescriptorMapping& mapping : bridgeMappings) {
            mapping.staging = ::fcntl(
                mapping.source,
                F_DUPFD_CLOEXEC,
                kFirstStagingFileDescriptor);
            if (mapping.staging < 0) {
                const int error = errno;
                closeStagingFileDescriptors();
                throw std::system_error(
                    error,
                    std::generic_category(),
                    "failed to stage QEMU bridge fd");
            }
        }

        int actionResult = posix_spawn_file_actions_init(&fileActions);
        if (actionResult != 0) {
            closeStagingFileDescriptors();
            throw std::system_error(actionResult,
                                    std::generic_category(),
                                    "failed to initialize QEMU file actions");
        }
        fileActionsPointer = &fileActions;

        for (const BridgeFileDescriptorMapping& mapping :
             bridgeMappings) {
            actionResult = posix_spawn_file_actions_adddup2(
                &fileActions,
                mapping.staging,
                mapping.target);
            if (actionResult == 0) {
                actionResult = posix_spawn_file_actions_addclose(
                    &fileActions,
                    mapping.staging);
            }
            if (actionResult != 0) {
                break;
            }
        }

        if (actionResult != 0) {
            posix_spawn_file_actions_destroy(&fileActions);
            closeStagingFileDescriptors();
            throw std::system_error(actionResult,
                                    std::generic_category(),
                                    "failed to configure QEMU bridge fd");
        }
    }

    pid_t child = -1;
    const int result = posix_spawnp(
        &child,
        config.executable.c_str(),
        fileActionsPointer,
        nullptr,
        argumentPointers.data(),
        environ);

    if (fileActionsPointer != nullptr) {
        posix_spawn_file_actions_destroy(&fileActions);
    }
    closeStagingFileDescriptors();

    if (result != 0) {
        throw std::system_error(result, std::generic_category(),
                                "failed to launch QEMU for tile " +
                                    std::to_string(config.tileId));
    }

    pid_ = child;
}

QemuExitStatus QemuProcess::decodeWaitStatus(int status)
{
    if (WIFEXITED(status)) {
        return {true, WEXITSTATUS(status), 0};
    }

    if (WIFSIGNALED(status)) {
        return {false, 0, WTERMSIG(status)};
    }

    throw std::runtime_error("QEMU returned an unsupported wait status");
}

std::optional<QemuExitStatus> QemuProcess::pollExit()
{
    if (!running()) {
        return std::nullopt;
    }

    int status = 0;
    pid_t result;
    do {
        result = waitpid(pid_, &status, WNOHANG);
    } while (result < 0 && errno == EINTR);

    if (result == 0) {
        return std::nullopt;
    }

    if (result < 0) {
        throw std::system_error(errno, std::generic_category(), "waitpid failed for QEMU");
    }

    pid_ = -1;
    return decodeWaitStatus(status);
}

QemuExitStatus QemuProcess::waitForExit()
{
    int status = 0;
    pid_t result;
    do {
        result = waitpid(pid_, &status, 0);
    } while (result < 0 && errno == EINTR);

    if (result < 0) {
        throw std::system_error(errno, std::generic_category(), "waitpid failed for QEMU");
    }

    pid_ = -1;
    return decodeWaitStatus(status);
}

void QemuProcess::terminate() noexcept
{
    if (!running()) {
        return;
    }

    const pid_t child = pid_;
    if (kill(child, SIGTERM) < 0 && errno != ESRCH) {
        // Cleanup must continue even if SIGTERM could not be delivered.
    }

    const auto deadline = std::chrono::steady_clock::now() + kTerminateGracePeriod;
    while (std::chrono::steady_clock::now() < deadline && running()) {
        try {
            if (pollExit().has_value()) {
                return;
            }
        } catch (...) {
            break;
        }
        std::this_thread::sleep_for(kTerminatePollInterval);
    }

    if (running()) {
        if (kill(child, SIGKILL) < 0 && errno != ESRCH) {
            // waitpid below remains the authoritative cleanup operation.
        }

        try {
            (void)waitForExit();
        } catch (...) {
            pid_ = -1;
        }
    }
}

} // namespace Mittens
} // namespace SST
