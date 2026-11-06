#pragma once
#ifdef _WIN32
#include "cloudiemail/config.hpp"
// OpenSSL must precede wincrypt.h, which defines conflicting X509 names.
#include <http.h>
#include <openssl/ssl.h>
#include <windows.h>
#include <winsock2.h>
namespace cm::detail {
// Preserve the original Host, Origin and framing headers for the shared handler.
std::string http_sys_request_head(const HTTP_REQUEST &request, const Config &config);
} // namespace cm::detail
#endif
