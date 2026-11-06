#pragma once
#include "database.hpp"
#include "http_stream.hpp"
#include "outbound.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <openssl/ssl.h>
#include <semaphore>
#include <set>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

namespace cm {
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalid_socket = INVALID_SOCKET;
#else
using Socket = int;
constexpr Socket invalid_socket = -1;
#endif
class Connection : public HttpStream {
    Socket socket_;
    SSL *ssl_ = nullptr;
    SSL_CTX *context_;
    std::string input_;
    std::atomic_bool &stopping_;
    std::chrono::steady_clock::time_point expires_;
    void wait(bool writing, std::chrono::steady_clock::time_point deadline);
    std::string read_chunk();

  public:
    Connection(Socket socket, SSL_CTX *context, std::atomic_bool &stopping, unsigned timeout,
               std::string address);
    ~Connection() override;
    void start_tls();
    bool secure() const {
        return ssl_ != nullptr;
    }
    std::string line(std::size_t limit = 8192) override;
    std::string read(std::size_t bytes) override;
    void send_response(int status, std::string_view reason, std::string_view headers,
                       std::string_view body) override;
    void write(std::string_view data);
};
struct Login {
    std::string email;
    Json identity;
    Secret key;
};
struct WebSession {
    std::shared_ptr<Login> login;
    std::string csrf;
    std::chrono::steady_clock::time_point expires;
};
class Server {
    Config config_;
    Database database_;
    std::atomic_bool stopping_{false}, running_{false};
    std::atomic_size_t active_connections_{0};
    std::counting_semaphore<2> kdf_slots_{2};
    std::mutex auth_mutex_, sessions_mutex_;
    struct Attempt {
        unsigned count = 0;
        std::chrono::steady_clock::time_point since;
    };
    std::map<std::string, Attempt> attempts_;
    std::map<std::string, WebSession> sessions_;
    Json dummy_;
    std::unique_ptr<Outbound> outbound_;
    mutable std::mutex log_mutex_;
    Json log_events_ = Json::array();
    std::uint64_t log_sequence_ = 0;

  public:
    explicit Server(Config config);
    void run();
    void stop() {
        stopping_ = true;
    }
    bool running() const {
        return running_;
    }
    bool stopping() const {
        return stopping_;
    }
    bool acquire_connection();
    void release_connection() {
        --active_connections_;
    }
    const Config &config() const {
        return config_;
    }
    Database &database() {
        return database_;
    }
    std::shared_ptr<Login> authenticate(const std::string &email, std::string_view password,
                                        const std::string &peer);
    std::string deliver(const std::string &sender, const std::vector<std::string> &recipients,
                        std::string_view message, const Login *authenticated = nullptr);
    void log(const std::string &event);
    Json logs(std::uint64_t after = 0) const;
    std::pair<std::string, WebSession> new_session(std::shared_ptr<Login> login);
    std::optional<WebSession> session(const std::string &token);
    void logout(const std::string &token);
};
std::string upper(std::string value);
std::vector<std::string> words(std::string_view value);
std::string dot_stuff(std::string_view value);
void smtp(Connection &connection, Server &server, bool submission);
void pop3(Connection &connection, Server &server);
void imap(Connection &connection, Server &server);
void http(HttpStream &connection, Server &server);
} // namespace cm
