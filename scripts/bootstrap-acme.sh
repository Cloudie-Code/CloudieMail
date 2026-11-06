#!/usr/bin/env bash
set -euo pipefail
cm_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cm_deps="$cm_root/.deps/lego"
mkdir -p "$cm_deps/linux" "$cm_deps/windows"
fetch() {
  local url="$1" target="$2" expected="$3"
  if [[ ! -f "$target" ]]; then curl --fail --location --proto '=https' --tlsv1.2 "$url" --output "$target"; fi
  printf '%s  %s\n' "$expected" "$target" | sha256sum --check --status
}
fetch https://github.com/go-acme/lego/releases/download/v5.6.0/lego_v5.6.0_linux_amd64.tar.gz "$cm_deps/linux.tar.gz" f02fc11b6f56b97e387fa7caa0268f86f5a336953b22458e6430e4651fea439a
fetch https://github.com/go-acme/lego/releases/download/v5.6.0/lego_v5.6.0_windows_amd64.zip "$cm_deps/windows.zip" 9c4e5f77996ef2ebbcb1ea270fa7e94190b531057e02a645b124b38f66c12755
tar -xzf "$cm_deps/linux.tar.gz" -C "$cm_deps/linux"
unzip -oq "$cm_deps/windows.zip" -d "$cm_deps/windows"
