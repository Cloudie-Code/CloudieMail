# Compilação e testes

## Linux

Dependências: C++20, CMake 3.25+, Ninja, GTK4 4.10+, OpenSSL 3, libcurl, libsodium, nlohmann-json, Python 3, curl, make e libtool.

Em Ubuntu 24.04, os pacotes de desenvolvimento correspondentes são:

```bash
sudo apt install build-essential cmake ninja-build pkg-config libgtk-4-dev \
  libssl-dev libcurl4-openssl-dev libsodium-dev nlohmann-json3-dev \
  python3 curl libtool-bin
```

Prepare o Firebird e compile:

```bash
bash scripts/bootstrap-linux.sh
bash scripts/build-linux.sh
```

O bootstrap baixa Firebird 5.0.4 e LibTomMath 1.3.0 para `.deps/`, conferindo os checksums. Não instala esses componentes globalmente. A build de depuração fica em `build/linux-debug/`.

Para a build usada nos pacotes:

```bash
bash scripts/build-linux.sh --static-runtime
```

Os executáveis ficam em `build/linux-release/`. `-static-libstdc++ -static-libgcc` incorpora o runtime do compilador. GTK4, Firebird e outras bibliotecas continuam compartilhadas.

Para executar uma build diretamente:

```bash
source scripts/local-env.sh
./build/linux-release/cloudiemail --help
./build/linux-release/cloudiemail-gui config/local.json
```

## Windows com MSVC

Use Visual Studio com as ferramentas C++, CMake, vcpkg e o SDK/runtime oficial Firebird 5 x64. No Developer PowerShell:

```powershell
cmake -S . -B build/windows-native -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DCLOUDIEMAIL_SODIUM_STATIC=ON `
  -DFIREBIRD_ROOT="C:/Program Files/Firebird/Firebird_5_0"
cmake --build build/windows-native --config Release
```

O padrão é `/MT` em Release e `/MTd` em Debug. Firebird continua sendo uma dependência de execução: copie o runtime completo, incluindo engine, plugins, ICU e bibliotecas do kit, não apenas `fbclient.dll`. Os testes Python precisam do executável OpenSSL.

A receita nativa ainda não foi homologada em uma máquina Windows limpa. Os binários publicados são compilados com MSVC pelo Wine e passam pelos testes compatíveis com esse ambiente.

## MSVC pelo Wine

Com MSVC e Wine disponíveis:

```bash
bash scripts/bootstrap-windows-wine.sh
bash scripts/build-windows-wine.sh
```

A saída fica em `build/windows-release/`. `CLOUDIEMAIL_MSVC_ROOT` e `CLOUDIEMAIL_MSVC_TOOLCHAIN` permitem indicar outra instalação do compilador. Consulte [WINDOWS-WINE.md](WINDOWS-WINE.md) para os limites desse ambiente.

## Testes

```bash
source scripts/local-env.sh
ctest --test-dir build/linux-release --output-on-failure
node tests/mime_test.js
```

A suíte cobre criptografia, isolamento entre contas, Firebird, configuração, SMTP/IMAP/POP3, HTTPS, sessões e assistente. Os testes SMTP usam sockets locais e certificados temporários; não enviam mensagens a pessoas reais.

O teste de seleção IPv4/IPv6 usa `localhost.localdomain`, que deve resolver para `127.0.0.1` e `::1`, com loopback IPv6 habilitado.

Para o navegador e as capturas de tela:

```bash
python3 -m venv .venv
.venv/bin/pip install -r tests/requirements.txt
.venv/bin/playwright install chromium
.venv/bin/python tests/webmail_browser.py build/linux-release/cloudiemail
```

`tests/gui_gtk_setup.cpp` exercita os widgets GTK reais e pode salvar capturas do assistente, painel e preferências. Os arquivos usam apenas dados de demonstração.

## Pacotes

O empacotador usado nas releases depende de `pacman`, `patchelf`, `objdump`, bibliotecas e fontes do ambiente CachyOS. Ele não é um instalador de dependências para outras distribuições.

```bash
python3 scripts/package.py
python3 tests/package_smoke.py
```

Isso gera `Output/Linux` e `Output/Windows`. Use `--platform linux` ou `--platform windows` para montar só um deles. `--replace-pristine` permite refazer um pacote intacto; o script recusa substituir pastas que tenham sido alteradas ou contenham dados de usuário.

O smoke test usa cópias em diretórios temporários com espaços no caminho e não cria contas nos pacotes originais. Veja [RELEASING.md](RELEASING.md) para os arquivos publicados.
