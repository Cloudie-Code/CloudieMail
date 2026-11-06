#include "cloudiemail/setup.hpp"
#include "cloudiemail/apache.hpp"
#include "cloudiemail/dns.hpp"
#include "cloudiemail/outbound.hpp"
#include <algorithm>
#include <fstream>
#include <memory>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <set>
#ifdef _WIN32
#include <aclapi.h>
#include <sddl.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cm {
#ifdef _WIN32
namespace {
std::runtime_error permission_error(const char *operation, const std::filesystem::path &path, DWORD code) {
    LPWSTR buffer = nullptr;
    auto count = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                    FORMAT_MESSAGE_IGNORE_INSERTS,
                                nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring message = count ? std::wstring(buffer, count) : L"Erro sem descricao";
    if (buffer)
        LocalFree(buffer);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n'))
        message.pop_back();
    auto bytes = WideCharToMultiByte(CP_UTF8, 0, message.data(), static_cast<int>(message.size()), nullptr, 0,
                                     nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(bytes), 0);
    WideCharToMultiByte(CP_UTF8, 0, message.data(), static_cast<int>(message.size()), utf8.data(), bytes,
                        nullptr, nullptr);
    std::string detail = std::string(operation) + "\nPasta: " + path_utf8(path) + "\nWindows " +
                         std::to_string(code) + ": " + utf8;
    if (code == ERROR_ACCESS_DENIED)
        detail += "\nExtraia o programa para uma pasta gravavel do seu usuario.";
    else if (code == ERROR_NOT_SUPPORTED || code == ERROR_INVALID_FUNCTION)
        detail += "\nA unidade precisa suportar permissoes Windows, como NTFS.";
    return std::runtime_error(detail);
}
} // namespace
#endif
void private_directory(const std::filesystem::path &path) {
    std::filesystem::create_directories(path);
#ifdef _WIN32
    auto native_path = std::filesystem::absolute(path).lexically_normal();
    native_path.make_preferred();
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        throw std::runtime_error("Nao foi possivel proteger a pasta de dados");
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<unsigned char> data(bytes);
    bool ok = GetTokenInformation(token, TokenUser, data.data(), bytes, &bytes) != 0;
    CloseHandle(token);
    if (!ok)
        throw std::runtime_error("Nao foi possivel consultar o usuario Windows");
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(data.data())->User.Sid, &sid))
        throw std::runtime_error("Nao foi possivel consultar SID");
    std::wstring sddl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;" + std::wstring(sid) + L")";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor,
                                                              nullptr))
        throw std::runtime_error("Nao foi possivel criar ACL");
    PACL dacl = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    if (!GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) || !present || !dacl) {
        auto error = GetLastError();
        LocalFree(descriptor);
        throw permission_error("Nao foi possivel preparar ACL da pasta", native_path,
                               error ? error : ERROR_INVALID_SECURITY_DESCR);
    }
    // Protected inheritance flags belong to SetNamedSecurityInfo, not SetFileSecurity.
    // SetNamedSecurityInfo returns its error code directly (not through GetLastError).
    auto native_name = native_path.wstring();
    auto error = SetNamedSecurityInfoW(native_name.data(), SE_FILE_OBJECT,
                                       DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                       nullptr, nullptr, dacl, nullptr);
    LocalFree(descriptor);
    if (error != ERROR_SUCCESS)
        throw permission_error("Nao foi possivel aplicar ACL da pasta", native_path, error);
#else
    if (chmod(path.c_str(), 0700) != 0)
        throw std::runtime_error("Nao foi possivel restringir permissoes da pasta");
#endif
}
void save_json(const std::filesystem::path &path, const Json &value) {
    private_directory(std::filesystem::absolute(path).parent_path());
    auto temporary = path;
    temporary += "." + random_id() + ".tmp";
    try {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file << value.dump(2) << '\n';
        file.close();
        if (!file)
            throw std::runtime_error("Falha ao gravar configuracao");
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Falha ao atualizar configuracao");
#else
        std::filesystem::permissions(temporary, std::filesystem::perms::owner_read |
                                                    std::filesystem::perms::owner_write);
        std::filesystem::rename(temporary, path);
#endif
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}
std::filesystem::path executable_directory() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(32768);
    auto size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!size || size >= buffer.size())
        throw std::runtime_error("Caminho do executavel indisponivel");
    return std::filesystem::path(std::wstring(buffer.data(), size)).parent_path();
