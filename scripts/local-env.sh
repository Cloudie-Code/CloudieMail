#!/usr/bin/env bash
# Source this file from any directory. No services or global packages are changed.
cm_project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
export FIREBIRD="$cm_project_root/.deps/firebird/opt/firebird"
export LD_LIBRARY_PATH="$FIREBIRD/lib:$cm_project_root/.deps/libtommath-1.3.0/.libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
unset cm_project_root
