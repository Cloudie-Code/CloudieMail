#include "cloudiemail/dns.hpp"
#include <cstdlib>
#include <curl/curl.h>
#include <memory>
#include <regex>
#include <sstream>

namespace cm {
Json dns_plan(const std::string &value, const std::string &ip) {
    auto domain = domain_name(value);
    std::istringstream stream(ip);
    std::string part;
    int count = 0;
    while (std::getline(stream, part, '.')) {
        if (part.empty() || part.size() > 3 || part.find_first_not_of("0123456789") != part.npos ||
            std::stoi(part) > 255 || (part.size() > 1 && part[0] == '0'))
            throw std::runtime_error("IPv4 invalido");
        ++count;
    }
    if (count != 4 || ip.back() == '.')
        throw std::runtime_error("IPv4 invalido");
    return Json::array(
        {{{"type", "A"}, {"name", "mail." + domain}, {"content", ip}, {"ttl", 300}, {"proxied", false}},
         {{"type", "A"}, {"name", "webmail." + domain}, {"content", ip}, {"ttl", 300}, {"proxied", false}},
         {{"type", "MX"}, {"name", domain}, {"content", "mail." + domain}, {"priority", 10}, {"ttl", 300}},
         {{"type", "TXT"}, {"name", domain}, {"content", "v=spf1 mx -all"}, {"ttl", 300}},
         {{"type", "TXT"},
          {"name", "_dmarc." + domain},
          {"content", "v=DMARC1; p=none; adkim=s; aspf=s"},
          {"ttl", 300}}});
}
std::string dns_txt_value(const std::string &value) {
    auto at = value.find_first_not_of(" \t\r\n");
    if (at == value.npos || value[at] != '"')
        return value;
    std::string result;
    while (at < value.size()) {
        if (value[at++] != '"')
            return value;
        bool closed = false;
        while (at < value.size()) {
            char c = value[at++];
            if (c == '"') {
                closed = true;
                break;
            }
            if (c == '\\') {
                if (at == value.size())
                    return value;
                if (at + 2 < value.size() && value[at] >= '0' && value[at] <= '9' && value[at + 1] >= '0' &&
                    value[at + 1] <= '9' && value[at + 2] >= '0' && value[at + 2] <= '9') {
                    unsigned byte = static_cast<unsigned>((value[at] - '0') * 100 +
                                                          (value[at + 1] - '0') * 10 + value[at + 2] - '0');
                    if (byte > 255)
                        return value;
                    c = static_cast<char>(byte);
                    at += 3;
                } else
                    c = value[at++];
            }
            result += c;
        }
        if (!closed)
            return value;
        at = value.find_first_not_of(" \t\r\n", at);
        if (at == value.npos)
            break;
    }
    return result;
}
std::string dns_zone_text(const Json &plan) {
    std::ostringstream out;
    for (const auto &r : plan) {
        out << r.at("name").get<std::string>() << ". 300 IN " << r.at("type").get<std::string>() << ' ';
        if (r.at("type") == "MX")
            out << r.at("priority") << ' ';
        if (r.at("type") == "TXT") {
            auto content = r.at("content").get<std::string>();
            if (content.empty())
                out << "\"\"";
            for (std::size_t at = 0; at < content.size();) {
                auto end = std::min(content.size(), at + 255);
                while (end < content.size() && (static_cast<unsigned char>(content[end]) & 0xc0) == 0x80)
                    --end;
                if (at)
                    out << ' ';
                out << Json(content.substr(at, end - at)).dump();
                at = end;
            }
        } else
            out << r.at("content").get<std::string>() << (r.at("type") == "MX" ? "." : "");
        out << '\n';
    }
    return out.str();
}
namespace {
size_t collect(char *data, size_t unit, size_t count, void *ptr) {
    auto *target = static_cast<std::string *>(ptr);
    auto size = unit * count;
    if (size > 2 * 1024 * 1024 || target->size() + size > 2 * 1024 * 1024)
        return 0;
    target->append(data, size);
    return size;
}
Json request(const std::string &url, const std::string &token, const Json *body = nullptr) {
    using Handle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
    Handle curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl)
        throw std::runtime_error("Falha ao iniciar HTTPS");
    curl_slist *raw = nullptr;
    raw = curl_slist_append(raw, ("Authorization: Bearer " + token).c_str());
    raw = curl_slist_append(raw, "Content-Type: application/json");
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(raw, curl_slist_free_all);
    std::string response, payload = body ? body->dump() : "";
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
    // Portable Linux packages carry a public CA bundle, independent of host paths.
    if (const auto *ca_bundle = std::getenv("CLOUDIEMAIL_CA_BUNDLE"); ca_bundle && *ca_bundle)
        curl_easy_setopt(curl.get(), CURLOPT_CAINFO, ca_bundle);
    if (body) {
        curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, payload.c_str());
    }
    auto rc = curl_easy_perform(curl.get());
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    if (rc != CURLE_OK || status < 200 || status >= 300)
        throw std::runtime_error("Falha na API Cloudflare (HTTP " + std::to_string(status) + ")");
    auto j = Json::parse(response);
    if (!j.value("success", false))
        throw std::runtime_error("Cloudflare recusou a operacao");
    return j.at("result");
}
} // namespace
std::string cloudflare_zone(const std::string &domain, const std::string &token) {
    if (token.empty() || token.find_first_of("\r\n") != token.npos)
        throw std::runtime_error("Token Cloudflare invalido");
    auto result = request("https://api.cloudflare.com/client/v4/zones?name=" + domain_name(domain), token);
    if (!result.is_array() || result.size() != 1 || result[0].value("name", "") != domain)
        throw std::runtime_error(
            "Zona nao encontrada. Verifique o dominio e as permissoes Zone Read / DNS Edit do token.");
    return result[0].at("id").get<std::string>();
}
Json apply_cloudflare(const std::string &zone, const std::string &token, const Json &plan) {
    if (!std::regex_match(zone, std::regex("[a-fA-F0-9]{32}")) || token.empty() ||
        token.find_first_of("\r\n") != token.npos)
        throw std::runtime_error("Credenciais Cloudflare invalidas");
    auto base = "https://api.cloudflare.com/client/v4/zones/" + zone;
    auto info = request(base, token);
    auto domain = info.at("name").get<std::string>();
    // Validate the whole plan before the first remote mutation; never overwrite existing mail services.
    Json missing = Json::array(), results = Json::array();
    for (const auto &record : plan) {
        auto name = record.at("name").get<std::string>();
        if (name != domain && !name.ends_with("." + domain))
            throw std::runtime_error("Registro fora da zona");
        auto existing = request(base + "/dns_records?name=" + name + "&per_page=100", token);
        bool match = false;
        for (const auto &item : existing) {
            if (item.at("type") == "CNAME" || item.at("type") == record.at("type")) {
                bool same = item.at("type") == record.at("type") &&
                            (record.at("type") == "TXT"
                                 ? dns_txt_value(item.at("content")) == dns_txt_value(record.at("content"))
                                 : item.at("content") == record.at("content")) &&
                            item.value("priority", 0) == record.value("priority", 0) &&
                            item.value("proxied", false) == record.value("proxied", false);
                if (!same)
                    throw std::runtime_error("Registro existente em conflito: " + name +
                                             "; revise manualmente");
                match = true;
            }
        }
        if (!match)
            missing.push_back(record);
        else
            results.push_back({{"name", name}, {"status", "unchanged"}});
    }
    for (const auto &record : missing) {
        request(base + "/dns_records", token, &record);
        results.push_back({{"name", record.at("name")}, {"status", "created"}});
    }
    return results;
}
} // namespace cm
