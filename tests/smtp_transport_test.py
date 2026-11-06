"""A real libcurl STARTTLS transaction; all sockets remain on loopback."""

import json
import os
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

binary = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="cloudiemail-smtp-") as folder:
    root = Path(folder)
    cert = root / "cert.pem"
    key = root / "key.pem"
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
            "/CN=127.0.0.1",
            "-addext",
            "subjectAltName=IP:127.0.0.1,DNS:localhost.localdomain",
        ],
        check=True,
        capture_output=True,
    )
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)

    def run(
        reply=250,
        tls=True,
        allow=True,
        trust=True,
        rejection=None,
        reject_at="RCPT",
        ipv6_listener=False,
        allow_ipv6=False,
    ):
        listener = socket.socket(socket.AF_INET6 if ipv6_listener else socket.AF_INET)
        if ipv6_listener:
            listener.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
        listener.bind(("::1" if ipv6_listener else "127.0.0.1", 0))
        listener.listen()
        listener.settimeout(4)
        port = listener.getsockname()[1]
        state = {"tls": False, "body": b"", "commands": []}
        errors = []

        def serve():
            try:
                connection, _ = listener.accept()
                connection.settimeout(4)
                with connection:
                    stream = connection.makefile("rb")
                    connection.sendall(b"220 mx.example.test ESMTP\r\n")
                    while True:
                        line = stream.readline()
                        state["commands"].append(line)
                        if not line:
                            break
                        if line.startswith(b"EHLO"):
                            if reject_at == "EHLO":
                                connection.sendall(rejection)
                            else:
                                connection.sendall(
                                    b"250-mx.example.test\r\n"
                                    + (
                                        b"250-STARTTLS\r\n"
                                        if tls and not state["tls"]
                                        else b""
                                    )
                                    + b"250 SIZE 100000\r\n"
                                )
                        elif line == b"STARTTLS\r\n":
                            connection.sendall(b"220 Ready\r\n")
                            stream.close()
                            connection = context.wrap_socket(
                                connection, server_side=True
                            )
                            stream = connection.makefile("rb")
                            state["tls"] = True
                        elif line.startswith(b"MAIL FROM:"):
                            connection.sendall(b"250 Sender OK\r\n")
                        elif line.startswith(b"RCPT TO:"):
                            connection.sendall(
                                rejection
                                if rejection and reject_at == "RCPT"
                                else f"{reply} Recipient\r\n".encode()
                            )
                        elif line == b"DATA\r\n":
                            assert state["tls"]
                            connection.sendall(b"354 Continue\r\n")
                            while True:
                                line = stream.readline()
                                if line in (b".\r\n", b""):
                                    break
                                state["body"] += line
                            connection.sendall(
                                rejection
                                if rejection and reject_at == "DATA"
                                else b"250 Accepted\r\n"
                            )
                        elif line == b"QUIT\r\n":
                            connection.sendall(b"221 Bye\r\n")
                            break
                        else:
                            connection.sendall(b"500 Unsupported\r\n")
                    stream.close()
                    connection.close()
            except (TimeoutError, ssl.SSLError, ConnectionError):
                pass
            except Exception as e:
                errors.append(e)
            finally:
                listener.close()

        thread = threading.Thread(target=serve)
        thread.start()
        env = dict(os.environ)
        env.pop("CLOUDIEMAIL_CA_BUNDLE", None)
        if trust:
            env["CLOUDIEMAIL_CA_BUNDLE"] = str(cert)
        arguments = [
            str(binary),
            str(port),
            "allow-loopback-test" if allow else "public-only",
        ]
        if ipv6_listener:
            arguments += [
                "localhost.localdomain",
                "allow-ipv6" if allow_ipv6 else "ipv4-only",
            ]
        result = subprocess.run(
            arguments, env=env, text=True, capture_output=True, timeout=15
        )
        thread.join(5)
        assert result.returncode == 0, result.stderr
        assert not errors, errors
        return json.loads(result.stdout), state

    result, state = run()
    assert result["success"], result
    assert state["tls"] and b"..dot\r\n" in state["body"]
    assert not any(x.startswith(b"AUTH") for x in state["commands"])
    print(
        "PASS direct MX transport uses STARTTLS, correct envelope and dot escaping; no external relay AUTH"
    )
    result, _ = run(reply=451)
    assert (
        not result["success"]
        and not result["permanent"]
        and "451 Recipient" in result["error"]
    )
    print("PASS SMTP 451 temporary classification and server explanation")
    result, _ = run(reply=550)
    assert (
        not result["success"]
        and result["permanent"]
        and "550 Recipient" in result["error"]
    )
    print("PASS SMTP 550 permanent classification and server explanation")
    for stage in ("RCPT", "DATA"):
        result, _ = run(
            rejection=b"550-5.7.26 Authentication failed\r\n550 5.7.26 SPF and DKIM required\r\n",
            reject_at=stage,
        )
        assert (
            result["permanent"]
            and "Authentication failed" in result["error"]
            and "SPF and DKIM required" in result["error"]
            and "[MX: 127.0.0.1]" in result["error"]
        ), result
        assert (
            "transport test" not in result["error"] and "hello" not in result["error"]
        )
        print(
            "PASS multiline rejection preserved at "
            + stage
            + " without outgoing message content"
        )
    result, _ = run(rejection=b"550 5.7.1 " + b"x" * 1200 + b"\x1b\t\r\n")
    assert (
        result["permanent"]
        and len(result["error"]) <= 500
        and "\x1b" not in result["error"]
    )
    print("PASS bounded rejection text")
    result, state = run(tls=False)
    assert not result["success"] and not state["body"]
    print("PASS TLS downgrade refused")
    result, state = run(trust=False)
    assert not result["success"] and not state["body"]
    print("PASS untrusted certificate refused")
    result, state = run(allow=False)
    assert not result["success"] and not state["body"]
    print("PASS private MX destination refused")
    result, state = run(ipv6_listener=True)
    assert not result["success"] and not state["commands"], result
    print("PASS default delivery never connects to IPv6-only listener")
    result, state = run(ipv6_listener=True, allow_ipv6=True)
    assert result["success"] and state["tls"], result
    print("PASS explicit IPv6 opt-in connects with verified STARTTLS")
