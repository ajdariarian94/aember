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
}

# Libraries loaded with dlopen (not in aember's NEEDED), packed explicitly:
# liblxc is only loaded by the `aember containerd` role.
DLOPEN_LIBS: List[str] = ["liblxc.so.1"]

# Modules always packed on all architectures
BASE_MODULES: Dict[str, List[str]] = {
    "x64-poky-linux": ["llc", "stp", "bridge", "veth", "br_netfilter"],
    "aarch64-poky-linux": ["llc", "stp", "ipv6", "bridge", "veth", "br_netfilter"],
    "arm-poky-linux": ["llc", "stp", "ipv6", "bridge", "veth", "br_netfilter"],
}

# Debug-only modules
DEBUG_ONLY_MODULES: Dict[str, List[str]] = {
    "x64-poky-linux": ["netconsole"],
    "aarch64-poky-linux": [],
    "arm-poky-linux": [],
}
