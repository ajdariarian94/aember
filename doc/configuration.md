# Configuration reference

A board's configuration is its rootfs overlay, `resources/<arch>/rootfs/`.
`aember app pack` copies it into the root filesystem image, so on the device
the files below are in `/etc/aember/`. Every file is JSON; an unknown key is
ignored, a key with the wrong type is an error that names the file and key
(in `/var/log/aember-init.log` or the service's log).

| File | Read by | What it lists |
|------|---------|---------------|
| [`services.json`](#servicesjson) | PID 1 (service-manager) | the services PID 1 starts and supervises |
| [`containers.json`](#containersjson) | aember-containerd | LXC containers and their bridges |
| [`network.json`](#networkjson) | aember-netd | interfaces, DNS, connectivity check |
| [`modules.json`](#modulesjson) | PID 1 (module-loader) | kernel modules to load at boot |
| [`packages.json`](#packagesjson) | `aember app pack` (build time) | opt-in programs from aember-resources |

Only what these files list goes into the image: a service's executable, a
container's SquashFS image and a package's files are packed only while an
entry names them.

## services.json

```json
{
  "processes": [
    {
      "name": "aember-netd",
      "executable": "/usr/bin/aember-netd",
      "ipc": true,
      "ready_timeout_ms": 120000,
      "stop_timeout_ms": 10000
    }
  ]
}
```

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `name` | string | required | unique name; used by `dependencies` and in logs |
| `executable` | string | required | absolute path in the rootfs |
| `description` | string | `""` | shown in the status file |
| `args` | array of strings | `[]` | arguments after the executable |
| `working_directory` | string | inherited (`/`) | |
| `environment` | object of strings | `{}` | added to PID 1's environment |
| `dependencies` | array of names | `[]` | start after these are running (or ready, with `ipc`) |
| `restart_on_failure` | bool | `true` | restart after a non-zero exit or a signal |
| `max_restarts` | unsigned | `0` | restarts before giving up; `0` = no limit |
| `restart_delay_ms` | unsigned | `1000` | wait between an exit and the restart |
| `stop_timeout_ms` | unsigned | `5000` | SIGTERM to SIGKILL at shutdown |
| `ipc` | bool | `false` | a message channel to PID 1 on fd 3 (see [services/README.md](../services/README.md)) |
| `ready_timeout_ms` | unsigned | `30000` | with `ipc`: how long to wait for `{"type": "ready"}` |

## containers.json

```json
{
  "bridges": [{ "name": "aember-br0", "address": "10.10.0.1/24" }],
  "containers": [
    {
      "name": "aember-stats",
      "container": {
        "squashfs": "/var/lib/aember/aember-stats/aember-stats.squashfs",
        "rootfs": "/var/lib/aember/containers/aember-stats",
        "config_path": "/var/lib/aember/aember-stats/lxc.container.conf"
      },
      "restart_policy": "on-failure"
    }
  ]
}
```

aember-build-helper writes the container entries
(`aember-cli/run.py install`); edit `restart_policy` and the timeouts by
hand if the defaults don't fit.

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `name` | string | required | the LXC container name |
| `container.squashfs` | string | required | the read-only image |
| `container.rootfs` | string | required | where containerd mounts image + writable layer |
| `container.config_path` | string | required | the LXC config |
| `container.args` | array of strings | `[]` | arguments for the container's init |
| `init` | bool | `true` | run the app under aember's init wrapper (reaps zombies, forwards signals) |
| `stop_timeout` | seconds | `10` | how long a stop waits before killing |
| `restart_policy` | `"no"`, `"on-failure"`, `"always"` | `"no"` | what happens when the container stops by itself |
| `max_restart_attempts` | unsigned | `0` | restarts in a row before giving up; `0` = no limit (a run of 60 s resets the count) |
| `restart_delay` | seconds | `5` | wait before a restart |

`bridges` (optional): Linux bridges aember-containerd creates before the
containers start, each `name` and optional `address` (CIDR). A container on
a bridge gets a veth pair (`"network": "<bridge>"` in its
aember-container.json); aember does no NAT.

aember-containerd also answers on `/run/aember-containerd/control.sock`
(SOCK_SEQPACKET, one JSON object per message): `{"op": "list"}`, and
`{"op": "stop"|"start"|"restart", "name": "<container>"}`. aember-fleet's
agent uses it.

## network.json

```json
{
  "ping_target": "8.8.8.8",
  "interfaces": [
    { "name": "eth0", "mode": "dhcp", "required": true, "dns": ["8.8.8.8"] },
    { "name": "eth1", "mode": "static", "address": "192.168.1.10/24", "gateway": "192.168.1.1" }
  ]
}
```

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `ping_target` | IPv4 | `"8.8.8.8"` | connectivity check (TCP probe) |
| `ping_interval_ms` | number | `10000` | how often |
| `ping_retries` | number | `3` | failures before "offline" |
| `dhcp_timeout_ms` | number | `30000` | per DHCP attempt |
| `dhcp_retries` | number | `3` | attempts before the interface counts as failed |
| `interfaces[].name` | string | required | e.g. `eth0`, `wlan0` |
| `interfaces[].mode` | `"dhcp"`, `"static"` | `"dhcp"` | |
| `interfaces[].address` / `gateway` | CIDR / IPv4 | | static mode |
| `interfaces[].dns` | array of IPv4 | `[]` | written to `/etc/resolv.conf` |
| `interfaces[].required` | bool | `false` | if it fails to come up, aember-netd exits (PID 1 restarts it); other interfaces are retried later |

aember-netd waits for an interface that isn't there yet (a USB adapter, a
Wi-Fi link that wpa_supplicant brings up) and configures it once its link
is usable. With ConnMan from the packages, drop aember-netd from
services.json instead ([packages.md](packages.md)).

## modules.json

```json
{ "modules": ["bridge", "veth", "overlay"] }
```

Kernel modules PID 1 loads in order, with their dependencies, from
`/lib/modules/<kernel>/`; pack copies only these (and what they depend on)
into the image.

## packages.json

```json
{ "packages": ["dropbear", "e2fsprogs"] }
```

Prebuilt programs from aember-resources: `dropbear`, `e2fsprogs`, `wifi`,
`wifi-ap`, `connman`. `aember fetch-resources` downloads only the listed
ones, and pack puts their files into the rootfs. Each needs its own
services.json entry. Setup and tested configurations:
[packages.md](packages.md).
