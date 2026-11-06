#!/usr/bin/env bash
set -euo pipefail
cm_project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$cm_project_root"
cm_win_deps="$cm_project_root/.deps/windows"
cmake -S . -B build/windows-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="${CLOUDIEMAIL_MSVC_TOOLCHAIN:-/opt/msvc/toolchain.cmake}" \
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded \
  -DCMAKE_CROSSCOMPILING_EMULATOR=/usr/bin/wine \
  -DCLOUDIEMAIL_SODIUM_STATIC=ON \
  -DFIREBIRD_ROOT="$cm_win_deps/firebird" \
  -DSODIUM_INCLUDE_DIR="$cm_win_deps/sodium/include" \
  -DSODIUM_LIBRARY="$cm_win_deps/sodium/x64/Release/v143/static/libsodium.lib" \
  -DOPENSSL_ROOT_DIR="$cm_win_deps/openssl-install" \
  -DOPENSSL_INCLUDE_DIR="$cm_win_deps/openssl-install/include" \
  -DOPENSSL_CRYPTO_LIBRARY="$cm_win_deps/openssl-install/lib/libcrypto.lib" \
  -DOPENSSL_SSL_LIBRARY="$cm_win_deps/openssl-install/lib/libssl.lib" \
  -DOPENSSL_USE_STATIC_LIBS=ON \
  -DCURL_DIR="$cm_win_deps/curl/lib/cmake/CURL" \
  -Dnlohmann_json_DIR="$cm_win_deps/json/share/cmake/nlohmann_json"
cmake --build build/windows-release -j"${CLOUDIEMAIL_BUILD_JOBS:-4}"
bash scripts/bootstrap-acme.sh
cp .deps/lego/windows/lego.exe build/windows-release/lego.exe
source scripts/windows-wine-env.sh
wine ./build/windows-release/cloudiemail-tests.exe --crypto-only
if [[ "${1:-}" == --full-tests ]]; then
  wine ./build/windows-release/cloudiemail-tests.exe
  CLOUDIEMAIL_TEST_WINE=1 python3 tests/integration.py build/windows-release/cloudiemail.exe
else
  printf 'Build e testes criptográficos concluídos. Integração Firebird/Wine pendente; detalhes em docs/WINDOWS-WINE.md.\n'
fi
