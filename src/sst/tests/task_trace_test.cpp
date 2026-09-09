#include "../profiling/taskTrace.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <cstdlib>

int main()
{
    using SST::Mittens::TaskTrace;
    std::string log;
    TaskTrace traced(3, "", [&](const std::string& text) { log += text; });
    traced.record(4, 5, false, 100, 20, 10);
    assert(traced.context().task == 4 && traced.context().execution == 5);
    traced.record(4, 5, true, 200, 50, 30);
    assert(!traced.context().task && traced.context().execution == 0);
    assert(log == "MITTENS_TASK_TRACE sim_time_ticks=100 event=start tile=3 task=4 execution=5\n"
                  "MITTENS_TASK_TRACE sim_time_ticks=200 event=finish tile=3 task=4 execution=5\n");
    bool rejected = false;
    try { traced.record(UINT32_MAX, 0, false, 0, 0, 0); }
    catch (const std::runtime_error&) { rejected = true; }
    assert(rejected && !traced.context().task);

    char pattern[] = "/tmp/mittens-task-trace-XXXXXX";
    const char* directory = mkdtemp(pattern);
    assert(directory);
    TaskTrace csv(3, directory, {});
    csv.record(4, 5, false, 100, 20, 10);
    csv.record(4, 5, true, 200, 50, 30);
    std::ifstream input(std::filesystem::path(directory) / "tile-3.csv");
    std::string data{std::istreambuf_iterator<char>(input), {}};
    assert(data == "sim_time_ticks,event,tile_id,task_id,execution_id,retired_instructions,cpu_cycles\n"
                   "100,start,3,4,5,20,10\n200,finish,3,4,5,50,30\n");
}
