#!/usr/bin/env python3
"""Package verified runtime folders for a GitHub release."""

import argparse
import hashlib
import re
import subprocess
import tarfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "Output"


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def verify(folder):
    expected = {}
    for line in (folder / "SHA256SUMS").read_text().splitlines():
        checksum, name = line.split("  ", 1)
        relative = Path(name)
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError(f"Invalid manifest path: {name}")
        expected[name] = checksum
    actual = {}
    for path in folder.rglob("*"):
        if path.is_symlink():
            raise ValueError(f"Unexpected symlink: {path}")
        if not path.is_file():
            continue
        name = path.relative_to(folder).as_posix()
        if name == "SHA256SUMS":
            continue
        if name.startswith("data/") or (
            name.startswith("config/") and name != "config/example.json"
        ):
            raise ValueError(f"Installation data must not be published: {name}")
        if path.suffix.lower() in {".pfx", ".p12", ".key", ".etl", ".log"}:
            raise ValueError(f"Private or runtime file in package: {name}")
        if path.suffix.lower() == ".fdb" and name.lower() not in {
            "bin/security5.fdb",
            "runtime/firebird/security5.fdb",
        }:
            raise ValueError(f"Unexpected database: {name}")
        actual[name] = digest(path)
    if expected != actual:
        raise ValueError(f"Package changed after assembly: {folder}")
    return sorted(folder.rglob("*"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    options = parser.parse_args()
    version = options.version
    if not re.fullmatch(r"\d+\.\d+\.\d+(?:-[a-zA-Z0-9.-]+)?", version):
        parser.error("Invalid version")
    source_version = re.search(
        r"project\(CloudieMail VERSION ([\d.]+)", (ROOT / "CMakeLists.txt").read_text()
    )[1]
    if version.split("-", 1)[0] != source_version:
        parser.error("Release version differs from CMakeLists.txt")
    files = {name: verify(OUTPUT / name) for name in ("Linux", "Windows")}
    destination = OUTPUT / "releases" / ("v" + version)
    destination.mkdir(parents=True, exist_ok=True)
    windows = destination / f"CloudieMail-{version}-windows-x64.zip"
    linux = destination / f"CloudieMail-{version}-linux-x86_64-v3.tar.zst"
    checksums = destination / "SHA256SUMS"
    if any(path.exists() for path in (windows, linux, checksums)):
        raise FileExistsError(
            "Release files already exist; move them aside before rebuilding"
        )
    with zipfile.ZipFile(
        windows, "w", zipfile.ZIP_DEFLATED, compresslevel=9
    ) as archive:
        for path in files["Windows"]:
            archive.write(
                path,
                "CloudieMail-Windows/"
                + path.relative_to(OUTPUT / "Windows").as_posix(),
            )
    with zipfile.ZipFile(windows) as archive:
        if archive.testzip() is not None:
            raise ValueError("ZIP verification failed")
    process = subprocess.Popen(
        ["zstd", "-T0", "-10", "-q", "-o", str(linux)], stdin=subprocess.PIPE
    )
    try:
        with tarfile.open(fileobj=process.stdin, mode="w|") as archive:
            for path in files["Linux"]:
                name = (
                    "CloudieMail-Linux/" + path.relative_to(OUTPUT / "Linux").as_posix()
                )
                info = archive.gettarinfo(str(path), arcname=name)
                info.uid = info.gid = 0
                info.uname = info.gname = ""
                if path.is_file():
                    with path.open("rb") as stream:
                        archive.addfile(info, stream)
                else:
                    archive.addfile(info)
        process.stdin.close()
        if process.wait() != 0:
            raise RuntimeError("zstd compression failed")
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
    subprocess.run(["zstd", "--test", "-q", str(linux)], check=True)
    checksums.write_text(
        "".join(f"{digest(path)}  {path.name}\n" for path in (linux, windows))
    )
    for path in (linux, windows, checksums):
        print(path)


if __name__ == "__main__":
    main()
