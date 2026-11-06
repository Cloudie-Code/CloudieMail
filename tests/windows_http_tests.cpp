#include "../src/platform/windows/http_sys_request.hpp"
#include <functional>
#include <iostream>
#include <ws2tcpip.h>

namespace {
unsigned checks = 0;
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
    ++checks;
}
void rejects(const std::function<void()> &fn, const char *message) {
    bool rejected = false;
    try {
        fn();
    } catch (const std::exception &) {
        rejected = true;
    }
    check(rejected, message);
}
} // namespace
int main() {
    try {
        cm::Config config;
        HTTP_REQUEST request{};
        HTTP_SSL_INFO ssl{};
        sockaddr_in local{};
        local.sin_family = AF_INET;
        inet_pton(AF_INET, "127.0.0.1", &local.sin_addr);
        request.Address.pLocalAddress = reinterpret_cast<sockaddr *>(&local);
        request.pSslInfo = &ssl;
        request.Verb = HttpVerbPOST;
        request.pRawUrl = "/api/login";
        request.RawUrlLength = 10;
        auto header = [&](HTTP_HEADER_ID id, const std::string &value) {
            request.Headers.KnownHeaders[id] = {static_cast<USHORT>(value.size()), value.data()};
        };
        const std::string host = "webmail.example.test", length = "19", cookie = "__Host-cloudiemail=test";
        header(HttpHeaderHost, host);
        header(HttpHeaderContentLength, length);
        header(HttpHeaderCookie, cookie);
        const char origin[] = "https://webmail.example.test";
        HTTP_UNKNOWN_HEADER unknown[] = {{6, sizeof(origin) - 1, "Origin", origin},
                                         {6, 5, "X-CSRF", "nonce"}};
        request.Headers.UnknownHeaderCount = 2;
        request.Headers.pUnknownHeaders = unknown;
        auto head = cm::detail::http_sys_request_head(request, config);
        check(head.starts_with("POST /api/login HTTP/1.1\r\n"), "Method/path changed");
        check(head.find("Host: webmail.example.test\r\n") != head.npos, "Host lost");
        check(head.find("Origin: https://webmail.example.test\r\n") != head.npos, "Origin lost");
        check(head.find("Content-Length: 19\r\n") != head.npos, "Framing lost");
        check(head.find("Cookie: __Host-cloudiemail=test\r\n") != head.npos, "Cookie lost");
        check(head.find("X-CSRF: nonce\r\n") != head.npos, "CSRF header lost");
        check(head.ends_with("\r\n\r\n"), "Headers not terminated");
        request.pSslInfo = nullptr;
        rejects([&] { cm::detail::http_sys_request_head(request, config); }, "Cleartext accepted");
        request.pSslInfo = &ssl;
        config.bind = "192.0.2.1";
        rejects([&] { cm::detail::http_sys_request_head(request, config); }, "Wrong local IP accepted");
        config.bind = "0.0.0.0";
        check(!cm::detail::http_sys_request_head(request, config).empty(), "Wildcard bind rejected");
        request.Verb = HttpVerbCONNECT;
        check(cm::detail::http_sys_request_head(request, config).starts_with("UNSUPPORTED "),
              "CONNECT became GET");
        const std::string oversized(17000, 'x');
        header(HttpHeaderCookie, oversized);
        rejects([&] { cm::detail::http_sys_request_head(request, config); }, "Oversized headers accepted");
        std::cout << checks << " HTTP.sys adapter checks passed (no native listener exercised)\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
