#!/bin/bash
# Test the aember AppImage on a clean Ubuntu 24.04 VM (QEMU + KVM, in the
# dev container): FUSE as shipped, pkexec's prompt in the AppImage's own
# window, aember isolated, cleanup when the window closes. See
# appimage-vm/guest-test.sh for what is checked.
#
#   tools/tests/appimage-vm-test.sh [build/appimage/aember-x86_64-v<V>.AppImage]
#   AEMBER_VMTEST_MODES=pkexec ...   only one of: sudo pkexec
#
# Prints the AITEST results; screenshots land in build/vmtest/out/.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
APPIMAGE=${1:-$(ls "$ROOT"/build/appimage/aember-x86_64-v*.AppImage | head -1)}
W=$ROOT/build/vmtest
IMG_URL=https://cloud-images.ubuntu.com/releases/noble/release-20261017/ubuntu-24.04-server-cloudimg-amd64.img
IMG_SHA=04e8f2461a16a44e78a370284471f29616cdd9baf1893df25807b71f985b6c94

mkdir -p "$W/out"
BASE=$W/ubuntu-24.04-cloudimg.img
if [ ! -f "$BASE" ] || [ "$(sha256sum "$BASE" | cut -d' ' -f1)" != "$IMG_SHA" ]; then
  echo "Downloading the Ubuntu cloud image (pinned)"
  curl -fL "$IMG_URL" -o "$BASE.part"
  [ "$(sha256sum "$BASE.part" | cut -d' ' -f1)" = "$IMG_SHA" ] || { echo "sha256 mismatch" >&2; exit 1; }
  mv "$BASE.part" "$BASE"
fi

# A fresh disk on top of the pristine image: every run starts clean.
rm -f "$W/disk.qcow2" "$W"/out/*
qemu-img create -q -f qcow2 -b "$BASE" -F qcow2 "$W/disk.qcow2" 12G
cp "$APPIMAGE" "$W/out/aember.AppImage"
cp "$ROOT/tools/tests/appimage-vm/guest-test.sh" "$W/out/guest-test.sh"
echo "${AEMBER_VMTEST_MODES:-sudo pkexec}" >"$W/out/modes"

cat >"$W/user-data" <<'UD'
#cloud-config
users:
  - name: tester
    groups: [sudo]
    shell: /bin/bash
    lock_passwd: false
    plain_text_passwd: aember-test
runcmd:
  - [bash, -c, "mkdir -p /out && mount -t 9p -o trans=virtio,version=9p2000.L out /out && bash /out/guest-test.sh; poweroff"]
UD
cloud-localds "$W/seed.img" "$W/user-data"

timeout 1500 qemu-system-x86_64 -enable-kvm -m 4096 -smp 4 -cpu host \
  -drive file="$W/disk.qcow2",if=virtio -drive file="$W/seed.img",if=virtio,format=raw \
  -virtfs local,path="$W/out",mount_tag=out,security_model=mapped-xattr,id=out \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -display none -serial file:"$W/serial.log" -monitor none || true

grep -hE "AITEST|POLKIT" "$W/out/guest.log" 2>/dev/null || { echo "no results; see $W/serial.log" >&2; exit 1; }
rm -f "$W/disk.qcow2" "$W/out/aember.AppImage"  # big; the base image is kept for reruns
