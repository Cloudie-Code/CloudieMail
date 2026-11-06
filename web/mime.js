/* MIME is decoded to text and downloads only. No message HTML is executed. */
"use strict";
const MailMime = (() => {
  const bytes = (s) => Uint8Array.from(s, (c) => c.charCodeAt(0) & 255);
  const binary = (b) => {
    let s = "";
    for (let i = 0; i < b.length; i += 8192)
      s += String.fromCharCode(...b.subarray(i, i + 8192));
    return s;
  };
  const decode = (s, charset = "utf-8") => {
    try {
      return new TextDecoder(charset).decode(bytes(s));
    } catch {
      return new TextDecoder().decode(bytes(s));
    }
  };
  const qp = (s) =>
    s
      .replace(/=\r?\n/g, "")
      .replace(/=([a-f\d]{2})/gi, (_, n) =>
        String.fromCharCode(parseInt(n, 16)),
      );
  function header(s = "") {
    return s
      .replace(/(\?=)[ \t\r\n]+(?==\?)/g, "$1")
      .replace(
        /=\?([^?]+)\?([bq])\?([^?]*)\?=/gi,
        (all, charset, kind, value) => {
          try {
            return decode(
              kind.toLowerCase() === "b"
                ? atob(value)
                : qp(value.replace(/_/g, " ")),
              charset,
            );
          } catch {
            return all;
          }
        },
      );
  }
  function parameters(s = "") {
    const out = { value: s.split(";")[0].trim().toLowerCase() };
    const re = /;\s*([\w*-]+)\s*=\s*(?:"((?:\\.|[^"\\])*)"|([^;\s]+))/g;
    for (const m of s.matchAll(re))
      out[m[1].toLowerCase()] = (m[2] ?? m[3]).replace(/\\(.)/g, "$1");
    for (const name of ["filename", "name"]) {
      let extended = out[name + "*"];
      if (!extended && out[name + "*0*"]) {
        extended = "";
        for (let n = 0; n < 20 && out[name + "*" + n + "*"]; n++)
          extended += out[name + "*" + n + "*"];
      }
      if (extended) {
        const m = extended.match(/^([^']*)'[^']*'(.*)$/);
        if (m)
          out[name] = decode(
            m[2].replace(/%([\da-f]{2})/gi, (_, n) =>
              String.fromCharCode(parseInt(n, 16)),
            ),
            m[1],
          );
      }
    }
    return out;
  }
  function parse(buffer) {
    const raw = binary(
      buffer instanceof Uint8Array ? buffer : new Uint8Array(buffer),
    );
    if (raw.length > 26 * 1024 * 1024) throw new Error("Message too large");
    let count = 0;
    function part(value, depth) {
      if (++count > 100 || depth > 10) throw new Error("MIME nesting limit");
      const split = /\r?\n\r?\n/.exec(value),
        headers = {};
      const head = split ? value.slice(0, split.index) : value;
      let body = split ? value.slice(split.index + split[0].length) : "";
      for (const line of head.replace(/\r?\n[ \t]+/g, " ").split(/\r?\n/)) {
        const colon = line.indexOf(":");
        if (colon > 0) {
          const key = line.slice(0, colon).toLowerCase();
          if (!(key in headers)) headers[key] = line.slice(colon + 1).trim();
        }
      }
      const type = parameters(headers["content-type"] || "text/plain"),
        disposition = parameters(headers["content-disposition"]);
      const result = { headers, text: "", attachments: [], htmlOnly: false };
      if (
        type.value.startsWith("multipart/") &&
        type.boundary &&
        type.boundary.length <= 200
      ) {
        const chunks = [];
        let active = false,
          lines = [];
        for (const line of body.split(/\r?\n/)) {
          const marker = line.replace(/[ \t]+$/, "");
          if (
            marker === "--" + type.boundary ||
            marker === "--" + type.boundary + "--"
          ) {
            if (active) chunks.push(part(lines.join("\r\n"), depth + 1));
            lines = [];
            active = marker !== "--" + type.boundary + "--";
            if (!active) break;
          } else if (active) lines.push(line);
        }
        if (active && lines.length)
          chunks.push(part(lines.join("\r\n"), depth + 1));
        result.text = chunks
          .map((c) => c.text)
          .filter(Boolean)
          .join("\n\n");
        result.attachments = chunks.flatMap((c) => c.attachments);
        result.htmlOnly = !result.text && chunks.some((c) => c.htmlOnly);
        return result;
      }
      const encoding = (
        headers["content-transfer-encoding"] || ""
      ).toLowerCase();
      if (encoding === "base64") {
        try {
          body = atob(body.replace(/\s/g, ""));
        } catch {
          throw new Error("Invalid MIME base64");
        }
      } else if (encoding === "quoted-printable") body = qp(body);
      if (
        disposition.value === "attachment" ||
        disposition.filename ||
        type.name ||
        !["text/plain", "text/html"].includes(type.value)
      ) {
        const name = header(disposition.filename || type.name || "attachment")
          .replace(/[\x00-\x1f\x7f/\\]/g, "_")
          .slice(0, 255);
        result.attachments.push({ name, type: type.value, bytes: bytes(body) });
      } else if (type.value === "text/plain")
        result.text = decode(body, type.charset);
      else result.htmlOnly = true; // HTML is intentionally not inserted into a DOM or loaded in an iframe.
      return result;
    }
    return part(raw, 0);
  }
  return { parse, header, binary };
})();
if (typeof module !== "undefined") module.exports = MailMime;
