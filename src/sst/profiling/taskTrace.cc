#include "taskTrace.h"

#include <sstream>
#include <stdexcept>
#include <utility>

namespace SST::Mittens
{

TaskTrace::TaskTrace(std::uint32_t tile, std::string directory, Log log)
    : tile_(tile), directory_(std::move(directory)), log_(std::move(log))
{
}

void TaskTrace::open()
{
    if (stream_.is_open())
        return;
    const auto path = directory_ + "/tile-" + std::to_string(tile_) + ".csv";
    stream_.open(path, std::ios::out | std::ios::trunc);
    if (!stream_)
    {
        throw std::runtime_error("tile " + std::to_string(tile_) +
                                 " could not open task trace file " + path);
    }
    stream_ << "sim_time_ticks,event,tile_id,task_id,execution_id,"
               "retired_instructions,cpu_cycles\n";
}

void TaskTrace::record(std::uint32_t task, std::uint64_t execution, bool finish, std::uint64_t tick,
                       std::uint64_t instructions, std::uint64_t guestIssueCycles)
{
    if (task == UINT32_MAX)
    {
        throw std::runtime_error("tile " + std::to_string(tile_) +
                                 " received a task trace event without a task ID");
    }
    if (!finish)
        context_ = {task, execution};
    const char* event = finish ? "finish" : "start";
    if (!directory_.empty())
    {
        open();
        stream_ << tick << ',' << event << ',' << tile_ << ',' << task << ',' << execution << ','
                << instructions << ',' << guestIssueCycles << '\n';
        stream_.flush();
    }
    else if (log_)
    {
        std::ostringstream message;
        message << "MITTENS_TASK_TRACE sim_time_ticks=" << tick << " event=" << event
                << " tile=" << tile_ << " task=" << task << " execution=" << execution << '\n';
        log_(message.str());
    }
    if (finish)
        context_ = {};
}

} // namespace SST::Mittens
