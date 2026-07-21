#include <atomic>
#include <omp.h>
#include <sched.h>
#include <stdio.h>

int main() {
  std::atomic<bool> dispatched{false};
  omp_set_dynamic(0);
  omp_set_num_threads(5);
  printf("[dispatch] OpenMP sees %d processors; max threads %d; thread limit %d\n",
         omp_get_num_procs(), omp_get_max_threads(), omp_get_thread_limit());
#pragma omp parallel num_threads(5) shared(dispatched)
  {
    const int worker = omp_get_thread_num();
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(worker, &cpus);
    sched_setaffinity(0, sizeof(cpus), &cpus);
    if (worker == 0) {
      puts("[dispatch] core 0 dispatching four print jobs");
      dispatched.store(true, std::memory_order_release);
    } else {
      while (!dispatched.load(std::memory_order_acquire)) {}
      printf("[worker] job executed on core %d (OpenMP thread %d)\n", sched_getcpu(), worker);
    }
#pragma omp barrier
    if (worker == 0) puts("[dispatch] done");
  }
}
