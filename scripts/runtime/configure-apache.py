#!/usr/bin/env python3
"""Install/remove only CloudieMail's vhost; validate and roll back before reload."""

import argparse
import json
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

MARKER = "# Managed by CloudieMail: "


def render(domain, backend, cert, key, modules, listen):
    if (
        not re.fullmatch(r"[a-z0-9](?:[a-z0-9.-]{0,251}[a-z0-9])?", domain)
        or ".." in domain
        or "." not in domain
    ):
        raise ValueError("Invalid domain")
    if not isinstance(backend, int) or not 1024 <= backend <= 65535 or backend == 443:
        raise ValueError("Backend port must be between 1024 and 65535, except 443")
    for path in (cert, key, *modules.values()):
        if any(c in str(path) for c in '\r\n"'):
            raise ValueError("Invalid Apache path")
    lines = [MARKER + domain]
    for name, path in modules.items():
        lines += [
            f"<IfModule !{name}_module>",
            f'  LoadModule {name}_module "{path}"',
            "</IfModule>",
        ]
    if listen:
        lines.append("Listen 443 https")
    lines += [
        "<VirtualHost *:443>",
        f"  ServerName webmail.{domain}",
        "  SSLEngine on",
        "  SSLProtocol -all +TLSv1.2 +TLSv1.3",
        f'  SSLCertificateFile "{cert}"',
        f'  SSLCertificateKeyFile "{key}"',
        "  ProxyRequests Off",
        "  ProxyPreserveHost On",
        "  ProxyAddHeaders Off",
        f"  ProxyPass / http://127.0.0.1:{backend}/ connectiontimeout=5 timeout=30",
        f"  ProxyPassReverse / http://127.0.0.1:{backend}/",
        "  LimitRequestBody 26214400",
        "  LimitRequestFields 64",
        "  LimitRequestFieldSize 8192",
        "  RequestReadTimeout header=15-30,MinRate=500 body=30,MinRate=500",
        "</VirtualHost>",
        "",
    ]
    return "\n".join(lines)


def command(args):
    result = subprocess.run(
        [str(a) for a in args],
        capture_output=True,
        text=True,
        timeout=45,
        env={"PATH": "/usr/sbin:/usr/bin:/sbin:/bin", "LANG": "C"},
    )
    if result.returncode:
        raise RuntimeError(
            (result.stdout + result.stderr)[-4000:] or f"{args[0]} failed"
        )
    return result.stdout + result.stderr


