#include "cloudiemail/antispam.hpp"
#include "cloudiemail/mail_message.hpp"
#include "cloudiemail/server.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <ctime>
#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <regex>
#include <sodium.h>
#include <sstream>
#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif
namespace cm {
namespace {
std::string lower(std::string s) {
    for (auto &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string trim(std::string_view s) {
    auto b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    return b == s.npos ? "" : std::string(s.substr(b, e - b + 1));
}
std::string compact(std::string_view s) {
    std::string out;
    bool space = false;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            space = true;
        else {
            if (space && !out.empty())
                out += ' ';
            out += c;
            space = false;
        }
    }
    return out;
}
std::string no_space(std::string s) {
    std::erase_if(s, [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; });
    return s;
}
std::map<std::string, std::string> tags(std::string_view s) {
    std::map<std::string, std::string> out;
    std::size_t pos = 0;
    while (pos < s.size()) {
        auto end = s.find(';', pos);
        if (end == s.npos)
            end = s.size();
        auto term = s.substr(pos, end - pos);
        auto eq = term.find('=');
        if (eq != term.npos) {
            auto key = lower(trim(term.substr(0, eq)));
            if (out.contains(key))
                throw std::runtime_error("Duplicate tag");
            out[key] = trim(term.substr(eq + 1));
        }
        pos = end + 1;
    }
    return out;
}
std::uint64_t number(std::string_view s) {
    std::uint64_t n = 0;
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (ec != std::errc() || end != s.data() + s.size())
        throw std::runtime_error("Invalid number");
    return n;
}
std::string dns_text(std::string_view s) {
    std::string out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ')
            ++i;
        if (i == s.size())
            break;
        if (s[i++] != '"')
            throw std::runtime_error("Invalid TXT");
        bool end = false;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') {
                end = true;
                break;
            }
            if (c == '\\') {
                if (i == s.size())
                    throw std::runtime_error("Invalid TXT escape");
                if (i + 2 < s.size() && std::isdigit(static_cast<unsigned char>(s[i])) &&
                    std::isdigit(static_cast<unsigned char>(s[i + 1])) &&
                    std::isdigit(static_cast<unsigned char>(s[i + 2]))) {
                    auto v = number(s.substr(i, 3));
                    if (v > 255)
                        throw std::runtime_error("Invalid TXT byte");
                    c = static_cast<char>(v);
                    i += 3;
                } else
                    c = s[i++];
            }
            out += c;
        }
        if (!end)
            throw std::runtime_error("Unterminated TXT");
    }
    return out;
}
std::vector<std::string> dns_https(const std::string &name, unsigned type, long milliseconds) {
    if (name.empty() || name.size() > 253 ||
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._") !=
            std::string::npos)
        throw std::runtime_error("Invalid DNS name");
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl)
        throw std::runtime_error("DNS unavailable");
    std::string url = "https://cloudflare-dns.com/dns-query?name=" + name + "&type=" + std::to_string(type),
                response;
    auto collect = +[](char *ptr, size_t unit, size_t count, void *opaque) -> size_t {
        auto &body = *static_cast<std::string *>(opaque);
        auto n = unit * count;
        if (n > 65536 || body.size() + n > 65536)
            return 0;
        body.append(ptr, n);
        return n;
    };
    auto *raw = curl_slist_append(nullptr, "Accept: application/dns-json");
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(raw, curl_slist_free_all);
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, milliseconds);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, std::min(1500L, milliseconds));
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    auto rc = curl_easy_perform(curl.get());
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    if (rc != CURLE_OK || status != 200)
        throw std::runtime_error("DNS request failed");
    auto doc = Json::parse(response);
    auto code = doc.at("Status").get<int>();
    if (code == 3)
        return {};
    if (code != 0 || doc.value("TC", false))
        throw std::runtime_error("DNS response failed");
    std::vector<std::string> result;
    for (auto &answer : doc.value("Answer", Json::array()))
        if (answer.at("type").get<unsigned>() == type) {
            auto data = answer.at("data").get<std::string>();
            result.push_back(type == 16 ? dns_text(data) : data);
            if (result.size() > 64)
                throw std::runtime_error("DNS record limit");
        }
    return result;
}
struct Lookup {
    SpamDns custom;
    unsigned queries = 0;
    std::map<std::pair<std::string, unsigned>, std::vector<std::string>> cache;
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(6);
    std::vector<std::string> operator()(const std::string &name, unsigned type) {
        auto key = std::make_pair(lower(name), type);
        if (cache.contains(key))
            return cache.at(key);
        auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())
                .count();
        if (++queries > 32 || remaining <= 0)
            throw std::runtime_error("DNS budget exceeded");
        auto records =
            custom ? custom(name, type)
                   : dns_https(name, type, static_cast<long>(std::min<std::int64_t>(remaining, 2000)));
        cache.emplace(key, records);
        return records;
    }
};
bool subnet(const std::string &ip, const std::string &network, unsigned bits) {
    auto family = network.find(':') == std::string::npos ? AF_INET : AF_INET6;
    unsigned maximum = family == AF_INET ? 32U : 128U;
    if (bits > maximum)
        return false;
    std::array<unsigned char, 16> a{}, b{};
    if (inet_pton(family, ip.c_str(), a.data()) != 1 || inet_pton(family, network.c_str(), b.data()) != 1)
        return false;
    for (unsigned i = 0; i < bits; ++i)
        if ((a[i / 8] & (128U >> (i % 8))) != (b[i / 8] & (128U >> (i % 8))))
            return false;
    return true;
}
std::string spf_check(Lookup &dns, const std::string &domain, const std::string &ip, unsigned &terms,
                      unsigned depth = 0) {
    if (depth > 10)
        return "permerror";
    auto records = dns(domain, 16);
    std::string policy;
    for (auto &r : records)
        if (lower(r) == "v=spf1" || lower(r).starts_with("v=spf1 ")) {
            if (!policy.empty())
                return "permerror";
            policy = r;
        }
    if (policy.empty())
        return "none";
    auto fields = words(policy);
    std::string redirect;
    for (std::size_t i = 1; i < fields.size(); ++i) {
        auto mechanism = lower(fields[i]);
        if (mechanism.starts_with("redirect=")) {
            if (!redirect.empty())
                return "permerror";
            redirect = mechanism.substr(9);
            continue;
        }
        if (mechanism.find('=') != mechanism.npos)
            continue;
        char qualifier = '+';
        if (!mechanism.empty() && std::string_view("+-~?").find(mechanism.front()) != std::string::npos) {
            qualifier = mechanism.front();
            mechanism.erase(0, 1);
        }
        bool match = false;
        auto colon = mechanism.find(':'), slash = mechanism.find('/');
        auto end = std::min(colon, slash);
        auto kind = mechanism.substr(0, end);
        auto target = colon == mechanism.npos
                          ? domain
                          : mechanism.substr(colon + 1, (slash == mechanism.npos ? mechanism.size() : slash) -
                                                            colon - 1);
        // Unimplemented macros and PTR cannot produce an authentication pass or fail.
        if (target.find('%') != target.npos || kind == "ptr")
            return "neutral";
        if (kind == "all")
            match = true;
        else if (kind == "ip4" || kind == "ip6") {
            if (colon == mechanism.npos)
                return "permerror";
            std::array<unsigned char, 16> parsed_address{};
            if (inet_pton(kind == "ip4" ? AF_INET : AF_INET6, target.c_str(), parsed_address.data()) != 1)
                return "permerror";
            unsigned bits = kind == "ip4" ? 32U : 128U;
            if (slash != mechanism.npos) {
                auto value = number(mechanism.substr(slash + 1));
                if (value > 128)
                    return "permerror";
                bits = static_cast<unsigned>(value);
            }
            if (bits > (kind == "ip4" ? 32U : 128U))
                return "permerror";
            match = subnet(ip, target, bits);
        } else {
            if (++terms > 10)
                return "permerror";
            if (kind == "include") {
                if (colon == mechanism.npos)
                    return "permerror";
                auto result = spf_check(dns, target, ip, terms, depth + 1);
                if (result == "temperror" || result == "permerror")
                    return result;
                if (result == "none")
                    return "permerror";
                match = result == "pass";
            } else if (kind == "exists") {
                if (colon == mechanism.npos)
                    return "permerror";
                match = !dns(target, 1).empty();
            } else if (kind == "a" || kind == "mx") {
                bool ipv6 = ip.find(':') != ip.npos;
                unsigned prefix = ipv6 ? 128U : 32U;
                if (slash != mechanism.npos) {
                    auto cidr = mechanism.substr(slash + 1);
                    auto dual = cidr.find('/');
                    if (!ipv6 && !cidr.empty() && cidr[0] != '/')
                        prefix = static_cast<unsigned>(number(cidr.substr(0, dual)));
                    if (ipv6 && dual != cidr.npos)
                        prefix = static_cast<unsigned>(number(cidr.substr(dual + 1)));
                }
                if (prefix > (ipv6 ? 128U : 32U))
                    return "permerror";
                std::vector<std::string> hosts{target};
                if (kind == "mx") {
                    hosts.clear();
                    auto mx = dns(target, 15);
                    if (mx.size() > 10)
                        return "permerror";
                    for (auto &r : mx) {
                        auto space = r.find(' ');
                        if (space == r.npos)
                            continue;
                        auto host = r.substr(space + 1);
                        if (host.ends_with('.'))
                            host.pop_back();
                        if (!host.empty())
                            hosts.push_back(host);
                    }
                }
                for (auto &host : hosts) {
                    auto addresses = dns(host, ipv6 ? 28U : 1U);
                    if (addresses.size() > 10)
                        return "permerror";
                    for (auto &address : addresses)
                        if (subnet(ip, address, prefix))
                            match = true;
                }
            } else
                return "permerror";
        }
        if (match)
            return qualifier == '+'   ? "pass"
                   : qualifier == '-' ? "fail"
                   : qualifier == '~' ? "softfail"
                                      : "neutral";
    }
    if (!redirect.empty()) {
        if (++terms > 10)
            return "permerror";
        if (redirect.find('%') != redirect.npos)
            return "neutral";
        auto result = spf_check(dns, redirect, ip, terms, depth + 1);
        return result == "none" ? "permerror" : result;
    }
    return "neutral";
}
std::string body_canonical(std::string_view body, bool relaxed) {
    std::string out;
    std::size_t pos = 0;
    while (pos < body.size()) {
        auto end = body.find("\r\n", pos);
        if (end == body.npos)
            end = body.size();
        auto line = body.substr(pos, end - pos);
        if (relaxed) {
            bool space = false;
            for (char ch : line) {
                if (ch == ' ' || ch == '\t')
                    space = true;
                else {
                    if (space)
                        out += ' ';
                    out += ch;
                    space = false;
                }
            }
        } else
            out += line;
        out += "\r\n";
        pos = end + 2;
    }
    while (out.ends_with("\r\n"))
        out.resize(out.size() - 2);
    if (!out.empty() || !relaxed)
        out += "\r\n";
    return out;
}
std::string header_canonical(std::string raw, bool relaxed) {
    if (!relaxed)
        return raw;
    auto colon = raw.find(':');
    if (colon == raw.npos)
        throw std::runtime_error("Invalid header");
    return lower(raw.substr(0, colon)) + ":" + compact(std::string_view(raw).substr(colon + 1)) + "\r\n";
}
std::string without_signature(std::string raw) {
    auto colon = raw.find(':');
    std::size_t pos = colon + 1;
    while (pos < raw.size()) {
        auto end = raw.find(';', pos);
        if (end == raw.npos)
            end = raw.size();
        auto field = std::string_view(raw).substr(pos, end - pos);
        auto eq = field.find('=');
        if (eq != field.npos && trim(field.substr(0, eq)) == "b") {
            auto tail = end;
            while (tail > pos + eq + 1 && (raw[tail - 1] == '\r' || raw[tail - 1] == '\n'))
                --tail;
            raw.erase(pos + eq + 1, tail - pos - eq - 1);
            return raw;
        }
        pos = end + 1;
    }
    throw std::runtime_error("Missing signature");
}
std::string decode64(const std::string &s) {
    auto input = no_space(s);
    std::string out(input.size(), '\0');
    std::size_t size = 0;
    if (sodium_base642bin(reinterpret_cast<unsigned char *>(out.data()), out.size(), input.data(),
                          input.size(), nullptr, &size, nullptr, sodium_base64_VARIANT_ORIGINAL) != 0)
        throw std::runtime_error("Invalid base64");
    out.resize(size);
    return out;
}
std::vector<std::string> dkim_check(Lookup &dns, const MailPart &mail, std::string &result) {
    std::vector<std::string> passed;
    unsigned attempts = 0;
    for (auto &signature : mail.headers)
        if (signature.name == "DKIM-SIGNATURE") {
            if (++attempts > 4)
                break;
            result = "fail";
            try {
                auto t = tags(signature.value);
                if (t["v"] != "1" || (t["a"] != "rsa-sha256" && t["a"] != "ed25519-sha256") ||
                    t["d"].empty() || t["s"].empty() || t["h"].empty())
                    continue;
                auto domain = domain_name(t["d"]);
                auto selector = t["s"];
                if (selector.size() > 63 ||
                    selector.find_first_not_of(
                        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._") != selector.npos)
                    continue;
                if (t.contains("x") && number(t["x"]) < static_cast<std::uint64_t>(std::time(nullptr)))
                    continue;
                if (t.contains("i")) {
                    auto identity = mime_decoded(t["i"], "QUOTED-PRINTABLE");
                    auto at = identity.rfind('@');
                    if (at == identity.npos)
                        continue;
                    auto iddomain = lower(identity.substr(at + 1));
                    if (iddomain != domain && !iddomain.ends_with("." + domain))
                        continue;
                }
                auto canonical = t.contains("c") ? t["c"] : "simple/simple";
                auto slash = canonical.find('/');
                auto hc = canonical.substr(0, slash),
                     bc = slash == canonical.npos ? "simple" : canonical.substr(slash + 1);
                if ((hc != "simple" && hc != "relaxed") || (bc != "simple" && bc != "relaxed"))
                    continue;
                auto body = body_canonical(mail.body, bc == "relaxed");
                if (t.contains("l") && number(t["l"]) != body.size())
                    continue; // Partial body signatures cannot authenticate an appended payload.
                std::array<unsigned char, 32> hash{};
                unsigned len = 0;
                EVP_Digest(body.data(), body.size(), hash.data(), &len, EVP_sha256(), nullptr);
                wipe(body);
                auto bh = decode64(t["bh"]);
                if (bh.size() != hash.size() || sodium_memcmp(bh.data(), hash.data(), hash.size()))
                    continue;
                std::string data;
                std::set<std::size_t> used;
                bool signed_from = false;
                std::istringstream names(t["h"]);
                std::string name;
                while (std::getline(names, name, ':')) {
                    name = upper(trim(name));
                    if (name == "FROM")
                        signed_from = true;
                    for (std::size_t j = mail.headers.size(); j > 0; --j) {
                        auto index = j - 1;
                        if (&mail.headers[index] != &signature && !used.contains(index) &&
                            mail.headers[index].name == name) {
                            used.insert(index);
                            data += header_canonical(std::string(mail.headers[index].raw), hc == "relaxed");
                            break;
                        }
                    }
                }
                if (!signed_from)
                    continue;
                auto empty = header_canonical(without_signature(std::string(signature.raw)), hc == "relaxed");
                if (empty.ends_with("\r\n"))
                    empty.resize(empty.size() - 2);
                data += empty;
                auto keys = dns(selector + "._domainkey." + domain, 16);
                std::map<std::string, std::string> key;
                unsigned key_count = 0;
                for (auto &record : keys) {
                    auto candidate = tags(record);
                    if (!candidate.contains("v") || candidate["v"] == "DKIM1") {
                        key = candidate;
                        ++key_count;
                    }
                }
                if (key_count != 1 || key["p"].empty())
                    continue;
                if (key.contains("h") && (":" + key["h"] + ":").find(":sha256:") == std::string::npos)
                    continue;
                if (key.contains("s") && key["s"] != "*" &&
                    (":" + key["s"] + ":").find(":email:") == std::string::npos)
                    continue;
                auto der = decode64(key["p"]), sig = decode64(t["b"]);
                EVP_PKEY *raw = nullptr;
                if (t["a"] == "rsa-sha256" && (!key.contains("k") || key["k"] == "rsa")) {
                    const auto *ptr = reinterpret_cast<const unsigned char *>(der.data());
                    raw = d2i_PUBKEY(nullptr, &ptr, static_cast<long>(der.size()));
                    if (raw && (EVP_PKEY_base_id(raw) != EVP_PKEY_RSA || EVP_PKEY_bits(raw) < 1024 ||
                                EVP_PKEY_bits(raw) > 8192)) {
                        EVP_PKEY_free(raw);
                        raw = nullptr;
                    }
                }
                if (t["a"] == "ed25519-sha256" && key["k"] == "ed25519" && der.size() == 32)
                    raw = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                                      reinterpret_cast<const unsigned char *>(der.data()),
                                                      der.size());
                std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> public_key(raw, EVP_PKEY_free);
                if (!public_key)
                    continue;
                std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(),
                                                                            EVP_MD_CTX_free);
                if (!ctx)
                    continue;
                bool valid = false;
                if (t["a"] == "rsa-sha256")
                    valid =
                        EVP_DigestVerifyInit(ctx.get(), nullptr, EVP_sha256(), nullptr, public_key.get()) ==
                            1 &&
                        EVP_DigestVerifyUpdate(ctx.get(), data.data(), data.size()) == 1 &&
                        EVP_DigestVerifyFinal(ctx.get(), reinterpret_cast<const unsigned char *>(sig.data()),
                                              sig.size()) == 1;
                else {
                    EVP_Digest(data.data(), data.size(), hash.data(), &len, EVP_sha256(), nullptr);
                    valid =
                        EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, public_key.get()) == 1 &&
                        EVP_DigestVerify(ctx.get(), reinterpret_cast<const unsigned char *>(sig.data()),
                                         sig.size(), hash.data(), hash.size()) == 1;
                }
                wipe(data);
                if (valid) {
                    passed.push_back(domain);
                    result = "pass";
                }
            } catch (...) {
                if (passed.empty())
                    result = "temperror";
            }
        }
    if (!passed.empty())
        result = "pass";
    return passed;
}
std::string from_domain(const MailPart &p) {
    unsigned count = 0;
    std::string value;
    for (auto &h : p.headers)
        if (h.name == "FROM") {
            ++count;
            value = h.value;
        }
    if (count != 1)
        return {};
    static const std::regex address(R"((?:^|[<\s])[^<>\s@,]+@([A-Za-z0-9.-]+)(?:>|\s|$))");
    std::smatch match;
    if (!std::regex_search(value, match, address))
        return {};
    try {
        return domain_name(match[1]);
    } catch (...) {
        return {};
    }
}
} // namespace
std::string SpamResult::headers() const {
    std::string rule_list;
    for (auto &rule : rules) {
        if (!rule_list.empty())
            rule_list += ',';
        rule_list += rule;
    }
    return "X-CloudieMail-Spam: " + std::string(junk ? "Yes" : "No") +
           "\r\nX-CloudieMail-Spam-Score: " + std::to_string(score) +
           "\r\nX-CloudieMail-Spam-Rules: " + rule_list + "\r\nX-CloudieMail-Authentication: spf=" + spf +
           "; dkim=" + dkim + "; dmarc=" + dmarc + "\r\n";
}
std::string remove_spam_headers(std::string_view message) {
    auto end = message.find("\r\n\r\n");
    if (end == message.npos || end > 65536)
        return "\r\n" + std::string(message);
    std::string out;
    std::size_t at = 0;
    bool omit = false;
    while (at < end) {
        auto next = message.find("\r\n", at);
        auto line = message.substr(at, next - at);
        if (!line.empty() && line.front() != ' ' && line.front() != '\t') {
            auto colon = line.find(':');
            auto name = upper(std::string(line.substr(0, colon)));
            omit = name.starts_with("X-CLOUDIEMAIL-SPAM") || name == "X-CLOUDIEMAIL-AUTHENTICATION" ||
                   name == "RETURN-PATH";
        }
        if (!omit)
            out += std::string(line) + "\r\n";
        at = next + 2;
    }
    return out + "\r\n" + std::string(message.substr(end + 4));
}
SpamResult classify_spam(const Config &config, std::string_view message, const std::string &sender,
                         const std::string &peer, const std::string &helo, SpamDns resolver) {
    SpamResult result;
    if (!config.antispam_enabled)
        return result;
    auto add = [&](int score, const std::string &rule) {
        if (std::find(result.rules.begin(), result.rules.end(), rule) == result.rules.end()) {
            result.score += score;
            result.rules.push_back(rule);
        }
    };
    MailPart mail;
    try {
        mail = parse_message(message);
    } catch (...) {
        add(6, "MALFORMED_MIME");
        result.junk = result.score >= config.antispam_threshold;
        return result;
    }
    if (std::count_if(mail.headers.begin(), mail.headers.end(),
                      [](const auto &h) { return h.name == "FROM"; }) > 1)
        add(6, "DUPLICATE_FROM");
    auto from = from_domain(mail);
    if (from.empty())
        add(3, "INVALID_FROM");
    if (mail.get("DATE").empty())
        add(1, "MISSING_DATE");
    if (mail.get("MESSAGE-ID").empty())
        add(1, "MISSING_MESSAGE_ID");
    auto envelope = sender.empty() ? helo : sender.substr(sender.find('@') + 1);
    for (auto &entry : config.antispam_blocklist)
        if (lower(entry) == lower(sender) || lower(entry) == lower(envelope))
            add(100, "BLOCKLIST");
    std::function<void(const MailPart &)> inspect = [&](const MailPart &p) {
        if (p.children.empty()) {
            auto body = mime_decoded(p.body, p.encoding);
            auto text = lower(body);
            if (body.find("XJS*C4JDBQADN1.NSBN3*2IDNEN*GTUBE-STANDARD-ANTI-UBE-TEST-EMAIL*C.34X") !=
                body.npos)
                add(100, "GTUBE");
            if (p.type == "TEXT" && p.subtype == "HTML") {
                if (text.find("<script") != text.npos || text.find("javascript:") != text.npos ||
                    text.find("<iframe") != text.npos)
                    add(5, "ACTIVE_HTML");
                if (text.find("<form") != text.npos && text.find("password") != text.npos)
                    add(6, "PASSWORD_FORM");
            }
            wipe(text);
            wipe(body);
        }
        auto metadata = lower(mime_words(p.get("CONTENT-DISPOSITION") + ";" + p.get("CONTENT-TYPE")));
        static const std::regex executable(
            R"(\.(exe|scr|com|bat|cmd|vbs|vbe|js|jse|ps1|msi|hta|lnk|iso|img)(["';\s]|$))");
        if (std::regex_search(metadata, executable))
            add(6, "EXECUTABLE_ATTACHMENT");
        for (auto &ch : p.children)
            inspect(ch);
    };
    inspect(mail);
    if (result.score >= config.antispam_threshold) {
        result.junk = true;
        return result;
    }
    if (config.antispam_dns && !peer.empty()) {
        Lookup dns;
        dns.custom = std::move(resolver);
        try {
            unsigned terms = 0;
            result.spf = spf_check(dns, domain_name(envelope), peer, terms);
        } catch (...) {
            result.spf = "temperror";
        }
        auto signed_domains = dkim_check(dns, mail, result.dkim);
        bool dkim_aligned =
            std::find(signed_domains.begin(), signed_domains.end(), from) != signed_domains.end();
        if (result.spf == "fail" && !dkim_aligned)
            add(2, "SPF_FAIL");
        if (result.dkim == "fail")
            add(1, "DKIM_FAIL");
        if (!from.empty())
            try {
                auto policies = dns("_dmarc." + from, 16);
                std::map<std::string, std::string> policy;
                unsigned count = 0;
                for (auto &r : policies)
                    if (r.starts_with("v=DMARC1;")) {
                        policy = tags(r);
                        ++count;
                    }
                if (count == 1 &&
                    (policy["p"] == "none" || policy["p"] == "quarantine" || policy["p"] == "reject")) {
                    if ((result.spf == "pass" && lower(envelope) == from) || dkim_aligned)
                        result.dmarc = "pass";
                    // Exact alignment is proven here. Relaxed/subdomain cases are left unevaluated without a
                    // public suffix database.
                    else if (policy["aspf"] == "s" && policy["adkim"] == "s" && result.spf != "temperror" &&
                             result.dkim != "temperror") {
                        result.dmarc = "fail";
                        if (policy["p"] != "none")
                            add(5, "DMARC_FAIL");
                    } else
                        result.dmarc = "neutral";
                }
            } catch (...) {
                result.dmarc = "temperror";
            }
        if (from == config.domain && result.spf != "pass" && !dkim_aligned)
            add(3, "LOCAL_DOMAIN_SPOOF");
    }
    result.junk = result.score >= config.antispam_threshold;
    return result;
}
} // namespace cm
