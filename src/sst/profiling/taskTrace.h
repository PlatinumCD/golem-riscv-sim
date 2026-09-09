#pragma once

#include <cstdint>
#include <fstream>
#include <functional>
#include <optional>
#include <string>

namespace SST::Mittens
{

// Measurement context, not execution state: it never schedules or resumes work.
class TaskTrace final
{
  public:
    struct Context
    {
        std::optional<std::uint32_t> task;
        std::uint64_t execution = 0;
    };
    using Log = std::function<void(const std::string&)>;

    TaskTrace(std::uint32_t tile, std::string directory, Log log);
    void record(std::uint32_t task, std::uint64_t execution, bool finish, std::uint64_t tick,
                std::uint64_t instructions, std::uint64_t guestIssueCycles);
    Context context() const noexcept
    {
        return context_;
    }

  private:
    void open();
    const std::uint32_t tile_;
    const std::string directory_;
    Log log_;
    Context context_;
    std::ofstream stream_;
};

} // namespace SST::Mittens
