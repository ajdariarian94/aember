# aember services

A **service** is a separate program that aember (PID 1) starts, supervises and
restarts, because it is listed in the board's `/etc/aember/services.json`.
aember's own network and container handling are services too:

| Directory     | Program             | What it does                                         |
|---------------|---------------------|------------------------------------------------------|
| `netd/`       | `aember-netd`       | Interfaces (DHCP/static), DNS, connectivity status   |
| `containerd/` | `aember-containerd` | LXC containers from `containers.json`, their bridges |
| `child-demo/` | `aember-child-demo` | Demo: logs every 10 s, exits cleanly on SIGTERM (the smallest service to start from) |

A board that doesn't list a service doesn't get it: `aember app pack` only
puts the services named in `services.json` into the image. A device can
run ConnMan instead of `aember-netd`, for example, and then carries no
netd code at all.

## Adding a service

Every directory here with a `CMakeLists.txt` is built automatically (the
`services` ExternalProject in the top-level `CMakeLists.txt`). A minimal
service:

```
services/hello/
  CMakeLists.txt
  src/main.cpp
```

```cmake
# services/hello/CMakeLists.txt
find_package(aember-libs REQUIRED COMPONENTS utils)

aember_service(
  aember-hello
  MAIN src/main.cpp
  LIBRARIES aember-libs::utils::ipc aember-libs::utils::logging
)
```

`aember_service(<name> MAIN <main.cpp> [SOURCES ...] [LIBRARIES ...]
[PRIVATE_INCLUDES ...])` (`cmake/AemberService.cmake`) builds the
executable, strips it in Release and installs it to
`build/install/<arch>/<type>/services/<name>`. With `SOURCES`, everything
except `main.cpp` becomes a static library `<name>-core` (with `include/`
as its public include directory) that your tests link against. Put the
headers under `include/aember-services/<dir>/` and the code in a namespace
like `aember::<dir>`.

Tests: add `tests/CMakeLists.txt` inside `if (BUILD_TESTS)`, name the
binaries `<something>-unittest` and install them to
`${CMAKE_INSTALL_BINDIR}/tests`; `aember test` then runs them (see
`netd/tests/`).

Then list it in `resources/<arch>/rootfs/etc/aember/services.json`:

```json
{
  "name": "aember-hello",
  "executable": "/usr/bin/aember-hello",
  "ipc": true,
  "ready_timeout_ms": 10000,
  "dependencies": ["aember-netd"],
  "restart_on_failure": true,
  "max_restarts": 0,
  "restart_delay_ms": 2000,
  "stop_timeout_ms": 5000
}
```

`max_restarts` 0 means "always restart". The executable must be
`/usr/bin/<program name>` for the packer to recognise it as one of these
services.

## Talking to PID 1 (optional, `"ipc": true`)

With `"ipc": true` the service inherits a channel to PID 1 as **fd 3**
(`aember-libs::utils::ipc`: a SOCK_SEQPACKET socket, one JSON object per
message):

```cpp
#include <aember-libs/utils/ipc/channel.h>
#include <aember-libs/utils/logging/logging.h>

namespace ipc = aember::utils::ipc;

int main() {
  aember::utils::logging::Logger log{"hello"};
  auto channel = ipc::Channel::Inherited();  // invalid when run by hand
  ipc::ReceiveControl(channel);  // e.g. muted while a debug shell is open
  aember::utils::logging::enable_file_logging("/var/log/aember-hello.log");

  // ... set up ...
  channel.Send(ipc::Ready());  // PID 1 marks the service running
  log.info("hello ready");

  // ... main loop: poll(channel.Fd()) and call ipc::ReceiveControl(channel)
  // when it is readable; exit cleanly on SIGTERM ...
}
```

- **Readiness:** with `ipc`, the service stays "starting" (and services
  that depend on it wait) until it sends `{"type":"ready"}` or
  `ready_timeout_ms` runs out. Without `ipc` it counts as running once
  started.
- **Events:** `ipc::Event("network", {...})` and `ipc::Event("container",
  {...})` feed PID 1's heartbeat (see netd/containerd for the fields).
- **Control from PID 1:** `{"type":"console","enabled":bool}` mutes the
  console log while a debug shell owns the console; `ipc::ReceiveControl`
  applies it. Call it right after `Inherited()` and whenever fd 3 is
  readable.
- **Stopping:** PID 1 sends SIGTERM and waits `stop_timeout_ms`, then
  SIGKILL. Services stop in reverse dependency order.

## Libraries

The packer copies what a service's `NEEDED` entries resolve to, searching
the SDK target sysroot and the LXC libraries from aember-resources. A
library that isn't there has to be linked statically (vcpkg) or come from
the rootfs overlay. Libraries loaded with `dlopen` aren't visible to the
packer: list them in `SERVICE_DLOPEN_LIBS` in
`aember-cli/aember/config/libs.py` (as `aember-containerd` does for
liblxc).

Services run from the real root filesystem, never from the initramfs, so
they don't take initramfs RAM.
