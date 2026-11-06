"use strict";
const $ = (id) => document.getElementById(id);
const languages = {
  "pt-BR": "Português (Brasil)",
  en: "English",
  ja: "日本語",
  ko: "한국어",
  "zh-TW": "繁體中文（台灣）",
};
let catalog = {},
  language = "pt-BR",
  csrf = "",
  account = "",
  messages = [],
  folders = [],
  labels = [];
let folder = "INBOX",
  label = "",
  starred = false,
  outgoing = false,
  offset = 0,
  total = 0,
  selected = null,
  parsed = null,
  raw = null;
let epoch = 0,
  generation = 0,
  maxBytes = 10 * 1024 * 1024,
  draftId = null,
  draftAttachments = [];
const tr = (key) => catalog[language]?.[key] || catalog.en?.[key] || key;
const folderName = (name) =>
  tr("folder." + name) === "folder." + name ? name : tr("folder." + name);
function node(tag, klass, text) {
  const el = document.createElement(tag);
  if (klass) el.className = klass;
  if (text !== undefined) el.textContent = text;
  return el;
}
function icon(name) {
  const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  svg.setAttribute("class", "icon");
  svg.setAttribute("aria-hidden", "true");
  const use = document.createElementNS(svg.namespaceURI, "use");
  use.setAttribute("href", "/icons.svg#" + name);
  svg.append(use);
  return svg;
}
function translate() {
  document.documentElement.lang = language;
  document
    .querySelectorAll("[data-i18n]")
    .forEach((el) => (el.textContent = tr(el.dataset.i18n)));
  document.querySelectorAll("[data-title]").forEach((el) => {
    el.title = tr(el.dataset.title);
    el.setAttribute("aria-label", el.title);
  });
  document
    .querySelectorAll("[data-placeholder]")
    .forEach((el) => (el.placeholder = tr(el.dataset.placeholder)));
  for (const id of ["login-language", "mail-language"]) $(id).value = language;
  if (account) {
    renderFolders();
    render();
    if (outgoing) refresh();
  }
}
async function api(path, options = {}) {
  const ownEpoch = epoch;
  const headers = { ...options.headers };
  if (options.method && options.method !== "GET") {
    headers["Content-Type"] = "application/json";
    headers["X-CSRF-Token"] = csrf;
  }
  const response = await fetch(path, {
    ...options,
    headers,
    credentials: "same-origin",
    cache: "no-store",
  });
  if (ownEpoch !== epoch) throw new Error(tr("error.session"));
  if (!response.ok) {
    const data = await response.json().catch(() => ({}));
    if (response.status === 401 && path !== "/api/login") showLogin();
    throw new Error(
      tr(
        path === "/api/login" ? "error.login" : data.code || "error.operation",
      ),
    );
  }
  return response;
}
const post = async (path, data, method = "POST") =>
  (await api(path, { method, body: JSON.stringify(data) })).json();
