#include "cloudiemail/mime.hpp"
#include "cloudiemail/config.hpp"
#include <algorithm>
#include <cstring>
#include <regex>
#include <sodium.h>
#include <sstream>
namespace cm {
namespace {
std::string encode(std::string_view value) {
    std::string encoded(sodium_base64_ENCODED_LEN(value.size(), sodium_base64_VARIANT_ORIGINAL), '\0');
    sodium_bin2base64(encoded.data(), encoded.size(), reinterpret_cast<const unsigned char *>(value.data()),
                      value.size(), sodium_base64_VARIANT_ORIGINAL);
    encoded.resize(std::strlen(encoded.c_str()));
    return encoded;
}
std::string wrap64(std::string_view encoded) {
    std::string result;
    for (std::size_t at = 0; at < encoded.size(); at += 76)
        result += std::string(encoded.substr(at, 76)) + "\r\n";
    return result;
}
std::string parameter(const std::string &name) {
    static const char digits[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
            c == '_' || c == '-')
            out += static_cast<char>(c);
        else {
            out += '%';
            out += digits[c >> 4];
            out += digits[c & 15];
        }
    }
    return out;
}
} // namespace
std::vector<std::string> parse_recipients(const std::string &text) {
    std::vector<std::string> recipients;
    std::istringstream stream(text);
    std::string item;
    while (std::getline(stream, item, ',')) {
        auto a = item.find_first_not_of(" \t");
        auto b = item.find_last_not_of(" \t");
        if (a == item.npos)
            continue;
        auto email = email_address(item.substr(a, b - a + 1));
        if (std::find(recipients.begin(), recipients.end(), email) == recipients.end())
            recipients.push_back(email);
        if (recipients.size() > 20)
            throw std::runtime_error("Too many recipients");
    }
    return recipients;
}
std::string compose_message(const std::string &sender, const std::vector<std::string> &recipients,
                            const Json &data, const std::string &domain) {
    auto subject = data.value("subject", std::string());
    auto body = data.value("body", std::string());
    if (subject.size() > 200 || subject.find_first_of("\r\n\0", 0, 3) != subject.npos ||
        body.size() > 25 * 1024 * 1024)
        throw std::runtime_error("Invalid message");
    std::string to;
    for (const auto &recipient : recipients) {
        if (!to.empty())
            to += ", ";
        to += recipient;
    }
    // RFC 2047 encoded words, each <= 75 characters; split only at UTF-8 boundaries.
    std::string encoded_subject;
    if (std::any_of(subject.begin(), subject.end(), [](unsigned char c) { return c >= 128; })) {
        for (std::size_t at = 0; at < subject.size();) {
            auto end = std::min(subject.size(), at + 42);
            while (end < subject.size() && (static_cast<unsigned char>(subject[end]) & 0xc0) == 0x80)
                --end;
            if (!encoded_subject.empty())
                encoded_subject += "\r\n ";
            encoded_subject += "=?UTF-8?B?" + encode(std::string_view(subject).substr(at, end - at)) + "?=";
            at = end;
        }
    } else
        encoded_subject = subject;
    std::string result = "From: " + email_address(sender) + "\r\nTo: " + to +
                         "\r\nSubject: " + encoded_subject + "\r\nDate: " + mail_date() +
                         "\r\nMessage-ID: <" + random_id() + "@" + domain_name(domain) +
                         ">\r\nMIME-Version: 1.0\r\n";
    auto attachments = data.value("attachments", Json::array());
    if (!attachments.is_array() || attachments.size() > 10)
        throw std::runtime_error("Too many attachments");
    auto boundary = "CloudieMail-" + random_id();
    if (!attachments.empty())
        result +=
            "Content-Type: multipart/mixed; boundary=\"" + boundary + "\"\r\n\r\n--" + boundary + "\r\n";
    result += "Content-Type: text/plain; charset=utf-8\r\nContent-Transfer-Encoding: base64\r\n\r\n" +
              wrap64(encode(body));
    wipe(body);
    for (const auto &attachment : attachments) {
        auto name = attachment.at("name").get<std::string>(),
             type = attachment.value("type", std::string("application/octet-stream")),
             bytes = attachment.at("data").get<std::string>();
        if (name.empty() || name.size() > 255 || name.find_first_of("\r\n\0", 0, 3) != name.npos ||
            bytes.size() > 35 * 1024 * 1024 ||
            !std::regex_match(type, std::regex("[A-Za-z0-9.+-]+/[A-Za-z0-9.+-]+")))
            throw std::runtime_error("Invalid attachment");
        // Decode/encode to reject malformed data and prevent line/header injection.
        Bytes decoded(bytes.size());
        std::size_t size = 0;
        if (sodium_base642bin(decoded.data(), decoded.size(), bytes.data(), bytes.size(), nullptr, &size,
                              nullptr, sodium_base64_VARIANT_ORIGINAL) != 0)
            throw std::runtime_error("Invalid attachment encoding");
        result += "\r\n--" + boundary + "\r\nContent-Type: " + type +
                  "\r\nContent-Disposition: attachment; filename=\"attachment\"; filename*=UTF-8''" +
                  parameter(name) + "\r\nContent-Transfer-Encoding: base64\r\n\r\n" + wrap64(bytes);
        sodium_memzero(decoded.data(), decoded.size());
        wipe(bytes);
        if (result.size() > 35 * 1024 * 1024)
            throw std::runtime_error("Message too large");
    }
    if (!attachments.empty())
        result += "\r\n--" + boundary + "--\r\n";
    return result;
}
} // namespace cm
