/**
 * @file signal.cpp
 * @author Arian Ajdari
 * @brief Library implementation for signal handling in Aember
 * @version 0.1
 * @date 2025-12-22
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/utils/shell/debug-shell.h>

#include <fcntl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace aember::utils::shell {

DebugShell::DebugShell() : log_("debug-shell") {}

bool DebugShell::CheckDebugShell() {
  log_.info("Press 'd' within 5 seconds for debug shell");

  int console_fd = open("/dev/console", O_RDWR);
  if (console_fd < 0) {
    log_.error("Failed to open /dev/console");
    return false;
  }

  struct termios oldt, newt;
  tcgetattr(console_fd, &oldt);
  newt = oldt;
  newt.c_lflag &= ~(ICANON | ECHO);
  tcsetattr(console_fd, TCSANOW, &newt);

  fd_set set;
  struct timeval timeout;
  FD_ZERO(&set);
  FD_SET(console_fd, &set);
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;

  int rv = select(console_fd + 1, &set, nullptr, nullptr, &timeout);

  bool debug_requested = false;
  if (rv > 0) {
    char c;
    if (read(console_fd, &c, 1) > 0 && (c == 'd' || c == 'D')) {
      log_.warn("Debug shell requested");
      debug_requested = true;
    }
  }

  tcsetattr(console_fd, TCSANOW, &oldt);
  close(console_fd);
  return debug_requested;
}

void DebugShell::SpawnDebugShell() {
  int console_fd = open("/dev/console", O_RDWR);
  if (console_fd < 0) {
    log_.error("Failed to open /dev/console for debug shell");
    return;
  }

  pid_t pid = fork();
  if (pid == 0) {
    setsid();
    dup2(console_fd, STDIN_FILENO);
    dup2(console_fd, STDOUT_FILENO);
    dup2(console_fd, STDERR_FILENO);
    if (console_fd > 2) close(console_fd);

    execl("/bin/sh", "sh", nullptr);
    _exit(1);
  } else if (pid > 0) {
    close(console_fd);

    int status;
    waitpid(pid, &status, 0);
    log_.info("Debug shell exited");
  } else {
    log_.error("fork() failed: {}", strerror(errno));
    close(console_fd);
  }
}

void DebugShell::SilenceAemberInBackground() {
  int null_fd = open("/dev/null", O_WRONLY);
  if (null_fd >= 0) {
    dup2(null_fd, STDOUT_FILENO);
    dup2(null_fd, STDERR_FILENO);
    close(null_fd);
  }
}

}  // namespace aember::utils::shell
