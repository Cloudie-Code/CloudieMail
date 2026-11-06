#!/usr/bin/env bash
set -euo pipefail
umask 077
cm_project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$cm_project_root"
if [[ -e data/tls.key || -e data/tls.crt ]]; then
  printf 'Certificado ou chave já existe; nada foi sobrescrito.\n' >&2
  exit 1
fi
mkdir -p data
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -noenc \
  -keyout data/tls.key -out data/tls.crt -days 7 -subj '/CN=webmail.example.test' \
  -addext 'subjectAltName=DNS:webmail.example.test,DNS:mail.example.test,IP:127.0.0.1'
printf 'Certificado local criado por 7 dias. Não é um certificado público de produção.\n'
