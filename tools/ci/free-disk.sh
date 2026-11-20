#!/usr/bin/env bash
# GitHub's runner images ship toolchains aember never uses (.NET, Android,
# Haskell, CodeQL, preloaded Docker images): ~30 GB. The dev image (~10 GB),
# the SDK (~3.6 GB) and a build need that room.
set -euo pipefail
df -h / | tail -1
sudo rm -rf /usr/share/dotnet /usr/local/lib/android /opt/ghc \
  /opt/hostedtoolcache/CodeQL /usr/local/share/boost /usr/share/swift
docker image prune -af >/dev/null 2>&1 || true
df -h / | tail -1
