#include "cloudiemail/outbound.hpp"
#include "cloudiemail/setup.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <curl/curl.h>
#include <fstream>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <set>
#include <sodium.h>
#include <thread>
#ifdef _WIN32
#include <windns.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <netdb.h>
#include <resolv.h>
#endif
namespace cm {
namespace {
std::string b64(std::span<const unsigned char> bytes) {
    std::string result(sodium_base64_ENCODED_LEN(bytes.size(), sodium_base64_VARIANT_ORIGINAL), '\0');
    sodium_bin2base64(result.data(), result.size(), bytes.data(), bytes.size(),
                      sodium_base64_VARIANT_ORIGINAL);
    result.resize(std::strlen(result.c_str()));
    return result;
}
Json read_json(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot read delivery keys");
    Json value;
    file >> value;
    return value;
}
using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
Key signing_key(const Config &config) {
    auto doc = read_json(config.database.parent_path() / "dkim.json");
    auto pem = doc.at("pem").get<std::string>();
    doc.clear();
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())),
                                                  BIO_free);
    Key key(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
    wipe(pem);
    if (!key || EVP_PKEY_get_security_bits(key.get()) < 112)
        throw std::runtime_error("Invalid DKIM key");
    return key;
}
std::string compact_space(std::string_view input) {
    std::string out;
    bool space = false;
    for (char c : input) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            space = true;
        else {
            if (space && !out.empty())
                out += ' ';
            space = false;
            out += c;
        }
    }
    return out;
}
std::string lowercase(std::string value) {
    for (auto &c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
std::string queue_aad(const QueuedMail &mail) {
    return mail.id + "\n" + mail.sender + "\n" + mail.recipient;
}
bool public_ip(const curl_sockaddr *address) {
    if (address->family == AF_INET) {
        auto n = ntohl(reinterpret_cast<const sockaddr_in *>(&address->addr)->sin_addr.s_addr);
        return (n >> 24) != 0 && (n >> 24) != 10 && (n >> 24) != 127 && (n >> 24) < 224 &&
               (n >> 16) != 0xa9fe && (n >> 20) != 0xac1 && (n >> 16) != 0xc0a8 && (n >> 22) != 0x191 &&
               (n >> 24) != 0xff && (n >> 24) != 0x7f && (n >> 8) != 0xc00000 && (n >> 8) != 0xc00002 &&
               (n >> 8) != 0xc05863 && (n >> 17) != 0x6309 && (n >> 8) != 0xc63364 && (n >> 8) != 0xcb0071;
    }
    if (address->family == AF_INET6) {
        auto *p = reinterpret_cast<const unsigned char *>(
            &reinterpret_cast<const sockaddr_in6 *>(&address->addr)->sin6_addr);
        return (p[0] & 0xe0) == 0x20 && !(p[0] == 0x20 && p[1] == 0x01 && p[2] == 0x0d && p[3] == 0xb8);
    }
    return false;
}
} // namespace
std::int64_t unix_time() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
void prepare_mail_keys(const Config &config) {
    auto path = config.database.parent_path() / "dkim.json";
    if (std::filesystem::exists(path)) {
        (void)signing_key(config);
        return;
    }
    private_directory(config.database.parent_path());
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
        EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
    EVP_PKEY *raw = nullptr;
    if (!context || EVP_PKEY_keygen_init(context.get()) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) <= 0 ||
        EVP_PKEY_keygen(context.get(), &raw) <= 0)
        throw std::runtime_error("Cannot generate DKIM key");
    Key key(raw, EVP_PKEY_free);
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), BIO_free);
    if (PEM_write_bio_PrivateKey(bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1)
        throw std::runtime_error("Cannot export DKIM key");
    char *memory = nullptr;
    auto length = BIO_get_mem_data(bio.get(), &memory);
    std::string pem(memory, static_cast<std::size_t>(length));
    save_json(path, {{"pem", pem}});
    wipe(pem);
    sodium_memzero(memory, static_cast<std::size_t>(length));
}
std::string dkim_record(const Config &config) {
    auto key = signing_key(config);
    int size = i2d_PUBKEY(key.get(), nullptr);
    if (size <= 0)
        throw std::runtime_error("Cannot encode DKIM public key");
    Bytes der(static_cast<std::size_t>(size));
    auto *at = der.data();
    i2d_PUBKEY(key.get(), &at);
    return "v=DKIM1; k=rsa; p=" + b64(der);
}
std::string dkim_sign(const Config &config, std::string_view message) {
    auto separator = message.find("\r\n\r\n");
    if (separator == message.npos || separator > 65536)
        throw std::runtime_error("Invalid message headers");
    std::vector<std::pair<std::string, std::string>> headers;
    std::size_t pos = 0;
    while (pos < separator) {
        auto end = message.find("\r\n", pos);
        auto line = message.substr(pos, end - pos);
        pos = end + 2;
        if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) {
            if (headers.empty())
                throw std::runtime_error("Invalid folded header");
            headers.back().second += " " + std::string(line);
            continue;
        }
        auto colon = line.find(':');
        if (colon == line.npos)
            throw std::runtime_error("Invalid message header");
        headers.emplace_back(lowercase(std::string(line.substr(0, colon))),
                             std::string(line.substr(colon + 1)));
    }
    std::string body;
    pos = separator + 4;
    while (pos < message.size()) {
        auto end = message.find("\r\n", pos);
        if (end == message.npos)
            end = message.size();
        auto line = message.substr(pos, end - pos);
        std::string normalized;
        bool space = false;
        for (char c : line) {
            if (c == ' ' || c == '\t')
                space = true;
            else {
                if (space)
                    normalized += ' ';
                space = false;
                normalized += c;
            }
        }
        body += normalized + "\r\n";
        pos = end + 2;
    }
    while (body.ends_with("\r\n"))
        body.resize(body.size() - 2);
    if (!body.empty())
        body += "\r\n";
    Bytes hash(32);
    unsigned length = 0;
    EVP_Digest(body.data(), body.size(), hash.data(), &length, EVP_sha256(), nullptr);
    wipe(body);
    std::string names, canonical;
    for (const auto &header : headers) {
        if (header.first == "from" || header.first == "to" || header.first == "subject" ||
            header.first == "date" || header.first == "message-id" || header.first == "mime-version" ||
            header.first == "content-type" || header.first == "content-transfer-encoding") {
            if ((":" + names + ":").find(":" + header.first + ":") != std::string::npos)
                throw std::runtime_error("Duplicate signed header");
            if (!names.empty())
                names += ':';
            names += header.first;
            canonical += header.first + ":" + compact_space(header.second) + "\r\n";
        }
    }
    if ((":" + names + ":").find(":from:") == std::string::npos)
        throw std::runtime_error("From header required");
    std::string value = "v=1; a=rsa-sha256; c=relaxed/relaxed; d=" + config.domain +
                        "; s=cloudiemail; t=" + std::to_string(unix_time()) + "; h=" + names +
                        "; bh=" + b64(hash) + "; b=";
    canonical += "dkim-signature:" + value;
    auto key = signing_key(config);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    std::size_t size = 0;
    if (!ctx || EVP_DigestSignInit(ctx.get(), nullptr, EVP_sha256(), nullptr, key.get()) != 1 ||
        EVP_DigestSignUpdate(ctx.get(), canonical.data(), canonical.size()) != 1 ||
        EVP_DigestSignFinal(ctx.get(), nullptr, &size) != 1)
        throw std::runtime_error("DKIM signing failed");
    Bytes signature(size);
    if (EVP_DigestSignFinal(ctx.get(), signature.data(), &size) != 1)
        throw std::runtime_error("DKIM signing failed");
    signature.resize(size);
    wipe(canonical);
    return "DKIM-Signature: " + value + b64(signature) + "\r\n" + std::string(message);
}
std::vector<std::string> resolve_mx(const std::string &domain_value) {
    auto domain = domain_name(domain_value);
    std::vector<std::pair<unsigned, std::string>> entries;
#ifdef _WIN32
    PDNS_RECORDA records = nullptr;
    auto status = DnsQuery_A(domain.c_str(), DNS_TYPE_MX, DNS_QUERY_STANDARD, nullptr,
                             reinterpret_cast<PDNS_RECORD *>(&records), nullptr);
    if (status != ERROR_SUCCESS && status != DNS_INFO_NO_RECORDS)
        throw std::runtime_error("MX lookup failed (" + std::to_string(status) + ")");
    struct Free {
        PDNS_RECORDA value;
        ~Free() {
            if (value)
                DnsRecordListFree(value, DnsFreeRecordList);
        }
    } cleanup{records};
    for (auto *record = records; record; record = record->pNext)
        if (record->wType == DNS_TYPE_MX)
            entries.emplace_back(record->Data.MX.wPreference, record->Data.MX.pNameExchange);
#else
    struct __res_state state{};
    if (res_ninit(&state) != 0)
        throw std::runtime_error("DNS resolver unavailable");
    struct Close {
        struct __res_state *state;
        ~Close() {
            res_nclose(state);
        }
    } close{&state};
    state.retrans = 2;
    state.retry = 1;
    unsigned char buffer[65536];
    auto size = res_nquery(&state, domain.c_str(), ns_c_in, ns_t_mx, buffer, sizeof(buffer));
    if (size < 0) {
        if (state.res_h_errno != NO_DATA)
            throw std::runtime_error("MX lookup failed");
    } else {
        ns_msg msg{};
        if (ns_initparse(buffer, size, &msg) < 0)
            throw std::runtime_error("Malformed MX answer");
        for (int i = 0; i < ns_msg_count(msg, ns_s_an); ++i) {
            ns_rr rr{};
            if (ns_parserr(&msg, ns_s_an, i, &rr) < 0)
                throw std::runtime_error("Malformed MX record");
            if (ns_rr_type(rr) != ns_t_mx || ns_rr_rdlen(rr) < 3)
                continue;
            char host[NS_MAXDNAME]{};
            if (dn_expand(buffer, buffer + size, ns_rr_rdata(rr) + 2, host, sizeof(host)) < 0)
                throw std::runtime_error("Malformed MX name");
            entries.emplace_back(ns_get16(ns_rr_rdata(rr)), host);
        }
    }
#endif
    if (entries.empty())
        return {domain}; // RFC 5321 implicit MX.
    std::sort(entries.begin(), entries.end());
    std::vector<std::string> result;
    for (auto [priority, host] : entries) {
        (void)priority;
        if (host == "." || host.empty())
            throw std::runtime_error("Domain publishes Null MX (does not accept mail)");
        if (host.ends_with('.'))
            host.pop_back();
        host = domain_name(host);
        if (std::find(result.begin(), result.end(), host) == result.end())
            result.push_back(host);
        if (result.size() == 10)
            break;
    }
    return result;
}
DeliveryResult smtp_deliver_to(const std::string &host, unsigned short port, const std::string &helo,
                               const QueuedMail &mail, std::string_view message, std::atomic_bool &stop,
                               bool allow_private_for_test, bool allow_ipv6) {
    using Curl = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
    Curl curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl)
        return {false, false, "SMTP transport unavailable"};
    struct Reader {
        std::string_view text;
        std::size_t at = 0;
    } reader{message};
    // Store rejection replies only; outgoing commands can contain message data.
    std::string rejection;
    char error_buffer[CURL_ERROR_SIZE]{};
    curl_easy_setopt(curl.get(), CURLOPT_ERRORBUFFER, error_buffer);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &rejection);
    curl_easy_setopt(
        curl.get(), CURLOPT_HEADERFUNCTION,
        +[](char *data, std::size_t unit, std::size_t count, void *context) noexcept -> std::size_t {
            const auto size = unit * count;
            if (size < 4 || (data[0] != '4' && data[0] != '5') || data[1] < '0' || data[1] > '9' ||
                data[2] < '0' || data[2] > '9' || (data[3] != '-' && data[3] != ' '))
                return size;
            try {
                auto &text = *static_cast<std::string *>(context);
                if (!text.empty() && text.size() < 400)
                    text += ' ';
                for (std::size_t i = 0; i < size && text.size() < 400; ++i) {
                    const auto c = static_cast<unsigned char>(data[i]);
                    if (c >= 32 && c <= 126)
                        text += static_cast<char>(c);
                    else if (c == '\t')
                        text += ' ';
                }
                return size;
            } catch (...) {
                return 0;
            }
        });
    auto url = "smtp://" + domain_name(host) + ":" + std::to_string(port) + "/" + domain_name(helo);
    auto from = "<" + email_address(mail.sender) + ">", to = "<" + email_address(mail.recipient) + ">";
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> recipients(
        curl_slist_append(nullptr, to.c_str()), curl_slist_free_all);
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    // IPv6 mail delivery requires independently configured reverse DNS and authentication.
    curl_easy_setopt(curl.get(), CURLOPT_IPRESOLVE,
                     static_cast<long>(allow_ipv6 ? CURL_IPRESOLVE_WHATEVER : CURL_IPRESOLVE_V4));
    curl_easy_setopt(curl.get(), CURLOPT_PROXY, "");
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "smtp");
    curl_easy_setopt(curl.get(), CURLOPT_MAIL_FROM, from.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_MAIL_RCPT, recipients.get());
    curl_easy_setopt(curl.get(), CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(
        curl.get(), CURLOPT_READFUNCTION,
        +[](char *out, std::size_t unit, std::size_t count, void *context) -> std::size_t {
            auto &r = *static_cast<Reader *>(context);
            auto n = std::min(unit * count, r.text.size() - r.at);
            std::memcpy(out, r.text.data() + r.at, n);
            r.at += n;
            return n;
        });
    curl_easy_setopt(curl.get(), CURLOPT_READDATA, &reader);
    curl_easy_setopt(curl.get(), CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_ALL));
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl.get(), CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2));
    if (const char *ca = std::getenv("CLOUDIEMAIL_CA_BUNDLE"); ca && *ca)
        curl_easy_setopt(curl.get(), CURLOPT_CAINFO, ca);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(
        curl.get(), CURLOPT_XFERINFOFUNCTION,
        +[](void *p, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
            return static_cast<std::atomic_bool *>(p)->load() ? 1 : 0;
        });
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &stop);
    curl_easy_setopt(curl.get(), CURLOPT_OPENSOCKETDATA, &allow_private_for_test);
    curl_easy_setopt(
        curl.get(), CURLOPT_OPENSOCKETFUNCTION,
        +[](void *p, curlsocktype, curl_sockaddr *addr) -> curl_socket_t {
            if (!*static_cast<bool *>(p) && !public_ip(addr))
                return CURL_SOCKET_BAD;
            return socket(addr->family, addr->socktype, addr->protocol);
        });
    auto status = curl_easy_perform(curl.get());
    long reply = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &reply);
    if (status == CURLE_OK && reply >= 200 && reply < 300)
        return {true, false, ""};
    const auto detail = !rejection.empty() ? rejection
                        : error_buffer[0]  ? std::string(error_buffer)
                                           : std::string(curl_easy_strerror(status));
    return {false, reply >= 500 && reply <= 599,
            ("SMTP " + std::to_string(reply) + ": " + detail + " [MX: " + host + "]").substr(0, 500)};
}
Outbound::Outbound(const Config &config) : config_(config) {
    prepare_mail_keys(config);
    auto path = config.database.parent_path() / "outbound-key.json";
    if (!std::filesystem::exists(path)) {
        randombytes_buf(key_.data(), 32);
        save_json(path, {{"key", hex(key_.bytes())}});
    } else {
        auto value = read_json(path);
        auto key = unhex(value.at("key").get<std::string>());
        if (key.size() != 32)
            throw std::runtime_error("Invalid outbound key");
        std::memcpy(key_.data(), key.data(), 32);
        sodium_memzero(key.data(), key.size());
    }
}
QueuedMail Outbound::seal(const std::string &sender, const std::string &recipient, std::string_view message) {
    QueuedMail mail;
    mail.id = random_id();
    mail.sender = email_address(sender);
    mail.recipient = email_address(recipient);
    mail.created = mail.next_attempt = unix_time();
    auto aad = queue_aad(mail);
    auto n = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
    mail.ciphertext.resize(n + message.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES);
    randombytes_buf(mail.ciphertext.data(), n);
    unsigned long long bytes = 0;
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(
            mail.ciphertext.data() + n, &bytes, reinterpret_cast<const unsigned char *>(message.data()),
            message.size(), reinterpret_cast<const unsigned char *>(aad.data()), aad.size(), nullptr,
            mail.ciphertext.data(), key_.data()) != 0)
        throw std::runtime_error("Cannot encrypt outgoing message");
    return mail;
}
std::string Outbound::open(const QueuedMail &mail) const {
    auto n = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
    auto aad = queue_aad(mail);
    if (mail.ciphertext.size() < n + crypto_aead_xchacha20poly1305_ietf_ABYTES)
        throw std::runtime_error("Invalid queued message");
    std::string plain(mail.ciphertext.size() - n - crypto_aead_xchacha20poly1305_ietf_ABYTES, '\0');
    unsigned long long bytes = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            reinterpret_cast<unsigned char *>(plain.data()), &bytes, nullptr, mail.ciphertext.data() + n,
            mail.ciphertext.size() - n, reinterpret_cast<const unsigned char *>(aad.data()), aad.size(),
            mail.ciphertext.data(), key_.data()) != 0) {
        wipe(plain);
        throw std::runtime_error("Queue authentication failed");
    }
    return plain;
}
bool Outbound::process_one(Database &database, std::int64_t now, const MailTransport &transport) {
    auto due = database.due_mail(now);
    if (due.empty())
        return false;
    auto mail = std::move(due[0]);
    DeliveryResult result;
    try {
        auto message = open(mail);
        try {
            result = transport(mail, message);
        } catch (...) {
            wipe(message);
            throw;
        }
        wipe(message);
    } catch (const std::exception &e) {
        result = {false, false, e.what()};
    }
    ++mail.attempts;
    mail.error = result.error;
    mail.next_attempt = now;
    if (result.success)
        mail.status = "sent";
    else if (result.permanent || now - mail.created >= 5 * 86400)
        mail.status = "failed";
    else
        mail.next_attempt = now + std::min<std::int64_t>(14400, 60LL * (1LL << std::min(mail.attempts, 8u)));
    database.update_delivery(mail);
    return true;
}
void Outbound::run(Database &database, std::atomic_bool &stop,
                   const std::function<void(const std::string &)> &log) {
    while (!stop) {
        try {
            bool processed =
                process_one(database, unix_time(), [&](const QueuedMail &mail, std::string_view message) {
                    DeliveryResult result{false, false, "No reachable MX"};
                    for (const auto &mx : resolve_mx(mail.recipient.substr(mail.recipient.find('@') + 1))) {
                        if (stop)
                            return DeliveryResult{false, false, "Delivery interrupted"};
                        result = smtp_deliver_to(mx, 25, config_.mail_host(), mail, message, stop, false,
                                                 config_.outbound_ipv6);
                        if (result.success || result.permanent)
                            break;
                    }
                    log(result.success     ? "delivery.sent"
                        : result.permanent ? "delivery.failed"
                                           : "delivery.retry");
                    return result;
                });
            if (processed)
                continue;
        } catch (...) {
            log("delivery.queue_error");
        }
        for (int i = 0; i < 20 && !stop; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
} // namespace cm
