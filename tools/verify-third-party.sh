#!/usr/bin/env bash
# Compara third_party/otcv8-dev com o OTCv8 original no commit fixado.
# Sai com erro se algum arquivo divergir fora da lista de patches documentados.
# Uso: tools/verify-third-party.sh [--show-diff]
set -euo pipefail
cd "$(cd "$(dirname "$0")" && pwd)/.."
upstream_url=https://github.com/OTCv8/otcv8-dev.git
upstream_commit=3d32139512cc4576b105682c3579f18fe0d534e4
patched=(
  src/framework/core/eventdispatcher.cpp
  src/framework/core/eventdispatcher.h
  src/framework/core/logger.cpp
  src/framework/core/resourcemanager.cpp
  src/framework/stdext/time.cpp
)
show_diff=0
case "${1:-}" in '') ;; --show-diff) show_diff=1 ;; *) echo 'Uso: tools/verify-third-party.sh [--show-diff]' >&2; exit 2 ;; esac

work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
git -C "$work" init -q
git -C "$work" fetch -q --depth 1 "$upstream_url" "$upstream_commit"
git -C "$work" checkout -q FETCH_HEAD

identical=0 failed=0
while IFS= read -r -d '' file; do
  rel=${file#third_party/otcv8-dev/}
  expected=0
  for p in "${patched[@]}"; do [ "$p" = "$rel" ] && expected=1; done
  if [ ! -f "$work/$rel" ]; then
    echo "NAO EXISTE NO UPSTREAM: $rel"; failed=1
  elif cmp -s "$file" "$work/$rel"; then
    if [ "$expected" = 1 ]; then echo "PATCH LISTADO MAS ARQUIVO IDENTICO: $rel"; failed=1; else identical=$((identical+1)); fi
  elif [ "$expected" = 1 ]; then
    echo "patch documentado: $rel"
    if [ "$show_diff" = 1 ]; then diff -u "$work/$rel" "$file" --label "upstream/$rel" --label "local/$rel" || true; fi
  else
    echo "DIVERGENTE SEM DOCUMENTACAO: $rel"; failed=1
  fi
done < <(git ls-files -z third_party/otcv8-dev)

echo "identicos ao upstream $upstream_commit: $identical; patches documentados: ${#patched[@]}"
if [ "$failed" != 0 ]; then echo 'FALHA: third_party difere do esperado.' >&2; exit 1; fi
echo 'OK: third_party corresponde ao upstream + patches documentados.'
