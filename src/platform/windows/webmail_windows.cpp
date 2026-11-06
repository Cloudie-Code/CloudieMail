#include "cloudiemail/webmail_windows.hpp"
#ifdef _WIN32
#include "cloudiemail/server.hpp"
#include "http_sys_request.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <http.h>
#include <limits>
#include <openssl/pkcs12.h>
#include <sddl.h>
#include <sodium.h>
#include <thread>
#include <wincrypt.h>
#include <windows.h>
#include <ws2tcpip.h>

namespace cm {
namespace {
constexpr GUID application_id{0xd758f987, 0x404e, 0x4481, {0x9d, 0xae, 0xa3, 0x69, 0x66, 0xa2, 0x14, 0x51}};
constexpr HTTPAPI_VERSION api_version{2, 0};
constexpr ULONG initialize_flags = HTTP_INITIALIZE_SERVER | HTTP_INITIALIZE_CONFIG;
[[noreturn]] void failure(const char *operation, ULONG code) {
    auto message = std::string(operation) + " (Windows " + std::to_string(code) + ")";
    if (code == ERROR_ACCESS_DENIED)
        message += ". Abra o CloudieMail como administrador para preparar ou renovar o HTTPS nativo.";
    throw std::runtime_error(message);
}
void checked(ULONG code, const char *operation) {
    if (code != NO_ERROR)
        failure(operation, code);
}
struct Store {
    HCERTSTORE value = nullptr;
    ~Store() {
        if (value)
            CertCloseStore(value, 0);
    }
};
struct Certificate {
    PCCERT_CONTEXT value = nullptr;
    ~Certificate() {
        if (value)
            CertFreeCertificateContext(value);
    }
};
struct Api {
    Api() {
        checked(HttpInitialize(api_version, initialize_flags, nullptr), "HTTP.sys indisponivel");
    }
    ~Api() {
        HttpTerminate(initialize_flags, nullptr);
    }
};
struct SensitiveBytes {
    Bytes value;
    ~SensitiveBytes() {
        if (!value.empty())
            sodium_memzero(value.data(), value.size());
    }
};
std::wstring widen_ascii(const std::string &value) {
    return {value.begin(), value.end()};
}
std::wstring url_prefix(const Config &config) {
    return L"https://" + widen_ascii(config.webmail_host()) + L":" + std::to_wstring(config.https_port) +
           L"/";
}
std::wstring user_sddl() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        failure("Nao foi possivel consultar usuario", GetLastError());
    struct Close {
        HANDLE handle;
        ~Close() {
            CloseHandle(handle);
        }
    } close{token};
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    Bytes bytes(size);
    if (!GetTokenInformation(token, TokenUser, bytes.data(), size, &size))
        failure("Nao foi possivel consultar SID", GetLastError());
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(bytes.data())->User.Sid, &sid))
        failure("Nao foi possivel converter SID", GetLastError());
    std::wstring descriptor = L"D:(A;;GX;;;SY)(A;;GX;;;" + std::wstring(sid) + L")";
    LocalFree(sid);
    return descriptor;
}
// Never replace an existing reservation: a different account or application may own it.
void ensure_url_reservation(const Config &config) {
    auto url = url_prefix(config);
    HTTP_SERVICE_CONFIG_URLACL_QUERY query{};
    query.QueryDesc = HttpServiceConfigQueryExact;
    query.KeyDesc.pUrlPrefix = url.data();
    ULONG bytes = 0;
    auto code = HttpQueryServiceConfiguration(nullptr, HttpServiceConfigUrlAclInfo, &query, sizeof(query),
                                              nullptr, 0, &bytes, nullptr);
    if (code == ERROR_INSUFFICIENT_BUFFER)
        return; // HttpAddUrlToUrlGroup enforces the existing ACL.
    if (code != ERROR_FILE_NOT_FOUND && code != ERROR_NO_MORE_ITEMS)
        checked(code, "Falha ao consultar reserva HTTPS");
    auto sddl = user_sddl();
    HTTP_SERVICE_CONFIG_URLACL_SET record{};
    record.KeyDesc.pUrlPrefix = url.data();
    record.ParamDesc.pStringSecurityDescriptor = sddl.data();
    checked(
        HttpSetServiceConfiguration(nullptr, HttpServiceConfigUrlAclInfo, &record, sizeof(record), nullptr),
        "Falha ao reservar endereco do webmail");
}
void import_certificate(SSL_CTX *tls, const Bytes &fingerprint) {
    Store personal{CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                                 CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_OPEN_EXISTING_FLAG, L"MY")};
    if (!personal.value)
        failure("Nao foi possivel abrir certificados do computador", GetLastError());
    CRYPT_HASH_BLOB hash{static_cast<DWORD>(fingerprint.size()), const_cast<BYTE *>(fingerprint.data())};
    Certificate existing{
        CertFindCertificateInStore(personal.value, X509_ASN_ENCODING, 0, CERT_FIND_HASH, &hash, nullptr)};
    DWORD property_size = 0;
    if (existing.value && CertGetCertificateContextProperty(existing.value, CERT_KEY_PROV_INFO_PROP_ID,
                                                            nullptr, &property_size))
        return;
    std::string password = random_id() + random_id();
    std::wstring wide_password = widen_ascii(password);
    struct WipePasswords {
        std::string &text;
        std::wstring &wide;
        ~WipePasswords() {
            wipe(text);
            if (!wide.empty())
                SecureZeroMemory(wide.data(), wide.size() * sizeof(wchar_t));
        }
    } wipe_passwords{password, wide_password};
    STACK_OF(X509) *chain = nullptr;
    SSL_CTX_get0_chain_certs(tls, &chain);
    std::unique_ptr<PKCS12, decltype(&PKCS12_free)> pfx(
        PKCS12_create(password.c_str(), "CloudieMail webmail", SSL_CTX_get0_privatekey(tls),
                      SSL_CTX_get0_certificate(tls), chain, NID_aes_256_cbc, NID_aes_256_cbc, 10000, 10000,
                      0),
        PKCS12_free);
    if (!pfx)
        throw std::runtime_error("Falha ao preparar certificado para HTTPS do Windows");
    int length = i2d_PKCS12(pfx.get(), nullptr);
    if (length <= 0)
        throw std::runtime_error("Falha ao codificar certificado Windows");
    SensitiveBytes encoded;
    encoded.value.resize(static_cast<std::size_t>(length));
    auto *position = encoded.value.data();
    if (i2d_PKCS12(pfx.get(), &position) != length)
        throw std::runtime_error("Falha ao codificar certificado Windows");
    CRYPT_DATA_BLOB blob{static_cast<DWORD>(encoded.value.size()), encoded.value.data()};
    Store imported{
        PFXImportCertStore(&blob, wide_password.c_str(), CRYPT_MACHINE_KEYSET | PKCS12_PREFER_CNG_KSP)};
    if (!imported.value)
        failure("Falha ao importar chave TLS para o Windows", GetLastError());
    Certificate leaf{
        CertFindCertificateInStore(imported.value, X509_ASN_ENCODING, 0, CERT_FIND_HASH, &hash, nullptr)};
    if (!leaf.value)
        throw std::runtime_error("Certificado do webmail ausente no PFX interno");
    if (!CertAddCertificateContextToStore(personal.value, leaf.value, CERT_STORE_ADD_REPLACE_EXISTING,
                                          nullptr))
        failure("Falha ao salvar certificado do webmail", GetLastError());
    // Intermediate certificates belong in CA, never automatically in trusted ROOT.
    Store intermediates{CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                                      CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_OPEN_EXISTING_FLAG,
                                      L"CA")};
    if (!intermediates.value)
        failure("Falha ao abrir certificados intermediarios", GetLastError());
    PCCERT_CONTEXT item = nullptr;
    while ((item = CertEnumCertificatesInStore(imported.value, item))) {
        if (CertCompareCertificate(X509_ASN_ENCODING, item->pCertInfo, leaf.value->pCertInfo) ||
            CertCompareCertificateName(X509_ASN_ENCODING, &item->pCertInfo->Subject,
                                       &item->pCertInfo->Issuer))
            continue;
        if (!CertAddCertificateContextToStore(intermediates.value, item, CERT_STORE_ADD_USE_EXISTING,
                                              nullptr)) {
            auto error = GetLastError();
            CertFreeCertificateContext(item);
            failure("Falha ao salvar cadeia intermediaria", error);
        }
    }
}
void ensure_sni(const Config &config, SSL_CTX *tls) {
    Bytes fingerprint(EVP_MAX_MD_SIZE);
    unsigned int length = 0;
    // HTTP.sys identifies a Windows certificate by its SHA-1 store thumbprint;
    // this does not select SHA-1 signatures or weaken the TLS certificate.
    if (X509_digest(SSL_CTX_get0_certificate(tls), EVP_sha1(), fingerprint.data(), &length) != 1)
        throw std::runtime_error("Falha ao identificar certificado TLS");
    fingerprint.resize(length);
    auto host = widen_ascii(config.webmail_host());
    HTTP_SERVICE_CONFIG_SSL_SNI_SET desired{};
    auto *address = reinterpret_cast<sockaddr_in *>(&desired.KeyDesc.IpPort);
    address->sin_family = AF_INET;
    address->sin_port = htons(config.https_port);
    desired.KeyDesc.Host = host.data();
    desired.ParamDesc.SslHashLength = static_cast<ULONG>(fingerprint.size());
    desired.ParamDesc.pSslHash = fingerprint.data();
    desired.ParamDesc.AppId = application_id;
    wchar_t store[] = L"MY";
    desired.ParamDesc.pSslCertStoreName = store;
    desired.ParamDesc.DefaultFlags =
        HTTP_SERVICE_CONFIG_SSL_FLAG_DISABLE_LEGACY_TLS | HTTP_SERVICE_CONFIG_SSL_FLAG_DISABLE_QUIC;
    HTTP_SERVICE_CONFIG_SSL_SNI_QUERY query{};
    query.QueryDesc = HttpServiceConfigQueryExact;
    query.KeyDesc = desired.KeyDesc;
    ULONG size = 0;
    auto code = HttpQueryServiceConfiguration(nullptr, HttpServiceConfigSslSniCertInfo, &query, sizeof(query),
                                              nullptr, 0, &size, nullptr);
    Bytes old;
    HTTP_SERVICE_CONFIG_SSL_SNI_SET *previous = nullptr;
    if (code == ERROR_INSUFFICIENT_BUFFER) {
        old.resize(size);
        checked(HttpQueryServiceConfiguration(nullptr, HttpServiceConfigSslSniCertInfo, &query, sizeof(query),
                                              old.data(), size, &size, nullptr),
                "Falha ao ler binding SNI");
        previous = reinterpret_cast<HTTP_SERVICE_CONFIG_SSL_SNI_SET *>(old.data());
        if (!IsEqualGUID(previous->ParamDesc.AppId, application_id))
            throw std::runtime_error(
                "O hostname HTTPS do webmail ja pertence a outro aplicativo; o binding foi preservado");
        if (previous->ParamDesc.SslHashLength == fingerprint.size() &&
            std::memcmp(previous->ParamDesc.pSslHash, fingerprint.data(), fingerprint.size()) == 0 &&
            previous->ParamDesc.DefaultFlags == desired.ParamDesc.DefaultFlags) {
            import_certificate(tls, fingerprint);
            return;
        }
    } else if (code != ERROR_FILE_NOT_FOUND && code != ERROR_NO_MORE_ITEMS) {
        checked(code, "Falha ao consultar binding SNI");
    }
    import_certificate(tls, fingerprint);
    if (previous)
        checked(HttpDeleteServiceConfiguration(nullptr, HttpServiceConfigSslSniCertInfo, previous,
                                               sizeof(*previous), nullptr),
                "Falha ao atualizar SNI do CloudieMail");
    code = HttpSetServiceConfiguration(nullptr, HttpServiceConfigSslSniCertInfo, &desired, sizeof(desired),
                                       nullptr);
    if (code != NO_ERROR) {
        if (previous) {
            auto restored = HttpSetServiceConfiguration(nullptr, HttpServiceConfigSslSniCertInfo, previous,
                                                        sizeof(*previous), nullptr);
            if (restored != NO_ERROR)
                failure("Falha ao restaurar binding SNI anterior do CloudieMail", restored);
        }
        failure("Falha ao configurar SSL/SNI do webmail", code);
    }
}
std::string numeric_address(const sockaddr *address) {
    char text[INET6_ADDRSTRLEN]{};
    if (!address)
        throw std::runtime_error("Endereco HTTP.sys ausente");
    if (address->sa_family == AF_INET) {
        inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in *>(address)->sin_addr, text, sizeof(text));
    } else if (address->sa_family == AF_INET6) {
        auto *ip = &reinterpret_cast<const sockaddr_in6 *>(address)->sin6_addr;
        if (IN6_IS_ADDR_V4MAPPED(ip))
            inet_ntop(AF_INET, &ip->u.Byte[12], text, sizeof(text));
        else
            inet_ntop(AF_INET6, ip, text, sizeof(text));
    }
    if (!text[0])
        throw std::runtime_error("Endereco HTTP.sys invalido");
    return text;
}
constexpr std::array<const char *, HttpHeaderRequestMaximum> request_headers = {"Cache-Control",
                                                                                "Connection",
                                                                                "Date",
                                                                                "Keep-Alive",
                                                                                "Pragma",
                                                                                "Trailer",
                                                                                "Transfer-Encoding",
                                                                                "Upgrade",
                                                                                "Via",
                                                                                "Warning",
                                                                                "Allow",
                                                                                "Content-Length",
                                                                                "Content-Type",
                                                                                "Content-Encoding",
                                                                                "Content-Language",
                                                                                "Content-Location",
                                                                                "Content-MD5",
                                                                                "Content-Range",
                                                                                "Expires",
                                                                                "Last-Modified",
                                                                                "Accept",
                                                                                "Accept-Charset",
                                                                                "Accept-Encoding",
                                                                                "Accept-Language",
                                                                                "Authorization",
                                                                                "Cookie",
                                                                                "Expect",
                                                                                "From",
                                                                                "Host",
                                                                                "If-Match",
                                                                                "If-Modified-Since",
                                                                                "If-None-Match",
                                                                                "If-Range",
                                                                                "If-Unmodified-Since",
                                                                                "Max-Forwards",
                                                                                "Proxy-Authorization",
                                                                                "Referer",
                                                                                "Range",
                                                                                "TE",
                                                                                "Translate",
                                                                                "User-Agent"};
