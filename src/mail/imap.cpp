#include "cloudiemail/mail_message.hpp"
#include "cloudiemail/server.hpp"
#include <algorithm>
#include <charconv>
#include <ctime>
#include <functional>
#include <regex>
#include <sodium.h>

namespace cm {
namespace {
struct Syntax : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct Token {
    std::string text;
    bool string = false;
};
using Tokens = std::vector<Token>;
std::uint64_t integer(std::string_view s, bool zero = false) {
    std::uint64_t n = 0;
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (ec != std::errc() || end != s.data() + s.size() || (!zero && !n) || n > UINT32_MAX)
        throw Syntax("Invalid number");
    return n;
}
Tokens tokens(Connection &c, std::size_t limit) {
    Tokens out;
    auto line = c.line(65536);
    std::size_t at = 0, total = 0;
    unsigned literals = 0;
    struct Clean {
        std::string &s;
        ~Clean() {
            wipe(s);
        }
    } clean{line};
    while (true) {
        while (at < line.size() && line[at] == ' ')
            ++at;
        if (at == line.size())
            break;
        if (out.size() >= 1024)
            throw Syntax("Too many arguments");
        char ch = line[at++];
        if (ch == '(' || ch == ')') {
            out.push_back({std::string(1, ch)});
            continue;
        }
        if (ch == '"') {
            std::string value;
            bool closed = false;
            while (at < line.size()) {
                char v = line[at++];
                if (v == '"') {
                    closed = true;
                    break;
                }
                if (v == '\\') {
                    if (at == line.size())
                        throw Syntax("Invalid escape");
                    v = line[at++];
                    if (v != '"' && v != '\\')
                        throw Syntax("Invalid escape");
                }
                if (v == '\0')
                    throw Syntax("Invalid string");
                value += v;
            }
            if (!closed)
                throw Syntax("Unterminated string");
            out.push_back({std::move(value), true});
        } else if (ch == '{') {
            if (line.back() != '}' || ++literals > 16 || out.size() < 2)
                throw Syntax("Invalid literal");
            auto n = integer(std::string_view(line).substr(at, line.size() - at - 1), true);
            // Close on oversize literals: unread payload must never become a command.
            if (n > limit || total + n > limit) {
                c.write("* BYE [TOOBIG] Literal limit exceeded\r\n");
                throw std::runtime_error("Literal limit");
            }
            c.write("+ Ready for literal\r\n");
            out.push_back({c.read(static_cast<std::size_t>(n)), true});
            total += static_cast<std::size_t>(n);
            wipe(line);
            line = c.line(65536);
            at = 0;
        } else {
            auto start = at - 1;
            bool bracket = false;
            while (at < line.size()) {
                char v = line[at];
                if (v == '[')
                    bracket = true;
                if (v == ']')
                    bracket = false;
                if (!bracket && (v == ' ' || v == '(' || v == ')'))
                    break;
                if (v == '\0' || v == '\r' || v == '\n')
                    throw Syntax("Invalid atom");
                ++at;
            }
            if (bracket)
                throw Syntax("Invalid section");
            out.push_back({line.substr(start, at - start)});
        }
        if (at < line.size() && line[at] != ' ' && line[at] != ')' && line[at] != '(')
            throw Syntax("Expected space");
    }
    return out;
}
class Sequence {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;

