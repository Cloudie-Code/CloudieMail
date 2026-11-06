#!/usr/bin/env python3
import os
import subprocess
import sys
import tempfile
from pathlib import Path

with tempfile.TemporaryDirectory(prefix="CloudieMail setup with spaces ") as temporary:
    folder = Path(temporary)
    cert, key = folder / "source.crt", folder / "source.key"
    subprocess.run(
        [
            "openssl",
            "req",
            "-x509",
            "-newkey",
            "ec",
            "-pkeyopt",
            "ec_paramgen_curve:P-256",
            "-noenc",
            "-keyout",
            str(key),
            "-out",
            str(cert),
            "-days",
            "1",
            "-subj",
            "/CN=example.test",
            "-addext",
            "subjectAltName=DNS:example.test,DNS:mail.example.test,DNS:webmail.example.test",
        ],
        check=True,
        capture_output=True,
    )
    env = dict(os.environ, LEGO_TLS_SKIP_VERIFY="true")
    subprocess.run(
        [str(Path(sys.argv[1]).resolve()), str(folder), str(cert), str(key)],
        check=True,
        env=env,
        timeout=40,
    )
