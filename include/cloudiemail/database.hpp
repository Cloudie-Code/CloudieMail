#pragma once
#include "config.hpp"
#include <memory>
#include <mutex>
#include <optional>

namespace cm {
struct MailboxError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct MessageInfo {
    std::uint64_t uid = 0;
    std::size_t size = 0;
    bool seen = false;
    std::string folder = "INBOX";
    Json labels = Json::array();
    bool flagged = false;
    Json imap_flags = Json::array();
    std::int64_t internal_date = 0;
    bool recent = false;
};
struct Delivery {
    std::string email;
    Bytes ciphertext;
    std::size_t size;
    std::string folder = "INBOX";
};
struct QueuedMail {
    std::string id, sender, recipient, status = "pending", error;
    Bytes ciphertext;
    unsigned attempts = 0;
    std::int64_t created = 0, next_attempt = 0;
};
class Database {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::mutex mutex_;

  public:
    explicit Database(const Config &config, bool create = false);
    ~Database();
    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;
    void add_account(const Json &document);
    std::optional<Json> account(const std::string &email);
    void replace_account(const std::string &email, const Json &expected, const Json &replacement);
    void deliver(const std::vector<Delivery> &deliveries, std::size_t quota,
                 const std::vector<QueuedMail> &outgoing = {});
    std::vector<MessageInfo> messages(const std::string &email, const std::string &folder = "INBOX",
                                      bool claim_recent = false);
    Json folders(const std::string &email);
    void create_folder(const std::string &email, const std::string &name);
    void rename_folder(const std::string &email, const std::string &name, const std::string &replacement);
    void remove_folder(const std::string &email, const std::string &name, bool purge = false);
    void subscribe(const std::string &email, const std::string &folder, bool enabled);
    Json subscriptions(const std::string &email);
    std::uint64_t append(const Delivery &delivery, std::size_t quota, const Json &flags,
                         std::int64_t internal_date);
    void store_flags(const std::string &email, const std::string &folder,
                     const std::vector<std::uint64_t> &uids, const Json &flags, int mode);
    std::vector<std::pair<std::uint64_t, std::uint64_t>>
    transfer(const std::string &email, const std::string &source, const std::string &destination,
             const std::vector<std::uint64_t> &uids, bool move, std::size_t quota);
    std::vector<std::uint64_t> expunge(const std::string &email, const std::string &folder,
                                       const std::vector<std::uint64_t> *uids = nullptr);
    std::uint64_t organize(const std::string &email, std::uint64_t uid, const std::string &folder,
                           const Json &labels, bool flagged);
    std::vector<QueuedMail> due_mail(std::int64_t now);
    void update_delivery(const QueuedMail &mail);
    Json delivery_status(const std::string &email);
    Bytes message(const std::string &email, std::uint64_t uid);
    void set_seen(const std::string &email, std::uint64_t uid, bool seen);
    void erase(const std::string &email, const std::vector<std::uint64_t> &uids);
    std::uint64_t uid_next();
    Json statistics();
};
} // namespace cm