def atomic(path, data, mode=0o600):
    if path.is_symlink():
        raise RuntimeError(f"Refusing symbolic link: {path}")
    fd, temporary = tempfile.mkstemp(prefix=".cloudiemail-", dir=path.parent)
    try:
        os.fchmod(fd, mode)
        with os.fdopen(fd, "wb") as out:
            out.write(data)
            out.flush()
            os.fsync(out.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def apply(config_path, disable=False):
    if os.geteuid() != 0:
        raise RuntimeError(
            "Administrator authentication is required to configure Apache"
        )
    config_path = config_path.resolve()
    config = json.loads(config_path.read_text())
    domain = config["domain"]
    # Validate all interpolated values before any privileged mutation.
    render(
        domain,
        config.get("webmail_backend_port", 8787),
        "/placeholder",
        "/placeholder",
        {},
        False,
    )
    apache = next(
        (
            Path(p)
            for p in (
                "/usr/sbin/apache2ctl",
                "/usr/sbin/apachectl",
                "/usr/bin/apachectl",
                "/usr/sbin/httpd",
                "/usr/bin/httpd",
            )
            if Path(p).is_file()
        ),
        None,
    )
    if apache is None:
        raise RuntimeError(
            "Apache 2.4 is not installed; install it before selecting Apache integration"
        )
    main = next(
        (
            Path(p)
            for p in ("/etc/apache2/apache2.conf", "/etc/httpd/conf/httpd.conf")
            if Path(p).is_file()
        ),
        None,
    )
    if main is None:
        raise RuntimeError(
            "Unsupported Apache layout: expected /etc/apache2 or /etc/httpd/conf"
        )
    command([apache, "-t"])
    included = command([apache, "-t", "-D", "DUMP_INCLUDES"])
    target_dir = main.parent / "cloudiemail"
    if target_dir.is_symlink():
        raise RuntimeError("Unsafe Apache configuration directory")
    target_dir.mkdir(mode=0o755, exist_ok=True)
    target = target_dir / f"{domain}.conf"
    original = target.read_bytes() if target.exists() else None
    if target.is_symlink() or (
        original is not None
        and not original.startswith((MARKER + domain + "\n").encode())
    ):
        raise RuntimeError(
            "Existing vhost belongs to another application; it was preserved"
        )
    main_before = main.read_bytes()
    include_line = (
        f'# CloudieMail include: {domain}\nIncludeOptional "{target}"\n'.encode()
    )
    main_after = main_before.replace(include_line, b"")
    if not disable:
        main_after = main_after.rstrip() + b"\n" + include_line
    cert_dir = Path("/etc/cloudiemail") / domain
    if cert_dir.is_symlink() or cert_dir.parent.is_symlink():
        raise RuntimeError("Unsafe certificate directory")
    cert_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.chmod(cert_dir, 0o700)
    cert, key = cert_dir / "webmail.crt", cert_dir / "webmail.key"
    backups = {main: main_before, target: original}
    modes = {p: p.stat().st_mode & 0o777 for p in backups if p.exists()}
    if not disable:
        source_cert = (config_path.parent / config["certificate"]).resolve()
        source_key = (config_path.parent / config["private_key"]).resolve()
        if source_cert.stat().st_size > 1048576 or source_key.stat().st_size > 65536:
            raise RuntimeError("Certificate input is too large")
        check = command(
            [
                "/usr/bin/openssl",
                "x509",
                "-in",
                source_cert,
                "-noout",
                "-checkhost",
                f"webmail.{domain}",
            ]
        )
        if "does match certificate" not in check:
            raise RuntimeError("Certificate does not cover the webmail hostname")
        if command(
            ["/usr/bin/openssl", "x509", "-in", source_cert, "-pubkey", "-noout"]
        ) != command(
            [
                "/usr/bin/openssl",
                "pkey",
                "-in",
                source_key,
                "-pubout",
                "-passin",
                "pass:",
            ]
        ):
            raise RuntimeError("Certificate and private key do not match")
        modules = {}
        for name in ("ssl", "proxy", "proxy_http", "reqtimeout"):
            module = next(
                (
                    Path(directory) / f"mod_{name}.so"
                    for directory in (
                        "/usr/lib/apache2/modules",
                        "/usr/lib64/httpd/modules",
                        "/usr/lib/httpd/modules",
                        "/etc/httpd/modules",
                    )
                    if (Path(directory) / f"mod_{name}.so").is_file()
                ),
                None,
            )
            if module is None:
                raise RuntimeError(f"Apache module {name} is not installed")
            modules[name] = module
        listen = True
        for name in re.findall(r"(/[^\r\n]+)", included):
            path = Path(name.strip())
            if path == target or not path.is_file():
                continue
            if re.search(
                r"^\s*Listen\s+(?:\S+:)?443(?:\s|$)",
                path.read_text(errors="replace"),
                re.M,
            ):
                listen = False
        body = render(
            domain, config.get("webmail_backend_port", 8787), cert, key, modules, listen
        ).encode()
        for path in (cert, key):
            if path.is_symlink():
                raise RuntimeError("Unsafe certificate destination")
            backups[path] = path.read_bytes() if path.exists() else None
            if path.exists():
                modes[path] = path.stat().st_mode & 0o777
    try:
        if disable:
            target.unlink(missing_ok=True)
        else:
            atomic(cert, source_cert.read_bytes())
            atomic(key, source_key.read_bytes())
            atomic(target, body, 0o644)
        atomic(main, main_after, main.stat().st_mode & 0o777)
        command([apache, "-t"])
        command([apache, "-k", "graceful"])
    except Exception:
        for path, content in backups.items():
            if content is None:
                path.unlink(missing_ok=True)
            else:
                atomic(path, content, modes.get(path, 0o600))
        raise
    print(
        "Apache: CloudieMail virtual host removed."
        if disable
        else f"Apache: https://webmail.{domain}/ configured and gracefully reloaded."
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("config", type=Path)
    parser.add_argument("--disable", action="store_true")
    args = parser.parse_args()
    try:
        apply(args.config, args.disable)
    except Exception as error:
        print(str(error))
        raise SystemExit(1)
