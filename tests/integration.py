#!/usr/bin/env python3
"""Black-box tests against actual TLS sockets and an isolated Firebird database."""

import contextlib
import imaplib
import json
import os
import poplib
import signal
import smtplib
import socket
import ssl
import subprocess
import sys
import tempfile
import time
from http.client import HTTPConnection, HTTPSConnection
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BINARY = (
    Path(sys.argv[1]).resolve()
    if len(sys.argv) > 1
    else ROOT / "build/linux-debug/cloudiemail"
)
APACHE = os.environ.get("CLOUDIEMAIL_TEST_APACHE") == "1"
WINE = os.environ.get("CLOUDIEMAIL_TEST_WINE") == "1"
RUNNER = ["wine", str(BINARY)] if WINE else [str(BINARY)]
PASSWORD = "integration test password 39!"
SENTINEL = "PRIVATE-BODY-9e5d770d-No-Plaintext-In-Firebird"
checks = 0


def check(condition, description):
    global checks
    if not condition:
        raise AssertionError(description)
    checks += 1
    print(f"PASS {description}", flush=True)


def run(*args, stdin=None, success=True):
    argv = [
        windows_path(arg) if WINE and isinstance(arg, Path) else str(arg)
        for arg in args
    ]
    result = subprocess.run(
        [*RUNNER, *argv], input=stdin, text=True, capture_output=True, timeout=30
    )
    if success and result.returncode:
        raise AssertionError(f"{args[0]} failed: {result.stderr}")
    return result


def windows_path(path):
    return "Z:" + str(path).replace("/", "\\")