  public:
    Sequence(std::string_view s, std::uint64_t maximum) {
        if (s.empty())
            throw Syntax("Empty sequence");
        std::size_t at = 0;
        while (at < s.size()) {
            auto comma = s.find(',', at);
            auto part = s.substr(at, comma == s.npos ? s.size() - at : comma - at);
            auto colon = part.find(':');
            auto parse = [&](std::string_view v) { return v == "*" ? maximum : integer(v); };
            auto a = parse(part.substr(0, colon)), b = colon == part.npos ? a : parse(part.substr(colon + 1));
            ranges.emplace_back(std::min(a, b), std::max(a, b));
            if (comma == s.npos)
                break;
            at = comma + 1;
            if (at == s.size())
                throw Syntax("Invalid sequence");
        }
    }
    bool contains(std::uint64_t n) const {
        for (auto [a, b] : ranges)
            if (n >= a && n <= b)
                return true;
        return false;
    }
};
std::string folder_name(const Token &t) {
    auto name = mailbox_decode(t.text);
    if (upper(name) == "INBOX")
        return "INBOX";
    return name;
}
bool exists(Database &db, const std::string &email, const std::string &name) {
    for (auto &f : db.folders(email))
        if (f["name"] == name)
            return true;
    return false;
}
std::set<std::string> flag_set(const MessageInfo &m) {
    auto f = m.imap_flags.get<std::set<std::string>>();
    if (m.seen)
        f.insert("\\Seen");
    if (m.flagged)
        f.insert("\\Flagged");
    return f;
}
std::string flags(const MessageInfo &m, bool recent) {
    auto f = flag_set(m);
    if (recent)
        f.insert("\\Recent");
    std::string s = "(";
    for (auto &v : f) {
        if (s.size() > 1)
            s += ' ';
        s += v;
    }
    return s + ")";
}
std::string canonical_flag(std::string s) {
    if (s.empty() || s.size() > 64)
        throw Syntax("Invalid flag");
    auto u = upper(s);
    for (auto v : {"\\Seen", "\\Answered", "\\Flagged", "\\Deleted", "\\Draft"})
        if (u == upper(v))
            return v;
    if (s.front() == '\\')
        throw Syntax("Unknown system flag");
    for (unsigned char ch : s)
        if (ch <= 32 || ch >= 127 ||
            std::string_view("(){%*\"\\]").find(static_cast<char>(ch)) != std::string_view::npos)
            throw Syntax("Invalid keyword");
    return u;
}
Json parse_flags(const Tokens &a, std::size_t &at, bool allow_bare = false) {
    Json result = Json::array();
    bool list = at < a.size() && !a[at].string && a[at].text == "(";
    if (list)
        ++at;
    else if (!allow_bare)
        throw Syntax("Expected flags list");
    while (at < a.size() && (!list || a[at].string || a[at].text != ")")) {
        result.push_back(canonical_flag(a[at++].text));
        if (result.size() > 64)
            throw Syntax("Too many flags");
    }
    if (list) {
        if (at == a.size())
            throw Syntax("Unclosed flags");
        ++at;
    }
    return result;
}
bool wildcard(std::string_view pattern, std::string_view value) {
    // Bounded dynamic programming avoids recursive wildcard backtracking.
    if (pattern.size() > 512 || value.size() > 1024)
        return false;
    std::vector<bool> row(value.size() + 1, false);
    row[0] = true;
    for (char p : pattern) {
        std::vector<bool> next(value.size() + 1, false);
        if (p == '*' || p == '%')
            next[0] = row[0];
        for (std::size_t i = 1; i <= value.size(); ++i)
            next[i] = (p == '*' || p == '%') ? (row[i] || (next[i - 1] && (p == '*' || value[i - 1] != '/')))
                                             : (row[i - 1] && p == value[i - 1]);
        row = std::move(next);
    }
    return row.back();
}
std::vector<std::uint64_t> selected_uids(const std::vector<MessageInfo> &messages, const std::string &set,
                                         bool uid) {
    Sequence selection(set, uid ? (messages.empty() ? 0 : messages.back().uid) : messages.size());
    std::vector<std::uint64_t> out;
    for (std::size_t i = 0; i < messages.size(); ++i)
        if (selection.contains(uid ? messages[i].uid : i + 1))
            out.push_back(messages[i].uid);
    return out;
}
std::string uid_list(const std::vector<std::uint64_t> &uids) {
    std::string out;
    for (auto n : uids) {
        if (!out.empty())
            out += ',';
        out += std::to_string(n);
    }
    return out;
}
std::string section(const MailPart &root, std::string expression, bool &found) {
    const MailPart *part = &root;
    found = true;
    std::size_t at = 0;
    bool numbered = false;
    while (at < expression.size() && expression[at] >= '0' && expression[at] <= '9') {
        auto end = expression.find('.', at);
        auto token = std::string_view(expression)
                         .substr(at, end == expression.npos ? expression.size() - at : end - at);
        auto number = integer(token);
        bool embedded =
            numbered && part->type == "MESSAGE" && part->subtype == "RFC822" && !part->children.empty();
        if (embedded)
            part = &part->children[0];
        if (part->type == "MULTIPART") {
            if (number > part->children.size()) {
                found = false;
                return {};
            }
            part = &part->children[static_cast<std::size_t>(number - 1)];
        } else if (number != 1 || (numbered && !embedded)) {
            found = false;
            return {};
        }
        numbered = true;
        at = end == expression.npos ? expression.size() : end + 1;
    }
    auto suffix = expression.substr(at);
    if (suffix.empty())
        return std::string(numbered ? part->body : part->raw);
    if (suffix == "MIME") {
        if (!numbered)
            throw Syntax("MIME requires a part number");
        return std::string(part->header);
    }
    if (numbered) {
        if (part->type != "MESSAGE" || part->subtype != "RFC822" || part->children.empty()) {
            found = false;
            return {};
        }
        part = &part->children[0];
    }
    if (suffix == "HEADER")
        return std::string(part->header);
    if (suffix == "TEXT")
        return std::string(part->body);
    bool negate = suffix.starts_with("HEADER.FIELDS.NOT ");
    if (negate || suffix.starts_with("HEADER.FIELDS ")) {
        auto start = suffix.find('('), end = suffix.rfind(')');
        if (start == suffix.npos || end != suffix.size() - 1)
            throw Syntax("Invalid header list");
        auto list = words(std::string_view(suffix).substr(start + 1, end - start - 1));
        if (list.empty())
            throw Syntax("Empty header list");
        std::set<std::string> names(list.begin(), list.end());
        std::string out;
        for (auto &h : part->headers)
            if (names.contains(h.name) != negate)
                out += h.raw;
        return out + "\r\n";
    }
    throw Syntax("Unknown body section");
}
struct FetchItem {
    std::string name, section;
    bool body = false, peek = false, partial = false;
    std::size_t offset = 0, length = 0;
};
std::vector<FetchItem> fetch_items(const Tokens &a, std::size_t at) {
    std::vector<std::string> names;
    bool list = at < a.size() && a[at].text == "(";
    if (list)
        ++at;
    while (at < a.size() && (!list || a[at].text != ")"))
        names.push_back(upper(a[at++].text));
    if (list) {
        if (at == a.size())
            throw Syntax("Unclosed FETCH");
        ++at;
    }
    if (at != a.size() || names.empty() || (!list && names.size() != 1))
        throw Syntax("Invalid FETCH");
    if (names.size() == 1) {
        auto macro = names[0];
        if (macro == "ALL" || macro == "FAST" || macro == "FULL") {
            names = {"FLAGS", "INTERNALDATE", "RFC822.SIZE"};
            if (macro != "FAST")
                names.push_back("ENVELOPE");
            if (macro == "FULL")
                names.push_back("BODY");
        }
    }
    std::vector<FetchItem> out;
    for (auto name : names) {
        FetchItem f;
        f.name = name;
        if (name.starts_with("BODY[") || name.starts_with("BODY.PEEK[")) {
            f.body = true;
            f.peek = name.starts_with("BODY.PEEK[");
            auto open = name.find('['), close = name.find(']');
            if (close == name.npos)
                throw Syntax("Invalid BODY");
            f.section = name.substr(open + 1, close - open - 1);
            f.name = "BODY[" + f.section + "]";
            auto tail = name.substr(close + 1);
            if (!tail.empty()) {
                auto dot = tail.find('.');
                if (tail.front() != '<' || tail.back() != '>' || dot == tail.npos)
                    throw Syntax("Invalid partial");
                f.offset = static_cast<std::size_t>(integer(std::string_view(tail).substr(1, dot - 1), true));
                f.length = static_cast<std::size_t>(
                    integer(std::string_view(tail).substr(dot + 1, tail.size() - dot - 2)));
                f.partial = true;
            }
        } else if (name == "RFC822" || name == "RFC822.TEXT" || name == "RFC822.HEADER") {
            f.body = true;
            f.peek = name == "RFC822.HEADER";
            f.section = name == "RFC822.TEXT" ? "TEXT" : name == "RFC822.HEADER" ? "HEADER" : "";
        } else if (name != "UID" && name != "FLAGS" && name != "INTERNALDATE" && name != "RFC822.SIZE" &&
                   name != "ENVELOPE" && name != "BODY" && name != "BODYSTRUCTURE")
            throw Syntax("Unknown FETCH attribute");
        out.push_back(std::move(f));
        if (out.size() > 64)
            throw Syntax("Too many FETCH items");
    }
    return out;
}
struct SearchContext {
    const MessageInfo &m;
    std::size_t sequence;
    bool recent;
    std::function<const MailPart &()> part;
};
using Predicate = std::function<bool(const SearchContext &)>;
bool contains(std::string value, const std::string &needle) {
    return upper(value).find(upper(needle)) != std::string::npos;
}
std::string searchable(const MailPart &p) {
    std::string s;
    for (auto &ch : p.children)
        s += searchable(ch);
    if (p.children.empty())
        s += mime_decoded(p.body, p.encoding);
    return s;
}
Predicate search_key(const Tokens &a, std::size_t &at, std::uint64_t max_uid, std::size_t count,
                     unsigned depth = 0) {
    if (depth > 32 || at == a.size())
        throw Syntax("Incomplete search");
    auto token = a[at++];
    auto key = upper(token.text);
    auto argument = [&] {
        if (at == a.size())
            throw Syntax("Missing search argument");
        return a[at++].text;
    };
    if (key == "(" && !token.string) {
        std::vector<Predicate> list;
        while (at < a.size() && (a[at].string || a[at].text != ")"))
            list.push_back(search_key(a, at, max_uid, count, depth + 1));
        if (at == a.size() || list.empty())
            throw Syntax("Invalid search group");
        ++at;
        return [list](const auto &c) {
            return std::all_of(list.begin(), list.end(), [&](const auto &p) { return p(c); });
        };
    }
    if (key == "NOT") {
        auto p = search_key(a, at, max_uid, count, depth + 1);
        return [p](const auto &c) { return !p(c); };
    }
    if (key == "OR") {
        auto p = search_key(a, at, max_uid, count, depth + 1),
             q = search_key(a, at, max_uid, count, depth + 1);
        return [p, q](const auto &c) { return p(c) || q(c); };
    }
    if (key == "ALL")
        return [](const auto &) { return true; };
    if (key == "NEW" || key == "OLD" || key == "RECENT")
        return [key](const auto &c) {
            return key == "OLD" ? !c.recent : c.recent && (key != "NEW" || !c.m.seen);
        };
    for (auto flag : {"SEEN", "ANSWERED", "FLAGGED", "DELETED", "DRAFT"})
        if (key == flag || key == std::string("UN") + flag) {
            auto name = canonical_flag("\\" + std::string(flag));
            bool neg = key.starts_with("UN");
            return [name, neg](const auto &c) { return flag_set(c.m).contains(name) != neg; };
        }
    if (key == "KEYWORD" || key == "UNKEYWORD") {
        auto flag = canonical_flag(argument());
        return [flag, key](const auto &c) { return flag_set(c.m).contains(flag) != (key == "UNKEYWORD"); };
    }
    if (key == "LARGER" || key == "SMALLER") {
        auto n = integer(argument(), true);
        return [n, key](const auto &c) { return key == "LARGER" ? c.m.size > n : c.m.size < n; };
    }
    if (key == "UID") {
        Sequence set(argument(), max_uid);
        return [set](const auto &c) { return set.contains(c.m.uid); };
    }
    if (key == "BEFORE" || key == "ON" || key == "SINCE" || key == "SENTBEFORE" || key == "SENTON" ||
        key == "SENTSINCE") {
        auto day = parse_mail_date(argument() + " 00:00:00 +0000", true) / 86400;
        return [day, key](const auto &c) {
            std::int64_t actual = c.m.internal_date / 86400;
            if (key.starts_with("SENT")) {
                try {
                    auto date = c.part().get("DATE");
                    std::string stripped;
                    unsigned comments = 0;
                    for (char ch : date) {
                        if (ch == '(') {
                            ++comments;
                            continue;
                        }
                        if (ch == ')' && comments) {
                            --comments;
                            continue;
                        }
                        if (!comments)
                            stripped += ch;
                    }
                    date = std::move(stripped);
                    while (!date.empty() && (date.back() == ' ' || date.back() == '\t'))
                        date.pop_back();
                    auto end = date.find_last_of(' ');
                    if (end != date.npos)
                        date.replace(end + 1, std::string::npos, "+0000");
                    actual = parse_mail_date(date) / 86400;
                } catch (...) {
                    return false;
                }
            }
            return key.ends_with("BEFORE")  ? actual < day
                   : key.ends_with("SINCE") ? actual >= day
                                            : actual == day;
        };
    }
    if (key == "HEADER") {
        auto name = upper(argument()), value = argument();
        return [name, value](const auto &c) {
            for (auto &h : c.part().headers)
                if (h.name == name && contains(mime_words(h.value), value))
                    return true;
            return false;
        };
    }
    if (key == "FROM" || key == "TO" || key == "CC" || key == "BCC" || key == "SUBJECT") {
        auto value = argument();
        return [key, value](const auto &c) {
            for (auto &h : c.part().headers)
                if (h.name == key && contains(mime_words(h.value), value))
                    return true;
            return false;
        };
    }
    if (key == "BODY" || key == "TEXT") {
        auto value = argument();
        return [key, value](const auto &c) {
            if (contains(searchable(c.part()), value))
                return true;
            if (key == "TEXT")
                for (auto &h : c.part().headers)
                    if (contains(mime_words(h.value), value))
                        return true;
            return false;
        };
    }
    if (!key.empty() && (std::isdigit(static_cast<unsigned char>(key[0])) || key[0] == '*')) {
        Sequence set(key, count);
        return [set](const auto &c) { return set.contains(c.sequence); };
    }
    throw Syntax("Unknown search key");
}
std::shared_ptr<Login> authenticate_plain(Server &s, Connection &c, std::string encoded) {
    std::string decoded = mime_decoded(encoded, "BASE64");
    wipe(encoded);
    struct Wiper {
        std::string &s;
        ~Wiper() {
            wipe(s);
        }
    } w{decoded};
    auto first = decoded.find('\0'),
         second = first == decoded.npos ? decoded.npos : decoded.find('\0', first + 1);
    if (first == decoded.npos || second == decoded.npos || decoded.find('\0', second + 1) != decoded.npos)
        throw Syntax("Invalid SASL data");
    auto user = decoded.substr(first + 1, second - first - 1);
    if (first && decoded.substr(0, first) != user)
        throw Syntax("Invalid authorization identity");
    return s.authenticate(user, std::string_view(decoded).substr(second + 1), c.peer);
}
} // namespace
void imap(Connection &c, Server &server) {
    const std::string capabilities =
        "IMAP4rev1 AUTH=PLAIN IDLE UIDPLUS MOVE UNSELECT NAMESPACE ID SPECIAL-USE";
    c.write("* OK [CAPABILITY " + capabilities + "] CloudieMail ready\r\n");
    std::shared_ptr<Login> login;
    std::string selected;
    bool readonly = false;
    std::vector<MessageInfo> messages;
    std::set<std::uint64_t> recent;
    auto &db = server.database();
    auto validity = [&] {
        auto key = login->identity.at("key_id").get<std::string>();
        auto n = std::stoull(key.substr(key.size() - 8), nullptr, 16);
        return std::to_string(n ? n : 1);
    };
    auto synchronize = [&](bool expunges, bool report_flags = true) {
        if (selected.empty())
            return;
        auto fresh = db.messages(login->email, selected, !readonly);
        std::map<std::uint64_t, MessageInfo> current;
        for (auto &m : fresh)
            current.emplace(m.uid, m);
        auto old_count = messages.size();
        std::size_t index = 0;
        while (index < messages.size()) {
            auto &old = messages[index];
            auto it = current.find(old.uid);
            if (it == current.end()) {
                if (!expunges) {
                    ++index;
                    continue;
                }
                c.write("* " + std::to_string(index + 1) + " EXPUNGE\r\n");
                recent.erase(old.uid);
                messages.erase(messages.begin() + static_cast<std::ptrdiff_t>(index));
                continue;
            }
            if (report_flags && flag_set(old) != flag_set(it->second))
                c.write("* " + std::to_string(index + 1) + " FETCH (UID " + std::to_string(old.uid) +
                        " FLAGS " + flags(it->second, recent.contains(old.uid)) + ")\r\n");
            old = it->second;
            current.erase(it);
            ++index;
        }
        auto before_recent = recent.size();
        for (auto &[uid, m] : current) {
            messages.push_back(m);
            if (m.recent)
                recent.insert(uid);
        }
        if (old_count != messages.size() || !current.empty())
            c.write("* " + std::to_string(messages.size()) + " EXISTS\r\n");
        if (before_recent != recent.size())
            c.write("* " + std::to_string(recent.size()) + " RECENT\r\n");
    };
    for (;;) {
        c.renew(1860);
        Tokens a;
        while (!c.readable(1000)) {
        }
        try {
            a = tokens(c, login ? server.config().max_message_bytes + 65536 : 8192);
        } catch (const Syntax &) {
            c.write("* BAD Invalid syntax\r\n");
            continue;
        }
        struct Wiper {
            Tokens &a;
            ~Wiper() {
                for (auto &t : a)
                    wipe(t.text);
            }
        } wiper{a};
        if (a.size() < 2 || a[0].string || a[0].text.empty() || a[0].text.size() > 64 ||
            a[0].text.find_first_of(" (){%*\"\\]+") != std::string::npos ||
            std::any_of(a[0].text.begin(), a[0].text.end(),
                        [](unsigned char ch) { return ch <= 32 || ch >= 127; })) {
            c.write("* BAD Invalid tag\r\n");
            continue;
        }
        auto tag = a[0].text, cmd = upper(a[1].text);
        auto ok = [&](const std::string &info = "Completed") { c.write(tag + " OK " + info + "\r\n"); };
        auto no = [&](const std::string &info) { c.write(tag + " NO " + info + "\r\n"); };
        auto exact = [&](std::size_t n) {
            if (a.size() != n)
                throw Syntax("Invalid arguments");
        };
        try {
            if (cmd == "CAPABILITY") {
                exact(2);
                c.write("* CAPABILITY " + capabilities + "\r\n");
                ok();
                continue;
            }
            if (cmd == "LOGOUT") {
                exact(2);
                c.write("* BYE Logging out\r\n");
                ok();
                return;
            }
            if (cmd == "NOOP") {
                exact(2);
                if (login)
                    synchronize(true);
                ok();
                continue;
            }
            if (cmd == "ID") {
                bool nil = a.size() == 3 && upper(a[2].text) == "NIL";
                if (!nil) {
                    if (a.size() < 4 || a[2].text != "(" || a.back().text != ")" || (a.size() - 4) % 2 ||
                        a.size() > 64)
                        throw Syntax("Invalid ID arguments");
                    for (std::size_t i = 3; i + 1 < a.size(); ++i)
                        if (a[i].text.size() > 1024)
                            throw Syntax("ID value too long");
                }
                c.write("* ID (\"name\" \"CloudieMail\" \"version\" \"" CLOUDIEMAIL_VERSION "\")\r\n");
                ok();
                continue;
            }
            if (cmd == "LOGIN" || cmd == "AUTHENTICATE") {
                if (login) {
                    throw Syntax("Already authenticated");
                }
                if (!c.secure()) {
                    no("[PRIVACYREQUIRED] TLS required");
                    continue;
                }
                try {
                    if (cmd == "LOGIN") {
                        exact(4);
                        login = server.authenticate(a[2].text, a[3].text, c.peer);
                    } else {
                        exact(3);
                        if (upper(a[2].text) != "PLAIN") {
                            no("Unsupported authentication mechanism");
                            continue;
                        }
                        c.write("+ \r\n");
                        auto response = c.line(8192);
                        if (response == "*") {
                            c.write(tag + " BAD Authentication cancelled\r\n");
                            continue;
                        }
                        login = authenticate_plain(server, c, std::move(response));
                    }
                    ok("Authenticated");
                } catch (const Syntax &) {
                    throw;
                } catch (...) {
                    no("[AUTHENTICATIONFAILED] Invalid credentials");
                }
                continue;
            }
            if (!login) {
                no("Authenticate first");
                continue;
            }
            bool uid = cmd == "UID";
            std::size_t base = uid ? 3 : 2;
            if (uid) {
                if (a.size() < 3)
                    throw Syntax("Missing UID command");
                cmd = upper(a[2].text);
                if (cmd != "FETCH" && cmd != "STORE" && cmd != "SEARCH" && cmd != "COPY" && cmd != "MOVE" &&
                    cmd != "EXPUNGE")
                    throw Syntax("Unknown UID command");
            }
            if (!selected.empty() && cmd != "SELECT" && cmd != "EXAMINE" && cmd != "CLOSE" &&
                cmd != "UNSELECT")
                synchronize(cmd != "FETCH" && cmd != "STORE" && cmd != "SEARCH", cmd != "STORE");
            if (cmd == "NAMESPACE") {
                exact(2);
                c.write("* NAMESPACE ((\"\" \"/\")) NIL NIL\r\n");
                ok();
                continue;
            }
            if (cmd == "LIST" || cmd == "LSUB") {
                std::size_t position = 2;
                bool special_only = false;
                if (cmd == "LIST" && a.size() > 4 && a[position].text == "(") {
                    if (a.size() < 7 || upper(a[position + 1].text) != "SPECIAL-USE" ||
                        a[position + 2].text != ")")
                        throw Syntax("Unknown LIST selection");
                    special_only = true;
                    position += 3;
                }
                if (a.size() < position + 2)
                    throw Syntax("Missing LIST arguments");
                auto reference = folder_name(a[position]), pattern = folder_name(a[position + 1]);
                position += 2;
                if (position < a.size()) {
                    if (cmd != "LIST" || a.size() != position + 4 || upper(a[position].text) != "RETURN" ||
                        a[position + 1].text != "(" || upper(a[position + 2].text) != "SPECIAL-USE" ||
                        a[position + 3].text != ")")
                        throw Syntax("Unknown LIST return options");
                }
                if (pattern.empty()) {
                    c.write("* " + cmd + " (\\Noselect) \"/\" \"\"\r\n");
                    ok();
                    continue;
                }
                if (!reference.empty() && !pattern.starts_with('/'))
                    pattern = reference + pattern;
                if (pattern.starts_with('/'))
                    pattern.erase(0, 1);
                std::set<std::string> names, real;
                for (auto &f : db.folders(login->email))
                    real.insert(f["name"].get<std::string>());
                if (cmd == "LSUB")
                    for (auto &s : db.subscriptions(login->email))
                        names.insert(s.get<std::string>());
                else
                    names = real;
                auto original = names;
                for (auto &name : original)
                    for (auto slash = name.find('/'); slash != name.npos; slash = name.find('/', slash + 1))
                        names.insert(name.substr(0, slash));
                for (auto &name : names)
                    if (wildcard(pattern, name)) {
                        bool child = std::any_of(names.begin(), names.end(),
                                                 [&](const auto &n) { return n.starts_with(name + "/"); });
                        std::string special;
                        if (real.contains(name)) {
                            if (name == "Sent")
                                special = "\\Sent";
                            if (name == "Drafts")
                                special = "\\Drafts";
                            if (name == "Junk")
                                special = "\\Junk";
                            if (name == "Trash")
                                special = "\\Trash";
                        }
                        if (special_only && special.empty())
                            continue;
                        auto attributes = real.contains(name) ? std::string("") : std::string("\\Noselect ");
                        if (cmd == "LIST" && !special.empty())
                            attributes += special + " ";
                        attributes += child ? "\\HasChildren" : "\\HasNoChildren";
                        c.write("* " + cmd + " (" + attributes + ") \"/\" " +
                                imap_string(mailbox_encode(name)) + "\r\n");
                    }
                ok();
                continue;
            }
            if (cmd == "CREATE" || cmd == "DELETE" || cmd == "RENAME" || cmd == "SUBSCRIBE" ||
                cmd == "UNSUBSCRIBE") {
                exact(cmd == "RENAME" ? 4 : 3);
                auto folder = folder_name(a[2]);
                if (cmd == "CREATE") {
                    if (folder.ends_with('/'))
                        folder.pop_back();
                    db.create_folder(login->email, folder);
                }
                if (cmd == "DELETE") {
                    if (!exists(db, login->email, folder))
                        throw std::runtime_error("Mailbox not found");
                    db.remove_folder(login->email, folder, true);
                }
                if (cmd == "RENAME")
                    db.rename_folder(login->email, folder, folder_name(a[3]));
                if (cmd == "SUBSCRIBE" || cmd == "UNSUBSCRIBE")
                    db.subscribe(login->email, folder, cmd == "SUBSCRIBE");
                if (selected == folder && (cmd == "DELETE" || cmd == "RENAME")) {
                    selected.clear();
                    messages.clear();
                    recent.clear();
                }
                ok();
                continue;
            }
            if (cmd == "SELECT" || cmd == "EXAMINE") {
                exact(3);
                selected.clear();
                messages.clear();
                recent.clear();
                auto folder = folder_name(a[2]);
                if (!exists(db, login->email, folder)) {
                    no("[NONEXISTENT] Mailbox not found");
                    continue;
                }
                selected = folder;
                readonly = cmd == "EXAMINE";
                messages = db.messages(login->email, selected, !readonly);
                for (auto &m : messages)
                    if (m.recent)
                        recent.insert(m.uid);
                c.write("* FLAGS (\\Seen \\Answered \\Flagged \\Deleted \\Draft)\r\n* " +
                        std::to_string(messages.size()) + " EXISTS\r\n* " + std::to_string(recent.size()) +
                        " RECENT\r\n* OK [PERMANENTFLAGS " +
                        std::string(readonly ? "()" : "(\\Seen \\Answered \\Flagged \\Deleted \\Draft \\*)") +
                        "] Flags\r\n* OK [UIDVALIDITY " + validity() + "] Validity\r\n* OK [UIDNEXT " +
                        std::to_string(db.uid_next()) + "] Next UID\r\n");
                for (std::size_t i = 0; i < messages.size(); ++i)
                    if (!messages[i].seen) {
                        c.write("* OK [UNSEEN " + std::to_string(i + 1) + "] First unseen\r\n");
                        break;
                    }
                ok(readonly ? "[READ-ONLY] Selected" : "[READ-WRITE] Selected");
                continue;
            }
            if (cmd == "STATUS") {
                if (a.size() < 6 || a[3].text != "(" || a.back().text != ")")
                    throw Syntax("Invalid STATUS");
                auto name = folder_name(a[2]);
                if (!exists(db, login->email, name)) {
                    no("Mailbox not found");
                    continue;
                }
                auto list = db.messages(login->email, name);
                std::string response;
                for (std::size_t i = 4; i + 1 < a.size(); ++i) {
                    auto attr = upper(a[i].text);
                    std::uint64_t value = 0;
                    if (attr == "MESSAGES")
                        value = list.size();
                    else if (attr == "UNSEEN")
                        value = static_cast<std::uint64_t>(
                            std::count_if(list.begin(), list.end(), [](const auto &m) { return !m.seen; }));
                    else if (attr == "RECENT")
                        value = static_cast<std::uint64_t>(
                            std::count_if(list.begin(), list.end(), [](const auto &m) { return m.recent; }));
                    else if (attr == "UIDNEXT")
                        value = db.uid_next();
                    else if (attr == "UIDVALIDITY")
                        value = std::stoull(validity());
                    else
                        throw Syntax("Unknown STATUS item");
                    if (!response.empty())
                        response += ' ';
                    response += attr + " " + std::to_string(value);
                }
                c.write("* STATUS " + imap_string(mailbox_encode(name)) + " (" + response + ")\r\n");
                ok();
                continue;
            }
            if (cmd == "APPEND") {
                if (a.size() < 4)
                    throw Syntax("Invalid APPEND");
                auto folder = folder_name(a[2]);
                std::size_t pos = 3;
                Json f = Json::array();
                if (!a[pos].string && a[pos].text == "(")
                    f = parse_flags(a, pos);
                std::int64_t date = std::time(nullptr);
                if (pos + 2 == a.size())
                    date = parse_mail_date(a[pos++].text, true);
                if (pos + 1 != a.size() || !a[pos].string || a[pos].text.empty() ||
                    a[pos].text.size() > server.config().max_message_bytes)
                    throw Syntax("Invalid message literal");
                if (a[pos].text.find('\0') != std::string::npos)
                    throw Syntax("NUL in message");
                auto id = db.append(
                    {login->email, seal_message(login->identity, a[pos].text), a[pos].text.size(), folder},
                    server.config().mailbox_quota_bytes, f, date);
                synchronize(true);
                ok("[APPENDUID " + validity() + " " + std::to_string(id) + "] Appended");
                continue;
            }
            if (cmd == "IDLE") {
                exact(2);
                c.write("+ Idling\r\n");
                auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(29);
                while (!server.stopping()) {
                    if (c.readable(1000)) {
                        auto done = c.line(16);
                        if (upper(done) != "DONE")
                            throw Syntax("Expected DONE");
                        break;
                    }
                    synchronize(true);
                    if (std::chrono::steady_clock::now() >= deadline) {
                        c.write("* BYE Idle timeout\r\n");
                        return;
                    }
                }
                if (server.stopping())
                    return;
                ok("IDLE terminated");
                continue;
            }
            if (selected.empty()) {
                no("Select a mailbox first");
                continue;
            }
            if (cmd == "UNSELECT") {
                exact(2);
                selected.clear();
                messages.clear();
                recent.clear();
                ok();
                continue;
            }
            if (cmd == "CHECK") {
                exact(2);
                ok();
                continue;
            }
            if (cmd == "CLOSE" || cmd == "EXPUNGE") {
                exact(uid ? 4 : 2);
                if (readonly && cmd == "EXPUNGE") {
                    no("Mailbox is read-only");
                    continue;
                }
                if (!readonly) {
                    auto set = uid ? selected_uids(messages, a[3].text, true) : std::vector<std::uint64_t>{};
                    db.expunge(login->email, selected, uid ? &set : nullptr);
                }
                if (cmd == "CLOSE") {
                    selected.clear();
                    messages.clear();
                    recent.clear();
                } else
                    synchronize(true);
                ok();
                continue;
            }
            if (cmd == "SEARCH") {
                std::size_t pos = base;
                if (pos < a.size() && upper(a[pos].text) == "CHARSET") {
                    if (pos + 1 == a.size())
                        throw Syntax("Missing charset");
                    auto charset = upper(a[pos + 1].text);
                    if (charset != "UTF-8" && charset != "US-ASCII") {
                        no("[BADCHARSET (US-ASCII UTF-8)] Unsupported charset");
                        continue;
                    }
                    pos += 2;
                }
                std::vector<Predicate> predicates;
                while (pos < a.size())
                    predicates.push_back(
                        search_key(a, pos, messages.empty() ? 0 : messages.back().uid, messages.size()));
                if (predicates.empty())
                    throw Syntax("Missing search key");
                std::string result = "* SEARCH";
                for (std::size_t i = 0; i < messages.size(); ++i) {
                    auto &m = messages[i];
                    std::string raw;
                    struct W {
                        std::string &s;
                        ~W() {
                            wipe(s);
                        }
                    } w{raw};
                    std::optional<MailPart> part;
                    auto load = [&]() -> const MailPart & {
                        if (!part) {
                            raw = open_message(login->identity, login->key, db.message(login->email, m.uid));
                            part = parse_message(raw);
                        }
                        return *part;
                    };
                    SearchContext context{m, i + 1, recent.contains(m.uid), load};
                    if (std::all_of(predicates.begin(), predicates.end(),
                                    [&](const auto &p) { return p(context); }))
                        result += " " + std::to_string(uid ? m.uid : i + 1);
                }
                c.write(result + "\r\n");
                ok();
                continue;
            }
            if (cmd == "COPY" || cmd == "MOVE") {
                exact(base + 2);
                if (readonly && cmd == "MOVE") {
                    no("Mailbox is read-only");
                    continue;
                }
                auto ids = selected_uids(messages, a[base].text, uid);
                auto map = db.transfer(login->email, selected, folder_name(a[base + 1]), ids, cmd == "MOVE",
                                       server.config().mailbox_quota_bytes);
                std::vector<std::uint64_t> source, target;
                for (auto [s, t] : map) {
                    source.push_back(s);
                    target.push_back(t);
                }
                if (cmd == "MOVE")
                    synchronize(true);
                ok(map.empty() ? "Completed"
                               : "[COPYUID " + validity() + " " + uid_list(source) + " " + uid_list(target) +
                                     "] Completed");
                continue;
            }
            if (cmd == "STORE") {
                if (readonly) {
                    no("Mailbox is read-only");
                    continue;
                }
                if (a.size() < base + 3)
                    throw Syntax("Invalid STORE");
                auto ids = selected_uids(messages, a[base].text, uid);
                auto operation = upper(a[base + 1].text);
                bool silent = operation.ends_with(".SILENT");
                if (silent)
                    operation.resize(operation.size() - 7);
                int mode = operation == "+FLAGS" ? 1 : operation == "-FLAGS" ? -1 : 0;
                if (operation != "FLAGS" && operation != "+FLAGS" && operation != "-FLAGS")
                    throw Syntax("Invalid STORE operation");
                auto pos = base + 2;
                auto f = parse_flags(a, pos, true);
                if (pos != a.size())
                    throw Syntax("Invalid STORE flags");
                db.store_flags(login->email, selected, ids, f, mode);
                auto current = db.messages(login->email, selected, !readonly);
                for (std::size_t i = 0; i < messages.size(); ++i)
                    for (auto &m : current)
                        if (messages[i].uid == m.uid) {
                            messages[i] = m;
                            if (!silent && std::find(ids.begin(), ids.end(), m.uid) != ids.end())
                                c.write("* " + std::to_string(i + 1) + " FETCH (" +
                                        std::string(uid ? "UID " + std::to_string(m.uid) + " " : "") +
                                        "FLAGS " + flags(m, recent.contains(m.uid)) + ")\r\n");
                            break;
                        }
                ok();
                continue;
            }
            if (cmd == "FETCH") {
                if (a.size() < base + 2)
                    throw Syntax("Invalid FETCH");
                auto ids = selected_uids(messages, a[base].text, uid);
                auto items = fetch_items(a, base + 1);
                if (uid &&
                    std::none_of(items.begin(), items.end(), [](const auto &f) { return f.name == "UID"; }))
                    items.push_back({"UID", ""});
                for (std::size_t i = 0; i < messages.size(); ++i) {
                    auto &m = messages[i];
                    if (std::find(ids.begin(), ids.end(), m.uid) == ids.end())
                        continue;
                    std::string raw;
                    bool needs_mime = std::any_of(items.begin(), items.end(), [](const auto &f) {
                        return f.body || f.name == "BODY" || f.name == "BODYSTRUCTURE" ||
                               f.name == "ENVELOPE";
                    });
                    try {
                        if (needs_mime)
                            raw = open_message(login->identity, login->key, db.message(login->email, m.uid));
                    } catch (...) {
                        continue;
                    }
                    struct W {
                        std::string &s;
                        ~W() {
                            wipe(s);
                        }
                    } w{raw};
                    auto part = needs_mime ? parse_message(raw) : MailPart{};
                    bool mark = !readonly && std::any_of(items.begin(), items.end(),
                                                         [](const auto &f) { return f.body && !f.peek; });
                    bool changed = mark && !m.seen;
                    // Validate all sections before changing flags or writing an untagged response.
                    std::vector<std::pair<bool, std::string>> sections;
                    std::size_t response_bytes = 0;
                    for (auto &f : items)
                        if (f.body) {
                            bool found;
                            auto value = section(part, f.section, found);
                            if (f.partial)
                                value = f.offset >= value.size() ? "" : value.substr(f.offset, f.length);
                            response_bytes += value.size();
                            if (response_bytes > server.config().max_message_bytes * 2 + 131072)
                                throw Syntax("FETCH response limit exceeded");
                            sections.emplace_back(found, std::move(value));
                        }
                    if (mark) {
                        db.set_seen(login->email, m.uid, true);
                        m.seen = true;
                    }
                    std::string response = "* " + std::to_string(i + 1) + " FETCH (";
                    std::size_t index = 0;
                    bool first = true;
                    for (auto &f : items) {
                        if (!first)
                            response += ' ';
                        first = false;
                        response += f.name;
                        if (f.body) {
                            auto &[found, value] = sections[index++];
                            if (f.partial)
                                response += "<" + std::to_string(f.offset) + ">";
                            response +=
                                found ? " {" + std::to_string(value.size()) + "}\r\n" + value : " NIL";
                            wipe(value);
                        } else if (f.name == "UID")
                            response += " " + std::to_string(m.uid);
                        else if (f.name == "FLAGS")
                            response += " " + flags(m, recent.contains(m.uid));
                        else if (f.name == "RFC822.SIZE")
                            response += " " + std::to_string(m.size);
                        else if (f.name == "INTERNALDATE")
                            response += " " + imap_string(imap_date(m.internal_date));
                        else if (f.name == "ENVELOPE")
                            response += " " + imap_envelope(part);
                        else
                            response += " " + imap_bodystructure(part, f.name == "BODYSTRUCTURE");
                    }
                    if (changed && std::none_of(items.begin(), items.end(),
                                                [](const auto &f) { return f.name == "FLAGS"; }))
                        response += " FLAGS " + flags(m, recent.contains(m.uid));
                    response += ")\r\n";
                    c.write(response);
                    wipe(response);
                }
                ok();
                continue;
            }
            throw Syntax("Unknown command");
        } catch (const Syntax &e) {
            c.write(tag + " BAD " + e.what() + "\r\n");
        } catch (const MailboxError &e) {
            no(e.what());
        } catch (...) {
            no("Command failed: check mailbox, message and quota");
        }
    }
}
} // namespace cm
