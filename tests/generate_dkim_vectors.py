"""Regenerate public DKIM fixtures using dkimpy (private keys stay temporary)."""

import base64
import json
import subprocess
import tempfile
from pathlib import Path

import dkim

message = (
    b"From: Alice <alice@example.test>\r\nTo: recipient@elsewhere.test\r\n"
    b"Date: Fri, 6 Nov 2026 12:00:00 +0000\r\nMessage-ID: <vector@example.test>\r\n"
    b"Subject:  Independent\r\n\tDKIM vector\r\n\r\n  Hello\t world \t\r\n\r\n"
)
with tempfile.TemporaryDirectory() as tmp:
    key = Path(tmp) / "key.pem"
    subprocess.run(
        ["openssl", "genrsa", "-out", str(key), "2048"], check=True, capture_output=True
    )
    public = subprocess.check_output(
        ["openssl", "rsa", "-in", str(key), "-pubout", "-outform", "DER"],
        stderr=subprocess.DEVNULL,
    )
    record = b"v=DKIM1; k=rsa; p=" + base64.b64encode(public)
    rows = []
    for header in (b"simple", b"relaxed"):
        for body in (b"simple", b"relaxed"):
            signature = dkim.sign(
                message,
                b"fixture",
                b"example.test",
                key.read_bytes(),
                canonicalize=(header, body),
                include_headers=[b"from", b"to", b"date", b"message-id", b"subject"],
            )
            signed = signature + message
            assert dkim.verify(signed, dnsfunc=lambda *_args, **_kwargs: record)
            rows.append(
                (
                    header.decode() + "/" + body.decode(),
                    signed.decode(),
                    record.decode(),
                )
            )
output = "#pragma once\n// Public test messages generated and verified with dkimpy 1.1.8.\nstruct DkimVector {const char *name, *message, *record;};\ninline const DkimVector dkim_vectors[] = {\n"
for row in rows:
    output += "    {" + ", ".join(json.dumps(s) for s in row) + "},\n"
output += "};\n"
Path(__file__).with_name("dkim_vectors.hpp").write_text(output)
