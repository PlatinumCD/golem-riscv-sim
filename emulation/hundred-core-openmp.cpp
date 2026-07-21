#include <omp.h>
#include <sched.h>
#include <stdio.h>

constexpr int kGridWidth = 10;
constexpr int kGridHeight = 10;
constexpr int kCores = kGridWidth * kGridHeight;

int main() {
  omp_set_dynamic(0);
  omp_set_num_threads(kCores);
  printf("[grid] requesting a %dx%d OpenMP grid (%d workers); guest reports %d CPUs\n",
         kGridWidth, kGridHeight, kCores, omp_get_num_procs());

#pragma omp parallel num_threads(kCores)
  {
    const int worker = omp_get_thread_num();
    const int row = worker / kGridWidth;
    const int column = worker % kGridWidth;

    // QEMU virt exposes a flat CPU ID space. This makes the logical mesh
    // mapping explicit: grid position (row, column) owns Linux CPU worker.
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(worker, &cpus);
    const int affinity_result = sched_setaffinity(0, sizeof(cpus), &cpus);

#pragma omp barrier
    if (worker == 0)
      puts("[grid] dispatch core released all 99 worker jobs");
    else
      printf("[grid] worker %02d owns cell (%d,%d), running on CPU %d%s\n",
             worker, row, column, sched_getcpu(),
             affinity_result == 0 ? "" : " (affinity failed)");

#pragma omp barrier
    if (worker == 0)
      puts("[grid] all 100 OpenMP workers reached the final barrier: done");
  }
  return 0;
}