class NativeStream final : public HttpStream {
    HANDLE queue_;
    HTTP_REQUEST_ID id_;
    Server &server_;
    std::string input_;
    std::size_t offset_ = 0;

  public:
    NativeStream(HANDLE queue, const HTTP_REQUEST &request, Server &server)
        : HttpStream(numeric_address(request.Address.pRemoteAddress)), queue_(queue), id_(request.RequestId),
          server_(server) {
        input_ = detail::http_sys_request_head(request, server.config());
    }
    ~NativeStream() override {
        wipe(input_);
    }
    std::string line(std::size_t limit) override {
        auto end = input_.find("\r\n", offset_);
        if (end == input_.npos || end - offset_ > limit)
            throw std::runtime_error("Linha HTTP invalida");
        auto result = input_.substr(offset_, end - offset_);
        offset_ = end + 2;
        return result;
    }
    std::string read(std::size_t bytes) override {
        if (bytes > server_.config().max_message_bytes)
            throw std::runtime_error("Corpo excedido");
        std::string body(bytes, '\0');
        try {
            std::size_t done = 0;
            while (done < bytes) {
                if (server_.stopping())
                    throw std::runtime_error("Servidor parando");
                ULONG received = 0;
                auto amount = static_cast<ULONG>(std::min<std::size_t>(bytes - done, 65536));
                auto code = HttpReceiveRequestEntityBody(queue_, id_, 0, body.data() + done, amount,
                                                         &received, nullptr);
                if (code != NO_ERROR || !received)
                    failure("Corpo HTTP incompleto", code);
                done += received;
            }
            return body;
        } catch (...) {
            wipe(body);
            throw;
        }
    }
    void send_response(int status, std::string_view reason, std::string_view headers,
                       std::string_view body) override {
        HTTP_RESPONSE response{};
        response.Version = {1, 1};
        response.StatusCode = static_cast<USHORT>(status);
        response.pReason = reason.data();
        response.ReasonLength = static_cast<USHORT>(reason.size());
        std::vector<HTTP_UNKNOWN_HEADER> unknown;
        std::size_t position = 0;
        while (position < headers.size()) {
            auto end = headers.find("\r\n", position);
            if (end == headers.npos)
                throw std::runtime_error("Resposta HTTP invalida");
            auto line = headers.substr(position, end - position);
            auto colon = line.find(':');
            if (colon == line.npos || colon + 2 > line.size())
                throw std::runtime_error("Cabecalho de resposta invalido");
            auto name = line.substr(0, colon), value = line.substr(colon + 2);
            if (name.size() > USHRT_MAX || value.size() > USHRT_MAX)
                throw std::runtime_error("Cabecalho excedido");
            int known = name == "Content-Type"     ? HttpHeaderContentType
                        : name == "Content-Length" ? HttpHeaderContentLength
                        : name == "Connection"     ? HttpHeaderConnection
                        : name == "Cache-Control"  ? HttpHeaderCacheControl
                        : name == "Set-Cookie"     ? HttpHeaderSetCookie
                                                   : -1;
            if (known >= 0)
                response.Headers.KnownHeaders[known] = {static_cast<USHORT>(value.size()), value.data()};
            else
                unknown.push_back({static_cast<USHORT>(name.size()), static_cast<USHORT>(value.size()),
                                   name.data(), value.data()});
            position = end + 2;
        }
        response.Headers.UnknownHeaderCount = static_cast<USHORT>(unknown.size());
        response.Headers.pUnknownHeaders = unknown.data();
        HTTP_DATA_CHUNK chunk{};
        if (!body.empty()) {
            if (body.size() > ULONG_MAX)
                throw std::runtime_error("Resposta HTTP excedida");
            chunk.DataChunkType = HttpDataChunkFromMemory;
            chunk.FromMemory.pBuffer = const_cast<char *>(body.data());
            chunk.FromMemory.BufferLength = static_cast<ULONG>(body.size());
            response.EntityChunkCount = 1;
            response.pEntityChunks = &chunk;
        }
        checked(HttpSendHttpResponse(queue_, id_, HTTP_SEND_RESPONSE_FLAG_DISCONNECT, &response, nullptr,
                                     nullptr, nullptr, 0, nullptr, nullptr),
                "Falha ao enviar resposta HTTP");
    }
};
} // namespace

