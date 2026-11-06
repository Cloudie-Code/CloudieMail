#include "cloudiemail/i18n.hpp"
#include "cloudiemail/mime.hpp"
#include "cloudiemail/server.hpp"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <sodium.h>
#include <sstream>

namespace cm {
namespace {
struct Request {
    std::string method, path, body;
    std::map<std::string, std::string> headers;
};
std::string lower(std::string text) {
    for (auto &c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}
void respond(HttpStream &c, int code, std::string_view type, std::string_view body,
             const std::string &extra = "") {
    std::string status = code == 200   ? "OK"
                         : code == 201 ? "Created"
                         : code == 400 ? "Bad Request"
                         : code == 401 ? "Unauthorized"
                         : code == 403 ? "Forbidden"
                         : code == 404 ? "Not Found"
                         : code == 413 ? "Payload Too Large"
                         : code == 429 ? "Too Many Requests"
                                       : "Internal Server Error";
    c.send_response(code, status,
                    "Content-Type: " + std::string(type) +
                        "\r\nContent-Length: " + std::to_string(body.size()) +
                        "\r\nConnection: close\r\nCache-Control: no-store\r\nStrict-Transport-Security: "
                        "max-age=31536000\r\nX-Content-Type-Options: nosniff\r\nX-Frame-Options: "
                        "DENY\r\nReferrer-Policy: "
                        "no-referrer\r\nPermissions-Policy: camera=(), microphone=(), "
                        "geolocation=()\r\nCross-Origin-Opener-Policy: "
                        "same-origin\r\nContent-Security-Policy: default-src "
                        "'none'; script-src 'self'; style-src 'self'; img-src 'self'; connect-src 'self'; "
                        "font-src 'self'; "
                        "base-uri 'none'; form-action 'self'; frame-ancestors 'none'; object-src 'none'\r\n" +
                        extra,
                    body);
}
void json_response(HttpStream &c, int code, const Json &data, const std::string &extra = "") {
    auto body = data.dump(-1, ' ', false, Json::error_handler_t::replace);
    respond(c, code, "application/json; charset=utf-8", body, extra);
    wipe(body);
}
void error(HttpStream &c, int code, const char *message) {
    const char *key = code == 401 ? "error.session" : code == 403 ? "error.csrf" : "error.operation";
    json_response(c, code, {{"error", message}, {"code", key}});
}
std::string cookie(const std::string &header) {
    std::istringstream input(header);
    std::string part;
    while (std::getline(input, part, ';')) {
        auto start = part.find_first_not_of(' ');
        if (start != part.npos)
            part.erase(0, start);
        constexpr std::string_view prefix = "__Host-cloudiemail=";
        if (part.starts_with(prefix))
            return part.substr(prefix.size());
    }
    return "";
}
std::string display_header(const std::string &value) {
    // Decode RFC 2047 UTF-8/ASCII words for in-memory search; never persist plaintext headers.
    std::string out;
    std::size_t at = 0;
    while (at < value.size()) {
        auto start = value.find("=?", at);
        if (start == value.npos) {
            out += value.substr(at);
            break;
        }
        out += value.substr(at, start - at);
        auto charset_end = value.find('?', start + 2),
             end = value.find("?=", charset_end == value.npos ? start : charset_end + 3);
        if (charset_end == value.npos || end == value.npos || charset_end + 2 >= value.size() ||
            value[charset_end + 2] != '?') {
            out += value.substr(start);
            break;
        }
        auto charset = lower(value.substr(start + 2, charset_end - start - 2));
        if (charset != "utf-8" && charset != "us-ascii") {
            out += value.substr(start, end + 2 - start);
            at = end + 2;
            continue;
        }
        auto input = value.substr(charset_end + 3, end - charset_end - 3);
        std::string decoded;
        bool valid = true;
        if (value[charset_end + 1] == 'B' || value[charset_end + 1] == 'b') {
            decoded.resize(input.size());
            std::size_t length = 0;
            valid = sodium_base642bin(reinterpret_cast<unsigned char *>(decoded.data()), decoded.size(),
                                      input.data(), input.size(), nullptr, &length, nullptr,
                                      sodium_base64_VARIANT_ORIGINAL) == 0;
            decoded.resize(length);
        } else if (value[charset_end + 1] == 'Q' || value[charset_end + 1] == 'q') {
            for (std::size_t i = 0; i < input.size(); ++i) {
                if (input[i] == '_')
                    decoded += ' ';
                else if (input[i] == '=' && i + 2 < input.size()) {
                    unsigned byte = 0;
                    auto parsed = std::from_chars(input.data() + i + 1, input.data() + i + 3, byte, 16);
                    if (parsed.ec != std::errc() || parsed.ptr != input.data() + i + 3) {
                        valid = false;
                        break;
                    }
                    decoded += static_cast<char>(byte);
                    i += 2;
                } else
                    decoded += input[i];
            }
        } else
            valid = false;
        out += valid ? decoded : value.substr(start, end + 2 - start);
        at = end + 2;
        auto next = value.find_first_not_of(" \t\r\n", at);
        if (next != value.npos && value.compare(next, 2, "=?") == 0)
            at = next;
    }
    return out;
}
std::string header_value(std::string_view message, std::string_view name) {
    auto end = message.find("\r\n\r\n");
    message = message.substr(0, end);
    std::size_t pos = 0;
    while (pos < message.size()) {
        auto next = message.find("\r\n", pos);
        auto line = message.substr(pos, next == message.npos ? message.size() - pos : next - pos);
        auto colon = line.find(':');
        if (colon != line.npos && lower(std::string(line.substr(0, colon))) == name) {
            auto value = std::string(line.substr(colon + 1));
            auto continuation = next;
            while (continuation != message.npos && continuation + 2 < message.size() &&
                   (message[continuation + 2] == ' ' || message[continuation + 2] == '\t')) {
                auto folded_end = message.find("\r\n", continuation + 2);
                value +=
                    " " + std::string(message.substr(continuation + 2, folded_end == message.npos
                                                                           ? message.size() - continuation - 2
                                                                           : folded_end - continuation - 2));
                continuation = folded_end;
            }
            auto start = value.find_first_not_of(" \t");
            return start == value.npos ? "" : display_header(value.substr(start, 4096));
        }
        if (next == message.npos)
            break;
        pos = next + 2;
    }
    return "";
}
} // namespace
void http(HttpStream &c, Server &server) {
    Request r;
    auto first = words(c.line(4096));
    if (first.size() != 3 || first[2] != "HTTP/1.1") {
        error(c, 400, "Requisicao invalida");
        return;
    }
    r.method = first[0];
    r.path = first[1];
    std::map<std::string, std::string> query;
    auto query_at = r.path.find('?');
    if (query_at != r.path.npos) {
        auto text = r.path.substr(query_at + 1);
        r.path.resize(query_at);
        std::istringstream params(text);
        std::string part;
        auto decode = [](std::string_view value) {
            std::string result;
            for (std::size_t i = 0; i < value.size(); ++i) {
                if (value[i] == '+')
                    result += ' ';
                else if (value[i] == '%' && i + 2 < value.size()) {
                    unsigned byte = 0;
                    auto parsed = std::from_chars(value.data() + i + 1, value.data() + i + 3, byte, 16);
                    if (parsed.ec != std::errc() || parsed.ptr != value.data() + i + 3 || byte == 0)
                        throw std::runtime_error("Invalid URL encoding");
                    result += static_cast<char>(byte);
                    i += 2;
                } else
                    result += value[i];
            }
            return result;
        };
        while (std::getline(params, part, '&')) {
            auto equal = part.find('=');
            if (equal != part.npos)
                query[decode(std::string_view(part).substr(0, equal))] =
                    decode(std::string_view(part).substr(equal + 1));
        }
    }
    std::size_t total = 0;
    for (unsigned i = 0;; ++i) {
        auto line = c.line(8192);
        total += line.size();
        if (total > 16384 || i > 64) {
            error(c, 400, "Cabecalhos excedidos");
            return;
        }
        if (line.empty())
            break;
        auto colon = line.find(':');
        if (colon == line.npos || colon == 0) {
            error(c, 400, "Cabecalho invalido");
            return;
        }
        auto key = lower(line.substr(0, colon));
        if (key.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != key.npos) {
            error(c, 400, "Cabecalho invalido");
            return;
        }
        auto value = line.substr(colon + 1);
        auto start = value.find_first_not_of(" \t");
        value = start == value.npos ? "" : value.substr(start);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
            value.pop_back();
        if (!r.headers.emplace(key, value).second) {
            error(c, 400, "Cabecalho duplicado");
            return;
        }
    }
    auto expected = server.config().webmail_host();
    auto host = r.headers["host"];
    auto port = server.config().https_port;
    auto authority = expected + (port == 443 ? "" : ":" + std::to_string(port));
    if (host != authority) {
        error(c, 400, "Host incorreto; use o subdominio webmail configurado");
        return;
    }
    if (r.headers.contains("transfer-encoding") || r.headers.contains("expect")) {
        error(c, 400, "Enquadramento HTTP nao suportado");
        return;
    }
    if (r.method == "POST" || r.method == "DELETE") {
        auto length = r.headers["content-length"];
        std::size_t count = 0;
        auto [ptr, ec] = std::from_chars(length.data(), length.data() + length.size(), count);
        if (ec != std::errc() || ptr != length.data() + length.size()) {
            error(c, 400, "Content-Length invalido");
            return;
        }
        if (count > server.config().max_message_bytes) {
            error(c, 413, "Mensagem muito grande");
            return;
        }
        r.body = c.read(count);
        if (r.headers["origin"] != "https://" + authority) {
            wipe(r.body);
            error(c, 403, "Origem invalida");
            return;
        }
        if (r.headers["content-type"] != "application/json" &&
            r.headers["content-type"] != "application/json; charset=utf-8") {
            wipe(r.body);
            error(c, 400, "Use application/json");
            return;
        }
    } else if (r.method != "GET") {
        error(c, 400, "Metodo nao suportado");
        return;
    }
    if (r.method == "GET" && (r.path == "/" || r.path == "/app.js" || r.path == "/style.css" ||
                              r.path == "/i18n.json" || r.path == "/icons.svg" || r.path == "/mime.js")) {
        auto file = server.config().web_root / (r.path == "/" ? "index.html" : r.path.substr(1));
        std::ifstream input(file, std::ios::binary);
        if (!input) {
            error(c, 404, "Arquivo indisponivel");
            return;
        }
        std::ostringstream content;
        content << input.rdbuf();
        auto type = r.path == "/"                                   ? "text/html; charset=utf-8"
                    : (r.path == "/app.js" || r.path == "/mime.js") ? "text/javascript; charset=utf-8"
                    : r.path == "/i18n.json"                        ? "application/json; charset=utf-8"
                    : r.path == "/icons.svg"                        ? "image/svg+xml"
                                                                    : "text/css; charset=utf-8";
        respond(c, 200, type, content.str());
        return;
    }
    if (r.method == "POST" && r.path == "/api/login") {
        try {
            if (r.body.size() > 4096)
                throw std::runtime_error("Login too large");
            auto data = Json::parse(r.body);
            wipe(r.body);
            auto pass = data.at("password").get<std::string>();
            data.erase("password");
            std::shared_ptr<Login> login;
            try {
                login = server.authenticate(data.at("email"), pass, c.peer);
            } catch (...) {
                wipe(pass);
                throw;
            }
            wipe(pass);
            auto [token, session] = server.new_session(login);
            json_response(c, 200,
                          {{"email", login->email},
                           {"csrf", session.csrf},
                           {"expires_in", server.config().session_timeout_seconds}},
                          "Set-Cookie: __Host-cloudiemail=" + token +
                              "; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=" +
                              std::to_string(server.config().session_timeout_seconds) + "\r\n");
        } catch (...) {
            wipe(r.body);
            error(
                c, 401,
                "Nao foi possivel autenticar. Confira as credenciais ou aguarde antes de tentar novamente.");
        }
        return;
    }
    auto token = cookie(r.headers["cookie"]);
    auto session = server.session(token);
    if (!session) {
        wipe(r.body);
        error(c, 401, "Sessao expirada");
        return;
    }
    if (r.method != "GET") {
        auto csrf = r.headers["x-csrf-token"];
        if (csrf.size() != session->csrf.size() ||
            sodium_memcmp(csrf.data(), session->csrf.data(), csrf.size()) != 0) {
            wipe(r.body);
            error(c, 403, "Token de seguranca invalido");
            return;
        }
    }
    auto &login = *session->login;
    try {
        if (r.path == "/api/session" && r.method == "GET") {
            json_response(c, 200,
                          {{"email", login.email},
                           {"csrf", session->csrf},
                           {"max_message_bytes", server.config().max_message_bytes}});
            return;
        }
        if (r.path == "/api/logout" && r.method == "POST") {
            server.logout(token);
            json_response(
                c, 200, {{"ok", true}},
                "Set-Cookie: __Host-cloudiemail=; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=0\r\n");
            return;
        }
        if (r.path == "/api/folders" && r.method == "GET") {
            auto folders = server.database().folders(login.email);
            std::set<std::string> labels;
            for (const auto &m : server.database().messages(login.email, ""))
                for (const auto &label : m.labels)
                    labels.insert(label.get<std::string>());
            json_response(c, 200, {{"folders", folders}, {"labels", labels}});
            return;
        }
        if (r.path == "/api/folders" && r.method == "POST") {
            auto data = Json::parse(r.body);
            auto name = data.at("name").get<std::string>();
            auto action = data.value("action", std::string("create"));
            if (action == "create")
                server.database().create_folder(login.email, name);
            else if (action == "rename")
                server.database().rename_folder(login.email, name, data.at("replacement"));
            else if (action == "delete")
                server.database().remove_folder(login.email, name);
            else
                throw std::runtime_error("Invalid folder action");
            json_response(c, 200, {{"ok", true}});
            return;
        }
        if (r.path == "/api/deliveries" && r.method == "GET") {
            json_response(c, 200, {{"deliveries", server.database().delivery_status(login.email)}});
            return;
        }
        if (r.path == "/api/messages" && r.method == "GET") {
            auto folder = query.contains("folder") ? query.at("folder") : "INBOX";
            if (folder.empty())
                throw std::runtime_error("Folder required");
            auto list = server.database().messages(
                login.email, (query.contains("label") || query.contains("starred")) ? "" : folder);
            std::reverse(list.begin(), list.end());
            Json messages = Json::array();
            for (const auto &m : list) {
                if (query.contains("starred") && !m.flagged)
                    continue;
                if (query.contains("label") &&
                    std::find(m.labels.begin(), m.labels.end(), Json(query.at("label"))) == m.labels.end())
                    continue;
                auto text =
                    open_message(login.identity, login.key, server.database().message(login.email, m.uid));
                auto from = header_value(text, "from"), subject = header_value(text, "subject"),
                     date = header_value(text, "date");
                wipe(text);
                if (query.contains("q") &&
                    lower(from + " " + subject).find(lower(query.at("q"))) == std::string::npos)
                    continue;
                messages.push_back({{"id", m.uid},
                                    {"size", m.size},
                                    {"seen", m.seen},
                                    {"folder", m.folder},
                                    {"labels", m.labels},
                                    {"flagged", m.flagged},
                                    {"from", from},
                                    {"subject", subject},
                                    {"date", date}});
            }
            std::size_t offset = 0;
            if (query.contains("offset")) {
                const auto &n = query.at("offset");
                auto [p, e] = std::from_chars(n.data(), n.data() + n.size(), offset);
                if (e != std::errc() || p != n.data() + n.size() || offset > 10000)
                    throw std::runtime_error("Invalid page");
            }
            auto message_count = messages.size();
            Json page = Json::array();
            for (auto i = offset; i < std::min(message_count, offset + 50); ++i)
                page.push_back(std::move(messages[i]));
            json_response(
                c, 200,
                {{"messages", page}, {"total", message_count}, {"offset", offset}, {"page_size", 50}});
            return;
        }
        if (r.path.starts_with("/api/messages/")) {
            auto id = r.path.substr(14);
            std::uint64_t uid = 0;
            auto [end, ec] = std::from_chars(id.data(), id.data() + id.size(), uid);
            if (ec != std::errc() || end != id.data() + id.size() || !uid) {
                error(c, 400, "ID invalido");
                return;
            }
            if (r.method == "GET") {
                auto text =
                    open_message(login.identity, login.key, server.database().message(login.email, uid));
                server.database().set_seen(login.email, uid, true);
                respond(c, 200, "text/plain; charset=utf-8", text);
                wipe(text);
                return;
            }
            if (r.method == "POST" || r.method == "DELETE") {
                auto list = server.database().messages(login.email, "");
                auto found =
                    std::find_if(list.begin(), list.end(), [&](const auto &m) { return m.uid == uid; });
                if (found == list.end())
                    throw std::runtime_error("Message unavailable");
                auto data = Json::parse(r.body);
                if (r.method == "DELETE" && data.value("permanent", false))
                    server.database().erase(login.email, {uid});
                else
                    uid = server.database().organize(
                        login.email, uid,
                        r.method == "DELETE" ? "Trash" : data.value("folder", found->folder),
                        data.value("labels", found->labels), data.value("flagged", found->flagged));
                if (data.contains("seen"))
                    server.database().set_seen(login.email, uid, data.at("seen").get<bool>());
                json_response(c, 200, {{"ok", true}});
                return;
            }
        }
        if ((r.path == "/api/send" || r.path == "/api/draft") && r.method == "POST") {
            auto data = Json::parse(r.body);
            wipe(r.body);
            auto recipients = parse_recipients(data.value("to", std::string()));
            auto message = compose_message(login.email, recipients, data, server.config().domain);
            if (message.size() > server.config().max_message_bytes) {
                wipe(message);
                throw std::runtime_error("Message too large");
            }
            std::string id;
            try {
                if (r.path == "/api/draft") {
                    server.database().deliver(
                        {{login.email, seal_message(login.identity, message), message.size(), "Drafts"}},
                        server.config().mailbox_quota_bytes);
                    id = "draft";
                } else
                    id = server.deliver(login.email, recipients, message, &login);
            } catch (...) {
                wipe(message);
                throw;
            }
            wipe(message);
            json_response(c, 201, {{"id", id}, {"queued", r.path == "/api/send"}});
            return;
        }
        error(c, 404, "Recurso indisponivel");
    } catch (...) {
        wipe(r.body);
        error(c, 400, "Nao foi possivel concluir a operacao");
    }
}
} // namespace cm
