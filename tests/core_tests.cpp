#include "cloudiemail/config.hpp"
#include "cloudiemail/crypto.hpp"
#include "cloudiemail/dns.hpp"
#include "cloudiemail/server.hpp"
#include <functional>
#include <iostream>

namespace {
int assertions = 0;
void expect(bool value, const char *message) {
    ++assertions;
    if (!value)
        throw std::runtime_error(message);
}
void rejects(const std::function<void()> &fn, const char *message) {
    bool rejected = false;
    try {
        fn();
    } catch (...) {
        rejected = true;
    }
    expect(rejected, message);
}
} // namespace
int main(int argc, char **argv) {
    try {
        bool crypto_only = argc == 2 && std::string(argv[1]) == "--crypto-only";
        if (argc > 1 && !crypto_only)
            throw std::runtime_error("Use --crypto-only or no arguments");
        cm::crypto_init();
        auto alice = cm::create_identity("alice@example.test", "correct horse battery staple");
        auto bob = cm::create_identity("bob@example.test", "another strong password");
        expect(alice.document.at("key_id").get<std::string>().starts_with("example.test/"),
               "Domain context missing");
        auto another = cm::create_identity("alice@example.test", "correct horse battery staple");
        expect(alice.document.at("public_key") != another.document.at("public_key"), "Keys must be random");
        std::string message = "From: sender@example.test\r\nSubject: confidential\r\n\r\nbody\xff\0";
        message.append("\0\xff", 2);
        auto cipher = cm::seal_message(alice.document, message);
        auto key = cm::unlock_identity(alice.document, "correct horse battery staple");
        expect(cm::open_message(alice.document, key, cipher) == message, "Binary round trip failed");
        rejects([&] { cm::unlock_identity(alice.document, "incorrect password"); },
                "Wrong password accepted");
        rejects([&] { cm::open_message(bob.document, bob.private_key, cipher); },
                "Cross-account read accepted");
        cipher.back() ^= 1;
        rejects([&] { cm::open_message(alice.document, key, cipher); }, "Tampered ciphertext accepted");
        cipher.back() ^= 1;
        auto tampered = alice.document;
        tampered["key_id"] = "attacker";
        rejects([&] { cm::unlock_identity(tampered, "correct horse battery staple"); },
                "Context tampering accepted");
        tampered = alice.document;
        tampered["kdf"]["memory_bytes"] = 1;
        rejects([&] { cm::unlock_identity(tampered, "correct horse battery staple"); },
                "KDF downgrade accepted");
        auto changed =
            cm::change_password(alice.document, "correct horse battery staple", "a new strong password!");
        expect(changed.at("public_key") == alice.document.at("public_key"),
               "Password change changed public key");
        auto new_key = cm::unlock_identity(changed, "a new strong password!");
        expect(cm::open_message(changed, new_key, cipher) == message, "Password change lost messages");
        rejects([&] { cm::unlock_identity(changed, "correct horse battery staple"); },
                "Old password accepted");
        rejects([&] { cm::create_identity("alice@example.test", "short"); }, "Short password accepted");
        expect(cm::email_address("Alice@Example.Test") == "alice@example.test", "Email canonicalization");
        for (const char *value : {"bad\r\n.test", "x..test", "-x.test", "test", "x_test.test"})
            rejects([&] { cm::domain_name(value); }, "Invalid domain accepted");
        for (const char *value :
             {"a' OR 1=1@example.test", "a..b@example.test", "a@x\n.test", "@example.test"})
            rejects([&] { cm::email_address(value); }, "Invalid email accepted");
        auto plan = cm::dns_plan("example.test", "192.0.2.10");
        expect(plan[0].at("proxied") == false, "Mail must be DNS only");
        expect(plan[1].at("name") == "webmail.example.test", "Webmail is not automatic");
        rejects([&] { cm::dns_plan("example.test", "256.1.1.1"); }, "Invalid IP accepted");
        expect(cm::dns_txt_value("\"v=DKIM1; \" \"p=key\"") == "v=DKIM1; p=key", "Split TXT normalization");
        auto txt = cm::dns_zone_text(cm::Json::array(
            {{{"type", "TXT"}, {"name", "x.example.test"}, {"content", std::string(600, 'a')}}}));
        auto content = txt.substr(txt.find("TXT ") + 4);
        if (content.ends_with('\n'))
            content.pop_back();
        expect(cm::dns_txt_value(content) == std::string(600, 'a') &&
                   std::count(content.begin(), content.end(), '"') == 6,
               "Long TXT must use 255-byte chunks");
        expect(cm::dot_stuff(".hello\r\nworld\r\n") == "..hello\r\nworld\r\n.\r\n", "Dot escaping failed");
        auto tokens = cm::words("a LOGIN \"alice@example.test\" \"pass with spaces\"");
        expect(tokens.size() == 4 && tokens[3] == "pass with spaces", "Quoted IMAP password");
        rejects([&] { cm::words("a LOGIN \"unclosed"); }, "Unclosed quote accepted");
        if (crypto_only) {
            std::cout << assertions << " crypto/input assertions passed (Firebird tests not run)\n";
            return 0;
        }
        auto directory = std::filesystem::temp_directory_path() / ("cloudiemail-core-" + cm::random_id());
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() {
                std::error_code ignored;
                std::filesystem::remove_all(path, ignored);
            }
        } cleanup{directory};
        cm::Config config;
        config.database = directory / "mail.fdb";
        {
            cm::Database db(config, true);
            db.add_account(alice.document);
            db.add_account(bob.document);
            auto sealed = cm::seal_message(alice.document, message);
            cm::Delivery delivery{"alice@example.test", sealed, message.size()};
            rejects(
                [&] { db.deliver({delivery, {"missing@example.test", sealed, message.size()}}, 1048576); },
                "Partial delivery committed");
            expect(db.messages("alice@example.test").empty(), "Rollback did not undo first recipient");
            db.deliver({delivery}, 1048576);
            auto list = db.messages("alice@example.test");
            expect(list.size() == 1, "Database delivery missing");
            auto uid = list.at(0).uid;
            rejects([&] { db.message("bob@example.test", uid); }, "Database permits cross-account read");
            db.erase("bob@example.test", {uid});
            expect(db.messages("alice@example.test").size() == 1, "Cross-account delete permitted");
            rejects([&] { db.deliver({delivery}, message.size()); }, "Quota not enforced");
            expect(db.messages("alice@example.test").size() == 1, "Quota failure changed mailbox");
            db.replace_account("alice@example.test", alice.document, changed);
            auto persisted = db.account("alice@example.test");
            auto unlocked = cm::unlock_identity(*persisted, "a new strong password!");
            expect(cm::open_message(*persisted, unlocked, db.message("alice@example.test", uid)) == message,
                   "Persisted password rotation lost data");
            db.set_seen("alice@example.test", uid, true);
            expect(db.messages("alice@example.test")[0].seen, "Read flag not persisted");
            db.erase("alice@example.test", {uid});
            expect(db.messages("alice@example.test").empty(), "Delete failed");
            expect(db.uid_next() > uid, "UID reused after deletion");
        }
        std::cout << assertions << " assertions passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
