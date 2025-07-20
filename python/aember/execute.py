import os
import sys
import subprocess

def execute():
    current_dir = os.getcwd()

    subprocess.run([
        "qemu-system-x86_64",
        "-kernel", f"{current_dir}/build/package/vmlinuz",
        "-initrd", f"{current_dir}/build/package/initramfs.img.gz",
        "-nographic",
        "-append", "console=ttyS0",
        "-m", "512M",
        "-cpu", "max",
    ],
    stdin=sys.stdin,
    stdout=sys.stdout,
    stderr=sys.stderr)

    #subprocess.run([
    #    "qemu-system-x86_64",
    #    "-kernel", f"{current_dir}/build/package/vmlinuz",             # path to your kernel
    #    "-initrd", f"{current_dir}/build/package/initramfs.img.gz",    # path to your initramfs
    #    "-nographic",                                                  # disable graphical output
    #    "-append", "console=ttyS0 console=tty0",                       # send boot+shell to serial and VGA
    #    "-m", "512M",                                                  # optional: give QEMU 512MB RAM
    #    "-cpu", "max",                                                 # optional: enable all CPU features
    #], check=True)