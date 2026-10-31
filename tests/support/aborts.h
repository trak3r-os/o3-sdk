// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A check that aborts (the Renderer's raw-section check, the Canvas's, the runtime's asset-table check: debug and host builds) is tested in a
// child process: aborts(fn) is true when running `fn` there ends in SIGABRT, and false when it returns. The child writes no core file.

#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace o3sdk::test {

template <typename Fn>
bool aborts(Fn fn) {
  const pid_t pid = fork();
  if (pid == 0) {
    const rlimit no_core{0, 0};
    (void)setrlimit(RLIMIT_CORE, &no_core);
    (void)signal(SIGABRT, SIG_DFL);  // not the test runner's crash handler, which would report in the child
    fn();
    _exit(0);
  }
  int status = 0;
  (void)waitpid(pid, &status, 0);
  return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

}  // namespace o3sdk::test
