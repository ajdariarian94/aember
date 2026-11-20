from typing import Dict, List

# ---------------------------------------------------------------------------
# Runtime library resolution per architecture
#
# Paths are derived from the repo root at runtime (see
# steps/packaging/libs.py): the SDK target sysroot and the LXC libs from
# build/aember-resources/<arch>/. The loader's install dir comes from the
# binary's PT_INTERP.
# ---------------------------------------------------------------------------

ARCH_CONFIG = {
    "x64-poky-linux": {
        "loader_name": "ld-linux-x86-64.so.2",
    },
    "aarch64-poky-linux": {
        "loader_name": "ld-linux-aarch64.so.1",
    },
    "arm-poky-linux": {
        "loader_name": "ld-linux-armhf.so.3",
    },
    "riscv64-poky-linux": {
        "loader_name": "ld-linux-riscv64-lp64d.so.1",
    },
    # musl: the loader is libc itself (/lib/ld-musl-x86_64.so.1 -> libc.so)
    "x64-poky-linux-musl": {
        "loader_name": "ld-musl-x86_64.so.1",
    },
}

# Libraries a service loads with dlopen (so not in its NEEDED), packed
# together with that service: aember-containerd loads liblxc.
SERVICE_DLOPEN_LIBS: Dict[str, List[str]] = {
    "aember-containerd": ["liblxc.so.1"],
}

# Modules always packed on all architectures
BASE_MODULES: Dict[str, List[str]] = {
    "x64-poky-linux": ["llc", "stp", "bridge", "veth", "br_netfilter"],
    "x64-poky-linux-musl": ["llc", "stp", "bridge", "veth", "br_netfilter"],
    "aarch64-poky-linux": ["llc", "stp", "ipv6", "bridge", "veth", "br_netfilter"],
    "arm-poky-linux": ["llc", "stp", "ipv6", "bridge", "veth", "br_netfilter"],
    "riscv64-poky-linux": ["llc", "stp", "ipv6", "bridge", "veth", "br_netfilter"],
}

# Debug-only modules
DEBUG_ONLY_MODULES: Dict[str, List[str]] = {
    "x64-poky-linux": ["netconsole"],
    "x64-poky-linux-musl": ["netconsole"],
    "aarch64-poky-linux": [],
    "arm-poky-linux": [],
    "riscv64-poky-linux": [],
}
