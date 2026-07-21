#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

int main() {
  const int console = open("/dev/console", O_RDWR);
  if (console >= 0) {
    dup2(console, STDIN_FILENO);
    dup2(console, STDOUT_FILENO);
    dup2(console, STDERR_FILENO);
  }
  puts("[emulation] booted RISC-V MUSL initramfs");
  mkdir("/proc", 0555);
  if (mount("proc", "/proc", "proc", 0, nullptr) != 0)
    perror("[emulation] mount /proc");
  // The minimal guest has no sysfs. The test itself sets thread affinity.
  setenv("KMP_AFFINITY", "disabled", 1);
  const pid_t child = fork();
  if (child == 0) {
    execl("/test", "/test", static_cast<char *>(nullptr));
    _exit(127);
  }
  int status = 0;
  waitpid(child, &status, 0);
  if (WIFEXITED(status)) printf("[emulation] test exited with status %d\n", WEXITSTATUS(status));
  else puts("[emulation] test did not exit normally");
  sync();
  reboot(RB_POWER_OFF);
  pause();
}
