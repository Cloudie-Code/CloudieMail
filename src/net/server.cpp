#include "cloudiemail/server.hpp"
#include "cloudiemail/i18n.hpp"
#include "cloudiemail/setup.hpp"
#include <algorithm>
#include <future>
#include <iostream>
#include <openssl/err.h>
#include <thread>
#ifdef _WIN32
#include "cloudiemail/webmail_windows.hpp"
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace cm {
namespace {
void close_socket(Socket s) {
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}
void nonblocking(Socket s) {
#ifdef _WIN32
    u_long mode = 1;
    if (ioctlsocket(s, FIONBIO, &mode))
        throw std::runtime_error("Falha no socket");
#else
    auto flags = fcntl(s, F_GETFL, 0);
    if (flags < 0 || fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0)
        throw std::runtime_error("Falha no socket");
#endif
}
bool would_block() {
#ifdef _WIN32
    auto e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINTR;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}
struct OwnedSocket {
    Socket value = invalid_socket;
    ~OwnedSocket() {
        if (value != invalid_socket)
            close_socket(value);
    }
    OwnedSocket() = default;
    OwnedSocket(const OwnedSocket &) = delete;
    OwnedSocket &operator=(const OwnedSocket &) = delete;
    OwnedSocket(OwnedSocket &&o) noexcept : value(o.value) {
        o.value = invalid_socket;
    }
};
} // namespace
Connection::Connection(Socket socket, SSL_CTX *context, std::atomic_bool &stopping, unsigned timeout,
                       std::string address)
    : HttpStream(std::move(address)), socket_(socket), context_(context), stopping_(stopping),
      expires_(std::chrono::steady_clock::now() + std::chrono::seconds(timeout)) {}
Connection::~Connection() {
    if (ssl_) {
        // One nonblocking shutdown sends close_notify without waiting on an untrusted peer.
        SSL_shutdown(ssl_);
        SSL_free(ssl_);
    }
    close_socket(socket_);
    wipe(input_);
}
void Connection::wait(bool writing, std::chrono::steady_clock::time_point deadline) {
    for (;;) {
        if (stopping_ || std::chrono::steady_clock::now() > std::min(deadline, expires_))
            throw std::runtime_error("Sessao encerrada/timeout");
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(socket_, &fds);
        timeval tv{0, 200000};
        auto rc = select(static_cast<int>(socket_ + 1), writing ? nullptr : &fds, writing ? &fds : nullptr,
                         nullptr, &tv);
        if (rc > 0)
            return;
        if (rc < 0 && !would_block())
            throw std::runtime_error("Socket indisponivel");
    }
}
void Connection::start_tls() {
    if (ssl_ || !input_.empty())
        throw std::runtime_error("Estado TLS invalido");
    ssl_ = SSL_new(context_);
    if (!ssl_ || SSL_set_fd(ssl_, static_cast<int>(socket_)) != 1)
        throw std::runtime_error("Falha TLS");
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        auto rc = SSL_accept(ssl_);
        if (rc == 1)
            return;
        auto e = SSL_get_error(ssl_, rc);
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE)
            throw std::runtime_error("Handshake TLS recusado");
        wait(e == SSL_ERROR_WANT_WRITE, deadline);
    }
}
std::string Connection::read_chunk() {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    char buf[8192];
    for (;;) {
        int n =
            ssl_ ? SSL_read(ssl_, buf, sizeof(buf)) : static_cast<int>(recv(socket_, buf, sizeof(buf), 0));
        if (n > 0)
            return std::string(buf, static_cast<std::size_t>(n));
        if (ssl_) {
            auto e = SSL_get_error(ssl_, n);
            if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE)
                throw std::runtime_error("Conexao encerrada");
            wait(e == SSL_ERROR_WANT_WRITE, deadline);
        } else {
            if (n == 0 || !would_block())
                throw std::runtime_error("Conexao encerrada");
            wait(false, deadline);
        }
    }
}
std::string Connection::line(std::size_t limit) {
    for (;;) {
        auto pos = input_.find("\r\n");
        if (pos != input_.npos) {
            if (pos > limit)
                throw std::runtime_error("Linha excedida");
            auto result = input_.substr(0, pos);
            input_.erase(0, pos + 2);
            return result;
        }
        if (input_.size() > limit + 1)
            throw std::runtime_error("Linha excedida");
        input_ += read_chunk();
    }
}
std::string Connection::read(std::size_t n) {
    while (input_.size() < n)
        input_ += read_chunk();
    auto out = input_.substr(0, n);
    input_.erase(0, n);
    return out;
}
void Connection::write(std::string_view value) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    std::size_t offset = 0;
    while (offset < value.size()) {
        if (stopping_ || std::chrono::steady_clock::now() > std::min(deadline, expires_))
            throw std::runtime_error("Timeout");
        auto amount = static_cast<int>(std::min<std::size_t>(16384, value.size() - offset));
        int n = ssl_ ? SSL_write(ssl_, value.data() + offset, amount)
                     : static_cast<int>(send(socket_, value.data() + offset, amount, 0));
        if (n > 0) {
            offset += static_cast<std::size_t>(n);
            continue;
        }
        if (ssl_) {
            auto e = SSL_get_error(ssl_, n);
            if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE)
                throw std::runtime_error("Conexao encerrada");
            wait(e == SSL_ERROR_WANT_WRITE, deadline);
        } else {
            if (!would_block())
                throw std::runtime_error("Conexao encerrada");
            wait(true, deadline);
        }
    }
}
Server::Server(Config config) : config_(std::move(config)), database_(config_) {
    if (database_.statistics().at("accounts").get<unsigned long long>() == 0)
        throw std::runtime_error("Crie a primeira conta antes de iniciar o servidor");
    validate_certificate(config_);
    if (config_.outbound_enabled)
        outbound_ = std::make_unique<Outbound>(config_);
    dummy_ = create_identity("invalid@" + config_.domain, random_id() + random_id()).document;
}
bool Server::acquire_connection() {
    auto count = active_connections_.load();
    while (count < config_.max_connections) {
        if (active_connections_.compare_exchange_weak(count, count + 1))
            return true;
    }
    return false;
}
void Connection::send_response(int status, std::string_view reason, std::string_view headers,
                               std::string_view body) {
    write("HTTP/1.1 " + std::to_string(status) + " " + std::string(reason) + "\r\n" + std::string(headers) +
          "\r\n");
    write(body);
}
std::shared_ptr<Login> Server::authenticate(const std::string &address, std::string_view password,
                                            const std::string &peer) {
    auto now = std::chrono::steady_clock::now();
    auto email = email_address(address);
    {
        std::lock_guard lock(auth_mutex_);
        std::erase_if(attempts_,
                      [&](const auto &item) { return now - item.second.since > std::chrono::minutes(5); });
        if (attempts_.size() > 4094)
            throw std::runtime_error("Autenticacao temporariamente indisponivel");
        for (const auto &id : {"ip:" + peer, "user:" + email}) {
            auto [it, inserted] = attempts_.try_emplace(id, Attempt{0, now});
            (void)inserted;
            if (it->second.count >= 10)
                throw std::runtime_error("Muitas tentativas; aguarde");
        }
        for (const auto &id : {"ip:" + peer, "user:" + email})
            ++attempts_.at(id).count;
    }
    if (!kdf_slots_.try_acquire())
        throw std::runtime_error("Autenticacao ocupada");
    struct Release {
        std::counting_semaphore<2> &slots;
        ~Release() {
            slots.release();
        }
    } release{kdf_slots_};
    auto document = database_.account(email);
    auto key = unlock_identity(document ? *document : dummy_, password);
    if (!document)
        throw std::runtime_error("Credenciais invalidas");
    auto login = std::make_shared<Login>();
    login->email = email;
    login->identity = *document;
    login->key = std::move(key);
    // Retain the IP counter even after successful logins to bound KDF CPU consumption.
    {
        std::lock_guard lock(auth_mutex_);
        attempts_.erase("user:" + email);
    }
    return login;
}
void Server::log(const std::string &event) {
    std::lock_guard lock(log_mutex_);
    log_events_.push_back({{"id", ++log_sequence_}, {"time", utc_now()}, {"event", event}});
    if (log_events_.size() > 500)
        log_events_.erase(log_events_.begin());
    std::cout << utc_now() << " " << tr(event, config_.language) << std::endl;
}
Json Server::logs(std::uint64_t after) const {
    std::lock_guard lock(log_mutex_);
    Json result = Json::array();
    for (const auto &event : log_events_)
        if (event.at("id").get<std::uint64_t>() > after)
            result.push_back(event);
    return result;
}
std::string Server::deliver(const std::string &sender, const std::vector<std::string> &recipients,
                            std::string_view message, const Login *authenticated) {
    if (message.size() > config_.max_message_bytes || recipients.empty() || recipients.size() > 20)
        throw std::runtime_error("Message limit exceeded");
    if (authenticated && authenticated->email != sender)
        throw std::runtime_error("Sender identity mismatch");
    std::string normalized;
    struct Wipe {
        std::string &value;
        ~Wipe() {
            wipe(value);
        }
    } wipe_normalized{normalized};
    if (authenticated) {
        auto end_headers = message.find("\r\n\r\n");
        if (end_headers == message.npos || end_headers > 65536)
            throw std::runtime_error("Invalid MIME headers");
        normalized = "From: " + sender + "\r\n";
        std::size_t at = 0;
        bool omit = false;
        while (at < end_headers) {
            auto end = message.find("\r\n", at);
            auto line = message.substr(at, end - at);
            at = end + 2;
            if (!line.empty() && line[0] != ' ' && line[0] != '\t') {
                auto colon = line.find(':');
                if (colon == line.npos)
                    throw std::runtime_error("Invalid MIME header");
                auto name = upper(std::string(line.substr(0, colon)));
                omit = name == "FROM" || name == "SENDER" || name == "RETURN-PATH" || name == "BCC" ||
                       name == "DKIM-SIGNATURE";
            }
            if (!omit)
                normalized += std::string(line) + "\r\n";
        }
        normalized += "\r\n" + std::string(message.substr(end_headers + 4));
        message = normalized;
    }
    auto id = random_id();
    auto received =
        "Received: by " + config_.mail_host() + " with ESMTP id " + id + "; " + mail_date() + "\r\n";
    auto content = received + "Return-Path: <" + sender + ">\r\n" + std::string(message);
    std::vector<Delivery> deliveries;
    std::vector<QueuedMail> outgoing;
    std::set<std::string> distinct;
    std::string signed_message;
    try {
        for (const auto &recipient : recipients) {
            auto email = email_address(recipient);
            if (!distinct.insert(email).second)
                continue;
            if (!email.ends_with("@" + config_.domain)) {
                if (!authenticated || !outbound_)
                    throw std::runtime_error("Authentication required for relay");
                if (signed_message.empty())
                    signed_message = dkim_sign(config_, message);
                outgoing.push_back(outbound_->seal(sender, email, signed_message));
                continue;
            }
            auto doc = database_.account(email);
            if (!doc)
                throw std::runtime_error("Recipient not found");
            deliveries.push_back({email, seal_message(*doc, content), content.size()});
        }
        if (authenticated)
            deliveries.push_back(
                {sender, seal_message(authenticated->identity, message), message.size(), "Sent"});
        database_.deliver(deliveries, config_.mailbox_quota_bytes, outgoing);
    } catch (...) {
        wipe(content);
        wipe(signed_message);
        throw;
    }
    wipe(content);
    wipe(signed_message);
    log(outgoing.empty() ? "message.received" : "delivery.queued");
    return id;
}
std::pair<std::string, WebSession> Server::new_session(std::shared_ptr<Login> login) {
    std::lock_guard lock(sessions_mutex_);
    auto now = std::chrono::steady_clock::now();
    std::erase_if(sessions_, [&](const auto &s) { return s.second.expires <= now; });
    if (sessions_.size() >= 128)
        throw std::runtime_error("Limite de sessoes");
    auto token = random_id() + random_id();
    WebSession session{std::move(login), random_id() + random_id(),
                       now + std::chrono::seconds(config_.session_timeout_seconds)};
    sessions_.emplace(token, session);
    return {token, session};
}
std::optional<WebSession> Server::session(const std::string &token) {
    std::lock_guard lock(sessions_mutex_);
    auto it = sessions_.find(token);
    if (it == sessions_.end())
        return {};
    if (it->second.expires <= std::chrono::steady_clock::now()) {
        sessions_.erase(it);
        return {};
    }
    return it->second;
}
void Server::logout(const std::string &token) {
    std::lock_guard lock(sessions_mutex_);
    sessions_.erase(token);
}
void Server::run() {
#ifndef _WIN32
    if (config_.webmail_driver != "socket" && config_.webmail_driver != "apache")
        throw std::runtime_error("HTTP.sys requer Windows; use HTTPS proprio em IP/porta livre no Linux");
#endif
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa))
        throw std::runtime_error("Winsock indisponivel");
    struct WinsockCleanup {
        ~WinsockCleanup() {
            WSACleanup();
        }
    } winsock_cleanup;