function banner(text = "") {
  $("mail-error").textContent = text;
  $("mail-error").hidden = !text;
}
function clearReader() {
  selected = parsed = raw = null;
  $("message-detail").hidden = true;
  $("reader-empty").hidden = false;
  for (const id of [
    "message-body",
    "message-subject",
    "message-from",
    "message-to",
    "message-date",
    "attachments",
    "message-labels",
  ])
    $(id).replaceChildren();
}
function showLogin() {
  ++epoch;
  ++generation;
  csrf = account = "";
  messages = folders = labels = [];
  clearReader();
  for (const id of ["messages", "folders", "labels", "deliveries-list"])
    $(id).replaceChildren();
  $("compose-dialog").close();
  $("edit-dialog").close();
  resetCompose();
  $("mail-screen").hidden = true;
  $("login-screen").hidden = false;
  $("password").value = "";
}
function showMailbox(data) {
  csrf = data.csrf;
  account = data.email;
  maxBytes = data.max_message_bytes || maxBytes;
  $("account-name").textContent = account;
  $("avatar").textContent = account[0].toUpperCase();
  $("login-screen").hidden = true;
  $("mail-screen").hidden = false;
  folder = "INBOX";
  label = "";
  starred = outgoing = false;
  offset = 0;
}
function nav(name, glyph, count, active, callback) {
  const el = node("button", "nav-item" + (active ? " active" : ""));
  el.type = "button";
  el.append(icon(glyph), node("span", "nav-text", name));
  if (count) el.append(node("span", "badge", count));
  el.addEventListener("click", callback);
  return el;
}
function selectFolder(name, options = {}) {
  $("mail-screen").classList.remove("navigation-open");
  folder = name;
  label = options.label || "";
  starred = !!options.starred;
  outgoing = !!options.outgoing;
  offset = 0;
  $("search").value = "";
  clearReader();
  refresh();
}
function renderFolders() {
  const glyphs = {
    INBOX: "inbox",
    Sent: "send",
    Drafts: "file",
    Trash: "trash",
    Junk: "shield",
  };
  $("folders").replaceChildren(
    ...folders.map((f) =>
      nav(
        folderName(f.name),
        glyphs[f.name] || "folder",
        f.unread,
        !label && !starred && !outgoing && f.name === folder,
        () => selectFolder(f.name),
      ),
    ),
  );
  $("folders").append(
    nav(tr("mail.starred"), "star", 0, starred, () =>
      selectFolder("INBOX", { starred: true }),
    ),
    nav(tr("mail.outgoing"), "clock", 0, outgoing, () =>
      selectFolder("INBOX", { outgoing: true }),
    ),
  );
  $("labels").replaceChildren(
    ...labels.map((l) =>
      nav(l, "tag", 0, l === label, () => selectFolder("INBOX", { label: l })),
    ),
  );
  $("folder-title").textContent = outgoing
    ? tr("mail.outgoing")
    : starred
      ? tr("mail.starred")
      : label || folderName(folder);
  const custom =
    !label &&
    !starred &&
    !outgoing &&
    folders.some((f) => f.name === folder && !f.system);
  $("rename-folder").hidden = $("remove-folder").hidden = !custom;
  $("mail-columns").hidden = outgoing;
  $("deliveries-panel").hidden = !outgoing;
  $("search").disabled = outgoing;
  $("move-message").replaceChildren(
    ...folders.map((f) => {
      const el = node("option", "", folderName(f.name));
      el.value = f.name;
      return el;
    }),
  );
  if (selected) $("move-message").value = selected.folder;
}
function render() {
  $("messages").replaceChildren();
  $("empty").hidden = messages.length > 0;
  $("total-count").textContent = `${total} ${tr("mail.count")}`;
  $("page-count").textContent =
    `${Math.floor(offset / 50) + 1} / ${Math.max(1, Math.ceil(total / 50))}`;
  $("previous").disabled = offset === 0;
  $("next").disabled = offset + 50 >= total;
  for (const m of messages) {
    const row = node(
      "button",
      `message-card ${m.seen ? "" : "unread"} ${selected?.id === m.id ? "selected" : ""}`,
    );
    row.type = "button";
    const top = node("span", "row-top");
    top.append(
      node(
        "span",
        "from",
        MailMime.header(m.from) || tr("mail.unknown_sender"),
      ),
    );
    if (m.flagged) top.append(icon("star"));
    row.append(
      top,
      node(
        "span",
        "subject",
        MailMime.header(m.subject) || tr("mail.no_subject"),
      ),
      node(
        "span",
        "date",
        m.date || `${m.size.toLocaleString(language)} bytes`,
      ),
    );
    const chips = node("span", "chips");
    for (const name of m.labels) chips.append(node("span", "chip", name));
    row.append(chips);
    row.addEventListener("click", () => openMessage(m));
    $("messages").append(row);
  }
}
async function refresh() {
  if (!account) return;
  const ticket = ++generation,
    ownEpoch = epoch;
  $("refresh").disabled = true;
  banner();
  try {
    const data = await (await api("/api/folders")).json();
    if (ticket !== generation || ownEpoch !== epoch) return;
    folders = data.folders;
    labels = data.labels;
    renderFolders();
    if (outgoing) {
      const result = await (await api("/api/deliveries")).json();
      if (ticket !== generation || ownEpoch !== epoch) return;
      $("deliveries-list").replaceChildren();
      if (!result.deliveries.length)
        $("deliveries-list").append(node("p", "muted", tr("mail.empty")));
      for (const d of result.deliveries) {
        const card = node("article", "delivery-card");
        card.append(
          node("strong", "", d.recipient),
          node(
            "span",
            "chip",
            tr(
              d.status === "sent"
                ? "mail.delivered"
                : d.status === "failed"
                  ? "mail.failed"
                  : "mail.pending",
            ),
          ),
          node("p", "muted", `${tr("mail.attempts")}: ${d.attempts}`),
        );
        if (d.error) card.append(node("p", "", d.error));
        if (d.status === "pending")
          card.append(
            node(
              "p",
              "muted",
              `${tr("mail.next_attempt")}: ${new Date(d.next_attempt * 1000).toLocaleString(language)}`,
            ),
          );
        $("deliveries-list").append(card);
      }
    } else {
      const params = new URLSearchParams({
        folder,
        offset,
        q: $("search").value,
      });
      if (label) params.set("label", label);
      if (starred) params.set("starred", "1");
      const result = await (await api("/api/messages?" + params)).json();
      if (ticket !== generation || ownEpoch !== epoch) return;
      messages = result.messages;
      total = result.total;
      render();
    }
  } catch (e) {
    if (ownEpoch === epoch) banner(e.message);
  } finally {
    if (ticket === generation) $("refresh").disabled = false;
  }
}
function download(content, name, type = "application/octet-stream") {
  const url = URL.createObjectURL(new Blob([content], { type })),
    a = node("a");
  a.href = url;
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}
async function openMessage(m) {
  clearReader();
  selected = m;
  render();
  const ownEpoch = epoch;
  try {
    const buffer = await (await api(`/api/messages/${m.id}`)).arrayBuffer();
    if (ownEpoch !== epoch || selected !== m) return;
    raw = buffer;
    parsed = MailMime.parse(buffer);
    m.seen = true;
    $("reader-empty").hidden = true;
    $("message-detail").hidden = false;
    $("message-subject").textContent =
      MailMime.header(m.subject) || tr("mail.no_subject");
    $("message-from").textContent = MailMime.header(m.from);
    $("message-to").textContent =
      `${tr("mail.to")}: ${MailMime.header(parsed.headers.to || account)}`;
    $("message-date").textContent = m.date;
    $("message-body").textContent =
      parsed.text || (parsed.htmlOnly ? tr("mail.html_only") : "");
    $("message-labels").replaceChildren(
      ...m.labels.map((l) => node("span", "chip", l)),
    );
    $("star-message").classList.toggle("active", m.flagged);
    $("star-message").setAttribute("aria-pressed", String(m.flagged));
    $("edit-draft").hidden = m.folder !== "Drafts";
    $("move-message").value = m.folder;
    for (const attachment of parsed.attachments) {
      const button = node(
        "button",
        "attachment-card",
        `${attachment.name} · ${attachment.bytes.length.toLocaleString(language)} bytes`,
      );
      button.prepend(icon("download"));
      button.onclick = () => download(attachment.bytes, attachment.name);
      $("attachments").append(button);
    }
    render();
  } catch (e) {
    if (ownEpoch === epoch) banner(e.message);
  }
}
async function modify(changes, method = "POST") {
  if (!selected) return;
  const id = selected.id;
  await post("/api/messages/" + id, changes, method);
  clearReader();
  await refresh();
}
function edit(title, description, value, action, input = true) {
  $("edit-title").textContent = title;
  $("edit-label").textContent = description;
  $("edit-value").value = value;
  $("edit-value").hidden = !input;
  $("edit-error").textContent = "";
  $("edit-form").onsubmit = async (event) => {
    event.preventDefault();
    const button = event.submitter;
    button.disabled = true;
    try {
      await action($("edit-value").value);
      $("edit-dialog").close();
    } catch (e) {
      $("edit-error").textContent = e.message;
    } finally {
      button.disabled = false;
    }
  };
  $("edit-dialog").showModal();
  if (input) $("edit-value").focus();
}
function resetCompose() {
  $("compose-form").reset();
  draftId = null;
  draftAttachments = [];
  $("compose-file-list").textContent = "";
  $("compose-error").textContent = "";
}
function compose(to = "", subject = "", body = "") {
  resetCompose();
  $("to").value = to;
  $("subject").value = subject;
  $("body").value = body;
  $("compose-dialog").showModal();
  $("to").focus();
}
async function submitCompose(draft) {
  $("send-button").disabled = $("save-draft").disabled = true;
  $("compose-error").textContent = "";
  const ownEpoch = epoch;
  try {
    const attachments = [...draftAttachments];
    let size = new TextEncoder().encode($("body").value).length;
    for (const file of $("compose-files").files) {
      size += file.size;
      if (size * 1.4 > maxBytes) throw new Error(tr("error.size"));
      attachments.push({
        name: file.name,
        type: file.type || "application/octet-stream",
        data: btoa(MailMime.binary(new Uint8Array(await file.arrayBuffer()))),
      });
    }
    if (attachments.length > 10) throw new Error(tr("error.attachments"));
    const payload = {
      to: $("to").value,
      subject: $("subject").value,
      body: $("body").value,
      attachments,
    };
    await post(draft ? "/api/draft" : "/api/send", payload);
    if (ownEpoch !== epoch) return;
    if (draftId)
      await post("/api/messages/" + draftId, { permanent: true }, "DELETE");
    $("compose-dialog").close();
    resetCompose();
    clearReader();
    await refresh();
    banner(tr(draft ? "mail.saved_draft" : "mail.sent"));
  } catch (e) {
    if (ownEpoch === epoch) $("compose-error").textContent = e.message;
  } finally {
    $("send-button").disabled = $("save-draft").disabled = false;
  }
}
$("login-form").onsubmit = async (event) => {
  event.preventDefault();
  $("login-button").disabled = true;
  $("login-error").textContent = "";
  const password = $("password").value;
  $("password").value = "";
  try {
    showMailbox(
      await post("/api/login", { email: $("email").value, password }),
    );
    await refresh();
  } catch (e) {
    $("login-error").textContent = e.message;
  } finally {
    $("login-button").disabled = false;
  }
};
$("logout").onclick = async () => {
  try {
    await post("/api/logout", {});
    showLogin();
  } catch (e) {
    banner(e.message);
  }
};
$("refresh").onclick = refresh;
$("navigation-toggle").onclick = () =>
  $("mail-screen").classList.toggle("navigation-open");
