#!/usr/bin/env bash
set -euo pipefail
cd "$(cd "$(dirname "$0")" && pwd)"
if [ "$#" -ne 0 ]; then echo 'Use ./run.sh without arguments; select your character after login.' >&2; exit 2; fi
if [ ! -x ./ultimate-headless ]; then
  echo 'Binary missing. Run ./build.sh or extract the binary release.' >&2
  exit 2
fi
bash tools/install-deps.sh runtime
exec bash tools/run-oracle-training.sh
