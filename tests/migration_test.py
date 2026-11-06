"""Create data with a schema-1 binary, then upgrade/read it with the current build.
Arguments: current binary, legacy binary or legacy Console.sh.
"""

import json
import os
import signal
import smtplib
import socket
import ssl
import subprocess
import sys
import tempfile
import time
from http.client import HTTPSConnection
from pathlib import Path

new, old = map(lambda p: str(Path(p).resolve()), sys.argv[1:3])
with tempfile.TemporaryDirectory(prefix="cloudiemail-migration-") as temporary:
    root = Path(temporary)
    ports = []
    for _ in range(5):
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            ports.append(s.getsockname()[1])
    config = dict(
        domain="example.test",
        bind="127.0.0.1",
        database=str(root / "mail.fdb"),
        certificate=str(root / "tls.crt"),
        private_key=str(root / "tls.key"),
        web_root=str(Path(__file__).resolve().parents[1] / "web"),
        smtp_port=ports[0],
        smtps_port=ports[1],
        imap_port=ports[2],
        pop3_port=ports[3],
        https_port=ports[4],
    )
    path = root / "config.json"
    path.write_text(json.dumps(config))
    password = "migration test strong password"
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
            config["private_key"],
            "-out",
            config["certificate"],
            "-days",
            "1",
            "-subj",
            "/CN=example.test",
            "-addext",
            "subjectAltName=DNS:mail.example.test,DNS:webmail.example.test,IP:127.0.0.1",
        ],
        capture_output=True,
        check=True,
    )
    subprocess.run([old, "init", str(path)], check=True, capture_output=True)
    subprocess.run(
        [old, "account-add", str(path), "alice@example.test"],
        input=password + "\n" + password + "\n",
        text=True,
        check=True,
        capture_output=True,
    )
    context = ssl.create_default_context(cafile=config["certificate"])

    def launch(binary):
        process = subprocess.Popen(
            [binary, "serve", str(path)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        for _ in range(100):
            if process.poll() is not None:
                raise AssertionError(process.stderr.read())
            try:
                with socket.create_connection(("127.0.0.1", ports[0]), timeout=0.1):
                    return process
            except OSError:
                time.sleep(0.1)
        raise AssertionError("startup timeout")

    def stop(process):
        process.send_signal(signal.SIGINT)
        assert process.wait(timeout=10) == 0

    process = launch(old)
    try:
        with smtplib.SMTP("127.0.0.1", ports[0]) as smtp:
            smtp.ehlo()
            smtp.starttls(context=context)
            smtp.ehlo()
            smtp.sendmail(
                "sender@example.org",
                ["alice@example.test"],
                "From: sender@example.org\r\nSubject: Legacy data\r\n\r\nLegacy ciphertext must survive.\r\n",
            )
    finally:
        stop(process)
    before = json.loads(subprocess.check_output([old, "status", str(path)], text=True))
    after = json.loads(subprocess.check_output([new, "status", str(path)], text=True))
    assert after == before
    process = launch(new)
    try:
        authority = f"webmail.example.test:{ports[4]}"

        def request(method, url, body=None, cookie=""):
            c = HTTPSConnection("127.0.0.1", ports[4], context=context, timeout=10)
            c.request(
                method,
                url,
                json.dumps(body) if body else None,
                {
                    "Host": authority,
                    "Origin": "https://" + authority,
                    "Content-Type": "application/json",
                    "Cookie": cookie,
                },
            )
            r = c.getresponse()
            result = (r.status, dict(r.headers), r.read())
            c.close()
            return result

        status, headers, body = request(
            "POST", "/api/login", dict(email="alice@example.test", password=password)
        )
        assert status == 200
        cookie = headers["Set-Cookie"].split(";")[0]
        status, _, body = request("GET", "/api/messages", cookie=cookie)
        data = json.loads(body)
        assert len(data["messages"]) == 1
        mail = data["messages"][0]
        assert (
            mail["folder"] == "INBOX" and mail["labels"] == [] and not mail["flagged"]
        )
        status, _, body = request("GET", f"/api/messages/{mail['id']}", cookie=cookie)
        assert status == 200 and b"Legacy ciphertext must survive." in body
    finally:
        stop(process)
    print(
        "Schema 1 -> 2: accounts, encrypted message, password and UID preserved; default folder/labels migrated"
    )