#endif
    stopping_ = false;
    using Context = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;
    Context ctx(SSL_CTX_new(TLS_server_method()), SSL_CTX_free);
    if (!ctx)
        throw std::runtime_error("TLS indisponivel");
    SSL_CTX_set_min_proto_version(ctx.get(), TLS1_2_VERSION);
    SSL_CTX_set_options(ctx.get(), SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
    SSL_CTX_set_session_cache_mode(ctx.get(), SSL_SESS_CACHE_OFF);
    SSL_CTX_set_num_tickets(ctx.get(), 0);
    if (SSL_CTX_set_cipher_list(ctx.get(), "ECDHE+AESGCM:ECDHE+CHACHA20") != 1 ||
        SSL_CTX_use_certificate_chain_file(ctx.get(), path_utf8(config_.certificate).c_str()) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx.get(), path_utf8(config_.private_key).c_str(), SSL_FILETYPE_PEM) !=
            1 ||
        SSL_CTX_check_private_key(ctx.get()) != 1)
        throw std::runtime_error("Certificado/chave TLS invalidos");
    std::vector<OwnedSocket> listeners;
    std::vector<std::size_t> kinds;
    const std::array<unsigned short, 6> ports = {
        config_.smtp_port,
        config_.smtps_port,
        config_.imap_port,
        config_.pop3_port,
        config_.webmail_driver == "apache" ? config_.webmail_backend_port : config_.https_port,
        config_.submission_port};
    for (std::size_t index = 0; index < ports.size(); ++index) {
        if (index == 4 && config_.webmail_driver == "http_sys")
            continue;
        auto port = ports[index];
        OwnedSocket listener;
        listener.value = socket(AF_INET, SOCK_STREAM, 0);
        if (listener.value == invalid_socket)
            throw std::runtime_error("Socket indisponivel");
#ifndef _WIN32
        int yes = 1;
        setsockopt(listener.value, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#else
        int yes = 1;
        setsockopt(listener.value, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&yes),
                   sizeof(yes));
#endif
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        auto address = index == 4 && config_.webmail_driver == "apache" ? "127.0.0.1" : config_.bind;
        if (inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1)
            throw std::runtime_error("Bind deve ser IPv4 numerico");
        if (bind(listener.value, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) ||
            listen(listener.value, 32))
            throw std::runtime_error("Nao foi possivel abrir " + config_.bind + ":" + std::to_string(port) +
                                     ". Verifique permissoes e se outro servidor ja usa esse IP/porta.");
        nonblocking(listener.value);
        listeners.push_back(std::move(listener));
        kinds.push_back(index);
    }
    std::vector<std::future<void>> workers;
    workers.reserve(config_.max_connections);
