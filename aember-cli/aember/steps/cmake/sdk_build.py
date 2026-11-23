"""What the CMake steps (configure, build) and pack share: the build and
install trees of a context, and running a command in the aember-resources
SDK's environment."""

import shlex
from pathlib import Path

from aember.config.build import JOBS_ENV, build_jobs
from aember.config.resources import sdk_dir
from aember.config.sdk import SDK_ENV_MAP
from aember.core.context import Context


def build_dir(ctx: Context) -> Path:
    return ctx.root / "build" / "configure" / ctx.architecture / ctx.variant


def install_dir(ctx: Context) -> Path:
    return ctx.root / "build" / "install" / ctx.architecture / ctx.variant


def sdk_env(ctx: Context) -> Path:
    """The SDK's environment-setup script for the context's architecture."""
    env_name = SDK_ENV_MAP.get(ctx.architecture)
    if not env_name:
        raise ValueError(
            f"No SDK environment mapping for architecture: '{ctx.architecture}'. "
            f"Known: {list(SDK_ENV_MAP)}"
        )
    return sdk_dir(ctx.root, ctx.architecture) / env_name


def sdk_shell(ctx: Context, cmd: list[str]) -> str:
    """A shell line running @p cmd in the SDK's environment, without its
    CC/CXX (vcpkg and CMake pick the compilers from the toolchain files) and
    with the job cap (WSL crashes when a build uses every core)."""
    jobs = build_jobs()
    return (
        f"source {shlex.quote(str(sdk_env(ctx)))} && unset CC CXX && "
        f"export {JOBS_ENV}={jobs} VCPKG_MAX_CONCURRENCY={jobs} && "
        f"{shlex.join(cmd)}"
    )
