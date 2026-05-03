#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
zxing_bin="${repo_root}/build/zxing"

if [[ ! -x "${zxing_bin}" ]]; then
  echo "Missing executable: ${zxing_bin}" >&2
  echo "Build the project first, for example: cd build && cmake --build ." >&2
  exit 2
fi

samples=(
  "123.png:123"
  "123_new.png:123"
  "123_destroy1.png:123"
  "123_destroy2.png:123"
  "123_destroy3.png:123"
  "123_destroy4.png:123"
  "123_destroy5.png:123"
  "123_destroy6.png:123"
  "123_destroy7.png:123"
  "123_destroy8.png:123"
  "888.png:888"
  "888_destroy1.png:888"
  "888_destroy2.png:888"
)

for item in "${samples[@]}"; do
  sample="${item%%:*}"
  expected="${item##*:}"
  path="${repo_root}/qr_fig/${sample}"
  if [[ ! -f "${path}" ]]; then
    echo "Missing sample: ${path}" >&2
    exit 2
  fi

  decoded="$("${zxing_bin}" --try-harder "${path}" | tr -d '\r')"
  if [[ "${decoded}" != "${expected}" ]]; then
    echo "${sample}: expected ${expected}, got ${decoded:-<empty>}" >&2
    exit 1
  fi
  echo "${sample}: ${decoded}"
done
