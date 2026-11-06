#pragma once
#include "database.hpp"
#include <atomic>
#include <functional>
namespace cm {
struct DeliveryResult {
    bool success = false, permanent = false;
    std::string error;
};
using MailTransport = std::function<DeliveryResult(const QueuedMail &, std::string_view)>;
void prepare_mail_keys(const Config &config);
std::string dkim_record(const Config &config);
std::string dkim_sign(const Config &config, std::string_view message);
std::vector<std::string> resolve_mx(const std::string &domain);
DeliveryResult smtp_deliver_to(const std::string &host, unsigned short port, const std::string &helo,
                               const QueuedMail &mail, std::string_view message, std::atomic_bool &stop,
                               bool allow_private_for_test = false, bool allow_ipv6 = false);
class Outbound {
    Config config_;
    Secret key_;

  public:
    explicit Outbound(const Config &config);
    QueuedMail seal(const std::string &sender, const std::string &recipient, std::string_view message);
    std::string open(const QueuedMail &mail) const;
    bool process_one(Database &database, std::int64_t now, const MailTransport &transport);
    void run(Database &database, std::atomic_bool &stop, const std::function<void(const std::string &)> &log);
};
std::int64_t unix_time();
} // namespace cm
