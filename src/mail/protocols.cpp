#include "cloudiemail/server.hpp"
#include <algorithm>
#include <charconv>
#include <regex>
#include <sodium.h>
#include <sstream>

namespace cm {
std::string upper(std::string s) {
    for (auto &c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::vector<std::string> words(std::string_view s) {
    std::vector<std::string> result;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ')
            ++i;
        if (i == s.size())
            break;
        std::string token;
        if (s[i] == '"') {
            ++i;
            bool closed = false;
            while (i < s.size()) {
                char c = s[i++];
                if (c == '"') {
                    closed = true;
                    break;
                }
                if (c == '\\') {
                    if (i == s.size())
                        throw std::runtime_error("Escape incompleto");
                    c = s[i++];
                }
                if (c == '\0' || c == '\r' || c == '\n')
                    throw std::runtime_error("Token invalido");
                token += c;
            }
            if (!closed || (i < s.size() && s[i] != ' '))
                throw std::runtime_error("Aspas invalidas");
        } else {
            while (i < s.size() && s[i] != ' ')
                token += s[i++];
        }
        result.push_back(std::move(token));
        if (result.size() > 64)
            throw std::runtime_error("Comando muito longo");
    }
    return result;
}
std::string dot_stuff(std::string_view s) {
    std::string out;
    bool start = true;
    for (char c : s) {
        if (start && c == '.')
            out += '.';
        out += c;
        start = c == '\n';
    }
    if (!out.ends_with("\r\n"))
        out += "\r\n";
    return out + ".\r\n";
}
namespace {
std::pair<std::string, std::string> command(std::string_view line) {
    auto pos = line.find(' ');
    return {upper(std::string(line.substr(0, pos))),
            pos == line.npos ? "" : std::string(line.substr(pos + 1))};
}
std::uint64_t number(std::string_view s) {
    std::uint64_t n = 0;
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (ec != std::errc() || end != s.data() + s.size() || n == 0)
        throw std::runtime_error("Numero invalido");
    return n;
}
std::string path_argument(const std::string &arg, const std::string &prefix, bool empty = false) {
    if (!upper(arg).starts_with(prefix + "<") || arg.empty() || arg.back() != '>')
        throw std::runtime_error("Endereco invalido");
    auto value = arg.substr(prefix.size() + 1, arg.size() - prefix.size() - 2);
    return value.empty() && empty ? "" : email_address(value);
}
std::shared_ptr<Login> plain_auth(Server &server, Connection &c, const std::string &encoded) {
    std::string decoded(encoded.size(), '\0');
    std::size_t n = 0;
    if (sodium_base642bin(reinterpret_cast<unsigned char *>(decoded.data()), decoded.size(), encoded.data(),
                          encoded.size(), nullptr, &n, nullptr, sodium_base64_VARIANT_ORIGINAL) != 0)
        throw std::runtime_error("AUTH invalido");
    decoded.resize(n);
    try {
        auto first = decoded.find('\0');
        auto second = first == decoded.npos ? decoded.npos : decoded.find('\0', first + 1);
        if (first == decoded.npos || second == decoded.npos || decoded.find('\0', second + 1) != decoded.npos)
            throw std::runtime_error("AUTH invalido");
        auto user = decoded.substr(first + 1, second - first - 1);
        if (first != 0 && decoded.substr(0, first) != user)
            throw std::runtime_error("Identidade de autorizacao invalida");
        auto login = server.authenticate(user, std::string_view(decoded).substr(second + 1), c.peer);
        wipe(decoded);
        return login;
    } catch (...) {
        wipe(decoded);
        throw;
    }
}
} // namespace
void smtp(Connection &c, Server &server, bool submission) {
    c.write("220 " + server.config().mail_host() + " ESMTP CloudieMail\r\n");
    bool greeted = false, have_sender = false;
    std::string sender, helo;
    std::vector<std::string> recipients;
    std::shared_ptr<Login> login;
    auto reset = [&] {
        have_sender = false;
        sender.clear();
        recipients.clear();
    };
    for (unsigned commands = 0; commands < 1000; ++commands) {
        auto line = c.line(2048);
        auto [cmd, arg] = command(line);
        wipe(line);
        if (cmd == "QUIT") {
            c.write("221 Bye\r\n");
            return;
        }
        if (cmd == "EHLO" || cmd == "HELO") {
            if (arg.empty()) {
                c.write("501 Host required\r\n");
                continue;
            }
            reset();
            greeted = true;
            helo = arg;
            if (cmd == "HELO")
                c.write("250 " + server.config().mail_host() + "\r\n");
            else {
                auto response = "250-" + server.config().mail_host() + "\r\n250-SIZE " +
                                std::to_string(server.config().max_message_bytes) + "\r\n";
                if (!c.secure())
                    response += "250-STARTTLS\r\n";
                else
                    response += "250-AUTH PLAIN LOGIN\r\n";
                response += "250 8BITMIME\r\n";
                c.write(response);
            }
            continue;
        }
        if (cmd == "NOOP") {
            c.write("250 OK\r\n");
            continue;
        }
        if (cmd == "RSET") {
            reset();
            c.write("250 Reset\r\n");
            continue;
        }
        if (cmd == "STARTTLS" && !c.secure() && greeted && arg.empty()) {
            c.write("220 Ready for TLS\r\n");
            c.start_tls();
            reset();
            login.reset();
            greeted = false;
            continue;
        }
        if (!greeted) {
            c.write("503 EHLO first\r\n");
            continue;
        }
        if (cmd == "AUTH") {
            if (!c.secure()) {
                c.write("538 TLS required\r\n");
                continue;
            }
            if (login || have_sender) {
                c.write("503 Invalid state\r\n");
                continue;
            }
            try {
                auto args = words(arg);
                wipe(arg);
                if (args.empty() || (upper(args[0]) != "PLAIN" && upper(args[0]) != "LOGIN") ||
                    args.size() > 2) {
                    c.write("504 Unsupported authentication\r\n");
                    continue;
                }
                if (upper(args[0]) == "LOGIN") {
                    std::string user, password, encoded;
                    auto decode = [&](const std::string &input) {
                        std::string output(input.size(), '\0');
                        std::size_t length = 0;
                        if (sodium_base642bin(reinterpret_cast<unsigned char *>(output.data()), output.size(),
                                              input.data(), input.size(), nullptr, &length, nullptr,
                                              sodium_base64_VARIANT_ORIGINAL) != 0) {
                            wipe(output);
                            throw std::runtime_error("AUTH invalid");
                        }
                        output.resize(length);
                        return output;
                    };
                    try {
                        if (args.size() == 2)
                            encoded = args[1];
                        else {
                            c.write("334 VXNlcm5hbWU6\r\n");
                            encoded = c.line(4096);
                        }
                        user = decode(encoded);
                        wipe(encoded);
                        c.write("334 UGFzc3dvcmQ6\r\n");
                        encoded = c.line(4096);
                        password = decode(encoded);
                        wipe(encoded);
                        login = server.authenticate(user, password, c.peer);
                        wipe(password);
                        wipe(user);
                    } catch (...) {
                        wipe(password);
                        wipe(user);
                        wipe(encoded);
                        for (auto &a : args)
                            wipe(a);
                        throw;
                    }
                    for (auto &a : args)
                        wipe(a);
                    c.write("235 Authenticated\r\n");
                    continue;
                }
                auto encoded = args.size() == 2 ? args[1] : "";
                if (encoded.empty()) {
                    c.write("334 \r\n");
                    encoded = c.line(4096);
                }
                try {
                    login = plain_auth(server, c, encoded);
                } catch (...) {
                    wipe(encoded);
                    for (auto &a : args)
                        wipe(a);
                    throw;
                }
                wipe(encoded);
                for (auto &a : args)
                    wipe(a);
                c.write("235 Authenticated\r\n");
            } catch (...) {
                c.write("535 Authentication failed\r\n");
            }
            continue;
        }
        if (!c.secure()) {
            c.write("530 STARTTLS required\r\n");
            continue;
        }
        if (submission && !login) {
            c.write("530 Authentication required\r\n");
            continue;
        }
        if (cmd == "MAIL") {
            reset();
            try {
                auto end = arg.find('>');
                if (end == arg.npos)
                    throw std::runtime_error("Invalid path");
                auto address = arg.substr(0, end + 1);
                auto tail = arg.substr(end + 1);
                for (const auto &option : words(tail)) {
                    auto opt = upper(option);
                    if (opt.starts_with("SIZE=")) {
                        if (number(opt.substr(5)) > server.config().max_message_bytes)
                            throw std::runtime_error("Too large");
                    } else if (opt != "BODY=8BITMIME" && opt != "BODY=7BIT")
                        throw std::runtime_error("Unsupported parameter");
                }
                sender = path_argument(address, "FROM:", true);
                if (login && sender != login->email) {
                    c.write("553 Sender must match authenticated account\r\n");
                    continue;
                }
                have_sender = true;
                c.write("250 Sender accepted\r\n");
            } catch (...) {
                c.write("501 Invalid sender or parameters\r\n");
            }
            continue;
        }
        if (cmd == "RCPT") {
            if (!have_sender) {
                c.write("503 MAIL first\r\n");
                continue;
            }
            if (recipients.size() >= 20) {
                c.write("452 Too many recipients\r\n");
                continue;
            }
            try {
                auto recipient = path_argument(arg, "TO:");
                if ((recipient.ends_with("@" + server.config().domain) &&
                     !server.database().account(recipient)) ||
                    (!recipient.ends_with("@" + server.config().domain) &&
                     (!login || !server.config().outbound_enabled))) {
                    c.write("550 Recipient unavailable; relay denied\r\n");
                    continue;
                }
                recipients.push_back(recipient);
                c.write("250 Recipient accepted\r\n");
            } catch (...) {
                c.write("550 Recipient unavailable\r\n");
            }
            continue;
        }
        if (cmd == "DATA" && arg.empty()) {
            if (!have_sender || recipients.empty()) {
                c.write("503 MAIL and RCPT required\r\n");
                continue;
            }
            c.write("354 End with <CRLF>.<CRLF>\r\n");
            std::string body;
            try {
                for (;;) {
                    auto data = c.line(998);
                    if (data == ".")
                        break;
                    if (data.starts_with('.'))
                        data.erase(0, 1);
                    if (body.size() + data.size() + 2 > server.config().max_message_bytes) {
                        wipe(body);
                        c.write("552 Message too large\r\n");
                        return;
                    }
                    body += data + "\r\n";
                    wipe(data);
                }
                auto id = server.deliver(sender, recipients, body, login.get(), c.peer, helo);
                wipe(body);
                c.write("250 Stored " + id + "\r\n");
            } catch (...) {
                wipe(body);
                c.write("451 Unable to store message\r\n");
                return;
            }
            reset();
            continue;
        }
        if (cmd == "VRFY" || cmd == "EXPN")
            c.write("252 Verification disabled\r\n");
        else
            c.write("502 Command not implemented\r\n");
    }
    c.write("421 Session command limit\r\n");
}
void pop3(Connection &c, Server &server) {
    c.write("+OK CloudieMail ready\r\n");
    std::string user;
    std::shared_ptr<Login> login;
    std::vector<MessageInfo> messages;
    std::set<std::uint64_t> deleted;
    for (unsigned commands = 0; commands < 1000; ++commands) {
        auto line = c.line(2048);
        auto [cmd, arg] = command(line);
        wipe(line);
        if (cmd == "QUIT") {
            if (login) {
                std::vector<std::uint64_t> uids(deleted.begin(), deleted.end());
                server.database().erase(login->email, uids);
            }
            c.write("+OK Bye\r\n");
            return;
        }
        if (cmd == "CAPA") {
            c.write("+OK Capabilities\r\nUSER\r\nUIDL\r\nTOP\r\nRESP-CODES\r\n.\r\n");
            continue;
        }
        if (!login) {
            if (cmd == "USER") {
                user = arg;
                c.write("+OK Send password\r\n");
            } else if (cmd == "PASS" && !user.empty()) {
                try {
                    login = server.authenticate(user, arg, c.peer);
                    wipe(arg);
                    messages = server.database().messages(login->email);
                    c.write("+OK Mailbox unlocked\r\n");
                } catch (...) {
                    wipe(arg);
                    c.write("-ERR [AUTH] Invalid credentials\r\n");
                }
            } else
                c.write("-ERR Authenticate first\r\n");
            continue;
        }
        if (cmd == "NOOP") {
            c.write("+OK\r\n");
            continue;
        }
        if (cmd == "RSET") {
            deleted.clear();
            c.write("+OK Reset\r\n");
            continue;
        }
        if (cmd == "STAT") {
            std::size_t total = 0, count = 0;
            for (const auto &m : messages)
                if (!deleted.contains(m.uid)) {
                    ++count;
                    total += m.size;
                }
            c.write("+OK " + std::to_string(count) + " " + std::to_string(total) + "\r\n");
            continue;
        }
        if ((cmd == "LIST" || cmd == "UIDL") && arg.empty()) {
            c.write("+OK\r\n");
            for (std::size_t i = 0; i < messages.size(); ++i) {
                const auto &m = messages[i];
                if (!deleted.contains(m.uid))
                    c.write(std::to_string(i + 1) + " " + std::to_string(cmd == "LIST" ? m.size : m.uid) +
                            "\r\n");
            }
            c.write(".\r\n");
            continue;
        }
        try {
            auto args = words(arg);
            if (args.empty())
                throw std::runtime_error("Missing message");
            auto index = number(args[0]);
            if (index > messages.size())
                throw std::runtime_error("Missing message");
            auto m = messages[static_cast<std::size_t>(index - 1)];
            if (deleted.contains(m.uid))
                throw std::runtime_error("Deleted message");
            if (cmd == "LIST" || cmd == "UIDL")
                c.write("+OK " + std::to_string(index) + " " +
                        std::to_string(cmd == "LIST" ? m.size : m.uid) + "\r\n");
            else if (cmd == "DELE") {
                deleted.insert(m.uid);
                c.write("+OK Marked\r\n");
            } else if (cmd == "RETR" || cmd == "TOP") {
                auto body =
                    open_message(login->identity, login->key, server.database().message(login->email, m.uid));
                if (cmd == "TOP") {
                    if (args.size() != 2)
                        throw std::runtime_error("TOP arguments");
                    std::uint64_t lines = args[1] == "0" ? 0 : number(args[1]);
                    auto split = body.find("\r\n\r\n");
                    if (split != body.npos) {
                        auto end = split + 4;
                        while (lines-- && end < body.size()) {
                            auto next = body.find("\r\n", end);
                            end = next == body.npos ? body.size() : next + 2;
                        }
                        body.resize(end);
                    }
                }
                c.write("+OK Message follows\r\n");
                auto response = dot_stuff(body);
                wipe(body);
                c.write(response);
                wipe(response);
            } else
                c.write("-ERR Unsupported command\r\n");
        } catch (...) {
            c.write("-ERR Message unavailable or invalid command\r\n");
        }
    }
}
} // namespace cm
