#!/usr/bin/env bash
# Source this file before running the Windows binaries under Wine.
cm_project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
export WINEDEBUG=-all
cm_firebird_win="$(winepath -w "$cm_project_root/.deps/windows/firebird")"
export FIREBIRD="$cm_firebird_win"
export WINEPATH="$cm_firebird_win${WINEPATH:+;$WINEPATH}"
unset cm_project_root cm_firebird_win
