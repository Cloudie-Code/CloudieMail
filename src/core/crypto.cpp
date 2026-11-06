#include "cloudiemail/crypto.hpp"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <sodium.h>
#include <sstream>
#include <stdexcept>

namespace cm {
namespace {
constexpr unsigned long long kOps = 3;
constexpr std::size_t kMemory = 64 * 1024 * 1024;
void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
std::string aad(const Json &doc) {
    return Json{{"version", doc.at("version")},
                {"email", doc.at("email")},
                {"key_id", doc.at("key_id")},
                {"created_at", doc.at("created_at")},
                {"public_key", doc.at("public_key")}}
        .dump();
}
Secret derive(std::string_view password, const Bytes &salt) {
    check(password.size() <= 1024, "Senha excede o limite de 1024 bytes");
    check(salt.size() == crypto_pwhash_SALTBYTES, "Salt invalido");
    Secret key;
    check(crypto_pwhash(key.data(), 32, password.data(), password.size(), salt.data(), kOps, kMemory,
                        crypto_pwhash_ALG_ARGON2ID13) == 0,
          "Memoria insuficiente para Argon2id");
    return key;
}
void wrap(Json &doc, const Secret &private_key, std::string_view password) {
    check(password.size() >= 12 && password.size() <= 1024, "Use uma senha entre 12 e 1024 bytes");
    std::array<unsigned char, crypto_pwhash_SALTBYTES> salt{};
    std::array<unsigned char, crypto_aead_xchacha20poly1305_ietf_NPUBBYTES> nonce{};
    randombytes_buf(salt.data(), salt.size());
    randombytes_buf(nonce.data(), nonce.size());
    auto key = derive(password, Bytes(salt.begin(), salt.end()));
    auto context = aad(doc);
    Bytes cipher(32 + crypto_aead_xchacha20poly1305_ietf_ABYTES);
    unsigned long long length = 0;
    check(crypto_aead_xchacha20poly1305_ietf_encrypt(cipher.data(), &length, private_key.data(), 32,
                                                     reinterpret_cast<const unsigned char *>(context.data()),
                                                     context.size(), nullptr, nonce.data(), key.data()) == 0,
          "Falha ao proteger chave");
    doc["kdf"] = {
        {"algorithm", "argon2id13"}, {"operations", kOps}, {"memory_bytes", kMemory}, {"salt", hex(salt)}};
    doc["private_key"] = {
        {"algorithm", "xchacha20poly1305-ietf"}, {"nonce", hex(nonce)}, {"ciphertext", hex(cipher)}};
}
} // namespace
void crypto_init() {
    static std::once_flag once;
    std::call_once(once, [] { check(sodium_init() >= 0, "Falha ao iniciar libsodium"); });
}
Secret::Secret() {
    crypto_init();
    (void)sodium_mlock(value_.data(), value_.size());
}
Secret::~Secret() {
    sodium_memzero(value_.data(), value_.size());
    (void)sodium_munlock(value_.data(), value_.size());
}
Secret::Secret(Secret &&other) noexcept : Secret() {
    value_ = other.value_;
    sodium_memzero(other.value_.data(), other.value_.size());
}
Secret &Secret::operator=(Secret &&other) noexcept {
    if (this != &other) {
        sodium_memzero(value_.data(), value_.size());
        value_ = other.value_;
        sodium_memzero(other.value_.data(), other.value_.size());
    }
    return *this;
}
void wipe(std::string &value) {
    if (!value.empty())
        sodium_memzero(value.data(), value.size());
    value.clear();
}
std::string hex(std::span<const unsigned char> bytes) {
    std::string out(bytes.size() * 2 + 1, '\0');
    sodium_bin2hex(out.data(), out.size(), bytes.data(), bytes.size());
    out.pop_back();
    return out;
}
Bytes unhex(std::string_view value) {
    check(value.size() % 2 == 0 && value.size() <= 64 * 1024 * 1024, "Hex invalido");
    Bytes out(value.size() / 2);
    std::size_t length = 0;
    check(sodium_hex2bin(out.data(), out.size(), value.data(), value.size(), nullptr, &length, nullptr) == 0,
          "Hex invalido");
    out.resize(length);
    return out;
}
std::string random_id() {
    crypto_init();
    std::array<unsigned char, 16> value{};
    randombytes_buf(value.data(), value.size());
    return hex(value);
}
std::string utc_now() {
    auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &time);
#else
    gmtime_r(&time, &tm);
#endif
    std::ostringstream stream;
    stream << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return stream.str();
}
std::string mail_date() {
    auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &time);