let searchTimer;
$("search").oninput = () => {
  clearTimeout(searchTimer);
  offset = 0;
  searchTimer = setTimeout(refresh, 300);
};
$("previous").onclick = () => {
  offset = Math.max(0, offset - 50);
  refresh();
};
$("next").onclick = () => {
  offset += 50;
  refresh();
};
$("compose").onclick = () => compose();
$("close-compose").onclick = () => $("compose-dialog").close();
$("compose-form").onsubmit = (event) => {
  event.preventDefault();
  submitCompose(false);
};
$("save-draft").onclick = () => submitCompose(true);
$("compose-files").onchange = () => {
  $("compose-file-list").textContent = [
    ...draftAttachments.map((a) => a.name),
    ...Array.from($("compose-files").files, (f) => f.name),
  ].join(" · ");
};
$("edit-cancel").onclick = () => $("edit-dialog").close();
$("new-folder").onclick = () =>
  edit(tr("mail.create_folder"), tr("mail.folder_name"), "", async (name) => {
    await post("/api/folders", { name });
    selectFolder(name);
  });
$("rename-folder").onclick = () =>
  edit(
    tr("mail.rename_folder"),
    tr("mail.folder_name"),
    folder,
    async (replacement) => {
      await post("/api/folders", {
        action: "rename",
        name: folder,
        replacement,
      });
      selectFolder(replacement);
    },
  );
