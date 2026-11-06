#!/usr/bin/env python3
"""Assemble local Linux/Windows runtime folders without touching existing installs."""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "Output"
LINUX_BUILD = ROOT / "build/linux-release"
WINDOWS_BUILD = ROOT / "build/windows-release"
FIREBIRD = ROOT / ".deps/firebird/opt/firebird"
WIN_DEPS = ROOT / ".deps/windows"
ENV = dict(
    os.environ,
    LD_LIBRARY_PATH=f"{FIREBIRD}/lib:{ROOT}/.deps/libtommath-1.3.0/.libs",
    LC_ALL="C",
)


def command(*args, **kwargs):
    return subprocess.check_output(
        [str(arg) for arg in args], text=True, **kwargs
    ).strip()


def copy(source, destination):
    destination = Path(destination)
    if not destination.resolve().is_relative_to(OUTPUT.resolve()):
        raise RuntimeError(f"Refusing to write outside Output: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def tree(source, destination):
    shutil.copytree(
        source,
        destination,
        symlinks=False,
        dirs_exist_ok=True,
        ignore=shutil.ignore_patterns("*.log", "*.lck", "*.pid"),
    )


def elf(path):
    if not path.is_file():
        return False
    with path.open("rb") as stream:
        return stream.read(4) == b"\x7fELF"


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def common(destination):
    destination.mkdir()
    tree(ROOT / "web", destination / "web")
    tree(ROOT / "docs", destination / "docs")
    copy(ROOT / "config/example.json", destination / "config/example.json")
    (destination / "data").mkdir(mode=0o700)
    copy(ROOT / "packaging/LEIA-ME.txt", destination / "LEIA-ME.txt")
    (destination / "licenses").mkdir()
    copy(ROOT / "LICENSE", destination / "LICENSE")
    copy(
        "/usr/share/licenses/nlohmann-json/LICENSE.MIT",
        destination / "licenses/nlohmann-json.txt",
    )


def linux(destination):
    common(destination)
    tree(FIREBIRD, destination / "runtime/firebird")
    for sdk_dir in ("include", "examples"):
        shutil.rmtree(destination / "runtime/firebird" / sdk_dir)
    for name in (
        "cloudiemail",
        "cloudiemail-gui",
        "cloudiemail-tests",
        "cloudiemail-setup-tests",
        "cloudiemail-config-tests",
        "cloudiemail-mail-tests",
    ):
        target = destination / "bin" / name
        copy(LINUX_BUILD / name, target)
        # Do not retain absolute RUNPATH entries pointing into the development tree.
        command("patchelf", "--remove-rpath", target)
    copy(ROOT / ".deps/lego/linux/lego", destination / "bin/lego")
    copy(
        LINUX_BUILD / "cloudiemail-process-probe",
        destination / "bin/cloudiemail-process-probe",
    )
    copy(ROOT / ".deps/lego/linux/LICENSE", destination / "licenses/lego.txt")
    copy("/usr/bin/openssl", destination / "bin/openssl")
    for source, target in [
        ("linux/run.sh", "run.sh"),
        ("linux/gui.sh", "CloudieMail.sh"),
        ("linux/console.sh", "Console.sh"),
    ]:
        copy(ROOT / "packaging" / source, destination / target)
        (destination / target).chmod(0o755)
    dev_certificate = (
        (ROOT / "scripts/dev-certificate.sh")
        .read_text()
        .replace("openssl req ", '"$cm_project_root/run.sh" openssl req ')
    )
    (destination / "scripts").mkdir()
    (destination / "scripts/dev-certificate.sh").write_text(dev_certificate)
    copy(
        ROOT / "scripts/runtime/configure-apache.py",
        destination / "scripts/configure-apache.py",
    )

    for source, target in [
        ("/usr/lib/gconv", "lib/gconv"),
        ("/usr/lib/ossl-modules", "lib/ossl-modules"),
        ("/usr/lib/gio/modules", "lib/gio/modules"),
        ("/usr/share/glib-2.0/schemas", "share/glib-2.0/schemas"),
        ("/usr/share/icons/Adwaita", "share/icons/Adwaita"),
        ("/usr/share/mime", "share/mime"),
    ]:
        tree(source, destination / target)
    for font in ("DejaVuSans.ttf", "DejaVuSans-Bold.ttf", "DejaVuSansMono.ttf"):
        copy(Path("/usr/share/fonts/TTF") / font, destination / "share/fonts" / font)
    copy(
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        destination / "share/fonts/NotoSansCJK-Regular.ttc",
    )
    copy(
        "/usr/share/licenses/noto-fonts-cjk/LICENSE",
        destination / "licenses/noto-cjk.txt",
    )
    (destination / "share/fonts.conf").write_text("""<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
<fontconfig>
  <include ignore_missing="yes">/etc/fonts/fonts.conf</include>
  <dir prefix="relative">fonts</dir>
  <cachedir prefix="xdg">fontconfig</cachedir>
  <alias><family>sans-serif</family><prefer><family>DejaVu Sans</family></prefer></alias>
  <alias><family>monospace</family><prefer><family>DejaVu Sans Mono</family></prefer></alias>
</fontconfig>
""")
    copy(
        "/etc/ssl/certs/ca-certificates.crt", destination / "share/certs/ca-bundle.crt"
    )
    (destination / "share/openssl.cnf").write_text("""openssl_conf = openssl_init
[openssl_init]
providers = providers
[providers]
default = default_provider
[default_provider]
activate = 1
[req]
distinguished_name = dn
[dn]
""")
    # Record all ELF dependency resolutions; any missing library aborts assembly.
    queue = [path for path in destination.rglob("*") if elf(path)]
    queue += [Path("/usr/lib/libnss_dns.so.2"), Path("/usr/lib/libnss_files.so.2")]
    libraries, visited = {}, set()
    while queue:
        source = queue.pop()
        real = source.resolve()
        if real in visited:
            continue
        visited.add(real)
        process = subprocess.run(
            ["ldd", str(source)], env=ENV, text=True, capture_output=True
        )
        if (
            process.returncode
            and "not a dynamic executable" in process.stdout + process.stderr
        ):
            continue
        if process.returncode:
            raise RuntimeError(
                f"Cannot inspect {source}: {process.stdout} {process.stderr}"
            )
        result = process.stdout
        if "not found" in result:
            raise RuntimeError(f"Missing dependency for {source}:\n{result}")
        for line in result.splitlines():
            match = re.match(r"\s*(\S+) => (/\S+) \(", line)
            if match:
                name, path = match.groups()
                name = Path(name).name
            else:
                match = re.match(r"\s*(/\S+) \(", line)
                if not match:
                    continue
                path = match.group(1)
                name = Path(path).name
            if name in libraries:
                if Path(libraries[name]).resolve() != Path(path).resolve() and digest(
                    Path(libraries[name])
                ) != digest(Path(path)):
                    raise RuntimeError(f"Conflicting libraries for {name}")
                continue
            libraries[name] = path
            copy(path, destination / "lib" / name)
            queue.append(Path(path))
    for name in ("libnss_dns.so.2", "libnss_files.so.2"):
        copy(Path("/usr/lib") / name, destination / "lib" / name)

    # Include distro license notices and version provenance, not private host config.
    packages = {}
    asset_owners = [
        Path("/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc"),
        Path("/usr/share/fonts/TTF/DejaVuSans.ttf"),
        Path("/usr/share/icons/Adwaita/index.theme"),
        Path("/usr/share/mime/mime.cache"),
        Path("/etc/ssl/certs/ca-certificates.crt"),
    ]
    for path in [*map(Path, libraries.values()), *asset_owners]:
        if not str(path).startswith(("/usr/", "/etc/")):
            continue
        result = subprocess.run(
            ["pacman", "-Qqo", str(path.resolve())], text=True, capture_output=True
        )
        if result.returncode:
            continue
        for name in result.stdout.splitlines():
            if name in packages:
                continue
            packages[name] = command("pacman", "-Q", name)
            license_dir = Path("/usr/share/licenses") / name
            if license_dir.is_dir():
                tree(license_dir, destination / "licenses" / name)
    tree("/usr/share/licenses/spdx", destination / "licenses/spdx")
    copy(
        ROOT / ".deps/libtommath-1.3.0/LICENSE", destination / "licenses/libtommath.txt"
    )
    # The loader's direct closure must resolve entirely inside the staged package.
    for program in ("cloudiemail", "cloudiemail-gui", "cloudiemail-tests", "openssl"):
        resolved = command(
            destination / "lib/ld-linux-x86-64.so.2",
            "--inhibit-cache",
            "--library-path",
            f"{destination}/lib:{destination}/runtime/firebird/lib",
            "--list",
            destination / "bin" / program,
            env={"LC_ALL": "C"},
        )
        for path in re.findall(r"=> (/\S+)", resolved):
            if not Path(path).is_relative_to(destination):
                raise RuntimeError(f"External library in package: {path}")
    provenance = {
        name: (
            str(Path(path).relative_to(destination))
            if Path(path).is_relative_to(destination)
            else str(Path(path).relative_to(ROOT))
            if Path(path).is_relative_to(ROOT)
            else path
        )
        for name, path in libraries.items()
    }
    return {
        "platform": "Linux x86-64-v3, kernel >= 6.1",
        "packages": packages,
        "libraries": provenance,
        "firebird": "5.0.4 official Linux kit",
        "libtommath": "1.3.0 locally built",
        "lego": "5.6.0 official static Linux amd64 binary",
        "runtime": "static GNU C++/GCC, other libraries bundled shared",
    }


def windows(destination):
    common(destination)
    tree(WIN_DEPS / "firebird", destination / "bin")
    for sdk_dir in ("include", "examples", "lib"):
        shutil.rmtree(destination / "bin" / sdk_dir)
    for name in (
        "cloudiemail.exe",
        "cloudiemail-gui.exe",
        "cloudiemail-tests.exe",
        "cloudiemail-setup-tests.exe",
        "cloudiemail-process-probe.exe",
        "cloudiemail-acl-tests.exe",
        "cloudiemail-http-tests.exe",
        "cloudiemail-config-tests.exe",
    ):
        copy(WINDOWS_BUILD / name, destination / "bin" / name)
    copy(ROOT / ".deps/lego/windows/lego.exe", destination / "bin/lego.exe")
    copy(ROOT / ".deps/lego/windows/LICENSE", destination / "licenses/lego.txt")
    for source, target in [
        ("windows/console.cmd", "Console.cmd"),
        ("windows/gui.cmd", "CloudieMail.cmd"),
    ]:
        content = (ROOT / "packaging" / source).read_text()
        (destination / target).write_bytes(
            content.replace("\n", "\r\n").encode("ascii")
        )
    for source, target in [
        (WIN_DEPS / "openssl-3.5.9/LICENSE.txt", "openssl.txt"),
        (WIN_DEPS / "curl-8.23.0/COPYING", "curl.txt"),
        (Path("/usr/share/licenses/libsodium/LICENSE"), "libsodium.txt"),
    ]:
        copy(source, destination / "licenses" / target)
    # Only Windows OS imports may be external; Firebird/Microsoft CRT DLLs must be included.
    system = {
        "advapi32.dll",
        "bcrypt.dll",
        "comctl32.dll",
        "comdlg32.dll",
        "crypt32.dll",
        "dnsapi.dll",
        "gdi32.dll",
        "httpapi.dll",
        "iphlpapi.dll",
        "kernel32.dll",
        "mpr.dll",
        "msvcrt.dll",
        "netapi32.dll",
        "ntdll.dll",
        "ole32.dll",
        "oleaut32.dll",
        "psapi.dll",
        "rpcrt4.dll",
        "secur32.dll",
        "shell32.dll",
        "shlwapi.dll",
        "user32.dll",
        "userenv.dll",
        "uxtheme.dll",
        "version.dll",
        "winmm.dll",
        "winspool.drv",
        "ws2_32.dll",
        "wldap32.dll",
    }
    provided = {p.name.lower() for p in (destination / "bin").glob("*") if p.is_file()}
    imports = {}
    for binary in (destination / "bin").rglob("*"):
        if binary.suffix.lower() not in (".exe", ".dll"):
            continue
        imported = re.findall(r"DLL Name: (\S+)", command("objdump", "-p", binary))
        imports[str(binary.relative_to(destination))] = imported
        for name in imported:
            lowered = name.lower()
            if lowered not in provided | system and not lowered.startswith(
                ("api-ms-win-", "ext-ms-win-")
            ):
                raise RuntimeError(f"Unbundled Windows import: {binary.name}: {name}")
    return {
        "platform": "Windows x64 / MSVC 19.44 /MT",
        "imports": imports,
        "firebird": "5.0.4 official Windows kit including CRT, ICU and plugins",
        "static_libraries": {
            "OpenSSL": "3.5.9",
            "curl": "8.23.0 Schannel",
            "libsodium": "1.0.22",
            "nlohmann-json": "3.12.0 headers",
        },
        "lego": "5.6.0 official Windows amd64 binary",
        "validation": "Crypto tested with Wine; Firebird embedded blocked in Wine; native Windows pending",
    }


def finish(destination, metadata):
    (destination / "THIRD-PARTY.json").write_text(json.dumps(metadata, indent=2) + "\n")
    checksums = []
    for file in sorted(destination.rglob("*")):
        if file.is_file():
            checksums.append(
                f"{digest(file)}  {file.relative_to(destination).as_posix()}\n"
            )
    (destination / "SHA256SUMS").write_text("".join(checksums))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--replace-pristine", action="store_true")
    parser.add_argument(
        "--platform", choices=("all", "linux", "windows"), default="all"
    )
    options = parser.parse_args()
    platforms = [("Linux", linux), ("Windows", windows)]
    if options.platform != "all":
        platforms = [
            (name, build)
            for name, build in platforms
            if name.lower() == options.platform
        ]
    OUTPUT.mkdir(exist_ok=True)
    replace = options.replace_pristine
    for name, _ in platforms:
        existing = OUTPUT / name
        if not existing.exists():
            continue
        if not replace:
            raise SystemExit(
                f"{existing} already exists. Use --replace-pristine only for unmodified generated packages."
            )
        expected = {}
        for line in (existing / "SHA256SUMS").read_text().splitlines():
            checksum, path = line.split("  ", 1)
            expected[path] = checksum
        actual = {
            str(p.relative_to(existing)): digest(p)
            for p in existing.rglob("*")
            if p.is_file() and p.name != "SHA256SUMS"
        }
        if actual != expected:
            raise SystemExit(
                f"{existing} has changed files or user data; not replaced. Preserve this installation separately."
            )
    with tempfile.TemporaryDirectory(prefix=".package-", dir=OUTPUT) as temporary:
        stage = Path(temporary)
        for name, assemble in platforms:
            print(f"Packaging {name}...", flush=True)
            destination = stage / name
            finish(destination, assemble(destination))
        for name, _ in platforms:
            if (OUTPUT / name).exists():
                (OUTPUT / name).rename(stage / ("previous-" + name))
            (stage / name).rename(OUTPUT / name)
            print(f"Ready: {OUTPUT / name}", flush=True)


if __name__ == "__main__":
    main()
