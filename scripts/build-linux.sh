#!/usr/bin/env bash
set -euo pipefail
cm_project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$cm_project_root"
source scripts/local-env.sh
cm_build_dir=build/linux-debug
cm_build_type=Debug
cm_static_runtime=OFF
case "${1:-}" in
  "") ;;
  --static-runtime)
    cm_build_dir=build/linux-release
    cm_build_type=Release
    cm_static_runtime=ON
    ;;
  *) printf 'Uso: %s [--static-runtime]\n' "$0" >&2; exit 2 ;;
esac
cmake -S . -B "$cm_build_dir" -G Ninja -DCMAKE_BUILD_TYPE="$cm_build_type" \
  -DCLOUDIEMAIL_STATIC_GNU_RUNTIME="$cm_static_runtime" \
  -DCMAKE_EXE_LINKER_FLAGS="-Wl,-rpath-link,$FIREBIRD/lib -Wl,-rpath-link,$PWD/.deps/libtommath-1.3.0/.libs"
cmake --build "$cm_build_dir" -j"${CLOUDIEMAIL_BUILD_JOBS:-4}"
bash scripts/bootstrap-acme.sh
cp .deps/lego/linux/lego "$cm_build_dir/lego"
ctest --test-dir "$cm_build_dir" --output-on-failure
