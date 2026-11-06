#!/usr/bin/env bash
set -euo pipefail
cm_bundle_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
if [[ $# == 0 ]]; then
  set -- "$cm_bundle_root/config/local.json"
fi
exec "$cm_bundle_root/run.sh" cloudiemail-gui "$@"
