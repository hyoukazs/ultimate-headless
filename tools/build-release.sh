#!/usr/bin/env bash
set -euo pipefail
cd "$(cd "$(dirname "$0")" && pwd)/.."
build_dir="${UH_BUILD_DIR:-build-release}"
jobs="${UH_BUILD_JOBS:-1}"
case "$jobs" in ''|*[!0-9]*|0) echo 'UH_BUILD_JOBS must be a positive integer' >&2; exit 2;; esac
cmake -S . -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --target ultimate-headless -j "$jobs"
stage=$(mktemp -d)
trap 'rm -rf -- "$stage"' EXIT
install -m 755 "$build_dir/ultimate-headless" "$stage/ultimate-headless"
install -d "$stage/scripts" "$stage/tools" "$stage/resources" "$stage/third_party/otcv8-dev"
for name in runtime powerdown trainer net_reconnect training live_training; do
  install -m 644 "scripts/$name.lua" "$stage/scripts/"
done
install -m 755 tools/run-oracle-training.sh "$stage/tools/"
install -m 755 run.sh "$stage/"
install -m 755 tools/install-deps.sh "$stage/tools/"
install -m 644 resources/login-context.bin resources/metadata.json "$stage/resources/"
install -m 644 README.md "$stage/README.md"
if [ -f LICENSE ]; then install -m 644 LICENSE "$stage/LICENSE"; fi
install -m 644 third_party/otcv8-dev/LICENSE "$stage/third_party/otcv8-dev/LICENSE"
mkdir -p out
archive="out/ultimate-headless-linux-$(uname -m).tar.gz"
chmod 755 "$stage"
tar -czf "$archive" --transform='s,^\.,ultimate-headless,' --owner=0 --group=0 --numeric-owner -C "$stage" .
(cd out && sha256sum "$(basename "$archive")") > "$archive.sha256"
echo "Package: $archive (runtime and compatibility resources included; no credentials)"
