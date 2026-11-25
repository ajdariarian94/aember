#!/usr/bin/env bash
# Run a command in the aember dev image the way the dev container does, for
# CI (GitHub Actions) or any machine with Docker:
#
#   tools/ci/run.sh aember test --build --root
#
# - The repo is mounted at /aember_ws/aember: the SDK from aember-resources
#   bakes that path in.
# - User dev (uid 1000), /venv on PATH (requirements.txt installed: the
#   published image may be older), `aember` linked like init.sh does.
# - --privileged: the root tests use namespaces, mounts and loop devices.
# - $AEMBER_CI_CACHE (optional) is mounted as dev's ~/.cache (vcpkg's
#   binary cache lives there).
# - $AEMBER_CI_DOCKER=1 (optional) passes the host's Docker socket in, with
#   its group: `aember app pack` builds the rootfs image with guestfish in a
#   `docker run` of this same image (HOST_ROOT_PATH is the repo's host path).
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
image="${AEMBER_DEV_IMAGE:-ghcr.io/ajdariarian94/aember-dev:latest}"

args=(--rm --privileged --net=host
  -v "$repo:/aember_ws/aember" -w /aember_ws/aember
  -e HOST_ROOT_PATH="$repo" -e CI="${CI:-}"
  -e AEMBER_BUILD_JOBS="${AEMBER_BUILD_JOBS:-}"
  -e GITHUB_STEP_SUMMARY=/aember_ws/aember/build/ci-summary.md)
if [ -n "${AEMBER_CI_CACHE:-}" ]; then
  mkdir -p "$AEMBER_CI_CACHE"
  args+=(-v "$AEMBER_CI_CACHE:/home/dev/.cache")
fi

if [ "${AEMBER_CI_DOCKER:-}" = 1 ]; then
  args+=(-v /var/run/docker.sock:/var/run/docker.sock
    --group-add "$(stat -c %g /var/run/docker.sock)")
fi

# printf %q keeps the arguments intact through bash -c.
docker run "${args[@]}" "$image" bash -c "
  set -euo pipefail
  export PATH=/venv/bin:\$PATH
  # The published image can lag behind requirements.txt (e.g. gcovr).
  pip install -q --disable-pip-version-check -r requirements.txt
  ln -sf /aember_ws/aember/aember-cli/run.py /venv/bin/aember
  git config --global --add safe.directory '*'
  $(printf '%q ' "$@")"
