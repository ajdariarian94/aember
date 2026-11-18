"""
Prebuilt low-level components from the aember-resources repo.

aember-resources (github.com/ajdariarian94/aember-resources) builds kernel,
BusyBox, LXC and the Poky SDK and publishes them as assets of the GitHub
release v<RESOURCES_VERSION>, described by its manifest.json. `FetchResources`
downloads them into build/aember-resources/<arch>/<component>/.

Bump RESOURCES_VERSION to move to a newer aember-resources release.
"""

import os
from pathlib import Path

RESOURCES_REPO = "ajdariarian94/aember-resources"
RESOURCES_VERSION = "0.1.5"

# Install root, relative to the repo root. The SDK installer bakes this path
# into the installed SDK, so moving it means re-installing the SDK.
RESOURCES_DIR = "build/aember-resources"

# Components → sub-directory of build/aember-resources/<arch>/
COMPONENT_DIRS: dict[str, str] = {
    "kernel": "kernel",
    "busybox": "busybox",
    "lxc": "virtualization",
    "sdk": "sdk",
}

# Marker inside each component dir: sha256 of the asset it was installed from
ASSET_MARKER = ".asset-sha256"


# Override the download location, e.g. to test an unpublished aember-resources
# release: AEMBER_RESOURCES_URL=file:///path/to/aember-resources/build/release/v0.1.1
RESOURCES_URL_ENV = "AEMBER_RESOURCES_URL"


def release_url(name: str) -> str:
    base = os.environ.get(RESOURCES_URL_ENV) or (
        f"https://github.com/{RESOURCES_REPO}/releases/download/v{RESOURCES_VERSION}"
    )
    return f"{base.rstrip('/')}/{name}"


def component_dir(root: Path, architecture: str, component: str) -> Path:
    try:
        sub = COMPONENT_DIRS[component]
    except KeyError:
        raise ValueError(
            f"Unknown resource component '{component}'. Known: {list(COMPONENT_DIRS)}"
        ) from None
    return root / RESOURCES_DIR / architecture / sub


def kernel_dir(root: Path, architecture: str) -> Path:
    return component_dir(root, architecture, "kernel")


def busybox_dir(root: Path, architecture: str) -> Path:
    return component_dir(root, architecture, "busybox")


def lxc_dir(root: Path, architecture: str) -> Path:
    return component_dir(root, architecture, "lxc")


def sdk_dir(root: Path, architecture: str) -> Path:
    return component_dir(root, architecture, "sdk")
