#include "cloudiemail/antispam.hpp"
#include "cloudiemail/mail_message.hpp"
#include "cloudiemail/outbound.hpp"
#include "dkim_vectors.hpp"
#include <iostream>
#include <map>
int main() {
    unsigned checks = 0;
    auto check = [&](bool ok, const char *description) {
        if (!ok)
            throw std::runtime_error(description);
        ++checks;
    };
    try {
        cm::crypto_init();
        cm::Config config;
        auto root = std::filesystem::temp_directory_path() / ("cloudiemail-protocol-" + cm::random_id());
        struct Cleanup {
            std::filesystem::path p;
            ~Cleanup() {
                std::error_code ec;
                std::filesystem::remove_all(p, ec);
            }
        } cleanup{root};
        config.database = root / "mail.fdb";
        cm::prepare_mail_keys(config);
        std::string message =
            "From: alice@example.test\r\nTo: receiver@example.org\r\nDate: Fri, 6 Nov 2026 12:00:00 "
            "+0000\r\nMessage-ID: <a@example.test>\r\nSubject: Good mail\r\n\r\nHello\r\n";
        using Key = std::pair<std::string, unsigned>;
        std::map<Key, std::vector<std::string>> records;
        records[{"example.test", 16}] = {"v=spf1 ip4:192.0.2.0/24 ip6:2001:db8::/32 -all"};
        records[{"cloudiemail._domainkey.example.test", 16}] = {cm::dkim_record(config)};
        records[{"_dmarc.example.test", 16}] = {"v=DMARC1; p=quarantine; aspf=s; adkim=s"};
        auto resolver = [&](const std::string &name, unsigned type) { return records[{name, type}]; };
        auto classify = [&](const std::string &data, const std::string &ip = "192.0.2.1") {
            return cm::classify_spam(config, data, "alice@example.test", ip, "mail.example.test", resolver);
        };
        auto good = classify(message);
        check(good.spf == "pass" && !good.junk && good.dmarc == "pass", "SPF v4 pass and DMARC alignment");
        check(classify(message, "2001:db8::1").spf == "pass", "SPF v6 pass");
        check(classify(message, "2001:db9::1").spf == "fail", "SPF v6 mismatch");
        check(classify(message, "198.51.100.1").junk, "DMARC failure quarantines spoof");
        auto signed_message = cm::dkim_sign(config, message);
        auto verified = classify(signed_message, "198.51.100.1");
        check(verified.dkim == "pass" && verified.dmarc == "pass" && !verified.junk,
              "DKIM verifies independently of SPF failure");
        auto modified = signed_message;
        modified.replace(modified.find("Hello"), 5, "HACK!");
        check(classify(modified).dkim == "fail", "DKIM body tampering rejected");
        modified = signed_message;
        modified.replace(modified.find("Good mail"), 9, "Bad mail!");
        check(classify(modified).dkim == "fail", "DKIM header tampering rejected");
        records[{"cloudiemail._domainkey.example.test", 16}] = {"v=DKIM1; p="};
        check(classify(signed_message).dkim == "fail", "revoked DKIM key rejected");
        records[{"cloudiemail._domainkey.example.test", 16}] = {cm::dkim_record(config)};
        auto empty = cm::dkim_sign(config, "From: alice@example.test\r\n\r\n");
        check(classify(empty).dkim == "pass", "empty relaxed DKIM body");
        records[{"example.test", 16}] = {"v=spf1 include:other.test -all"};
        records[{"other.test", 16}] = {"v=spf1 a mx -all"};
        records[{"other.test", 1}] = {"192.0.2.1"};
        check(classify(message).spf == "pass", "SPF include and A");
        records[{"other.test", 1}] = {};
        records[{"other.test", 15}] = {"10 mx.other.test."};
        records[{"mx.other.test", 1}] = {"192.0.2.1"};
        check(classify(message).spf == "pass", "SPF MX");
        records[{"example.test", 16}] = {"v=spf1 redirect=other.test"};
        check(classify(message).spf == "pass", "SPF redirect");
        records[{"other.test", 16}] = {"v=spf1 include:example.test -all"};
        check(classify(message).spf == "permerror", "SPF recursion bounded");
        records[{"example.test", 16}] = {"v=spf1 -all", "v=spf1 +all"};
        check(classify(message).spf == "permerror", "ambiguous SPF rejected");
        auto failure = cm::classify_spam(
            config, message, "outside@elsewhere.test", "192.0.2.1", "mail.elsewhere.test",
            [](auto &, auto) -> std::vector<std::string> { throw std::runtime_error("DNS offline"); });
        check(failure.spf == "temperror" && !failure.junk, "DNS failure alone does not quarantine");
        for (const auto &vector : dkim_vectors) {
            records[{"fixture._domainkey.example.test", 16}] = {vector.record};
            auto result = classify(vector.message);
            check(result.dkim == "pass", vector.name);
        }
        config.antispam_dns = false;
        auto gtube = message + "XJS*C4JDBQADN1.NSBN3*2IDNEN*GTUBE-STANDARD-ANTI-UBE-TEST-EMAIL*C.34X\r\n";
        check(classify(gtube).junk, "GTUBE detection");
        check(classify("From: a@example.test\r\nContent-Type: text/plain\r\nContent-Disposition: attachment; "
                       "filename=\"invoice.exe\"\r\n\r\nMZ")
                  .junk,
              "executable attachment detection");
        check(classify(
                  "From: a@example.test\r\nContent-Type: text/html\r\n\r\n<form><input type=password></form>")
                  .junk,
              "credential form detection");
        config.antispam_blocklist = {"example.test"};
        check(classify(message).junk, "domain blocklist");
        config.antispam_blocklist = {};
        config.antispam_enabled = false;
        check(!classify(gtube).junk, "filter can be disabled");
        auto forged =
            "X-CloudieMail-Spam: No\r\n\tforged\r\nX-CloudieMail-Authentication: dkim=pass\r\n" + message;
        check(cm::remove_spam_headers(forged) == message, "strip forged and folded results");
        for (auto name : {"INBOX", "日本語", "仕事/受信", "Olá & amigos", "한국어", "繁體中文", "Emoji 😀"})
            check(cm::mailbox_decode(cm::mailbox_encode(name)) == name, "mailbox UTF-7 roundtrip");
        check(cm::mailbox_encode("日本語") == "&ZeVnLIqe-", "modified UTF-7 reference vector");
        bool rejected = false;
        try {
            cm::mailbox_decode("&2AA-");
        } catch (...) {
            rejected = true;
        }
        check(rejected, "unpaired surrogate rejected");
        check(cm::mime_words("=?UTF-8?B?T2zDoQ==?= \r\n =?UTF-8?Q?_mundo?=") == "Olá mundo",
              "encoded header words");
        check(cm::mime_decoded("Ol=C3=A1=\r\n mundo", "QUOTED-PRINTABLE") == "Olá mundo",
              "quoted printable soft wrapping");
        auto parsed = cm::parse_message(message);
        check(cm::imap_envelope(parsed).find("\"alice\" \"example.test\"") != std::string::npos,
              "envelope address");
        check(cm::imap_bodystructure(parsed, true).starts_with("(\"TEXT\" \"PLAIN\""),
              "default MIME structure");
        check(cm::imap_date(cm::parse_mail_date("06-Nov-2026 09:00:00 -0300", true)) ==
                  "06-Nov-2026 12:00:00 +0000",
              "internal date timezone conversion");
        check(cm::imap_date(cm::parse_mail_date("Fri, 6 Nov 26 09:00 -0300 (BRT)")) ==
                  "06-Nov-2026 12:00:00 +0000",
              "Date with comments, two-digit year and optional seconds");
        bool bad_date = false;
        try {
            cm::parse_mail_date("31-Feb-2026 12:00:00 +0000", true);
        } catch (...) {
            bad_date = true;
        }
        check(bad_date, "Invalid calendar date rejected");
        std::cout << checks << " MIME/antispam checks passed\n";
    } catch (const std::exception &e) {
        std::cerr << "After " << checks << " checks: " << e.what() << '\n';
        return 1;
    }
}
