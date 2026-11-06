#!/usr/bin/env bash
set -euo pipefail
cm_project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$cm_project_root"
mkdir -p .deps
fetch() {
  local url="$1" destination="$2" checksum="$3"
  if [[ ! -f "$destination" ]]; then
    curl --fail --location --proto '=https' --tlsv1.2 "$url" --output "$destination.tmp"
    mv "$destination.tmp" "$destination"
  fi
  printf '%s  %s\n' "$checksum" "$destination" | sha256sum --check --status
}
fetch 'https://github.com/FirebirdSQL/firebird/releases/download/v5.0.4/Firebird-5.0.4.1812-0-linux-x64.tar.gz' \
  .deps/firebird.tar.gz ab6a15a0258f38b022be496bb5e038c14e8628ce9acd0f9a06288a3baedd917b
fetch 'https://github.com/libtom/libtommath/archive/refs/tags/v1.3.0.tar.gz' \
  .deps/tommath.tar.gz 6d099e93ff00fa9b18346f4bcd97dcc48c3e91286f7e16c4ac5515a7171c3149
if [[ ! -f .deps/firebird/opt/firebird/lib/libfbclient.so ]]; then
  tar -xzf .deps/firebird.tar.gz -C .deps
  mkdir -p .deps/firebird
  tar -xzf .deps/Firebird-5.0.4.1812-0-linux-x64/buildroot.tar.gz -C .deps/firebird
fi
if [[ ! -f .deps/libtommath-1.3.0/.libs/libtommath.so.1 ]]; then
  tar -xzf .deps/tommath.tar.gz -C .deps
  make -C .deps/libtommath-1.3.0 -f makefile.shared -j"${CLOUDIEMAIL_BUILD_JOBS:-4}"
fi
printf 'Dependências locais prontas. Execute: source scripts/local-env.sh\n'
