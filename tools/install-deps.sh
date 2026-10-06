#!/usr/bin/env bash
set -euo pipefail
case "${1:-runtime}" in
  runtime) packages=(libluajit-5.1-2 libphysfs1 libzip4t64) ;;
  build) packages=(build-essential cmake pkg-config libluajit-5.1-dev libphysfs-dev libzip-dev libssl-dev libboost-system-dev libboost-filesystem-dev zlib1g-dev libbz2-dev) ;;
  *) echo 'Expected runtime or build' >&2; exit 2 ;;
esac
if command -v dpkg-query >/dev/null && dpkg-query -W -f='${Status}\n' "${packages[@]}" 2>/dev/null |
    awk '$0 != "install ok installed" {bad=1} END {exit bad}'; then
  exit 0
fi
if ! command -v apt-get >/dev/null; then
  echo 'Automatic dependency installation requires Ubuntu 24.04/apt-get.' >&2
  exit 2
fi
admin=()
if [ "$(id -u)" -ne 0 ]; then
  if ! command -v sudo >/dev/null; then echo 'Install dependencies as root or with sudo.' >&2; exit 2; fi
  admin=(sudo)
fi
echo "Installing ${1:-runtime} dependencies (may request your sudo password)..."
"${admin[@]}" apt-get update
"${admin[@]}" apt-get install -y "${packages[@]}"
