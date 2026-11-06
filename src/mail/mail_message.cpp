#include "cloudiemail/mail_message.hpp"
#include "cloudiemail/server.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <map>
#include <regex>
#include <sodium.h>
#include <sstream>

namespace cm {
namespace {
std::string trim(std::string_view s) {
    auto b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    return b == s.npos ? "" : std::string(s.substr(b, e - b + 1));
}
std::vector<std::string> split(std::string_view s, char delimiter) {
    std::vector<std::string> out;
    std::string item;
    bool quoted = false, escape = false;
    unsigned comment = 0, angle = 0;
    for (char c : s) {
        if (escape) {
            item += c;
            escape = false;
            continue;
        }
        if (c == '\\') {
            item += c;
            escape = true;
            continue;
        }
        if (!comment && c == '"')
            quoted = !quoted;
        if (!quoted) {
            if (c == '(')
                ++comment;
            if (c == ')' && comment)
                --comment;
            if (c == '<')
                ++angle;
            if (c == '>' && angle)
                --angle;
        }
        if (c == delimiter && !quoted && !comment && !angle) {
            out.push_back(trim(item));
            item.clear();
        } else
            item += c;
    }
    out.push_back(trim(item));
    return out;
}
std::string unquote(std::string s) {
    s = trim(s);
    if (s.size() < 2 || s.front() != '"' || s.back() != '"')
        return s;
    std::string out;
    for (std::size_t i = 1; i + 1 < s.size(); ++i) {
        if (s[i] == '\\' && i + 2 < s.size())
            ++i;
        out += s[i];
    }
    return out;
}
std::pair<std::string, std::map<std::string, std::string>> parameters(std::string_view value) {
    auto fields = split(value, ';');
    std::map<std::string, std::string> p;
    for (std::size_t i = 1; i < fields.size(); ++i) {
        auto eq = fields[i].find('=');
        if (eq != std::string::npos)
            p[upper(trim(std::string_view(fields[i]).substr(0, eq)))] = unquote(fields[i].substr(eq + 1));
    }
    return {upper(fields[0]), p};
}
MailPart parse(std::string_view raw, unsigned depth, unsigned &count, bool digest = false) {
    if (depth > 32 || ++count > 512)
        throw std::runtime_error("MIME nesting limit");
    MailPart p;
    p.raw = raw;
    auto end = raw.find("\r\n\r\n");
    if (raw.starts_with("\r\n")) {
        p.header = raw.substr(0, 2);
        p.body = raw.substr(2);
    } else if (end == raw.npos) {
        p.header = {};
        p.body = raw;
    } else {
        p.header = raw.substr(0, end + 4);
        p.body = raw.substr(end + 4);
    }
    if (p.header.size() > 65536)
        throw std::runtime_error("MIME header limit");
    std::size_t at = 0;
    while (at < p.header.size()) {
        auto next = p.header.find("\r\n", at);
        if (next == p.header.npos || next == at)
            break;
        auto line = p.header.substr(at, next - at);
        if ((line[0] == ' ' || line[0] == '\t') && !p.headers.empty()) {
            auto &h = p.headers.back();
            h.value += ' ' + trim(line);
            h.raw = raw.substr(static_cast<std::size_t>(h.raw.data() - raw.data()),
                               next + 2 - static_cast<std::size_t>(h.raw.data() - raw.data()));
        } else {
            auto colon = line.find(':');
            if (colon != line.npos)
                p.headers.push_back({upper(std::string(line.substr(0, colon))), trim(line.substr(colon + 1)),
                                     raw.substr(at, next + 2 - at)});
        }
        at = next + 2;
    }
    auto content = p.get("CONTENT-TYPE");
    if (content.empty())
        content = digest ? "message/rfc822" : "text/plain; charset=us-ascii";
    auto [kind, params] = parameters(content);
    auto slash = kind.find('/');
    if (slash != kind.npos) {
        p.type = kind.substr(0, slash);
        p.subtype = kind.substr(slash + 1);
    }
    p.parameters = std::move(params);
    if (auto enc = p.get("CONTENT-TRANSFER-ENCODING"); !enc.empty())
        p.encoding = upper(trim(enc));
    if (p.type == "MULTIPART" && p.parameters.contains("BOUNDARY")) {
        auto boundary = "--" + p.parameters.at("BOUNDARY");
        if (boundary.size() > 202)
            throw std::runtime_error("MIME boundary limit");
        std::size_t pos = 0, begin = std::string::npos;
        while (pos < p.body.size()) {
            auto next = p.body.find("\r\n", pos);
            if (next == p.body.npos)
                next = p.body.size();
            auto line = trim(p.body.substr(pos, next - pos));
            if (line == boundary || line == boundary + "--") {
                if (begin != std::string::npos) {
                    auto finish = pos >= 2 ? pos - 2 : pos;
                    p.children.push_back(parse(p.body.substr(begin, finish >= begin ? finish - begin : 0),
                                               depth + 1, count, p.subtype == "DIGEST"));
                }
                if (line == boundary + "--") {
                    begin = std::string::npos;
                    break;
                }
                begin = std::min(next + 2, p.body.size());
            }
            pos = std::min(next + 2, p.body.size());
        }
        if (begin != std::string::npos && begin < p.body.size())
            p.children.push_back(parse(p.body.substr(begin), depth + 1, count, p.subtype == "DIGEST"));
    } else if (p.type == "MESSAGE" && p.subtype == "RFC822")
        p.children.push_back(parse(p.body, depth + 1, count));
    return p;
}
std::string nstring(const std::string &s) {
    return s.empty() ? "NIL" : imap_string(s);
}
std::string params(const std::map<std::string, std::string> &p) {
    if (p.empty())
        return "NIL";
    std::string out = "(";
    for (auto &[k, v] : p) {
        if (out.size() > 1)
            out += ' ';
        out += imap_string(k) + " " + imap_string(v);
    }
    return out + ")";
}
std::string addresses(std::string value) {
    if (value.empty())
        return "NIL";
    std::string result = "(";
    bool group = false;
    for (auto item : split(value, ',')) {
        // Colons outside quotes start RFC 5322 address groups.
        auto colon = item.find(':');
        auto bracket = item.find('<');
        if (!group && colon != item.npos && (bracket == item.npos || colon < bracket) &&
            item.find('"') == item.npos) {
            result += "(NIL NIL " + imap_string(trim(std::string_view(item).substr(0, colon))) + " NIL)";
            item = trim(std::string_view(item).substr(colon + 1));
            group = true;
        }
        bool group_end = group && !item.empty() && item.back() == ';';
        if (group_end)
            item.pop_back();
        std::string name, address;
        auto lt = item.find('<'), gt = item.rfind('>');
        if (lt != item.npos && gt > lt) {
            name = unquote(item.substr(0, lt));
            address = trim(std::string_view(item).substr(lt + 1, gt - lt - 1));
        } else {
            unsigned comment = 0;
            bool quote = false, escape = false;
            for (char c : item) {
                if (escape) {
                    if (!comment)
                        address += c;
                    escape = false;
                    continue;
                }
                if (c == '\\') {
                    escape = true;
                    continue;
                }
                if (c == '"' && !comment)
                    quote = !quote;
                if (!quote && c == '(') {
                    ++comment;
                    continue;
                }
                if (!quote && c == ')' && comment) {
                    --comment;
                    continue;
                }
                if (!comment)
                    address += c;
            }
            address = trim(address);
        }
        if (!address.empty()) {
            std::string route;
            if (address.front() == '@') {
                auto c = address.find(':');
                if (c != address.npos) {
                    route = address.substr(0, c);
                    address.erase(0, c + 1);
                }
            }
            auto at = address.rfind('@');
            result += "(" + nstring(name) + " " + nstring(route) + " " +
                      imap_string(unquote(address.substr(0, at))) + " " +
                      (at == address.npos ? "NIL" : imap_string(address.substr(at + 1))) + ")";
        }
        if (group_end) {
            result += "(NIL NIL NIL NIL)";
            group = false;
        }
    }
    if (group)
        result += "(NIL NIL NIL NIL)";
    return result == "(" ? "NIL" : result + ")";
}
std::size_t lines(std::string_view s) {
    return static_cast<std::size_t>(std::count(s.begin(), s.end(), '\n'));
}
std::string b64(std::string_view s) {
    std::string out(sodium_base64_ENCODED_LEN(s.size(), sodium_base64_VARIANT_ORIGINAL), '\0');
    sodium_bin2base64(out.data(), out.size(), reinterpret_cast<const unsigned char *>(s.data()), s.size(),
                      sodium_base64_VARIANT_ORIGINAL);
    out.resize(std::strlen(out.c_str()));
    return out;
}
} // namespace
std::string MailPart::get(std::string_view name) const {
    for (auto &h : headers)
        if (h.name == name)
            return h.value;
    return {};
}
MailPart parse_message(std::string_view raw) {
    unsigned count = 0;
    return parse(raw, 0, count);
}
std::string imap_string(std::string_view value) {
    if (std::any_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c >= 127; }))
        return "{" + std::to_string(value.size()) + "}\r\n" + std::string(value);
    std::string out = "\"";
    for (char c : value) {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out + '"';
}
std::string imap_envelope(const MailPart &p) {
    auto from = p.get("FROM"), sender = p.get("SENDER"), reply = p.get("REPLY-TO");
    return "(" + nstring(p.get("DATE")) + " " + nstring(p.get("SUBJECT")) + " " + addresses(from) + " " +
           addresses(sender.empty() ? from : sender) + " " + addresses(reply.empty() ? from : reply) + " " +
           addresses(p.get("TO")) + " " + addresses(p.get("CC")) + " " + addresses(p.get("BCC")) + " " +
           nstring(p.get("IN-REPLY-TO")) + " " + nstring(p.get("MESSAGE-ID")) + ")";
}
std::string imap_bodystructure(const MailPart &p, bool extended) {
    auto disposition = [&] {
        auto [kind, par] = parameters(p.get("CONTENT-DISPOSITION"));
        return kind.empty() ? std::string("NIL") : "(" + imap_string(kind) + " " + params(par) + ")";
    };
    auto language = [&] {
        auto lang = p.get("CONTENT-LANGUAGE");
        if (lang.empty())
            return std::string("NIL");
        auto list = split(lang, ',');
        if (list.size() == 1)
            return imap_string(list[0]);
        std::string out = "(";
        for (auto &v : list) {
            if (out.size() > 1)
                out += ' ';
            out += imap_string(v);
        }
        return out + ")";
    };
    std::string out = "(";
    if (p.type == "MULTIPART" && !p.children.empty()) {
        for (auto &part : p.children)
            out += imap_bodystructure(part, extended);
        out += " " + imap_string(p.subtype);
        if (extended)
            out += " " + params(p.parameters) + " " + disposition() + " " + language() + " " +
                   nstring(p.get("CONTENT-LOCATION"));
    } else {
        out += imap_string(p.type) + " " + imap_string(p.subtype) + " " + params(p.parameters) + " " +
               nstring(p.get("CONTENT-ID")) + " " + nstring(p.get("CONTENT-DESCRIPTION")) + " " +
               imap_string(p.encoding) + " " + std::to_string(p.body.size());
        if (p.type == "TEXT")
            out += " " + std::to_string(lines(p.body));
        if (p.type == "MESSAGE" && p.subtype == "RFC822" && !p.children.empty())
            out += " " + imap_envelope(p.children[0]) + " " + imap_bodystructure(p.children[0], extended) +
                   " " + std::to_string(lines(p.body));
        if (extended)
            out += " " + nstring(p.get("CONTENT-MD5")) + " " + disposition() + " " + language() + " " +
                   nstring(p.get("CONTENT-LOCATION"));
    }
    return out + ")";
}
std::string mime_decoded(std::string_view text, std::string_view encoding) {
    if (encoding == "BASE64") {
        std::string result(text.size(), '\0');
        std::size_t n = 0;
        if (sodium_base642bin(reinterpret_cast<unsigned char *>(result.data()), result.size(), text.data(),
                              text.size(), " \t\r\n", &n, nullptr, sodium_base64_VARIANT_ORIGINAL) == 0) {
            result.resize(n);
            return result;
        }
    } else if (encoding == "QUOTED-PRINTABLE") {
        std::string result;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '=' && i + 2 < text.size()) {
                if (text.substr(i + 1, 2) == "\r\n") {
                    i += 2;
                    continue;
                }
                unsigned n = 0;
                auto [end, ec] = std::from_chars(text.data() + i + 1, text.data() + i + 3, n, 16);
                if (ec == std::errc() && end == text.data() + i + 3) {
                    result += static_cast<char>(n);
                    i += 2;
                    continue;
                }
            }
            result += text[i];
        }
        return result;
    }
    return std::string(text);
}
std::string mime_words(std::string_view text) {
    std::string result;
    std::size_t pos = 0;
    bool previous = false;
    while (pos < text.size()) {
        auto start = text.find("=?", pos);
        if (start == text.npos) {
            result += text.substr(pos);
            break;
        }
        auto c = text.find('?', start + 2), e = c == text.npos ? text.npos : text.find('?', c + 1),
             end = e == text.npos ? text.npos : text.find("?=", e + 1);
        if (end == text.npos) {
            result += text.substr(pos);
            break;
        }
        auto gap = text.substr(pos, start - pos);
        if (!previous || !trim(gap).empty())
            result += gap;
        auto charset = upper(std::string(text.substr(start + 2, c - start - 2))),
             mode = upper(std::string(text.substr(c + 1, e - c - 1)));
        auto payload = std::string(text.substr(e + 1, end - e - 1));
        if (mode == "Q")
            std::replace(payload.begin(), payload.end(), '_', ' ');
        auto decoded = mime_decoded(payload, mode == "B" ? "BASE64" : "QUOTED-PRINTABLE");
        if (charset == "ISO-8859-1") {
            for (unsigned char ch : decoded) {
                if (ch < 128)
                    result += static_cast<char>(ch);
                else {
                    result += static_cast<char>(0xc0 | (ch >> 6));
                    result += static_cast<char>(0x80 | (ch & 63));
                }
            }
        } else
            result += decoded;
        pos = end + 2;
        previous = true;
    }
    return result;
}
std::string mailbox_encode(std::string_view utf8) {
    // JSON validates UTF-8 and supplies Unicode scalar values without locale-dependent conversion.
    (void)Json(std::string(utf8)).dump();
    std::string out, run;
    auto flush = [&] {
        if (run.empty())
            return;
        auto x = b64(run);
        while (x.ends_with('='))
            x.pop_back();
        std::replace(x.begin(), x.end(), '/', ',');
        out += '&' + x + '-';
        run.clear();
    };
    for (std::size_t i = 0; i < utf8.size();) {
        auto ch = static_cast<unsigned char>(utf8[i++]);
        std::uint32_t cp = ch;
        if (ch >= 128) {
            unsigned n = (ch & 0xe0) == 0xc0 ? 1 : (ch & 0xf0) == 0xe0 ? 2 : 3;
            cp = ch & ((1U << (6 - n)) - 1);
            while (n--)
                cp = (cp << 6) | (static_cast<unsigned char>(utf8[i++]) & 63);
        }
        if (cp >= 32 && cp <= 126) {
            flush();
            out += cp == '&' ? "&-" : std::string(1, static_cast<char>(cp));
        } else {
            auto put = [&](std::uint32_t v) {
                run += static_cast<char>(v >> 8);
                run += static_cast<char>(v & 255);
            };
            if (cp > 65535) {
                cp -= 65536;
                put(0xd800 + (cp >> 10));
                put(0xdc00 + (cp & 1023));
            } else
                put(cp);
        }
    }
    flush();
    return out;
}
std::string mailbox_decode(std::string_view input) {
    std::string result;
    for (std::size_t i = 0; i < input.size();) {
        unsigned char c = static_cast<unsigned char>(input[i++]);
        if (c != '&') {
            if (c < 32 || c > 126)
                throw std::runtime_error("Invalid mailbox encoding");
            result += static_cast<char>(c);
            continue;
        }
        auto end = input.find('-', i);
        if (end == input.npos)
            throw std::runtime_error("Invalid mailbox encoding");
        if (end == i) {
            result += '&';
            ++i;
            continue;
        }
        auto encoded = std::string(input.substr(i, end - i));
        std::replace(encoded.begin(), encoded.end(), ',', '/');
        while (encoded.size() % 4)
            encoded += '=';
        auto raw = mime_decoded(encoded, "BASE64");
        if (raw == encoded || raw.size() % 2)
            throw std::runtime_error("Invalid UTF-7");
        for (std::size_t j = 0; j < raw.size(); j += 2) {
            std::uint32_t cp =
                (static_cast<unsigned char>(raw[j]) << 8) | static_cast<unsigned char>(raw[j + 1]);
            if (cp >= 0xd800 && cp <= 0xdbff) {
                if (j + 3 >= raw.size())
                    throw std::runtime_error("Invalid surrogate");
                auto low =
                    (static_cast<unsigned char>(raw[j + 2]) << 8) | static_cast<unsigned char>(raw[j + 3]);
                if (low < 0xdc00 || low > 0xdfff)
                    throw std::runtime_error("Invalid surrogate");
                cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                j += 2;
            } else if (cp >= 0xdc00 && cp <= 0xdfff)
                throw std::runtime_error("Invalid surrogate");
            if (cp < 128)
                result += static_cast<char>(cp);
            else if (cp < 2048) {
                result += static_cast<char>(0xc0 | (cp >> 6));
                result += static_cast<char>(0x80 | (cp & 63));
            } else if (cp < 65536) {
                result += static_cast<char>(0xe0 | (cp >> 12));
                result += static_cast<char>(0x80 | ((cp >> 6) & 63));
                result += static_cast<char>(0x80 | (cp & 63));
            } else {
                result += static_cast<char>(0xf0 | (cp >> 18));
                result += static_cast<char>(0x80 | ((cp >> 12) & 63));
                result += static_cast<char>(0x80 | ((cp >> 6) & 63));
                result += static_cast<char>(0x80 | (cp & 63));
            }
        }
        i = end + 1;
    }
    if (mailbox_encode(result) != input)
        throw std::runtime_error("Noncanonical UTF-7");
    return result;
}
std::string imap_date(std::int64_t timestamp) {
    using namespace std::chrono;
    auto instant = sys_seconds{seconds{timestamp}};
    auto day = floor<days>(instant);
    year_month_day date{day};
    hh_mm_ss time{instant - day};
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (!date.ok() || int(date.year()) < 1 || int(date.year()) > 9999)
        throw std::runtime_error("Invalid internal date");
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setfill('0') << std::setw(2) << unsigned(date.day()) << '-'
        << months[unsigned(date.month()) - 1] << '-' << std::setw(4) << int(date.year()) << ' '
        << std::setw(2) << time.hours().count() << ':' << std::setw(2) << time.minutes().count() << ':'
        << std::setw(2) << time.seconds().count() << " +0000";
    return out.str();
}
std::int64_t parse_mail_date(std::string text, bool imap) {
    if (text.size() > 1024)
        throw std::runtime_error("Invalid date");
    if (!imap) {
        std::string stripped;
        unsigned comments = 0;
        bool escape = false;
        for (char c : text) {
            if (escape) {
                escape = false;
                continue;
            }
            if (c == '\\' && comments) {
                escape = true;
                continue;
            }
            if (c == '(') {
                ++comments;
                continue;
            }
            if (c == ')' && comments) {
                --comments;
                continue;
            }
            if (!comments)
                stripped += c;
        }
        if (comments)
            throw std::runtime_error("Invalid date comment");
        text = std::move(stripped);
        auto comma = text.find(',');
        if (comma != text.npos)
            text.erase(0, comma + 1);
    }
    static const std::regex date_re(
        R"(^\s*([0-9]{1,2})[- ]([A-Za-z]{3})[- ]([0-9]{2,4})\s+([0-9]{2}):([0-9]{2})(?::([0-9]{2}))?\s+([+-][0-9]{4}|[A-Za-z]+)\s*$)");
    std::smatch match;
    if (!std::regex_match(text, match, date_re))
        throw std::runtime_error("Invalid date");
    unsigned day = static_cast<unsigned>(std::stoul(match[1])),
             year = static_cast<unsigned>(std::stoul(match[3]));
    auto month = upper(match[2]);
    std::string month_names = "JANFEBMARAPRMAYJUNJULAUGSEPOCTNOVDEC";
    auto index = month_names.find(month);
    if (index == std::string::npos || index % 3)
        throw std::runtime_error("Invalid month");
    if (imap && (match[3].length() != 4 || !match[6].matched))
        throw std::runtime_error("Invalid IMAP date");
    if (!imap && year < 100)
        year += year < 50 ? 2000 : 1900;
    unsigned hour = static_cast<unsigned>(std::stoul(match[4])),
             minute = static_cast<unsigned>(std::stoul(match[5])),
             second = match[6].matched ? static_cast<unsigned>(std::stoul(match[6])) : 0;
    if (!year || hour > 23 || minute > 59 || second > 60)
        throw std::runtime_error("Invalid time");
    auto zone = upper(match[7]);
    int offset = 0;
    if (zone.size() == 5 && (zone[0] == '+' || zone[0] == '-')) {
        auto n = std::stoul(zone.substr(1));
        if (n / 100 > 23 || n % 100 > 59)
            throw std::runtime_error("Invalid zone");
        offset = static_cast<int>((n / 100) * 3600 + (n % 100) * 60) * (zone[0] == '-' ? -1 : 1);
    } else {
        static const std::map<std::string, int> zones = {{"UT", 0},   {"GMT", 0},  {"EST", -5}, {"EDT", -4},
                                                         {"CST", -6}, {"CDT", -5}, {"MST", -7}, {"MDT", -6},
                                                         {"PST", -8}, {"PDT", -7}};
        if (imap)
            throw std::runtime_error("Invalid IMAP zone");
        if (auto found = zones.find(zone); found != zones.end())
            offset = found->second * 3600;
        else if (zone.size() != 1 || zone == "J")
            throw std::runtime_error("Invalid zone");
    }
    std::chrono::year_month_day date{std::chrono::year{static_cast<int>(year)},
                                     std::chrono::month{static_cast<unsigned>(index / 3 + 1)},
                                     std::chrono::day{day}};
    if (!date.ok())
        throw std::runtime_error("Invalid calendar date");
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::sys_days{date}.time_since_epoch())
               .count() +
           hour * 3600 + minute * 60 + second - offset;
}
} // namespace cm
