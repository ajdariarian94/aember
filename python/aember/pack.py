import os
import subprocess

def pack():
    current_dir = f"{os.getcwd()}"

    subprocess.run(["mkdir", "-p", f"{current_dir}/build/package/initramfs"])
    subprocess.run(["cp", f"{current_dir}/resources/kernel/vmlinuz", f"{current_dir}/build/package"])
    
    subprocess.run(["cp", "-a", os.path.join(f"{os.getcwd()}/resources/initramfs"), f"{current_dir}/build/package"])

    subprocess.run(["cp", f"{current_dir}/build/install/bin/aember", f"{current_dir}/build/package/initramfs/usr/bin/aember"])

    subprocess.run(["find . | cpio -H newc -o > ../initramfs.img"], cwd=f"{current_dir}/build/package/initramfs", shell=True, check=True)
    subprocess.run(["gzip -9 < ../initramfs.img > ../initramfs.img.gz"], cwd=f"{current_dir}/build/package/initramfs", shell=True, check=True)