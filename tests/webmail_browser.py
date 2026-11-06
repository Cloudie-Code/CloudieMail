"""End-to-end browser tests. Requires Playwright and Chromium; never sends real email."""

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
from pathlib import Path

from playwright.sync_api import expect, sync_playwright

ROOT = Path(__file__).resolve().parents[1]
binary = (
    Path(sys.argv[1]).resolve()
    if len(sys.argv) > 1
    else ROOT / "build/linux-release/cloudiemail"
)
with tempfile.TemporaryDirectory(prefix="cloudiemail-browser-") as temporary:
    root = Path(temporary)
    screenshots = Path(
        os.environ.get("CLOUDIEMAIL_SCREENSHOTS", str(root / "screenshots"))
    )
    screenshots.mkdir(parents=True, exist_ok=True)
    password = "webmail browser test password"
    reservations = [socket.socket() for _ in range(6)]
    for s in reservations:
        s.bind(("127.0.0.1", 0))
    ports = [s.getsockname()[1] for s in reservations]
    config = dict(
        domain="example.test",
        bind="127.0.0.1",
        database=str(root / "mail.fdb"),
        certificate=str(root / "tls.crt"),
        private_key=str(root / "tls.key"),
        web_root=str(ROOT / "web"),
        smtp_port=ports[0],
        smtps_port=ports[1],
        imap_port=ports[2],
        pop3_port=ports[3],
        https_port=ports[4],
        submission_port=ports[5],
        outbound_enabled=True,
    )
    path = root / "config.json"
    path.write_text(json.dumps(config))
    port = ports[4]
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
            "subjectAltName=DNS:webmail.example.test,DNS:mail.example.test,IP:127.0.0.1",
        ],
        check=True,
        capture_output=True,
    )
    subprocess.run([str(binary), "init", str(path)], check=True, capture_output=True)
    subprocess.run(
        [str(binary), "account-add", str(path), "alice@example.test"],
        input=password + "\n" + password + "\n",
        text=True,
        check=True,
        capture_output=True,
    )
    for s in reservations:
        s.close()
    process = subprocess.Popen(
        [str(binary), "serve", str(path)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )
    try:
        for _ in range(100):
            try:
                with socket.create_connection(("127.0.0.1", ports[0]), timeout=0.1):
                    break
            except OSError:
                time.sleep(0.1)
        else:
            raise AssertionError("server did not start")
        ctx = ssl.create_default_context(cafile=config["certificate"])
        with smtplib.SMTP("127.0.0.1", config["submission_port"]) as smtp:
            smtp.ehlo()
            assert smtp.mail("alice@example.test")[0] == 530
            smtp.starttls(context=ctx)
            smtp.ehlo()
            smtp.login("alice@example.test", password)
            smtp.sendmail(
                "alice@example.test",
                ["alice@example.test"],
                "From: forged@other.test\r\nTo: alice@example.test\r\nSubject: Bem-vindo ao CloudieMail\r\n\r\nMensagem recebida com armazenamento criptografado.\r\n",
            )
        with sync_playwright() as p:
            browser = p.chromium.launch(
                args=[
                    "--no-proxy-server",
                    "--host-resolver-rules=MAP webmail.example.test 127.0.0.1",
                ]
            )
            context = browser.new_context(
                ignore_https_errors=True,
                viewport={"width": 1440, "height": 980},
                locale="pt-BR",
            )
            page = context.new_page()
            errors = []
            page.on("pageerror", lambda e: errors.append(str(e)))
            page.goto(f"https://webmail.example.test:{port}/")
            expect(page.locator("#login-button")).to_have_text("Entrar")
            page.screenshot(path=str(screenshots / "webmail-login.png"))
            page.locator("#email").fill("alice@example.test")
            page.locator("#password").fill(password)
            page.locator("#login-button").click()
            expect(page.locator("#mail-screen")).to_be_visible()
            expect(page.locator(".message-card")).to_have_count(1)
            page.locator(".message-card").click()
            expect(page.locator("#message-body")).to_contain_text("Mensagem recebida")
            expect(page.locator("#message-from")).to_have_text("alice@example.test")
            page.locator("#new-folder").click()
            page.locator("#edit-value").fill("Projetos")
            page.locator("#edit-form button[type=submit]").click()
            expect(page.locator("#folder-title")).to_have_text("Projetos")
            page.get_by_role("button", name="Entrada", exact=False).click()
            page.locator(".message-card").click()
            page.locator("#labels-message").click()
            page.locator("#edit-value").fill("Trabalho, 重要")
            page.locator("#edit-form button[type=submit]").click()
            expect(page.locator("#edit-dialog")).not_to_be_visible()
            page.locator(".message-card").click()
            page.locator("#star-message").click()
            expect(page.locator("#messages .icon")).to_have_count(1)
            page.locator(".message-card").click()
            page.locator("#move-message").select_option("Projetos")
            expect(page.locator(".message-card")).to_have_count(0)
            page.get_by_role("button", name="Projetos", exact=False).click()
            expect(page.locator(".message-card")).to_have_count(1)
            page.locator(".message-card").click()
            expect(page.locator("#message-body")).to_contain_text("Mensagem recebida")
            page.screenshot(path=str(screenshots / "webmail.png"))
            page.locator("#compose").click()
            page.locator("#to").fill("alice@example.test")
            page.locator("#subject").fill("こんにちは 한국어 中文")
            page.locator("#body").fill("Olá!\nTexto UTF-8 seguro.")
            page.locator("#compose-files").set_input_files(
                {
                    "name": "日本語.txt",
                    "mimeType": "text/plain",
                    "buffer": b"attachment test",
                }
            )
            page.locator("#save-draft").click()
            expect(page.locator("#compose-dialog")).not_to_be_visible()
            page.get_by_role("button", name="Rascunhos", exact=False).click()
            expect(page.locator(".message-card")).to_have_count(1)
            page.locator(".message-card").click()
            expect(page.locator("#message-body")).to_contain_text("Texto UTF-8 seguro.")
            expect(page.locator("#attachments")).to_contain_text("日本語.txt")
            page.locator("#edit-draft").click()
            page.locator("#send-button").click()
            expect(page.locator("#compose-dialog")).not_to_be_visible()
            expect(page.locator(".message-card")).to_have_count(0)
            page.get_by_role("button", name="Entrada", exact=False).click()
            expect(page.locator(".message-card")).to_have_count(1)
            page.locator(".message-card").click()
            expect(page.locator("#message-subject")).to_have_text(
                "こんにちは 한국어 中文"
            )
            page.locator("#search").fill("こんにちは")
            expect(page.locator(".message-card")).to_have_count(1)
            page.wait_for_timeout(500)
            expect(page.locator(".message-card")).to_have_count(1)
            page.locator("#search").fill("")
            page.wait_for_timeout(400)
            for lang in ["en", "ja", "ko", "zh-TW", "pt-BR"]:
                page.locator("#mail-language").select_option(lang)
                assert page.locator("html").get_attribute("lang") == lang
                assert not page.locator("#compose").inner_text().startswith("mail.")
            page.locator("#compose").click()
            page.locator("#to").fill("nobody@recipient.invalid")
            page.locator("#subject").fill("Reserved-domain queue test")
            page.locator("#body").fill("No real recipient.")
            page.locator("#send-button").click()
            expect(page.locator("#compose-dialog")).not_to_be_visible()
            page.get_by_role("button", name="Entregas de saída", exact=True).click()
            expect(page.locator(".delivery-card")).to_have_count(1)
            expect(page.locator(".delivery-card")).to_contain_text(
                "nobody@recipient.invalid"
            )
            page.set_viewport_size({"width": 390, "height": 844})
            page.screenshot(path=str(screenshots / "webmail-mobile.png"))
            page.locator("#navigation-toggle").click()
            page.locator("#logout").click()
            expect(page.locator("#login-screen")).to_be_visible()
            assert page.locator("#message-body").inner_text() == ""
            assert not errors, errors
            browser.close()
            print(
                "Browser checks: SMTP AUTH, sender identity, login, folders, labels, starred, move, encrypted drafts, attachments, Unicode, 5 languages, outbound queue, logout: passed"
            )
    finally:
        process.send_signal(signal.SIGINT)
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            raise
