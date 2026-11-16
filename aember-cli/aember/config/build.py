"""
Build parallelism.

Using every core crashes WSL, so builds use CPU_SHARE of the logical CPUs.
AEMBER_BUILD_JOBS=<n> overrides it. The same value caps `cmake --build`,
vcpkg (VCPKG_MAX_CONCURRENCY) and LTO (the toolchain files read
AEMBER_BUILD_JOBS too).
"""

import os

CPU_SHARE = 0.66
JOBS_ENV = "AEMBER_BUILD_JOBS"


def build_jobs() -> int:
    override = os.environ.get(JOBS_ENV)
    if override:
        return max(1, int(override))
    return max(1, int((os.cpu_count() or 1) * CPU_SHARE))
