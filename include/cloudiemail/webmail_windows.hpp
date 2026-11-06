#pragma once
#ifdef _WIN32
#include <memory>
#include <openssl/ssl.h>
namespace cm {
class Server;
class HttpSysWebmail {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    HttpSysWebmail(Server &server, SSL_CTX *tls);
    ~HttpSysWebmail();
    void check() const;
    void stop();
};
} // namespace cm
#endif
