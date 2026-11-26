# Kernel command line shared by every board (QEMU, the Pi, USB images).

# The kernel prints only warnings and worse on the console. On a serial
# console every info line costs boot time (115200 baud: ~11 KB/s), and PID 1
# spent most of its boot CPU on kernel work that includes that output
# (doc/performance.md). For the full kernel log at boot, add loglevel=7
# (`aember app launch --append loglevel=7`, or the board's cmdline); dmesg
# has everything either way.
KERNEL_QUIET = "quiet"
