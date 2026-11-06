"""Validate the helper against an isolated Apache installation, never /etc on the host.
Usage: python3 tests/apache_test.py /tmp/extracted-apache-root
"""

import http.client
import http.server
import importlib.util
import json
import os
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "cm_apache",
    Path(__file__).resolve().parents[1] / "scripts/runtime/configure-apache.py",
)
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)
for bad in ["a.test\nListen 123", "../a.test", 'a".test']:
    try:
        helper.render(bad, 8787, "/c", "/k", {}, False)
        raise AssertionError("injection accepted")
    except ValueError:
        pass
installation = Path(sys.argv[1]).resolve()
httpd = installation / "usr/bin/httpd"
modules = installation / "usr/lib/httpd/modules"
with tempfile.TemporaryDirectory(prefix="cloudiemail-apache-test-") as temp:
    root = Path(temp)
    main = root / "etc/httpd/conf/httpd.conf"
    main.parent.mkdir(parents=True)
    (root / "run").mkdir()
    (root / "htdocs").mkdir()
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            self.send_response(
                200 if self.headers["Host"] == "webmail.example.test" else 400
            )
            self.end_headers()
            self.wfile.write(b"proxied CloudieMail backend")

        def log_message(self, *args):
            pass

    backend = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=backend.serve_forever, daemon=True).start()
    base = f'ServerRoot "{root}"\nServerName localhost\nPidFile "{root}/run/httpd.pid"\nErrorLog "{root}/error.log"\nDocumentRoot "{root}/htdocs"\n'
    for name in ["mpm_event", "unixd", "authz_core"]:
        base += f'LoadModule {name}_module "{modules}/mod_{name}.so"\n'
    # A separate pre-existing site remains bound to the shared TLS listener.
    cert = root / "source.crt"
    key = root / "source.key"
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
            "/CN=webmail.example.test",
            "-addext",
            "subjectAltName=DNS:webmail.example.test",
        ],
        check=True,
        capture_output=True,
    )
    base += f'LoadModule ssl_module "{modules}/mod_ssl.so"\nListen 127.0.0.1:{port} https\n<VirtualHost *:{port}>\nServerName other.example.test\nSSLEngine on\nSSLCertificateFile "{cert}"\nSSLCertificateKeyFile "{key}"\n</VirtualHost>\n'
    main.write_text(base)
    main.chmod(0o640)
    config = root / "config.json"
    config.write_text(
        json.dumps(
            dict(
                domain="example.test",
                webmail_backend_port=backend.server_port,
                certificate=str(cert),
                private_key=str(key),
            )
        )
    )

    def mapped(value):
        value = str(value)
        if value.startswith("/etc/"):
            return root / value.lstrip("/")
        if value.startswith("/usr/") and value != "/usr/bin/openssl":
            return installation / value.lstrip("/")
        return Path(value)

    env = dict(os.environ, LD_LIBRARY_PATH=str(installation / "usr/lib"))
    calls = []
    fail = False

    def command(args):
        global fail
        if Path(args[0]).name in ("httpd", "apachectl"):
            if "-k" in args:
                calls.append("reload")
                if fail:
                    raise RuntimeError("simulated reload failure")
                return ""
            args = [httpd, "-f", main, *args[1:]]
        result = subprocess.run(
            [str(a) for a in args], capture_output=True, text=True, env=env, timeout=20
        )
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        return result.stdout + result.stderr

    original_render = helper.render

    def render(*args):
        return (
            original_render(*args)
            .replace("Listen 443 https", "")
            .replace("<VirtualHost *:443>", f"<VirtualHost *:{port}>")
        )

    with (
        patch.object(helper, "Path", mapped),
        patch.object(helper.os, "geteuid", return_value=0),
        patch.object(helper, "command", command),
        patch.object(helper, "render", render),
    ):
        helper.apply(config)
        target = main.parent / "cloudiemail/example.test.conf"
        assert target.exists()
        assert base.rstrip() in main.read_text()
        assert main.stat().st_mode & 0o777 == 0o640
        saved = target.read_bytes()
        main_saved = main.read_bytes()
        fail = True
        try:
            helper.apply(config)
            raise AssertionError("reload failure ignored")
        except RuntimeError as e:
            assert "simulated" in str(e)
        assert (
            target.read_bytes() == saved
            and main.read_bytes() == main_saved
            and main.stat().st_mode & 0o777 == 0o640
        )
        fail = False
        process = subprocess.Popen(
            [str(httpd), "-f", str(main), "-DFOREGROUND"],
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        try:
            for _ in range(40):
                try:
                    connection = http.client.HTTPSConnection(
                        "127.0.0.1",
                        port,
                        context=ssl._create_unverified_context(),
                        timeout=2,
                    )
                    connection.sock = ssl._create_unverified_context().wrap_socket(
                        socket.create_connection(("127.0.0.1", port), timeout=2),
                        server_hostname="webmail.example.test",
                    )
                    connection.request(
                        "GET", "/", headers={"Host": "webmail.example.test"}
                    )
                    response = connection.getresponse()
                    body = response.read()
                    connection.close()
                    assert response.status == 200 and b"proxied CloudieMail" in body, (
                        response.status,
                        body,
                    )
                    break
                except ConnectionRefusedError:
                    time.sleep(0.1)
            else:
                raise AssertionError("Apache did not start")
        finally:
            process.terminate()
            process.wait(timeout=10)
        helper.apply(config, True)
        assert not target.exists()
        assert main.read_bytes() == base.encode()
    backend.shutdown()
    print(
        "Apache: real syntax validation, shared TLS vhosts, Host-preserving proxy, rollback and removal passed"
    )