with tempfile.TemporaryDirectory(prefix="cloudiemail-test-") as temporary:
    folder = Path(temporary)
    reservations = [socket.socket() for _ in range(6)]
    for sock in reservations:
        sock.bind(("127.0.0.1", 0))
    ports = [sock.getsockname()[1] for sock in reservations]
    config = dict(
        domain="example.test",
        bind="127.0.0.1",
        database=str(folder / "mail.fdb"),
        certificate=str(folder / "tls.crt"),
        private_key=str(folder / "tls.key"),
        web_root=os.environ.get("CLOUDIEMAIL_TEST_WEB_ROOT", str(ROOT / "web")),
        smtp_port=ports[0],
        smtps_port=ports[1],
        imap_port=ports[2],
        pop3_port=ports[3],
        https_port=ports[4],
        submission_port=ports[5],
        outbound_enabled=False,
        max_message_bytes=65536,
        max_connections=8,
        mailbox_quota_bytes=1048576,
    )
    if APACHE:
        config.update(
            webmail_driver="apache", https_port=443, webmail_backend_port=ports[4]
        )
    config_path = folder / "config.json"
    serialized_config = config.copy()
    if WINE:
        for key in ["database", "certificate", "private_key", "web_root"]:
            serialized_config[key] = windows_path(config[key])
    config_path.write_text(json.dumps(serialized_config))
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
            "/CN=webmail.example.test",
            "-addext",
            "subjectAltName=DNS:webmail.example.test,DNS:mail.example.test,IP:127.0.0.1",
        ],
        check=True,
        capture_output=True,
    )
    run("init", config_path)
    check(
        run("init", config_path, success=False).returncode != 0,
        "init refuses to overwrite database",
    )
    for user in ["alice", "bob"]:
        run(
            "account-add",
            config_path,
            f"{user}@example.test",
            stdin=f"{PASSWORD}\n{PASSWORD}\n",
        )
    check(
        run(
            "account-add",
            config_path,
            "alice@example.test",
            stdin=f"{PASSWORD}\n{PASSWORD}\n",
            success=False,
        ).returncode
        != 0,
        "duplicate account rejected",
    )
    check(
        run(
            "account-add", config_path, "bad@outside.test", stdin="", success=False
        ).returncode
        != 0,
        "out-of-domain account rejected",
    )
    context = ssl.create_default_context(cafile=config["certificate"])
    for sock in reservations:
        sock.close()
    log = open(folder / "server.log", "w+")
    server_args = ["serve", windows_path(config_path) if WINE else str(config_path)]
    if WINE:
        server_args.extend(["--run-for", "30"])
    process = subprocess.Popen([*RUNNER, *server_args], stdout=log, stderr=log)
    try:
        for _ in range(100):
            if process.poll() is not None:
                log.seek(0)
                raise AssertionError("server exited: " + log.read())
            try:
                with socket.create_connection(("127.0.0.1", ports[0]), timeout=0.2):
                    break
            except OSError:
                time.sleep(0.1)
        else:
            raise AssertionError("server did not start")
        with smtplib.SMTP("127.0.0.1", ports[0], timeout=10) as smtp:
            smtp.ehlo()
            check(
                smtp.docmd("MAIL FROM:<sender@outside.test>")[0] == 530,
                "SMTP refuses plaintext mail",
            )
            smtp.starttls(context=context)
            smtp.ehlo()
            smtp.mail("sender@outside.test")
            check(
                smtp.rcpt("recipient@outside.test")[0] == 550, "SMTP blocks open relay"
            )
            smtp.rset()
            message = f"From: sender@outside.test\r\nTo: alice@example.test\r\nSubject: Secret subject marker\r\n\r\n{SENTINEL}\r\n.dot-prefixed\r\n"
            check(
                smtp.sendmail("sender@outside.test", ["alice@example.test"], message)
                == {},
                "SMTP STARTTLS accepts encrypted local delivery",
            )
        with smtplib.SMTP_SSL(
            "127.0.0.1", ports[1], context=context, timeout=10
        ) as smtp:
            smtp.ehlo()
            check(
                smtp.mail("alice@example.test")[0] == 530,
                "submission requires authentication",
            )
            import base64

            assert smtp.docmd("AUTH LOGIN")[0] == 334
            assert (
                smtp.docmd(base64.b64encode(b"alice@example.test").decode())[0] == 334
            )
            assert smtp.docmd(base64.b64encode(PASSWORD.encode()).decode())[0] == 235
            check(
                smtp.mail("bob@example.test")[0] == 553,
                "submission prevents sender impersonation",
            )
            smtp.rset()
            smtp.sendmail(
                "alice@example.test",
                ["bob@example.test"],
                f"From: alice@example.test\r\nSubject: Hello Bob\r\n\r\n{SENTINEL}-bob\r\n",
            )
            check(True, "authenticated SMTPS delivery")
        with imaplib.IMAP4_SSL(
            "127.0.0.1", ports[2], ssl_context=context, timeout=10
        ) as imap:
            imap.login("alice@example.test", PASSWORD)
            check(imap.select("INBOX")[1] == [b"1"], "IMAP selects own mailbox")
            result, data = imap.uid("search", None, "ALL")
            alice_uid = int(data[0])
            result, data = imap.uid("fetch", str(alice_uid), "(BODY.PEEK[])")
            check(
                result == "OK" and SENTINEL.encode() in data[0][1],
                "IMAP decrypts received email",
            )
            result, data = imap.uid("fetch", str(alice_uid), "(RFC822.SIZE RFC822)")
            check(
                result == "OK" and SENTINEL.encode() in data[0][1],
                "IMAP fetches body alongside size",
            )
            check(
                imap.uid("fetch", str(alice_uid), "(BODYSTRUCTURE)")[0] == "NO",
                "unsupported IMAP attribute is explicitly rejected",
            )
            imap.select("INBOX", readonly=True)
            check(
                imap.store("1", "+FLAGS", r"(\Deleted)")[0] == "NO",
                "IMAP EXAMINE is read-only",
            )
        pop = poplib.POP3_SSL("127.0.0.1", ports[3], context=context, timeout=10)
        pop.user("alice@example.test")
        pop.pass_(PASSWORD)
        check(pop.stat()[0] == 1, "POP3 sees own mailbox")
        response, lines, size = pop.retr(1)
        check(
            SENTINEL.encode() in b"\n".join(lines) and b".dot-prefixed" in lines,
            "POP3 retrieves and unescapes message",
        )
        pop.dele(1)
        pop.rset()
        pop.quit()

        authority = (
            "webmail.example.test" if APACHE else f"webmail.example.test:{ports[4]}"
        )
        origin = "https://" + authority

        def http(method, path, body=None, cookie="", csrf="", override=None):
            conn = (
                HTTPConnection("127.0.0.1", ports[4], timeout=15)
                if APACHE
                else HTTPSConnection("127.0.0.1", ports[4], context=context, timeout=15)
            )
            headers = {
                "Host": authority,
                "Origin": origin,
                "Content-Type": "application/json",
            }
            if cookie:
                headers["Cookie"] = cookie
            if csrf:
                headers["X-CSRF-Token"] = csrf
            if override:
                headers.update(override)
            conn.request(
                method, path, json.dumps(body) if body is not None else None, headers
            )
            response = conn.getresponse()
            result = response.status, dict(response.getheaders()), response.read()
            conn.close()
            return result

        status, headers, body = http("GET", "/")
        check(
            status == 200 and b"CloudieMail" in body, "HTTPS serves integrated webmail"
        )
        check(
            "frame-ancestors 'none'" in headers["Content-Security-Policy"]
            and headers["X-Content-Type-Options"] == "nosniff",
            "webmail security headers",
        )
        check(
            http("GET", "/", override={"Host": "evil.test"})[0] == 400,
            "webmail rejects unconfigured host",
        )
        check(
            http("GET", "/api/messages")[0] == 401,
            "unauthenticated mailbox access rejected",
        )
        check(
            http(
                "POST",
                "/api/login",
                {"email": "alice@example.test", "password": PASSWORD},
                override={"Origin": "https://evil.test"},
            )[0]
            == 403,
            "cross-origin login rejected",
        )
        status, headers, body = http(
            "POST", "/api/login", {"email": "bob@example.test", "password": PASSWORD}
        )
        check(status == 200, "webmail login unlocks mailbox")
        cookie = headers["Set-Cookie"].split(";", 1)[0]
        csrf = json.loads(body)["csrf"]
        check(
            all(
                flag in headers["Set-Cookie"]
                for flag in ["Secure", "HttpOnly", "SameSite=Strict", "Path=/"]
            ),
            "session cookie protections",
        )
        status, headers, body = http("GET", "/api/messages", cookie=cookie)
        check(
            status == 200 and len(json.loads(body)["messages"]) == 1,
            "webmail lists only own messages",
        )
        check(
            http("GET", f"/api/messages/{alice_uid}", cookie=cookie)[0] == 400,
            "webmail blocks cross-account message access",
        )
        check(
            http("POST", "/api/send", {}, cookie=cookie)[0] == 403,
            "write without CSRF token rejected",
        )
        status, _, _ = http(
            "POST",
            "/api/send",
            {
                "to": "alice@example.test",
                "subject": "Webmail test",
                "body": "Sent through the integrated webmail",
            },
            cookie,
            csrf,
        )
        check(status == 201, "webmail sends local message")
        check(
            http(
                "POST",
                "/api/send",
                {"to": "outside@external.test", "subject": "No relay", "body": "test"},
                cookie,
                csrf,
            )[0]
            == 400,
            "webmail also blocks external relay",
        )
        check(
            http("POST", "/api/logout", {}, cookie, csrf)[0] == 200, "logout succeeds"
        )
        check(
            http("GET", "/api/messages", cookie=cookie)[0] == 401,
            "logout revokes session",
        )
        # Malformed request framing must never reach the application parser.
        raw_socket = socket.create_connection(("127.0.0.1", ports[4]), timeout=10)
        with (
            raw_socket
            if APACHE
            else context.wrap_socket(raw_socket, server_hostname="127.0.0.1")
        ) as sock:
            sock.sendall(
                f"GET / HTTP/1.1\r\nHost: {authority}\r\nContent-Length: 0\r\nContent-Length: 1\r\n\r\n".encode()
            )
            check(
                b"400" in sock.recv(1024).split(b"\r\n")[0],
                "duplicate HTTP framing rejected",
            )
        # Exhaust the IP budget with invalid credentials, then verify correct credentials are rate limited.
        for _ in range(10):
            http(
                "POST",
                "/api/login",
                {"email": "nobody@example.test", "password": "invalid password"},
            )
        check(
            http(
                "POST",
                "/api/login",
                {"email": "bob@example.test", "password": PASSWORD},
            )[0]
            == 401,
            "authentication rate limit enforced",
        )
    finally:
        if not WINE:
            process.send_signal(signal.SIGINT)
        try:
            process.wait(timeout=40 if WINE else 10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            raise AssertionError("graceful shutdown timed out")
        log.close()
    check(process.returncode == 0, "graceful shutdown")
    raw = Path(config["database"]).read_bytes()
    check(
        SENTINEL.encode() not in raw
        and b"Secret subject marker" not in raw
        and PASSWORD.encode() not in raw,
        "message body, subject and password absent from raw Firebird file",
    )
    stats = json.loads(run("status", config_path).stdout)
    check(
        stats["accounts"] == 2 and stats["messages"] == 5,
        "Firebird persists accounts and delivered messages",
    )
    changed = "new integration password 72!"
    run(
        "password-change",
        config_path,
        "alice@example.test",
        stdin=f"{PASSWORD}\n{changed}\n{changed}\n",
    )
    check(True, "password rotation persists in Firebird")
    print(f"\n{checks} integration checks passed")
