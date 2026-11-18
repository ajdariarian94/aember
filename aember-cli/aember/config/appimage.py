"""
`aember appimage`: an x86-64 AppImage that runs aember on a Linux desktop,
isolated in its own namespaces (steps/appimage/).

Third-party pieces are pinned by version and sha256.
"""

ARCHITECTURE = "x64-poky-linux"

# AppImage runtime with an automatic fallback (namespaces, then
# extract-and-run) when FUSE is missing. The payload is SquashFS.
RUNTIME_URL = (
    "https://github.com/VHSgunzo/uruntime/releases/download/v0.8.1/"
    "uruntime-appimage-squashfs-x86_64"
)
RUNTIME_SHA256 = "2153e68d63f570c7ebe3d1f38df0fa16814f34d839c5cce2ee42667e327708e8"

# User-mode NAT for the sandbox's network namespace (static binary).
SLIRP4NETNS_URL = (
    "https://github.com/rootless-containers/slirp4netns/releases/download/"
    "v1.3.5/slirp4netns-x86_64"
)
SLIRP4NETNS_SHA256 = "8e54132bc80fc60d53af4b544dae63a81151774b56f129e572f7f1a2e89a57cf"

# The window the launcher opens: xterm from the dev image, with its
# libraries. Every host has these (and they must match the host): not bundled.
TERMINAL = "/usr/bin/xterm"
HOST_LIBS = (
    "linux-vdso.so",
    "ld-linux-x86-64.so",
    "libc.so",
    "libm.so",
    "libdl.so",
    "libpthread.so",
    "librt.so",
    "libresolv.so",
)

# Payload compression: xz mounts on every kernel with SquashFS (WSL's lacks
# zstd), and squashfuse/the extract fallback read it too.
SQUASHFS_COMP = "xz"