std::string detail::http_sys_request_head(const HTTP_REQUEST &request, const Config &config) {
    std::string input;
    if (!request.pSslInfo)
        throw std::runtime_error("Webmail exige TLS");
    if (config.bind != "0.0.0.0" && numeric_address(request.Address.pLocalAddress) != config.bind)
        throw std::runtime_error("IP local fora da configuracao");
    const char *method = request.Verb == HttpVerbGET      ? "GET"
                         : request.Verb == HttpVerbPOST   ? "POST"
                         : request.Verb == HttpVerbDELETE ? "DELETE"
                                                          : "UNSUPPORTED";
    input = std::string(method) + " " + std::string(request.pRawUrl, request.RawUrlLength) + " HTTP/1.1\r\n";
    auto append = [&](std::string_view name, std::string_view value) {
        if (input.size() + name.size() + value.size() + 4 > 16384)
            throw std::runtime_error("Cabecalhos excedidos");
        input += name;
        input += ": ";
        input += value;
        input += "\r\n";
    };
    for (std::size_t i = 0; i < request_headers.size(); ++i) {
        const auto &header = request.Headers.KnownHeaders[i];
        if (header.RawValueLength)
            append(request_headers[i], {header.pRawValue, header.RawValueLength});
    }
    for (USHORT i = 0; i < request.Headers.UnknownHeaderCount; ++i) {
        const auto &header = request.Headers.pUnknownHeaders[i];
        append({header.pName, header.NameLength}, {header.pRawValue, header.RawValueLength});
    }
    input += "\r\n";
    return input;
}