#else
    // When started through an explicit ELF loader, /proc/self/exe is the loader.
    // AT_EXECFN contains the application path, including in the portable package.
    extern std::filesystem::path linux_executable_directory();
    return linux_executable_directory();
#endif
}
void validate_certificate(const Config &config) {
    using Context = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;
    Context ctx(SSL_CTX_new(TLS_server_method()), SSL_CTX_free);
    if (!ctx)
        throw std::runtime_error("Nao foi possivel validar TLS");
    SSL_CTX_set_default_passwd_cb(ctx.get(), +[](char *, int, int, void *) { return 0; });
    if (SSL_CTX_use_certificate_chain_file(ctx.get(), path_utf8(config.certificate).c_str()) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx.get(), path_utf8(config.private_key).c_str(), SSL_FILETYPE_PEM) !=
            1 ||
        SSL_CTX_check_private_key(ctx.get()) != 1)
        throw std::runtime_error(
            "Certificado/chave PEM invalidos ou incompativeis. Use uma chave sem senha para o servico.");
    auto *certificate = SSL_CTX_get0_certificate(ctx.get());
    if (X509_cmp_current_time(X509_get0_notBefore(certificate)) >= 0 ||
        X509_cmp_current_time(X509_get0_notAfter(certificate)) <= 0)
        throw std::runtime_error("Certificado expirado ou ainda nao valido");
    for (const auto &host : {config.mail_host(), config.webmail_host()})
        if (X509_check_host(certificate, host.c_str(), host.size(), X509_CHECK_FLAG_NEVER_CHECK_SUBJECT,
                            nullptr) != 1)
            throw std::runtime_error("Certificado precisa incluir o nome " + host + " no SAN");
    auto *key = SSL_CTX_get0_privatekey(ctx.get());
    if (EVP_PKEY_get_security_bits(key) < 112)
        throw std::runtime_error("Chave TLS insuficiente: use EC P-256 ou RSA 2048 bits ou superior");
}
Setup::Setup(std::filesystem::path path) : path_(std::filesystem::absolute(path)), state_path_(path_) {
    state_path_ += ".setup.json";
    if (std::filesystem::exists(path_))
        config = Config::load(path_);
    else {
#ifdef _WIN32
        config.https_port = 443;
        config.webmail_driver = "http_sys";
#endif
        auto root = path_.parent_path().parent_path();
        config.database = root / "data/cloudiemail.fdb";
        config.certificate = root / "data/tls.crt";
        config.private_key = root / "data/tls.key";
        config.web_root = root / "web";
    }
    if (std::filesystem::exists(state_path_)) {
        std::ifstream file(state_path_);
        file >> state_;
        if (state_.value("version", 0) != 1)
            throw std::runtime_error("Estado de configuracao inconsistente");
        if (state_.value("domain", "") != config.domain)
            state_ = Json::object();
    }
}
void Setup::save_state() {
    save_json(state_path_, state_);
}
SetupStep Setup::step() {
    if (!state_.value("configured", false))
        return SetupStep::Domain;
    if (!state_.value("dns_done", false))
        return SetupStep::Dns;
    if (!state_.value("tls_done", false) ||
        (config.webmail_driver == "apache" && !state_.value("apache_done", false)))
        return SetupStep::Certificate;
    try {
        validate_certificate(config);
    } catch (...) {
        return SetupStep::Certificate;
    }
    if (!std::filesystem::exists(config.database))
        return SetupStep::Account;
    Database db(config);
    return db.statistics().at("accounts").get<unsigned long long>() ? SetupStep::Ready : SetupStep::Account;
}
void Setup::configure(const std::string &domain, const std::string &ipv4, const std::string &bind,
                      const std::vector<unsigned short> &ports, const std::string &webmail_driver) {
    auto next = config;
    next.domain = domain_name(domain);
    (void)dns_plan(next.domain, ipv4);
    (void)dns_plan(next.domain, bind);
    if ((ports.size() != 5 && ports.size() != 6) ||
        std::set<unsigned short>(ports.begin(), ports.end()).size() != ports.size() ||
        std::find(ports.begin(), ports.end(), 0) != ports.end())
        throw std::runtime_error("Escolha portas distintas entre 1 e 65535");
    if (std::filesystem::exists(config.database) && next.domain != config.domain) {
        Database db(config);
        if (db.statistics().at("accounts").get<unsigned long long>())
            throw std::runtime_error("Nao e possivel trocar o dominio de um banco que ja tem contas");
    }
    next.bind = bind;
    next.smtp_port = ports[0];
    next.smtps_port = ports[1];
    next.imap_port = ports[2];
    next.pop3_port = ports[3];
    next.https_port = ports[4];
    if (ports.size() == 6)
        next.submission_port = ports[5];
    if (!webmail_driver.empty()) {
        if (webmail_driver != "socket" && webmail_driver != "http_sys" && webmail_driver != "apache")
            throw std::runtime_error("Transporte webmail invalido");
#ifndef _WIN32
        if (webmail_driver == "http_sys")
            throw std::runtime_error("HTTP.sys requer Windows");
#endif
        next.webmail_driver = webmail_driver;
    }
    if (next.webmail_driver == "apache")
        next.https_port = 443;
    next.save(path_);
    prepare_mail_keys(next);
    bool same_domain = next.domain == config.domain;
    bool same_dns = same_domain && ipv4 == this->ipv4() && state_.value("dns_done", false);
    config = next;
    state_ = {{"version", 1},
              {"configured", true},
              {"domain", config.domain},
              {"ipv4", ipv4},
              {"dns_done", same_dns},
              {"tls_done", same_domain && state_.value("tls_done", false)},
              {"apache_done", false},
              {"apache_domain", state_.value("apache_domain", std::string())},
              {"contact", contact()},
              {"dns_mode", dns_mode()}};
    save_state();
}
std::string Setup::dns_records() const {
    auto plan = dns_plan(config.domain, ipv4());
    if (std::filesystem::exists(config.database.parent_path() / "dkim.json"))
        plan.push_back({{"type", "TXT"},
                        {"name", "cloudiemail._domainkey." + config.domain},
                        {"content", dkim_record(config)},
                        {"ttl", 300}});
    return dns_zone_text(plan);
}
void Setup::dns_manual_confirmed() {
    if (!state_.value("configured", false))
        throw std::runtime_error("Configure o dominio primeiro");
    state_["dns_mode"] = "manual";
    state_["dns_done"] = true;
    save_state();
}
void Setup::dns_cloudflare(const std::string &token) {
    if (!state_.value("configured", false))
        throw std::runtime_error("Configure o dominio primeiro");
    prepare_mail_keys(config);
    auto plan = dns_plan(config.domain, ipv4());
    plan.push_back({{"type", "TXT"},
                    {"name", "cloudiemail._domainkey." + config.domain},
                    {"content", dkim_record(config)},
                    {"ttl", 300}});
    apply_cloudflare(cloudflare_zone(config.domain, token), token, plan);
    state_["dns_mode"] = "cloudflare";
    state_["dns_done"] = true;
    save_state();
}
void Setup::import_certificate(const std::filesystem::path &certificate, const std::filesystem::path &key) {
    if (!state_.value("dns_done", false))
        throw std::runtime_error("Conclua o DNS primeiro");
    Config candidate = config;
    candidate.certificate = certificate;
    candidate.private_key = key;
    validate_certificate(candidate);
    auto directory = config.database.parent_path() / "certificates";
    private_directory(config.database.parent_path());
    private_directory(directory);
    auto id = random_id();
    auto cert_target = directory / (id + ".crt"), key_target = directory / (id + ".key");
    std::filesystem::copy_file(certificate, cert_target);
    try {
        std::filesystem::copy_file(key, key_target);
#ifndef _WIN32
        std::filesystem::permissions(key_target, std::filesystem::perms::owner_read |
                                                     std::filesystem::perms::owner_write);
#endif
        candidate.certificate = cert_target;
        candidate.private_key = key_target;
        candidate.save(path_);
    } catch (...) {
        std::filesystem::remove(cert_target);
        std::filesystem::remove(key_target);
        throw;
    }
    config = candidate;
    state_["tls_done"] = true;
    state_["apache_done"] = false;
    save_state();
}
void Setup::configure_webhost(const Progress &output, std::atomic_bool &cancel, std::atomic_uint &input) {
#ifndef _WIN32
    auto installed = state_.value("apache_domain", std::string());
    if (!installed.empty() && (config.webmail_driver != "apache" || installed != config.domain)) {
        auto old = config;
        old.domain = installed;
        auto path = path_;
        path += ".apache-remove.json";
        old.save(path);
        try {
            configure_apache(path, true, output, cancel, input);
        } catch (...) {
            std::filesystem::remove(path);
            throw;
        }
        std::filesystem::remove(path);
        state_["apache_domain"] = "";
        state_["apache_done"] = false;
        save_state();
    }
    if (config.webmail_driver == "apache" && state_.value("tls_done", false)) {
        validate_certificate(config);
        configure_apache(path_, false, output, cancel, input);
        state_["apache_domain"] = config.domain;
        state_["apache_done"] = true;
        save_state();
    }
#else
    (void)output;
    (void)cancel;
    (void)input;
#endif
}
void Setup::issue_certificate(const std::string &contact_value, const std::string &provider,
                              const std::string &token, const Progress &output, std::atomic_bool &cancel,
                              std::atomic_uint &continue_input, bool force) {
    if (!state_.value("dns_done", false))
        throw std::runtime_error("Conclua o DNS primeiro");
    auto email = email_address(contact_value);
    if (provider != "manual" && provider != "cloudflare")
        throw std::runtime_error("Provedor ACME invalido");
    if (provider == "cloudflare" && (token.empty() || token.find_first_of("\r\n") != token.npos))
        throw std::runtime_error("Informe o token Cloudflare");
    auto directory = config.database.parent_path() / "acme";
    private_directory(config.database.parent_path());
    private_directory(directory);
    auto binary = executable_directory() /
#ifdef _WIN32
                  "lego.exe";
#else
                  "lego";
#endif
    if (!std::filesystem::is_regular_file(binary))
        throw std::runtime_error("Cliente ACME ausente no pacote. Refaça o empacotamento.");
    state_["contact"] = email;
    save_state();
    std::vector<std::string> args = {"run",
                                     "--accept-tos",
                                     "--email",
                                     email,
                                     "--dns",
                                     provider,
                                     "--domains",
                                     config.domain,
                                     "--domains",
                                     config.mail_host(),
                                     "--domains",
                                     config.webmail_host(),
                                     "--path",
                                     path_utf8(directory),
                                     "--cert.name",
                                     config.domain,
                                     "--server",
                                     "letsencrypt",
                                     "--key-type",
                                     "EC256",
                                     "--http-timeout",
                                     "30",
                                     "--log.format",
                                     "text"};
    if (force)
        args.push_back("--renew-force");
    std::map<std::string, std::string> environment;
    if (provider == "cloudflare")
        environment["CF_DNS_API_TOKEN"] = token;
    int status = run_child(binary, args, environment, output, cancel, continue_input);
    if (cancel)
        throw std::runtime_error("Emissao cancelada. Revise os registros TXT apresentados.");
    if (status != 0)
        throw std::runtime_error("A emissao ACME falhou. Consulte as mensagens acima e tente novamente.");
    auto cert_directory = directory / "certificates";
    import_certificate(cert_directory / (config.domain + ".crt"), cert_directory / (config.domain + ".key"));
}
void Setup::first_account(const std::string &address, std::string password, std::string confirmation) {
    struct Wipe {
        std::string &a, &b;
        ~Wipe() {
            wipe(a);
            wipe(b);
        }
    } wipe_passwords{password, confirmation};
    if (step() != SetupStep::Account)
        throw std::runtime_error("Conclua dominio, DNS e certificado antes da primeira conta");
    auto email = email_address(address);
    if (!email.ends_with("@" + config.domain))
        throw std::runtime_error("Use o dominio configurado");
    if (password != confirmation)
        throw std::runtime_error("As senhas nao conferem");
    auto identity = create_identity(email, password);
    private_directory(config.database.parent_path());
    Database db(config, !std::filesystem::exists(config.database));
    if (db.statistics().at("accounts").get<unsigned long long>())
        throw std::runtime_error("A primeira conta ja existe");
    db.add_account(identity.document);
}
} // namespace cm
