"""
The services a board's services.json lists, and each one's own folder.

services.json says only which services start and what each one needs first:

    {"services": [{"name": "aember-netd"},
                  {"name": "aember-containerd", "dependencies": ["aember-netd"]}]}

A service's settings (executable, ipc, timeouts, restart policy) are its
config.json, at /usr/lib/aember/services/<name>/config.json on the device.
aember's own services keep theirs next to their code
(services/<dir>/config.json); `aember app pack` installs it with the
program. A board (or an opt-in package's service) can bring its own folder
in the overlay (usr/lib/aember/services/<name>/), which wins.
"""

import json
from pathlib import Path

SERVICES_JSON = Path("etc/aember/services.json")
SERVICES_DIR = Path("usr/lib/aember/services")


def service_names(overlay: Path) -> list[dict]:
    """services.json's entries ({"name", "dependencies"}). Raises ValueError
    when it can't be read or is in the format before service folders."""
    path = overlay / SERVICES_JSON
    try:
        data = json.loads(path.read_text())
    except (OSError, ValueError) as e:
        raise ValueError(f"cannot read {path}: {e}") from e
    if isinstance(data, dict) and "processes" in data:
        raise ValueError(
            f"{path} is in the format before service folders: list "
            '{"name", "dependencies"} under "services" (each service\'s '
            "settings are its config.json)"
        )
    entries = data.get("services") if isinstance(data, dict) else None
    if not isinstance(entries, list) or not all(
        isinstance(e, dict) and isinstance(e.get("name"), str) for e in entries
    ):
        raise ValueError(f"{path}: 'services' must be a list of {{\"name\": ...}}")
    return entries


def builtin_configs(root: Path) -> dict[str, Path]:
    """name -> services/<dir>/config.json of aember's own services."""
    configs = {}
    for path in sorted((root / "services").glob("*/config.json")):
        try:
            name = json.loads(path.read_text()).get("name")
        except (OSError, ValueError):
            continue
        if isinstance(name, str):
            configs[name] = path
    return configs


def service_config(name: str, overlay: Path, root: Path) -> tuple[dict, Path] | None:
    """The config.json of @p name and where it comes from: the overlay's
    folder, else aember's own service; None if neither has it."""
    own = overlay / SERVICES_DIR / name / "config.json"
    path = own if own.is_file() else builtin_configs(root).get(name)
    if path is None:
        return None
    config = json.loads(path.read_text())
    config.setdefault("name", name)
    return config, path
