#include "cloudiemail/mime.hpp"
#include "cloudiemail/outbound.hpp"
#include <fstream>
#include <iostream>
namespace {
int count = 0;
void check(bool value, const char *message) {
    ++count;
    if (!value)
        throw std::runtime_error(message);
}
template <class F> void rejects(F f, const char *message) {
    bool rejected = false;
    try {
        f();
    } catch (...) {
        rejected = true;
    }
    check(rejected, message);
}
} // namespace
int main(int argc, char **argv) {
    try {
        cm::crypto_init();
        auto root = std::filesystem::temp_directory_path() / ("cloudiemail-features-" + cm::random_id());
        struct Cleanup {
            std::filesystem::path p;
            ~Cleanup() {
                std::error_code e;
                std::filesystem::remove_all(p, e);
            }
        } cleanup{root};
        cm::Config config;
        config.database = root / "mail.fdb";
        cm::Outbound queue(config);
        std::string message = "From: alice@example.test\r\nTo: outside@example.org\r\nSubject: DKIM "
                              "test\r\n\r\n  Line with  spaces\t \r\n\r\n";
        auto signed_message = cm::dkim_sign(config, message);
        if (argc == 2) {
            std::filesystem::create_directories(argv[1]);
            std::ofstream(std::filesystem::path(argv[1]) / "message.eml", std::ios::binary) << signed_message;
            std::ofstream(std::filesystem::path(argv[1]) / "empty.eml", std::ios::binary)
                << cm::dkim_sign(config, "From: alice@example.test\r\nSubject: Empty\r\n\r\n");
            std::ofstream(std::filesystem::path(argv[1]) / "dkim.txt") << cm::dkim_record(config);
        }
        check(signed_message.starts_with("DKIM-Signature:"), "DKIM missing");
        check(cm::dkim_sign(config, "From: alice@example.test\r\n\r\n")
                      .find("bh=47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=") != std::string::npos,
              "Empty relaxed body is not canonical");
        rejects([&] { cm::dkim_sign(config, "From: a@example.test\r\nFrom: b@example.test\r\n\r\n"); },
                "Ambiguous From signed");
        auto mail = queue.seal("alice@example.test", "outside@example.org", signed_message);
        check(queue.open(mail) == signed_message, "Queue encryption roundtrip");
        {
            cm::Outbound reopened(config);
            check(reopened.open(mail) == signed_message, "Persistent queue key lost");
        }
        auto altered = mail;
        altered.recipient = "attacker@example.org";
        rejects([&] { queue.open(altered); }, "Recipient substitution accepted");
        altered = mail;
        altered.id = "attacker";
        rejects([&] { queue.open(altered); }, "Queue ID substitution accepted");
        altered = mail;
        altered.ciphertext.back() ^= 1;
        rejects([&] { queue.open(altered); }, "Queue tampering accepted");
        auto recipients = cm::parse_recipients("a@example.org, b@example.org, a@example.org");
        check(recipients.size() == 2, "Recipient deduplication");
        rejects([&] { cm::parse_recipients("a@example.org\r\nBcc: b@example.org"); },
                "Recipient injection accepted");
        auto composed = cm::compose_message(
            "alice@example.test", recipients,
            {{"subject", "こんにちは 한국어 中文"},
             {"body", "Olá"},
             {"attachments",
              cm::Json::array({{{"name", "日本語.txt"}, {"type", "text/plain"}, {"data", "aGVsbG8="}}})}},
            config.domain);
        check(composed.find("=?UTF-8?B?") != std::string::npos &&
                  composed.find("multipart/mixed") != std::string::npos,
              "MIME encoding absent");
        rejects(
            [&] {
                cm::compose_message("alice@example.test", recipients,
                                    {{"subject", "bad\r\nBcc: a@example.org"}}, config.domain);
            },
            "Subject injection accepted");
        if (argc == 2)
            std::ofstream(std::filesystem::path(argv[1]) / "mime.eml", std::ios::binary) << composed;
        auto alice = cm::create_identity("alice@example.test", "feature test strong password"),
             bob = cm::create_identity("bob@example.test", "another strong password");
        {
            cm::Database db(config, true);
            db.add_account(alice.document);
            db.add_account(bob.document);
            check(db.folders("alice@example.test").size() == 5, "Default folders missing");
            db.create_folder("alice@example.test", "仕事");
            db.create_folder("bob@example.test", "仕事");
            rejects([&] { db.rename_folder("alice@example.test", "Sent", "INBOX"); },
                    "Rename overwrote INBOX");
            rejects([&] { db.create_folder("alice@example.test", "../bad"); }, "Invalid folder accepted");
            cm::Delivery delivery{"alice@example.test", cm::seal_message(alice.document, message),
                                  message.size(), "INBOX"};
            db.deliver({delivery}, 1048576, {mail});
            auto uid = db.messages("alice@example.test")[0].uid;
            db.organize("alice@example.test", uid, "仕事", cm::Json::array({"重要", "Work"}), true);
            auto moved_uid = db.messages("alice@example.test", "仕事")[0].uid;
            check(moved_uid > uid, "Move reused IMAP UID");
            uid = moved_uid;
            check(db.messages("alice@example.test").empty() &&
                      db.messages("alice@example.test", "仕事").size() == 1,
                  "Move failed");
            rejects([&] { db.organize("bob@example.test", uid, "仕事", cm::Json::array(), false); },
                    "Cross-account organize accepted");
            rejects([&] { db.remove_folder("alice@example.test", "仕事"); }, "Nonempty folder deleted");
            db.rename_folder("alice@example.test", "仕事", "Projetos");
            auto list = db.messages("alice@example.test", "Projetos");
            check(list.size() == 1 && list[0].flagged && list[0].labels.size() == 2, "Rename lost metadata");
            check(db.folders("bob@example.test").back()["name"] == "仕事", "Cross-account rename");
            check(cm::open_message(alice.document, alice.private_key,
                                   db.message("alice@example.test", uid)) == message,
                  "Move damaged ciphertext");
            check(db.delivery_status("bob@example.test").empty(), "Queue metadata leaks across accounts");
            auto now = cm::unix_time();
            check(
                queue.process_one(
                    db, now, [](const auto &, auto) { return cm::DeliveryResult{false, false, "SMTP 451"}; }),
                "Queue not processed");
            auto status = db.delivery_status("alice@example.test")[0];
            check(status["status"] == "pending" && status["attempts"] == 1 &&
                      status["next_attempt"].get<long long>() > now,
                  "Temporary failure not rescheduled");
            check(!queue.process_one(db, now,
                                     [](const auto &, auto) {
                                         throw std::runtime_error("too soon");
                                         return cm::DeliveryResult{};
                                     }),
                  "Retry ran early");
            queue.process_one(db, now + 1000,
                              [](const auto &, auto) { return cm::DeliveryResult{true, false, ""}; });
            check(db.delivery_status("alice@example.test")[0]["status"] == "sent",
                  "Successful delivery not recorded");
            auto rejected = queue.seal("alice@example.test", "bad@example.org", message);
            db.deliver({}, 1048576, {rejected});
            queue.process_one(db, now + 1000,
                              [](const auto &, auto) { return cm::DeliveryResult{false, true, "SMTP 550"}; });
            check(db.due_mail(now + 2000).empty(), "Permanent failure still queued");
            auto expired = queue.seal("alice@example.test", "old@example.org", message);
            db.deliver({}, 1048576, {expired});
            queue.process_one(db, now + 6 * 86400, [](const auto &, auto) {
                return cm::DeliveryResult{false, false, "Temporary"};
            });
            check(db.due_mail(now + 7 * 86400).empty(), "Expired delivery still queued");
            auto rollback = queue.seal("alice@example.test", "rollback@example.org", message);
            rejects([&] { db.deliver({delivery}, 1, {rollback}); }, "Quota ignored");
            check(db.due_mail(now + 8 * 86400).empty(), "Queue committed after mailbox rollback");
            db.organize("alice@example.test", uid, "Trash", cm::Json::array(), false);
            db.remove_folder("alice@example.test", "Projetos");
        }
        {
            cm::Database db(config);
            check(db.messages("alice@example.test", "Trash").size() == 1, "Reopen lost folders");
        }
        std::cout << count << " mail feature checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
