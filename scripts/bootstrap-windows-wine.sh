#!/usr/bin/env bash
# Prepare project-local Windows dependencies with the already-installed MSVC/Wine toolchain.
set -euo pipefail
cm_project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$cm_project_root"
cm_win_deps="$cm_project_root/.deps/windows"
cm_msvc_root="${CLOUDIEMAIL_MSVC_ROOT:-/opt/msvc}"
cm_jobs="${CLOUDIEMAIL_BUILD_JOBS:-4}"
export WINEDEBUG=-all
mkdir -p "$cm_win_deps"
fetch() {
  local url="$1" destination="$2" checksum="$3"
  if [[ ! -f "$destination" ]]; then
    curl --fail --location --proto '=https' --tlsv1.2 "$url" --output "$destination.tmp"
    mv "$destination.tmp" "$destination"
  fi
  printf '%s  %s\n' "$checksum" "$destination" | sha256sum --check --status
}
fetch 'https://github.com/FirebirdSQL/firebird/releases/download/v5.0.4/Firebird-5.0.4.1812-0-windows-x64.zip' "$cm_win_deps/firebird.zip" 01e844fce4d5f53272a76205dbc3a1ba4b782ab8e8eadcd808cdbccd9ce13b72
fetch 'https://download.libsodium.org/libsodium/releases/libsodium-1.0.22-msvc.zip' "$cm_win_deps/sodium.zip" 3e03a726fac4bc09cb61d8f29d658ef7a5eca0811de59082130414f7ca2e4279
fetch 'https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz' "$cm_win_deps/openssl.tar.gz" 603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
fetch 'https://curl.se/download/curl-8.23.0.tar.xz' "$cm_win_deps/curl.tar.xz" 8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5
fetch 'https://github.com/StrawberryPerl/Perl-Dist-Strawberry/releases/download/SP_54231_64bit/strawberry-perl-5.42.3.1-64bit-portable.zip' "$cm_win_deps/perl.zip" 6a081a811781c30aca51dbc036afd93092af91e3297901f02c17043795a10690
fetch 'https://download.qt.io/official_releases/jom/jom_1_1_6.zip' "$cm_win_deps/jom.zip" e480a22a99fc41d6afbe1b27faf34b3b9dad76ca40095179614605c3fbff637f
if [[ ! -f "$cm_win_deps/firebird/fbclient.dll" ]]; then
  unzip -q "$cm_win_deps/firebird.zip" -d "$cm_win_deps/firebird"
fi
if [[ ! -f "$cm_win_deps/sodium/include/sodium.h" ]]; then
  unzip -q "$cm_win_deps/sodium.zip" -d "$cm_win_deps/sodium-unpack"
  cp -a "$cm_win_deps/sodium-unpack/libsodium" "$cm_win_deps/sodium"
fi
if [[ ! -f "$cm_win_deps/perl/perl/bin/perl.exe" ]]; then
  unzip -q "$cm_win_deps/perl.zip" 'perl/*' 'c/bin/*.dll' -d "$cm_win_deps/perl"
fi
if [[ ! -f "$cm_win_deps/jom/jom.exe" ]]; then
  unzip -q "$cm_win_deps/jom.zip" -d "$cm_win_deps/jom"
fi
if [[ ! -f "$cm_win_deps/json/include/nlohmann/json.hpp" ]]; then
  mkdir -p "$cm_win_deps/json/include" "$cm_win_deps/json/share/cmake"
  cp -a /usr/include/nlohmann "$cm_win_deps/json/include/"
  cp -a /usr/share/cmake/nlohmann_json "$cm_win_deps/json/share/cmake/"
fi
if [[ ! -f "$cm_win_deps/openssl-install/lib/libssl.lib" ]]; then
  [[ -d "$cm_win_deps/openssl-3.5.9" ]] || tar -xzf "$cm_win_deps/openssl.tar.gz" -C "$cm_win_deps"
  (
    source "$cm_msvc_root/bin/x64/msvcenv.sh"
    export WINEPATH="$(winepath -w "$BINDIR");$(winepath -w "$cm_win_deps/perl/perl/bin");$(winepath -w "$cm_win_deps/perl/c/bin");$WINEPATH"
    cd "$cm_win_deps/openssl-3.5.9"
    wine "$cm_win_deps/perl/perl/bin/perl.exe" Configure VC-WIN64A no-asm no-shared no-tests no-docs no-apps -Z7 \
      "--prefix=$(winepath -w "$cm_win_deps/openssl-install")" > "$cm_win_deps/openssl-config.log" 2>&1
    wine "$cm_win_deps/jom/jom.exe" -j"$cm_jobs" > "$cm_win_deps/openssl-build.log" 2>&1
    wine "$cm_win_deps/jom/jom.exe" install_sw > "$cm_win_deps/openssl-install.log" 2>&1
  )
fi
if [[ ! -f "$cm_win_deps/curl/lib/libcurl.lib" ]]; then
  [[ -d "$cm_win_deps/curl-8.23.0" ]] || tar -xf "$cm_win_deps/curl.tar.xz" -C "$cm_win_deps"
  cmake -S "$cm_win_deps/curl-8.23.0" -B "$cm_win_deps/curl-build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${CLOUDIEMAIL_MSVC_TOOLCHAIN:-$cm_msvc_root/toolchain.cmake}" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded \
    -DCMAKE_INSTALL_PREFIX="$cm_win_deps/curl" -DBUILD_SHARED_LIBS=OFF -DBUILD_CURL_EXE=OFF \
    -DBUILD_TESTING=OFF -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF \
    -DCURL_USE_SCHANNEL=ON -DCURL_USE_OPENSSL=OFF -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF \
    -DCURL_ZSTD=OFF -DCURL_BROTLI=OFF -DCURL_ZLIB=OFF -DUSE_LIBIDN2=OFF > "$cm_win_deps/curl-config.log" 2>&1
  cmake --build "$cm_win_deps/curl-build" -j"$cm_jobs" > "$cm_win_deps/curl-build.log" 2>&1
  cmake --install "$cm_win_deps/curl-build" > "$cm_win_deps/curl-install.log" 2>&1
fi
printf 'Dependências Windows prontas. Execute bash scripts/build-windows-wine.sh\n'
