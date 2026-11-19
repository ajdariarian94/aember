/**
 * @file init-wrapper.h
 * @author Arian Ajdari
 * @brief The init wrapper containerd runs a container's app under.
 * @version 0.1
 * @date 2026-11-19
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#pragma once

namespace aember::containerd {

/// The host's static BusyBox, bind mounted into the container. The name
/// must start with "busybox": BusyBox picks the applet from argv[0]
/// otherwise ("/.aember-busybox sh" fails with "applet not found").
inline constexpr const char* kInitInContainer = "/busybox.aember-init";

/// The init wrapper (BusyBox sh as the container's init), run as
/// `sh -c kInitScript aember-init <app> <args...>`:
/// - in a container (LXC sets container=lxc) with a console (a character
///   device that opens; never creates a file there), the app's
///   output goes to /dev/console: LXC gives a daemonized init /dev/null
///   as stdout, the console pty is what LXC reads (and keeps in
///   lxc.console.logfile when the container's config sets one);
/// - starts the app and passes SIGTERM/INT/HUP on (a shell with a trap
///   gets them, an app that is the namespace init doesn't);
/// - exits with the app's status.
inline constexpr const char* kInitScript =
    "if [ \"$container\" = lxc ] && [ -c /dev/console ] && "
    "(: >/dev/console) 2>/dev/null; then "
    "exec </dev/null >/dev/console 2>&1; fi; "
    "trap 'kill -TERM $pid 2>/dev/null' TERM INT HUP; "
    "\"$@\" & pid=$!; "
    "while :; do wait $pid; rc=$?; kill -0 $pid 2>/dev/null || exit $rc; done";

}  // namespace aember::containerd
