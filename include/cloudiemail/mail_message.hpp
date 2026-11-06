#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace cm {
struct MailHeader {
    std::string name, value;
    std::string_view raw;
};
struct MailPart {
    std::string_view raw, header, body;
    std::vector<MailHeader> headers;
    std::string type = "TEXT", subtype = "PLAIN", encoding = "7BIT";
    std::map<std::string, std::string> parameters;
    std::vector<MailPart> children;
    std::string get(std::string_view name) const;
};
MailPart parse_message(std::string_view raw);
std::string imap_string(std::string_view value);
std::string imap_envelope(const MailPart &part);
std::string imap_bodystructure(const MailPart &part, bool extended);
std::string mime_decoded(std::string_view text, std::string_view encoding);
std::string mime_words(std::string_view text);
std::string mailbox_encode(std::string_view utf8);
std::string mailbox_decode(std::string_view utf7);
std::string imap_date(std::int64_t timestamp);
std::int64_t parse_mail_date(std::string text, bool imap = false);
} // namespace cm