#else
    gmtime_r(&time, &tm);
#endif
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::put_time(&tm, "%a, %d %b %Y %H:%M:%S +0000");
    return stream.str();
}
Identity create_identity(std::string_view email, std::string_view password) {
    crypto_init();
    auto at = email.find('@');
    check(at != email.npos, "Email invalido");
    Identity identity;
    std::array<unsigned char, 32> public_key{};
    check(crypto_box_keypair(public_key.data(), identity.private_key.data()) == 0, "Falha ao gerar chaves");
    auto created = utc_now();
    identity.document = {{"version", 1},
                         {"email", email},
                         {"created_at", created},
                         {"key_id", std::string(email.substr(at + 1)) + "/" + created + "/" + random_id()},
                         {"public_key", hex(public_key)},
                         {"encryption", "x25519-xsalsa20poly1305-sealedbox"}};
    wrap(identity.document, identity.private_key, password);
    return identity;
}
Secret unlock_identity(const Json &doc, std::string_view password) {
    check(doc.at("version") == 1 && doc.at("encryption") == "x25519-xsalsa20poly1305-sealedbox",
          "Formato de chave desconhecido");
    const auto &kdf = doc.at("kdf");
    check(kdf.at("algorithm") == "argon2id13" && kdf.at("operations") == kOps &&
              kdf.at("memory_bytes") == kMemory,
          "Parametros KDF invalidos");
    check(doc.at("private_key").at("algorithm") == "xchacha20poly1305-ietf", "Formato de chave desconhecido");
    auto salt = unhex(kdf.at("salt").get<std::string>());
    auto nonce = unhex(doc.at("private_key").at("nonce").get<std::string>());
    auto cipher = unhex(doc.at("private_key").at("ciphertext").get<std::string>());
    check(nonce.size() == 24 && cipher.size() == 48, "Chave protegida invalida");
    auto key = derive(password, salt);
    Secret private_key;
    auto context = aad(doc);
    unsigned long long size = 0;
    check(crypto_aead_xchacha20poly1305_ietf_decrypt(private_key.data(), &size, nullptr, cipher.data(),
                                                     cipher.size(),
                                                     reinterpret_cast<const unsigned char *>(context.data()),
                                                     context.size(), nonce.data(), key.data()) == 0,
          "Credenciais invalidas");
    std::array<unsigned char, 32> public_key{};
    crypto_scalarmult_base(public_key.data(), private_key.data());
    check(hex(public_key) == doc.at("public_key").get<std::string>(), "Par de chaves invalido");
    return private_key;
}
Json change_password(const Json &doc, std::string_view old_password, std::string_view new_password) {
    auto key = unlock_identity(doc, old_password);
    auto changed = doc;
    wrap(changed, key, new_password);
    return changed;
}
Bytes seal_message(const Json &doc, std::string_view message) {
    auto key = unhex(doc.at("public_key").get<std::string>());
    check(key.size() == 32, "Chave publica invalida");
    // The recipient context is inside the authenticated ciphertext, preventing cross-account substitution.
    auto payload = std::string("CM1\n") + doc.at("key_id").get<std::string>() + "\n" + std::string(message);
    Bytes cipher(payload.size() + crypto_box_SEALBYTES);
    auto rc = crypto_box_seal(cipher.data(), reinterpret_cast<const unsigned char *>(payload.data()),
                              payload.size(), key.data());
    wipe(payload);
    check(rc == 0, "Falha ao criptografar mensagem");
    return cipher;
}
std::string open_message(const Json &doc, const Secret &key, std::span<const unsigned char> cipher) {
    check(cipher.size() >= crypto_box_SEALBYTES, "Mensagem truncada");
    auto public_key = unhex(doc.at("public_key").get<std::string>());
    check(public_key.size() == 32, "Chave publica invalida");
    std::string payload(cipher.size() - crypto_box_SEALBYTES, '\0');
    if (crypto_box_seal_open(reinterpret_cast<unsigned char *>(payload.data()), cipher.data(), cipher.size(),
                             public_key.data(), key.data()) != 0) {
        wipe(payload);
        throw std::runtime_error("Mensagem adulterada ou chave incorreta");
    }
    auto prefix = std::string("CM1\n") + doc.at("key_id").get<std::string>() + "\n";
    if (!payload.starts_with(prefix)) {
        wipe(payload);
        throw std::runtime_error("Destinatario incorreto");
    }
    auto message = payload.substr(prefix.size());
    wipe(payload);
    return message;
}
} // namespace cm
