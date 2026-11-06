"""Exercise IMAP transactions, MIME, concurrency and spam routing over real TLS."""

import contextlib
import imaplib
import json
import smtplib
import socket
import ssl
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(sys.argv[1]).resolve()
PASSWORD = "imap integration 73! password"
checks = 0
imaplib.Commands["ID"] = ("NONAUTH", "AUTH", "SELECTED")


def check(value, description):
    global checks
    assert value, description
    checks += 1
    print("PASS", description, flush=True)


with tempfile.TemporaryDirectory(prefix="cloudiemail-imap-") as tmp:
    root = Path(tmp)
    sockets = [socket.socket() for _ in range(6)]
    for s in sockets:
        s.bind(("127.0.0.1", 0))
    ports = [s.getsockname()[1] for s in sockets]
    config = dict(
        domain="example.test",
        bind="127.0.0.1",
        database=str(root / "mail.fdb"),
        certificate=str(root / "cert.pem"),
        private_key=str(root / "key.pem"),
        web_root=str(ROOT / "web"),
        smtp_port=ports[0],
        smtps_port=ports[1],
        imap_port=ports[2],
        pop3_port=ports[3],
        https_port=ports[4],
        submission_port=ports[5],
        outbound_enabled=False,
        antispam_dns=False,
        max_message_bytes=131072,
        max_connections=16,
        mailbox_quota_bytes=1048576,
    )
    path = root / "config.json"
    path.write_text(json.dumps(config))
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
            "/CN=mail.example.test",
            "-addext",
            "subjectAltName=DNS:mail.example.test,DNS:webmail.example.test,IP:127.0.0.1",
        ],
        check=True,
        capture_output=True,
    )

    def run(*args, password=None):
        subprocess.run(
            [str(BINARY), *map(str, args)],
            input=password,
            text=True,
            check=True,
            capture_output=True,
            timeout=30,
        )

    run("init", path)
    for user in ("alice", "bob"):
        run(
            "account-add",
            path,
            user + "@example.test",
            password=PASSWORD + "\n" + PASSWORD + "\n",
        )
    for s in sockets:
        s.close()
    context = ssl.create_default_context(cafile=config["certificate"])
    log = open(root / "server.log", "w+")
    process = subprocess.Popen(
        [str(BINARY), "serve", str(path)], stdout=log, stderr=log
    )

    def client(user="alice"):
        c = imaplib.IMAP4_SSL("127.0.0.1", ports[2], ssl_context=context, timeout=12)
        c.login(user + "@example.test", PASSWORD)
        return c

    def send(body, authenticated=False):
        with smtplib.SMTP("127.0.0.1", ports[0], timeout=12) as s:
            s.starttls(context=context)
            if authenticated:
                s.login("alice@example.test", PASSWORD)
            s.sendmail(
                "alice@example.test" if authenticated else "sender@outside.test",
                ["alice@example.test"],
                body,
            )

    simple = b"From: Sender <sender@outside.test>\r\nTo: alice@example.test\r\nDate: Fri, 6 Nov 2026 12:00:00 +0000\r\nMessage-ID: <one@outside.test>\r\nSubject: =?UTF-8?B?T2zDoSBtdW5kbw==?=\r\n\r\nIMAP-PRIVATE-SENTINEL\r\n"
    multipart = (
        b'From: "Sender, One" <sender@outside.test>\r\nTo: Friends: A <a@example.test>, B <b@example.test>;\r\nDate: Fri, 6 Nov 2026 12:00:00 +0000\r\nSubject: Attachment\r\nMessage-ID: <two@outside.test>\r\nMIME-Version: 1.0\r\nContent-Type: multipart/mixed; boundary="test-boundary"\r\n\r\n'
        b"--test-boundary\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Transfer-Encoding: quoted-printable\r\n\r\nOl=C3=A1 decoded text\r\n"
        b'--test-boundary\r\nContent-Type: application/octet-stream; name="hello.txt"\r\nContent-Disposition: attachment; filename="hello.txt"\r\nContent-Transfer-Encoding: base64\r\n\r\naGVsbG8gYXR0YWNobWVudA==\r\n--test-boundary--\r\n'
    )
    try:
        for _ in range(100):
            if process.poll() is not None:
                log.seek(0)
                raise AssertionError(log.read())
            try:
                with socket.create_connection(("127.0.0.1", ports[0]), 0.2):
                    break
            except OSError:
                time.sleep(0.1)
        with client() as c:
            check(
                {"IMAP4REV1", "IDLE", "UIDPLUS", "MOVE"} <= set(c.capabilities),
                "capabilities",
            )
            check(c.namespace()[0] == "OK", "personal namespace")
            check(c._simple_command("ID", "NIL")[0] == "OK", "client identification")
            typ, _ = c._simple_command("LIST", "(SPECIAL-USE)", '""', '"*"')
            special = c.response("LIST")[1]
            check(
                typ == "OK" and len(special) == 4 and b"\\Junk" in b" ".join(special),
                "special-use mailbox discovery",
            )
            check(
                c.list()[0] == "OK" and b'"Sent"' in b" ".join(c.list()[1]),
                "all built-in folders listed",
            )
            check(
                c.create('"Projects/2026"')[0] == "OK", "hierarchical mailbox creation"
            )
            check(
                b"Noselect" in b" ".join(c.list('""', '"Projects"')[1]),
                "nonselectable parent",
            )
            check(c.subscribe('"Projects/2026"')[0] == "OK", "subscribe")
            check(b"Projects/2026" in b" ".join(c.lsub()[1]), "subscription persisted")
            encoded = "&ZeVnLIqe-"  # 日本語
            check(c.create('"' + encoded + '"')[0] == "OK", "modified UTF-7 mailbox")
            check(
                c.append(
                    '"' + encoded + '"',
                    r"(\Seen \Draft $Label1)",
                    '"06-Nov-2026 12:00:00 +0000"',
                    simple,
                )[0]
                == "OK",
                "APPEND with date and flags",
            )
            check(c.select('"' + encoded + '"')[1] == [b"1"], "select Unicode mailbox")
            c.fetch("1", "(FLAGS INTERNALDATE)")
            check(
                b"\\Draft" in b" ".join(c.fetch("1", "FLAGS")[1]),
                "persistent draft flag",
            )
            check(
                c.rename('"Projects"', '"Work"')[0] == "OK",
                "rename nonselectable parent",
            )
            check(
                c.rename('"Work/2026"', '"Work/Archive"')[0] == "OK",
                "rename child mailbox",
            )
            c.select("INBOX")
            check(c.append("INBOX", None, None, simple)[0] == "OK", "append INBOX")
            check(
                c.append("INBOX", None, None, multipart)[0] == "OK", "append multipart"
            )
            c.select("INBOX")
            uids = c.uid("SEARCH", None, "ALL")[1][0].split()
            first, second = map(bytes.decode, uids)
            check(len(uids) == 2, "UID SEARCH")
            code, data = c.uid(
                "FETCH",
                second,
                "(UID FLAGS INTERNALDATE RFC822.SIZE ENVELOPE BODY BODYSTRUCTURE)",
            )
            check(
                code == "OK"
                and b'"MIXED"' in data[0]
                and b'"BASE64"' in data[0]
                and b'"Sender, One"' in data[0],
                "envelope and multipart structures",
            )
            check(
                c.uid("FETCH", second, "BODY.PEEK[2]")[1][0][1]
                == b"aGVsbG8gYXR0YWNobWVudA==",
                "attachment section",
            )
            check(
                c.uid("FETCH", second, "BODY.PEEK[2]<4.8>")[1][0][1] == b"bG8gYXR0",
                "partial attachment fetch",
            )
            check(
                b"Content-Disposition:"
                in c.uid("FETCH", second, "BODY.PEEK[2.MIME]")[1][0][1],
                "part MIME headers",
            )
            header = c.uid(
                "FETCH", second, "(BODY.PEEK[HEADER.FIELDS (SUBJECT FROM)])"
            )[1][0][1]
            check(
                b"Subject:" in header
                and b"From:" in header
                and b"To:" not in header
                and header.endswith(b"\r\n\r\n"),
                "filtered headers",
            )
            header = c.uid("FETCH", second, "(BODY.PEEK[HEADER.FIELDS.NOT (SUBJECT)])")[
                1
            ][0][1]
            check(
                b"Subject:" not in header and b"From:" in header,
                "negative header selection",
            )
            check(
                c.uid("SEARCH", None, "UNSEEN")[1][0].split() == uids,
                "PEEK does not mark seen",
            )
            check(
                c.uid("SEARCH", "CHARSET", "UTF-8", "BODY", '"decoded"')[1][0]
                == second.encode(),
                "search decodes MIME",
            )
            check(
                c.uid(
                    "SEARCH",
                    None,
                    "OR",
                    "SUBJECT",
                    '"Attachment"',
                    "HEADER",
                    '"Message-ID"',
                    '"one@"',
                )[1][0].split()
                == uids,
                "OR and HEADER search",
            )
            check(
                c.uid("SEARCH", None, "NOT", "SUBJECT", '"Attachment"')[1][0]
                == first.encode(),
                "NOT search",
            )
            check(
                c.uid("SEARCH", None, "SENTON", "6-Nov-2026")[1][0].split() == uids,
                "date search",
            )
            check(
                c.uid("SEARCH", None, "UID", first + ":" + second)[1][0].split()
                == uids,
                "UID criterion",
            )
            check(
                c.search(None, "(OR SEEN UNSEEN)", "SMALLER", "10000")[1][0] == b"1 2",
                "grouped boolean and size search",
            )
            check(
                c.uid("FETCH", second, "BODY.PEEK[99]")[0] == "OK",
                "missing part returns NIL",
            )
            nested = (
                b"From: a@example.test\r\nContent-Type: message/rfc822\r\n\r\n" + simple
            )
            c.append("Drafts", None, None, nested)
            with client() as embedded:
                embedded.select("Drafts")
                check(
                    embedded.fetch("1", "BODY.PEEK[1]")[1][0][1] == simple,
                    "message/rfc822 outer section",
                )
                check(
                    b"Subject:" in embedded.fetch("1", "BODY.PEEK[1.HEADER]")[1][0][1],
                    "embedded message headers",
                )
                check(
                    embedded.fetch("1", "BODY.PEEK[1.1]")[1][0][1]
                    == b"IMAP-PRIVATE-SENTINEL\r\n",
                    "embedded message body section",
                )
            check(
                c.append("Missing", None, None, simple)[0] == "NO"
                and c.response("TRYCREATE")[1][0] is not None,
                "APPEND returns TRYCREATE",
            )
            c.uid("FETCH", first, "BODY[TEXT]")
            check(
                c.uid("SEARCH", None, "SEEN")[1][0] == first.encode(),
                "body read marks seen",
            )
            check(
                c.uid("STORE", first, "+FLAGS.SILENT", r"(\Answered \Flagged $Custom)")[
                    0
                ]
                == "OK",
                "store standard flags and keywords",
            )
            with client() as other:
                other.select("INBOX")
                check(
                    b"\\Answered" in other.uid("FETCH", first, "FLAGS")[1][0],
                    "flags visible across connections",
                )
                other.uid("STORE", first, "+FLAGS", r"(\Deleted)")
            check(
                c.uid("SEARCH", None, "DELETED")[1][0] == first.encode(),
                "deleted flag survives disconnect",
            )
            c.select("INBOX", readonly=True)
            check(
                c.uid("STORE", first, "FLAGS", r"(\Seen)")[0] == "NO",
                "EXAMINE blocks writes",
            )
            check(c.uid("MOVE", first, "Sent")[0] == "NO", "EXAMINE blocks moves")
            c.select("INBOX")
            check(c.uid("COPY", second, "Sent")[0] == "OK", "copy message")
            c.select("Sent")
            copied = c.uid("SEARCH", None, "ALL")[1][0].decode()
            check(int(copied) > int(second), "copy assigns fresh UID")
            check(
                c.uid("FETCH", copied, "BODY.PEEK[]")[1][0][1] == multipart,
                "copy preserves ciphertext content",
            )
            c.select("INBOX")
            check(c.uid("MOVE", second, "Trash")[0] == "OK", "atomic move")
            check(
                c.uid("SEARCH", None, "ALL")[1][0] == first.encode(),
                "move expunges source",
            )
            check(c.uid("EXPUNGE", first)[0] == "OK", "UID EXPUNGE")
            check(c.search(None, "ALL")[1] == [b""], "expunge removes deleted message")
            c.append("INBOX", None, None, simple)
            c.store("1", "+FLAGS", r"(\Deleted)")
            check(c.unselect()[0] == "OK", "UNSELECT preserves deleted messages")
            c.select("INBOX")
            check(c.search(None, "ALL")[1][0] != b"", "unselect did not expunge")
            check(c.close()[0] == "OK", "CLOSE expunges")
            c.select("INBOX")
            check(c.search(None, "ALL")[1] == [b""], "close removed deleted message")
            check(
                c.status("INBOX", "(MESSAGES UIDNEXT UIDVALIDITY UNSEEN RECENT)")[0]
                == "OK",
                "full STATUS",
            )
            with client("bob") as bob:
                bob.select("INBOX")
                check(
                    bob.uid("FETCH", copied, "BODY.PEEK[]")[1] == [None],
                    "cross-account message isolation",
                )
            c.create("Temporary")
            c.append("Temporary", None, None, simple)
            check(c.delete("Temporary")[0] == "OK", "DELETE populated mailbox")
            c.create("Tree")
            c.create('"Tree/Child"')
            check(
                c.rename("Tree", "Renamed")[0] == "OK"
                and b"Renamed/Child" in b" ".join(c.list()[1]),
                "rename subtree",
            )
            c.append("INBOX", None, None, simple)
            check(
                c.rename("INBOX", "ArchivedInbox")[0] == "OK",
                "rename INBOX moves messages and keeps INBOX",
            )
            check(
                c.select("INBOX")[1] == [b"0"]
                and c.select("ArchivedInbox")[1] == [b"1"],
                "INBOX rename semantics",
            )
            c.select("INBOX")
            # Read IDLE through imaplib's buffered stream, without simultaneous readers.
            tag = c._new_tag()
            c.send(tag + b" IDLE\r\n")
            check(c.readline().startswith(b"+"), "IDLE continuation")
            send(simple)
            got_exists = False
            for _ in range(4):
                line = c.readline()
                if b" EXISTS" in line:
                    got_exists = True
                    break
            check(got_exists, "IDLE pushes new delivery")
            c.send(b"DONE\r\n")
            while not c.readline().startswith(tag + b" OK"):
                pass
            check(c.noop()[0] == "OK", "DONE restores command mode")
            spam = simple.replace(
                b"IMAP-PRIVATE-SENTINEL",
                b"XJS*C4JDBQADN1.NSBN3*2IDNEN*GTUBE-STANDARD-ANTI-UBE-TEST-EMAIL*C.34X",
            )
            send(spam)
            check(c.select("Junk")[1] == [b"1"], "GTUBE routed to Junk")
            junk = c.fetch("1", "BODY.PEEK[]")[1][0][1]
            check(
                b"X-CloudieMail-Spam: Yes" in junk and b"GTUBE" in junk,
                "spam reason stored with message",
            )
            forged = simple.replace(
                b"From:",
                b"X-CloudieMail-Spam: Yes\r\nX-CloudieMail-Spam-Score: 999\r\nFrom:",
            )
            send(forged)
            c.select("INBOX")
            newest = c.search(None, "ALL")[1][0].split()[-1].decode()
            fetched = c.fetch(newest, "BODY.PEEK[]")[1][0][1]
            check(
                fetched.count(b"X-CloudieMail-Spam: ") == 1
                and b"X-CloudieMail-Spam: No" in fetched
                and b"999" not in fetched,
                "untrusted spam headers replaced",
            )
            send(spam, authenticated=True)
            check(
                c.select("INBOX")[1] == [b"3"],
                "authenticated submission bypasses incoming filter",
            )
            check(c.select("Junk")[1] == [b"1"], "submission not quarantined")
        with imaplib.IMAP4_SSL("127.0.0.1", ports[2], ssl_context=context) as c:
            check(
                c.authenticate(
                    "PLAIN", lambda _: b"\0alice@example.test\0" + PASSWORD.encode()
                )[0]
                == "OK",
                "SASL PLAIN",
            )
        # Invalid FETCH grammar must not partially alter flags.
        with client() as c:
            c.select("INBOX")
            with contextlib.suppress(imaplib.IMAP4.error):
                c.fetch("1", "(BODY[TEXT] UNKNOWN)")
            check(c.noop()[0] == "OK", "connection survives malformed command")
            unchanged = c.fetch("1", "FLAGS")[1]
            with contextlib.suppress(imaplib.IMAP4.error):
                c.store("1", "+FLAGS", r"(\Recent)")
            check(
                c.fetch("1", "FLAGS")[1] == unchanged,
                "invalid flags do not mutate message",
            )
            before = c.search(None, "ALL")[1]
            with contextlib.suppress(imaplib.IMAP4.error):
                c.uid("MOVE", "1,,2", "Trash")
            check(
                c.search(None, "ALL")[1] == before,
                "malformed sequence does not move messages",
            )
            c.store("1", "+FLAGS", r"(\Answered)")
        process.terminate()
        process.wait(timeout=15)
        check(
            b"IMAP-PRIVATE-SENTINEL" not in (root / "mail.fdb").read_bytes(),
            "APPEND and SMTP bodies remain encrypted at rest",
        )
        process = subprocess.Popen(
            [str(BINARY), "serve", str(path)], stdout=log, stderr=log
        )
        for _ in range(100):
            try:
                with socket.create_connection(("127.0.0.1", ports[0]), 0.2):
                    break
            except OSError:
                time.sleep(0.1)
        with client() as reopened:
            reopened.select("INBOX")
            check(
                reopened.search(None, "ANSWERED")[1][0] != b"",
                "IMAP flags survive server restart",
            )
            check(
                b"Work/2026" not in b" ".join(reopened.list()[1]),
                "renames survive restart",
            )
            check(
                reopened.select("Junk")[1] == [b"1"],
                "encrypted quarantine survives restart",
            )
        print(f"{checks} IMAP/antispam integration checks passed", flush=True)
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        if sys.exc_info()[0]:
            log.seek(0)
            print(log.read(), file=sys.stderr)
        log.close()
