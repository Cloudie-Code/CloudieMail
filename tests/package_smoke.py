#!/usr/bin/env python3
"""Validate relocated runtime packages; never write test accounts to Output."""

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ENV = os.environ.copy()
for name in (
    "FIREBIRD",
    "LD_LIBRARY_PATH",
    "WINEPATH",
    "OPENSSL_CONF",
    "OPENSSL_MODULES",
    "CLOUDIEMAIL_CA_BUNDLE",
):
    ENV.pop(name, None)
ENV["WINEDEBUG"] = "-all"


def run(*args, **kwargs):
    result = subprocess.run(
        [str(arg) for arg in args],
        env=ENV,
        text=True,
        capture_output=True,
        timeout=120,
        **kwargs,
    )
    if result.returncode:
        raise RuntimeError(f"{args}:\n{result.stdout}\n{result.stderr}")
    print(result.stdout.strip(), flush=True)
    return result


with tempfile.TemporaryDirectory(
    prefix="CloudieMail package with spaces "
) as temporary:
    directory = Path(temporary)
    linux, windows = directory / "Linux", directory / "Windows"
    shutil.copytree(ROOT / "Output/Linux", linux)
    shutil.copytree(ROOT / "Output/Windows", windows)
    run(linux / "Console.sh", "--help", cwd=directory)
    run(linux / "run.sh", "cloudiemail-tests", cwd=directory)
    run(linux / "run.sh", "cloudiemail-config-tests", cwd=directory)
    run(linux / "run.sh", "cloudiemail-mail-tests", cwd=directory)
    run(linux / "run.sh", "cloudiemail-protocol-tests", cwd=directory)
    run("bash", linux / "scripts/dev-certificate.sh", cwd=directory)
    run(
        linux / "run.sh",
        "openssl",
        "x509",
        "-in",
        linux / "data/tls.crt",
        "-noout",
        "-checkhost",
        "webmail.example.test",
        cwd=directory,
    )
    result = subprocess.run(
        ["bash", str(linux / "scripts/dev-certificate.sh")],
        env=ENV,
        text=True,
        capture_output=True,
        timeout=20,
    )
    if result.returncode == 0:
        raise RuntimeError("Development certificate script overwrote an existing key")
    run(
        linux / "run.sh",
        "cloudiemail-setup-tests",
        directory / "Setup checks",
        linux / "data/tls.crt",
        linux / "data/tls.key",
        cwd=directory,
    )
    answers = [
        "1",
        "example.test",
        "192.0.2.10",
        "127.0.0.1",
        "2525",
        "2465",
        "2993",
        "2995",
        "8443",
        "2587",
        "nao",
        "2",
        "3",
        str(linux / "data/tls.crt"),
        str(linux / "data/tls.key"),
        "1",
        "admin@example.test",
        "packaged onboarding password!",
        "packaged onboarding password!",
    ]
    run(
        linux / "Console.sh",
        "setup",
        linux / "config/local.json",
        input="\n".join(answers) + "\n",
        cwd=directory,
    )
    run(linux / "run.sh", "lego", "--version", cwd=directory)
    ENV["CLOUDIEMAIL_TEST_WEB_ROOT"] = str(linux / "web")
    run(
        sys.executable,
        ROOT / "tests/integration.py",
        linux / "Console.sh",
        cwd=directory,
    )
    run(
        sys.executable,
        ROOT / "tests/imap_integration.py",
        linux / "Console.sh",
        cwd=directory,
    )
    gui = subprocess.Popen(
        [str(linux / "CloudieMail.sh")],
        env=ENV,
        cwd=directory,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        try:
            out, err = gui.communicate(timeout=4)
            raise RuntimeError(
                f"Packaged GTK4 GUI exited early ({gui.returncode}): {out}\n{err}"
            )
        except subprocess.TimeoutExpired:
            print("GTK4 package GUI stayed open for four seconds", flush=True)
    finally:
        gui.terminate()
        try:
            out, err = gui.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            gui.kill()
            out, err = gui.communicate(timeout=10)
            raise RuntimeError("GTK4 did not exit after SIGTERM")
        if err:
            print(f"GTK4 diagnostics: {err}", flush=True)

    # App-local DLL resolution must work without the development WINEPATH.
    run("wine", windows / "bin/cloudiemail-tests.exe", "--crypto-only", cwd=directory)
    run("wine", windows / "bin/cloudiemail-config-tests.exe", cwd=directory)
    run("wine", windows / "bin/cloudiemail-protocol-tests.exe", cwd=directory)
    run("wine", windows / "bin/cloudiemail-http-tests.exe", cwd=directory)
    run(
        "wine",
        windows / "bin/cloudiemail-setup-tests.exe",
        "--process-only",
        cwd=directory,
    )
    run("wine", windows / "bin/lego.exe", "--version", cwd=directory)
    run("wine", "cmd", "/d", "/c", "Console.cmd", "--help", cwd=windows)
    print(
        "Relocated Linux IMAP/antispam and GTK4 passed; Windows unit tests and launcher passed under Wine.",
        flush=True,
    )
