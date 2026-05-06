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
  "333.png:333"
  "888.png:888"
  "888_destroy1.png:888"
  "888_destroy2.png:888"
  "888_destroy3.png:888"
  "333_destroy1.png:333"
  "333_destroy2.png:333"
  "333v15.png:333"
  "333v20.png:333"
  "333v30.png:333"
  "333v35.png:333"
  "333v38.png:84244086"
  "333v40.png:333"
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
