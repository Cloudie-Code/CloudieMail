# Dependências e licenças

A licença MIT do CloudieMail cobre o código deste projeto. Bibliotecas, fontes e ferramentas distribuídas junto aos executáveis mantêm suas próprias licenças e avisos de copyright.

Cada pacote inclui:

- `THIRD-PARTY.json`: versões, bibliotecas e dependências identificadas pelo empacotador;
- `licenses/`: textos de licença fornecidos pelos projetos e pacotes de origem;
- `SHA256SUMS`: hashes dos arquivos do pacote.

## Componentes principais

| Componente | Fonte e informações |
| --- | --- |
| Firebird | https://github.com/FirebirdSQL/firebird — IPL/IDPL |
| LibTomMath | https://github.com/libtom/libtommath — Unlicense |
| libsodium | https://github.com/jedisct1/libsodium — ISC |
| OpenSSL | https://github.com/openssl/openssl — Apache-2.0 |
| curl | https://github.com/curl/curl — curl license |
| nlohmann-json | https://github.com/nlohmann/json — MIT |
| Lego | https://github.com/go-acme/lego — MIT |
| GTK4 / GLib | https://gitlab.gnome.org/GNOME/gtk e https://gitlab.gnome.org/GNOME/glib — LGPL |
| Noto CJK | https://github.com/notofonts/noto-cjk — SIL Open Font License |
| DejaVu | https://dejavu-fonts.github.io/ — licença Bitstream Vera e extensões |

Os scripts `bootstrap-*.sh` registram URLs e checksums dos downloads fixados. O pacote Linux também inclui bibliotecas do sistema CachyOS e seus avisos em `licenses/`. As bibliotecas compartilhadas permanecem separadas dos executáveis e podem ser substituídas por versões compatíveis. As receitas e fontes dos pacotes do sistema estão em https://github.com/CachyOS/CachyOS-PKGBUILDS e https://gitlab.archlinux.org/archlinux/packaging/packages.

Ao redistribuir um pacote modificado, preserve os avisos e cumpra os requisitos de cada dependência. A licença MIT do aplicativo não altera as licenças de terceiros.
