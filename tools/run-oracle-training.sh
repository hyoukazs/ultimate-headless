#!/usr/bin/env bash
set -euo pipefail
cd "$(cd "$(dirname "$0")" && pwd)/.."
if [ "$#" -ne 0 ]; then
  echo 'Use sem argumentos; escolha o personagem depois de autenticar.' >&2
  exit 2
fi
umask 077
mkdir -p run
exec ./ultimate-headless --login \
  --host cliente.ntoultimate.com.br --port 7173 --upstream-860 \
  --official-context "$PWD/resources/login-context.bin" --local-platform-context \
  --metadata "$PWD/resources/metadata.json" \
  --outfit-extra-bytes 6 --nto-item-info \
  --training-modules "$PWD/scripts" --stay-online --daemon --reconnect \
  --profile-dir "$PWD/run"
