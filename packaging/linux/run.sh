#!/usr/bin/env bash
set -euo pipefail
cm_bundle_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cm_program="${1:-cloudiemail}"
shift || true
case "$cm_program" in
  cloudiemail|cloudiemail-gui|cloudiemail-tests|cloudiemail-setup-tests|cloudiemail-config-tests|cloudiemail-mail-tests|openssl|lego) ;;
  *) printf 'Programa desconhecido.\n' >&2; exit 2 ;;
esac
export FIREBIRD="$cm_bundle_root/runtime/firebird"
export GCONV_PATH="$cm_bundle_root/lib/gconv"
export OPENSSL_MODULES="$cm_bundle_root/lib/ossl-modules"
export OPENSSL_CONF="$cm_bundle_root/share/openssl.cnf"
export SSL_CERT_FILE="$cm_bundle_root/share/certs/ca-bundle.crt"
export CLOUDIEMAIL_CA_BUNDLE="$SSL_CERT_FILE"
export GSETTINGS_SCHEMA_DIR="$cm_bundle_root/share/glib-2.0/schemas"
export GIO_MODULE_DIR="$cm_bundle_root/lib/gio/modules"
export XDG_DATA_DIRS="$cm_bundle_root/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
export FONTCONFIG_FILE="$cm_bundle_root/share/fonts.conf"
export GSK_RENDERER="${GSK_RENDERER:-cairo}"
# Apply the bundled loader only to this process; host browsers/tools keep their own libc.
unset LD_LIBRARY_PATH LD_PRELOAD LD_AUDIT
if [[ "$cm_program" == lego ]]; then exec "$cm_bundle_root/bin/lego" "$@"; fi
exec "$cm_bundle_root/lib/ld-linux-x86-64.so.2" \
  --inhibit-cache --library-path "$cm_bundle_root/lib:$FIREBIRD/lib" \
  "$cm_bundle_root/bin/$cm_program" "$@"