$("remove-folder").onclick = () =>
  edit(
    tr("mail.remove_folder"),
    folderName(folder),
    "",
    async () => {
      await post("/api/folders", { action: "delete", name: folder });
      selectFolder("INBOX");
    },
    false,
  );
$("labels-message").onclick = () => {
  if (selected)
    edit(
      tr("mail.edit_labels"),
      tr("mail.label_prompt"),
      selected.labels.join(", "),
      (value) =>
        modify({
          labels: [
            ...new Set(
              value
                .split(",")
                .map((x) => x.trim())
                .filter(Boolean),
            ),
          ],
        }),
    );
};
const guard = (fn) => async () => {
  try {
    await fn();
  } catch (e) {
    banner(e.message);
  }
};
$("move-message").onchange = guard(() =>
  modify({ folder: $("move-message").value }),
);
$("star-message").onclick = guard(
  () => selected && modify({ flagged: !selected.flagged }),
);
$("unread-message").onclick = guard(() => modify({ seen: false }));
$("delete-message").onclick = guard(() => {
  if (!selected) return;
  if (selected.folder === "Trash")
    edit(
      tr("mail.delete"),
      tr("mail.delete_confirm"),
      "",
      () => modify({ permanent: true }, "DELETE"),
      false,
    );
  else return modify({}, "DELETE");
});
$("download-message").onclick = () => {
  if (raw) download(raw, "message.eml");
};
$("reply").onclick = () => {
  if (parsed) {
    const value = parsed.headers["reply-to"] || parsed.headers.from || "";
    const address = value.match(/<([^<>]+)>/)?.[1] || value;
    compose(
      address,
      "Re: " + MailMime.header(parsed.headers.subject || ""),
      "\n\n" +
        parsed.text
          .split("\n")
          .map((l) => "> " + l)
          .join("\n"),
    );
  }
};
$("forward").onclick = () => {
  if (parsed)
    compose(
      "",
      "Fwd: " + MailMime.header(parsed.headers.subject || ""),
      "\n\n" + parsed.text,
    );
};
$("edit-draft").onclick = () => {
  if (!parsed || !selected) return;
  const id = selected.id,
    attachments = parsed.attachments.map((a) => ({
      name: a.name,
      type: a.type,
      data: btoa(MailMime.binary(a.bytes)),
    }));
  compose(
    parsed.headers.to || "",
    MailMime.header(parsed.headers.subject || ""),
    parsed.text,
  );
  draftId = id;
  draftAttachments = attachments;
  $("compose-file-list").textContent = attachments
    .map((a) => a.name)
    .join(" · ");
};
(async () => {
  try {
    catalog = await (await fetch("/i18n.json", { cache: "no-cache" })).json();
    let saved;
    try {
      saved = localStorage.getItem("cloudiemail.language");
    } catch {}
    const detected = navigator.language.startsWith("pt")
      ? "pt-BR"
      : navigator.language.startsWith("zh")
        ? "zh-TW"
        : navigator.language.split("-")[0];
    language = languages[saved] ? saved : languages[detected] ? detected : "en";
    for (const id of ["login-language", "mail-language"]) {
      $(id).replaceChildren(
        ...Object.entries(languages).map(([value, name]) => {
          const el = node("option", "", name);
          el.value = value;
          return el;
        }),
      );
      $(id).onchange = () => {
        language = $(id).value;
        try {
          localStorage.setItem("cloudiemail.language", language);
        } catch {}
        translate();
      };
    }
    translate();
    try {
      showMailbox(await (await api("/api/session")).json());
      await refresh();
    } catch {
      showLogin();
    }
    setInterval(() => {
      if (
        account &&
        !document.hidden &&
        !$("compose-dialog").open &&
        !$("edit-dialog").open
      )
        refresh();
    }, 60000);
  } catch {
    $("login-error").textContent =
      "CloudieMail: failed to load interface resources.";
  }
})();
