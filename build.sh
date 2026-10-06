#!/usr/bin/env bash
set -euo pipefail
cd "$(cd "$(dirname "$0")" && pwd)"
if [ "$#" -ne 0 ]; then echo 'Use ./build.sh without arguments.' >&2; exit 2; fi
bash tools/install-deps.sh build
bash tools/build-release.sh
install -m 755 "${UH_BUILD_DIR:-build-release}/ultimate-headless" ./ultimate-headless
echo 'Build ready. Start with ./run.sh or upload the package in out/ to your VM.'