#ifdef _WIN32
    std::unique_ptr<HttpSysWebmail> native_webmail;
    if (config_.webmail_driver == "http_sys")
        native_webmail = std::make_unique<HttpSysWebmail>(*this, ctx.get());
#endif
    std::jthread delivery_worker;
    if (outbound_)
        delivery_worker = std::jthread(
            [this] { outbound_->run(database_, stopping_, [this](const auto &event) { log(event); }); });
    running_ = true;
    log("server.started");

    try {
        while (!stopping_) {
#ifdef _WIN32
            if (native_webmail)
                native_webmail->check();
#endif
            std::erase_if(workers, [](auto &worker) {
                if (worker.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    worker.get();
                    return true;
                }
                return false;
            });
            {
                std::lock_guard lock(sessions_mutex_);
                auto now = std::chrono::steady_clock::now();
                std::erase_if(sessions_, [&](const auto &session) { return session.second.expires <= now; });
            }
            fd_set set;
            FD_ZERO(&set);
            Socket maximum = 0;
            for (const auto &s : listeners) {
                FD_SET(s.value, &set);
                maximum = std::max(maximum, s.value);
            }
            timeval tv{0, 200000};
            auto ready = select(static_cast<int>(maximum + 1), &set, nullptr, nullptr, &tv);
            if (ready <= 0)
                continue;
            for (std::size_t index = 0; index < listeners.size(); ++index) {
                if (!FD_ISSET(listeners[index].value, &set))
                    continue;
                sockaddr_in address{};
#ifdef _WIN32
                int size = sizeof(address);
#else
                socklen_t size = sizeof(address);
#endif
                auto client = accept(listeners[index].value, reinterpret_cast<sockaddr *>(&address), &size);
                if (client == invalid_socket)
                    continue;
                if (workers.size() >= config_.max_connections) {
                    close_socket(client);
                    continue;
                }
                try {
                    nonblocking(client);
                } catch (...) {
                    close_socket(client);
                    continue;
                }
                char ip[INET_ADDRSTRLEN]{};
                inet_ntop(AF_INET, &address.sin_addr, ip, sizeof(ip));
                std::string peer(ip);
                if (!acquire_connection()) {
                    close_socket(client);
                    continue;
                }
                auto kind = kinds[index];
                try {
                    workers.push_back(std::async(std::launch::async, [this, client, kind, context = ctx.get(),
                                                                      peer] {
                        struct Release {
                            Server *server;
                            ~Release() {
                                server->release_connection();
                            }
                        } release{this};
                        try {
                            Connection c(client, context, stopping_, config_.session_timeout_seconds, peer);
                            if (kind == 1 || kind == 2 || kind == 3 ||
                                (kind == 4 && config_.webmail_driver != "apache"))
                                c.start_tls();
                            if (kind < 2 || kind == 5)
                                smtp(c, *this, kind != 0);
                            else if (kind == 2)
                                imap(c, *this);
                            else if (kind == 3)
                                pop3(c, *this);
                            else
                                http(c, *this);
                        } catch (const std::exception
                                     &) { /* Client errors never log credentials or message content. */
                        }
                    }));
                } catch (...) {
                    release_connection();
                    close_socket(client);
                    throw;
                }
            }
        }
    } catch (...) {
        stopping_ = true;
        running_ = false;
        throw;
    }
    stopping_ = true;
#ifdef _WIN32
    if (native_webmail)
        native_webmail->stop();
#endif
    for (auto &worker : workers)
        worker.get();
    {
        std::lock_guard lock(sessions_mutex_);
        sessions_.clear();
    }
    running_ = false;
    log("server.stopped");
}
} // namespace cm
