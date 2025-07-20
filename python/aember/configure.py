import os
import subprocess

def configure():
    command = (
        f"cmake "
        f"-DCMAKE_BUILD_TYPE=Release "
        f"-DCMAKE_INSTALL_PREFIX=build/install "
        f"-DCMAKE_TOOLCHAIN_FILE={os.getcwd()}/vcpkg/scripts/buildsystems/vcpkg.cmake "
        f"-DCMAKE_EXPORT_NO_PACKAGE_REGISTRY=ON "
        f"-DVCPKG_OVERLAY_PORTS={os.getcwd()}/vcpkg_overlays "
        f"-DVCPKG_TARGET_TRIPLET=x64-linux "
        f"-DVCPKG_HOST_TRIPLET=x64-linux "
        f"-B build/configure/x86_64-gnu -S ."
    )

    subprocess.run(command, shell=True, check=True)