struct HttpSysWebmail::Impl {
    Api api;
    Server &server;
    HTTP_SERVER_SESSION_ID session = 0;
    HTTP_URL_GROUP_ID group = 0;
    HANDLE queue = nullptr;
    std::atomic_bool stopping{false};
    std::atomic<ULONG> error{NO_ERROR};
    std::mutex mutex;
    std::set<HTTP_REQUEST_ID> requests;
    std::vector<std::thread> workers;
    explicit Impl(Server &value, SSL_CTX *tls) : server(value) {
        try {
            ensure_sni(server.config(), tls);
            ensure_url_reservation(server.config());
            checked(HttpCreateServerSession(api_version, &session, 0), "Falha ao criar sessao HTTP.sys");
            checked(HttpCreateUrlGroup(session, &group, 0), "Falha ao criar grupo HTTP.sys");
            checked(HttpCreateRequestQueue(api_version, nullptr, nullptr, 0, &queue),
                    "Falha ao criar fila HTTP.sys");
            ULONG queue_length = static_cast<ULONG>(server.config().max_connections);
            checked(HttpSetRequestQueueProperty(queue, HttpServerQueueLengthProperty, &queue_length,
                                                sizeof(queue_length), 0, nullptr),
                    "Falha ao limitar fila HTTP.sys");
            HTTP_BINDING_INFO binding{};
            binding.Flags.Present = 1;
            binding.RequestQueueHandle = queue;
            checked(HttpSetUrlGroupProperty(group, HttpServerBindingProperty, &binding, sizeof(binding)),
                    "Falha no grupo HTTP.sys");
            HTTP_TIMEOUT_LIMIT_INFO timeout{};
            timeout.Flags.Present = 1;
            timeout.EntityBody = timeout.DrainEntityBody = timeout.IdleConnection = 30;
            timeout.HeaderWait = 15;
            timeout.RequestQueue = 10;
            timeout.MinSendRate = 1024;
            checked(HttpSetUrlGroupProperty(group, HttpServerTimeoutsProperty, &timeout, sizeof(timeout)),
                    "Falha nos limites HTTP.sys");
            auto url = url_prefix(server.config());
            checked(HttpAddUrlToUrlGroup(group, url.c_str(), 0, 0),
                    "Nao foi possivel registrar URL do webmail; verifique conflito de hostname");
            auto count = std::min<std::size_t>(4, server.config().max_connections);
            for (std::size_t i = 0; i < count; ++i)
                workers.emplace_back([this] { serve(); });
        } catch (...) {
            stop();
            cleanup();
            throw;
        }
    }
    ~Impl() {
        stop();
        cleanup();
    }
    void cleanup() {
        if (group) {
            HttpCloseUrlGroup(group);
            group = 0;
        }
        if (queue) {
            HttpCloseRequestQueue(queue);
            queue = nullptr;
        }
        if (session) {
            HttpCloseServerSession(session);
            session = 0;
        }
    }
    void stop() {
        stopping = true;
        if (queue) {
            HttpShutdownRequestQueue(queue);
            std::lock_guard lock(mutex);
            for (auto id : requests)
                HttpCancelHttpRequest(queue, id, nullptr);
        }
        for (auto &thread : workers)
            if (thread.joinable())
                thread.join();
        workers.clear();
    }
    void serve() noexcept {
        while (!stopping && !server.stopping()) {
            alignas(HTTP_REQUEST) std::array<unsigned char, 32768> buffer{};
            auto *request = reinterpret_cast<HTTP_REQUEST *>(buffer.data());
            ULONG received = 0;
            auto code = HttpReceiveHttpRequest(queue, HTTP_NULL_ID, 0, request,
                                               static_cast<ULONG>(buffer.size()), &received, nullptr);
            if (code != NO_ERROR) {
                if (code == ERROR_MORE_DATA) {
                    HttpCancelHttpRequest(queue, request->RequestId, nullptr);
                    continue;
                }
                if (!stopping && !server.stopping() && code != ERROR_CONNECTION_INVALID &&
                    code != ERROR_OPERATION_ABORTED)
                    error = code;
                if (code == ERROR_CONNECTION_INVALID)
                    continue;
                break;
            }
            auto id = request->RequestId;
            bool acquired = false;
            try {
                {
                    std::lock_guard lock(mutex);
                    if (stopping || server.stopping()) {
                        HttpCancelHttpRequest(queue, id, nullptr);
                        break;
                    }
                    requests.insert(id);
                }
                acquired = server.acquire_connection();
                if (acquired) {
                    NativeStream stream(queue, *request, server);
                    http(stream, server);
                } else {
                    HttpCancelHttpRequest(queue, id, nullptr);
                }
            } catch (...) {
                HttpCancelHttpRequest(queue, id, nullptr);
            }
            if (acquired)
                server.release_connection();
            {
                std::lock_guard lock(mutex);
                requests.erase(id);
            }
            sodium_memzero(buffer.data(), buffer.size());
        }
    }
};
HttpSysWebmail::HttpSysWebmail(Server &server, SSL_CTX *tls) : impl_(std::make_unique<Impl>(server, tls)) {}
HttpSysWebmail::~HttpSysWebmail() = default;
void HttpSysWebmail::check() const {
    if (auto code = impl_->error.load(); code != NO_ERROR)
        failure("Fila HTTP.sys interrompida", code);
}
void HttpSysWebmail::stop() {
    impl_->stop();
}
} // namespace cm
#endif
