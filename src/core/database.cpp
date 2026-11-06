#include "cloudiemail/database.hpp"
#include <cstdlib>
#include <cstring>
#include <ibase.h>
#include <limits>
#include <stdexcept>

namespace cm {
namespace {
using Rows = std::vector<std::vector<std::string>>;
struct Param {
    std::string value;
    bool blob = false;
    Param(std::string s, bool b = false) : value(std::move(s)), blob(b) {}
};
void checked(ISC_STATUS rc, const ISC_STATUS *status) {
    if (!rc)
        return;
    char buffer[512]{};
    const ISC_STATUS *cursor = status;
    std::string message = "Firebird: ";
    while (fb_interpret(buffer, sizeof(buffer), &cursor) > 0) {
        message += buffer;
        message += ' ';
    }
    throw std::runtime_error(message);
}
struct Descriptor {
    std::unique_ptr<char[]> storage;
    XSQLDA *value;
    explicit Descriptor(short count)
        : storage(new char[static_cast<std::size_t>(XSQLDA_LENGTH(count))]{}),
          value(reinterpret_cast<XSQLDA *>(storage.get())) {
        value->version = SQLDA_VERSION1;
        value->sqln = count;
    }
};
} // namespace
struct Database::Impl {
    isc_db_handle db = 0;
    ~Impl() {
        if (db) {
            ISC_STATUS s[20]{};
            isc_detach_database(s, &db);
        }
    }
    struct Transaction {
        Impl &owner;
        isc_tr_handle handle = 0;
        explicit Transaction(Impl &o) : owner(o) {
            ISC_STATUS s[20]{};
            const char tpb[] = {isc_tpb_version3, isc_tpb_write, isc_tpb_read_committed, isc_tpb_rec_version,
                                isc_tpb_nowait};
            checked(isc_start_transaction(s, &handle, 1, &owner.db, static_cast<int>(sizeof(tpb)), tpb), s);
        }
        ~Transaction() {
            if (handle) {
                ISC_STATUS s[20]{};
                isc_rollback_transaction(s, &handle);
            }
        }
        void commit() {
            ISC_STATUS s[20]{};
            checked(isc_commit_transaction(s, &handle), s);
        }
    };
    struct Statement {
        isc_stmt_handle handle = 0;
        ~Statement() {
            if (handle) {
                ISC_STATUS s[20]{};
                isc_dsql_free_statement(s, &handle, DSQL_drop);
            }
        }
    };
    ISC_QUAD write_blob(Transaction &tx, const std::string &content) {
        isc_blob_handle blob = 0;
        ISC_QUAD id{};
        ISC_STATUS s[20]{};
        checked(isc_create_blob2(s, &db, &tx.handle, &blob, &id, 0, nullptr), s);
        try {
            for (std::size_t offset = 0; offset < content.size();) {
                auto n = static_cast<unsigned short>(std::min<std::size_t>(32768, content.size() - offset));
                checked(isc_put_segment(s, &blob, n, content.data() + offset), s);
                offset += n;
            }
            checked(isc_close_blob(s, &blob), s);
        } catch (...) {
            isc_cancel_blob(s, &blob);
            throw;
        }
        return id;
    }
    std::string read_blob(Transaction &tx, ISC_QUAD id) {
        isc_blob_handle blob = 0;
        ISC_STATUS s[20]{};
        checked(isc_open_blob2(s, &db, &tx.handle, &blob, &id, 0, nullptr), s);
        std::string result;
        try {
            for (;;) {
                char buffer[32768];
                unsigned short length = 0;
                auto rc = isc_get_segment(s, &blob, &length, sizeof(buffer), buffer);
                if (rc && rc != isc_segment && rc != isc_segstr_eof)
                    checked(rc, s);
                result.append(buffer, length);
                if (result.size() > 26 * 1024 * 1024)
                    throw std::runtime_error("BLOB excede o limite");
                if (rc == isc_segstr_eof)
                    break;
            }
            checked(isc_close_blob(s, &blob), s);
        } catch (...) {
            isc_close_blob(s, &blob);
            throw;
        }
        return result;
    }
    Rows query(Transaction &tx, const std::string &sql, const std::vector<Param> &params = {}) {
        ISC_STATUS s[20]{};
        Statement st;
        checked(isc_dsql_allocate_statement(s, &db, &st.handle), s);
        Descriptor desc(32);
        checked(isc_dsql_prepare(s, &tx.handle, &st.handle, 0, sql.c_str(), 3, desc.value), s);
        if (desc.value->sqld > 32)
            throw std::runtime_error("Muitas colunas");
        Descriptor inputs(static_cast<short>(params.size()));
        inputs.value->sqld = static_cast<short>(params.size());
        std::vector<ISC_QUAD> blobs(params.size());
        std::vector<short> nulls(params.size(), 0);
        for (std::size_t i = 0; i < params.size(); ++i) {
            auto &v = inputs.value->sqlvar[i];
            v.sqlind = &nulls[i];
            if (params[i].blob) {
                blobs[i] = write_blob(tx, params[i].value);
                v.sqltype = SQL_BLOB;
                v.sqllen = sizeof(ISC_QUAD);
                v.sqldata = reinterpret_cast<char *>(&blobs[i]);
            } else {
                if (params[i].value.size() > 32760)
                    throw std::runtime_error("Parametro muito longo");
                v.sqltype = SQL_TEXT;
                v.sqllen = static_cast<short>(params[i].value.size());
                v.sqldata = const_cast<char *>(params[i].value.data());
                v.sqlsubtype = 0;
            }
        }
        checked(isc_dsql_execute(s, &tx.handle, &st.handle, 3, params.empty() ? nullptr : inputs.value), s);
        Rows rows;
        if (desc.value->sqld) {
            std::vector<std::vector<char>> buffers(static_cast<std::size_t>(desc.value->sqld));
            std::vector<short> indicators(buffers.size());
            for (std::size_t i = 0; i < buffers.size(); ++i) {
                auto &v = desc.value->sqlvar[i];
                buffers[i].resize(static_cast<std::size_t>(v.sqllen) + 2);
                v.sqldata = buffers[i].data();
                v.sqlind = &indicators[i];
            }
            for (;;) {
                auto rc = isc_dsql_fetch(s, &st.handle, 3, desc.value);
                if (rc == 100)
                    break;
                checked(rc, s);
                std::vector<std::string> row;
                for (std::size_t i = 0; i < buffers.size(); ++i) {
                    auto &v = desc.value->sqlvar[i];
                    if (indicators[i] < 0) {
                        row.emplace_back();
                        continue;
                    }
                    switch (v.sqltype & ~1) {
                    case SQL_VARYING: {
                        unsigned short n = 0;
                        std::memcpy(&n, v.sqldata, 2);
                        row.emplace_back(v.sqldata + 2, n);
                        break;
                    }
                    case SQL_TEXT:
                        row.emplace_back(v.sqldata, static_cast<std::size_t>(v.sqllen));
                        break;
                    case SQL_SHORT: {
                        short n = 0;
                        std::memcpy(&n, v.sqldata, 2);
                        row.push_back(std::to_string(n));
                        break;
                    }
                    case SQL_LONG: {
                        std::int32_t n = 0;
                        std::memcpy(&n, v.sqldata, 4);
                        row.push_back(std::to_string(n));
                        break;
                    }
                    case SQL_INT64: {
                        std::int64_t n = 0;
                        std::memcpy(&n, v.sqldata, 8);
                        row.push_back(std::to_string(n));
                        break;
                    }
                    case SQL_BLOB: {
                        ISC_QUAD id{};
                        std::memcpy(&id, v.sqldata, sizeof(id));
                        row.push_back(read_blob(tx, id));
                        break;
                    }
                    default:
                        throw std::runtime_error("Tipo SQL nao suportado");
                    }
                }
                rows.push_back(std::move(row));
                if (rows.size() > 10000)
                    throw std::runtime_error("Limite de resultados excedido");
            }
        }
        return rows;
    }
};
Database::Database(const Config &config, bool create) : impl_(std::make_unique<Impl>()) {
    static std::once_flag shutdown_hook;
    std::call_once(shutdown_hook, [] { std::atexit([] { fb_shutdown(5000, fb_shutrsn_app_stopped); }); });
    auto path = path_utf8(config.database);
    std::string dpb(1, isc_dpb_version1);
    auto add = [&](char tag, const std::string &value) {
        if (value.size() > 255)
            throw std::runtime_error("Parametro Firebird muito longo");
        dpb += tag;
        dpb += static_cast<char>(value.size());
        dpb += value;
    };
    add(isc_dpb_user_name, config.database_user);
    if (const char *pass = std::getenv(config.database_password_env.c_str()))
        add(isc_dpb_password, pass);
    add(isc_dpb_lc_ctype, "UTF8");
    add(isc_dpb_utf8_filename, "");
    if (create)
        add(isc_dpb_sql_dialect, std::string({3, 0, 0, 0}));
    ISC_STATUS status[20]{};
    if (create) {
        if (std::filesystem::exists(config.database))
            throw std::runtime_error("Banco ja existe");
        bool created_directory = std::filesystem::create_directories(config.database.parent_path());
#ifndef _WIN32
        if (created_directory)
            std::filesystem::permissions(config.database.parent_path(), std::filesystem::perms::owner_all);
#else
        (void)created_directory;
#endif
        checked(isc_create_database(status, 0, path.c_str(), &impl_->db, static_cast<short>(dpb.size()),
                                    dpb.data(), 0),
                status);
#ifndef _WIN32
        std::filesystem::permissions(config.database, std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
#endif
        try {
            for (const auto *sql :
                 {"CREATE TABLE CM_SCHEMA (VERSION INTEGER NOT NULL PRIMARY KEY)", "CREATE SEQUENCE CM_UID",
                  "CREATE TABLE CM_ACCOUNTS (EMAIL VARCHAR(254) CHARACTER SET ASCII NOT NULL PRIMARY KEY, "
                  "KEY_DOC BLOB SUB_TYPE BINARY NOT NULL)",
                  "CREATE TABLE CM_MESSAGES (UID BIGINT NOT NULL PRIMARY KEY, EMAIL VARCHAR(254) CHARACTER "
                  "SET "
                  "ASCII NOT NULL REFERENCES CM_ACCOUNTS(EMAIL), OCTETS INTEGER NOT NULL, SEEN SMALLINT "
                  "DEFAULT "
                  "0 NOT NULL, PAYLOAD BLOB SUB_TYPE BINARY NOT NULL, RECEIVED_AT TIMESTAMP DEFAULT "
                  "CURRENT_TIMESTAMP NOT NULL)",
                  "CREATE INDEX CM_MESSAGES_OWNER ON CM_MESSAGES(EMAIL, UID)",
                  "INSERT INTO CM_SCHEMA (VERSION) VALUES (1)"}) {
                // Firebird metadata must be committed before dependent statements are prepared.
                Impl::Transaction tx(*impl_);
                impl_->query(tx, sql);
                tx.commit();
            }
        } catch (...) {
            ISC_STATUS cleanup[20]{};
            isc_drop_database(cleanup, &impl_->db);
            throw;
        }
    } else {
        checked(isc_attach_database(status, 0, path.c_str(), &impl_->db, static_cast<short>(dpb.size()),
                                    dpb.data()),
                status);
        Impl::Transaction tx(*impl_);
        auto rows = impl_->query(tx, "SELECT VERSION FROM CM_SCHEMA");
        if (rows.size() != 1 || (rows[0][0] != "1" && rows[0][0] != "2"))
            throw std::runtime_error("Versao de banco incompatível");
        tx.commit();
    }
    // Metadata and the version marker commit together; existing payloads are never rewritten.
    {
        Impl::Transaction tx(*impl_);
        auto version = impl_->query(tx, "SELECT VERSION FROM CM_SCHEMA WITH LOCK");
        if (version.at(0).at(0) == "1") {
            for (const auto *sql :
                 {"ALTER TABLE CM_MESSAGES ADD FOLDER VARCHAR(128) CHARACTER SET UTF8 DEFAULT 'INBOX' NOT "
                  "NULL",
                  "ALTER TABLE CM_MESSAGES ADD LABELS VARCHAR(2048) CHARACTER SET UTF8 DEFAULT '[]' NOT NULL",
                  "ALTER TABLE CM_MESSAGES ADD FLAGGED SMALLINT DEFAULT 0 NOT NULL",
                  "CREATE TABLE CM_FOLDERS (EMAIL VARCHAR(254) CHARACTER SET ASCII NOT NULL REFERENCES "
                  "CM_ACCOUNTS(EMAIL), NAME VARCHAR(128) CHARACTER SET UTF8 NOT NULL, PRIMARY KEY (EMAIL, "
                  "NAME))",
                  "CREATE TABLE CM_OUTBOUND (ID VARCHAR(64) CHARACTER SET ASCII NOT NULL PRIMARY KEY, SENDER "
                  "VARCHAR(254) CHARACTER SET ASCII NOT NULL REFERENCES CM_ACCOUNTS(EMAIL), RECIPIENT "
                  "VARCHAR(254) CHARACTER SET ASCII NOT NULL, STATUS VARCHAR(16) CHARACTER SET ASCII NOT "
                  "NULL, ATTEMPTS INTEGER NOT NULL, CREATED BIGINT NOT NULL, NEXT_ATTEMPT BIGINT NOT NULL, "
                  "LAST_ERROR VARCHAR(512) CHARACTER SET UTF8 NOT NULL, PAYLOAD BLOB SUB_TYPE BINARY NOT "
                  "NULL)",
                  "CREATE INDEX CM_OUTBOUND_DUE ON CM_OUTBOUND(STATUS, NEXT_ATTEMPT)",
                  "UPDATE CM_SCHEMA SET VERSION = 2"})
                impl_->query(tx, sql);
        }
        tx.commit();
    }
    wipe(dpb);
}
Database::~Database() = default;
void Database::add_account(const Json &doc) {
    std::lock_guard lock(mutex_);
    Impl::Transaction tx(*impl_);
    impl_->query(tx, "INSERT INTO CM_ACCOUNTS (EMAIL, KEY_DOC) VALUES (?, ?)",
                 {Param(email_address(doc.at("email"))), Param(doc.dump(), true)});
    tx.commit();
}
std::optional<Json> Database::account(const std::string &email) {
    std::lock_guard lock(mutex_);
    Impl::Transaction tx(*impl_);
    auto rows = impl_->query(tx, "SELECT KEY_DOC FROM CM_ACCOUNTS WHERE EMAIL = ?", {Param(email)});
    tx.commit();
    if (rows.empty())
        return {};
    auto doc = Json::parse(rows[0][0]);
    if (doc.at("email") != email)
        throw std::runtime_error("Identidade inconsistente");
    return doc;
}
void Database::replace_account(const std::string &email, const Json &expected, const Json &replacement) {
    std::lock_guard lock(mutex_);
    Impl::Transaction tx(*impl_);
    auto rows = impl_->query(tx, "SELECT KEY_DOC FROM CM_ACCOUNTS WHERE EMAIL = ? WITH LOCK", {Param(email)});
    if (rows.empty() || Json::parse(rows[0][0]) != expected)
        throw std::runtime_error("Conta alterada simultaneamente");
    impl_->query(tx, "UPDATE CM_ACCOUNTS SET KEY_DOC = ? WHERE EMAIL = ?",
                 {Param(replacement.dump(), true), Param(email)});
    tx.commit();
}
void Database::deliver(const std::vector<Delivery> &deliveries, std::size_t quota,
                       const std::vector<QueuedMail> &outgoing) {
    std::lock_guard lock(mutex_);
    Impl::Transaction tx(*impl_);
    for (const auto &d : deliveries) {
        auto owner =
            impl_->query(tx, "SELECT EMAIL FROM CM_ACCOUNTS WHERE EMAIL = ? WITH LOCK", {Param(d.email)});
        if (owner.empty())
            throw std::runtime_error("Destinatario inexistente");
        auto total =
            impl_->query(tx, "SELECT COALESCE(SUM(OCTETS),0), COUNT(*) FROM CM_MESSAGES WHERE EMAIL = ?",
                         {Param(d.email)});
        if (std::stoull(total[0][0]) + d.size > quota || std::stoull(total[0][1]) >= 1000)
            throw std::runtime_error("Quota da caixa excedida");
        impl_->query(
            tx,
            "INSERT INTO CM_MESSAGES (UID, EMAIL, OCTETS, PAYLOAD, FOLDER) VALUES (NEXT VALUE FOR CM_UID, ?, "
            "?, ?, ?)",
            {Param(d.email), Param(std::to_string(d.size)),
             Param(std::string(reinterpret_cast<const char *>(d.ciphertext.data()), d.ciphertext.size()),
                   true),
             Param(d.folder)});
    }
    for (const auto &mail : outgoing) {
        impl_->query(tx, "SELECT EMAIL FROM CM_ACCOUNTS WHERE EMAIL = ? WITH LOCK", {Param(mail.sender)});
        auto usage =
            impl_->query(tx, "SELECT COUNT(*) FROM CM_OUTBOUND WHERE SENDER = ? AND STATUS = 'pending'",
                         {Param(mail.sender)});
        if (std::stoull(usage[0][0]) >= 100)
            throw std::runtime_error("Outgoing queue quota exceeded");
        impl_->query(tx,
                     "INSERT INTO CM_OUTBOUND "
                     "(ID,SENDER,RECIPIENT,STATUS,ATTEMPTS,CREATED,NEXT_ATTEMPT,LAST_ERROR,PAYLOAD) VALUES "
                     "(?,?,?,'pending',0,?,?,'',?)",
                     {Param(mail.id), Param(mail.sender), Param(mail.recipient),
                      Param(std::to_string(mail.created)), Param(std::to_string(mail.next_attempt)),
                      Param(std::string(mail.ciphertext.begin(), mail.ciphertext.end()), true)});
    }
    tx.commit();
}
std::vector<MessageInfo> Database::messages(const std::string &email, const std::string &folder) {
    std::lock_guard lock(mutex_);
    Impl::Transaction tx(*impl_);
    auto rows = folder.empty() ? impl_->query(tx,
                                              "SELECT UID,OCTETS,SEEN,FOLDER,LABELS,FLAGGED FROM CM_MESSAGES "
                                              "WHERE EMAIL = ? ORDER BY UID",
                                              {Param(email)})
                               : impl_->query(tx,
                                              "SELECT UID,OCTETS,SEEN,FOLDER,LABELS,FLAGGED FROM CM_MESSAGES "
                                              "WHERE EMAIL = ? AND FOLDER = ? ORDER BY UID",
                                              {Param(email), Param(folder)});
    std::vector<MessageInfo> result;
    for (const auto &row : rows)
        result.push_back({std::stoull(row[0]), static_cast<std::size_t>(std::stoull(row[1])), row[2] == "1",
                          row[3], Json::parse(row[4]), row[5] == "1"});
    tx.commit();
    return result;
}
Bytes Database::message(const std::string &email, std::uint64_t uid) {
    std::lock_guard lock(mutex_);
    Impl::Transaction tx(*impl_);
    auto rows = impl_->query(tx, "SELECT PAYLOAD FROM CM_MESSAGES WHERE EMAIL = ? AND UID = ?",
                             {Param(email), Param(std::to_string(uid))});
    tx.commit();
    if (rows.empty())
        throw std::runtime_error("Mensagem inexistente");
    return Bytes(rows[0][0].begin(), rows[0][0].end());
}
void Database::set_seen(const std::string &email, std::uint64_t uid, bool seen) {
    std::lock_guard lock(mutex_);
    Impl::Transaction tx(*impl_);
    impl_->query(tx, "UPDATE CM_MESSAGES SET SEEN = ? WHERE EMAIL = ? AND UID = ?",
                 {Param(seen ? "1" : "0"), Param(email), Param(std::to_string(uid))});
    tx.commit();
}
void Database::erase(const std::string &email, const std::vector<std::uint64_t> &uids) {
    std::lock_guard lock(mutex_);
    Impl::Transaction tx(*impl_);
    for (auto uid : uids)
        impl_->query(tx, "DELETE FROM CM_MESSAGES WHERE EMAIL = ? AND UID = ?",
                     {Param(email), Param(std::to_string(uid))});
    tx.commit();
}
std::uint64_t Database::uid_next() {
    std::lock_guard lock(mutex_);
    Impl::Transaction tx(*impl_);
    auto rows = impl_->query(tx, "SELECT GEN_ID(CM_UID, 0) + 1 FROM RDB$DATABASE");
    tx.commit();
    return std::stoull(rows[0][0]);
}
Json Database::statistics() {
    std::lock_guard lock(mutex_);
    Impl::Transaction tx(*impl_);
    auto a = impl_->query(tx, "SELECT COUNT(*) FROM CM_ACCOUNTS");
    auto m = impl_->query(tx, "SELECT COUNT(*), COALESCE(SUM(OCTETS),0) FROM CM_MESSAGES");
    tx.commit();
    return {{"accounts", std::stoull(a[0][0])},
            {"messages", std::stoull(m[0][0])},
            {"message_bytes", std::stoull(m[0][1])}};
}

namespace {
bool builtin_folder(const std::string &name) {
    return name == "INBOX" || name == "Sent" || name == "Drafts" || name == "Trash" || name == "Junk";
}
void valid_name(const std::string &name, std::size_t limit = 128) {
    if (name.empty() || name.size() > limit || name.front() == ' ' || name.back() == ' ')
        throw std::runtime_error("Invalid folder or label name");
    for (unsigned char c : name)
        if (c < 32 || c == 127 || c == '/' || c == '\\')
            throw std::runtime_error("Invalid folder or label name");
    (void)Json(name).dump(); // Reject invalid UTF-8 before passing it to Firebird.
}
} // namespace
Json Database::folders(const std::string &email) {
    std::lock_guard lock(mutex_);
    Impl::Transaction tx(*impl_);
    Json result = Json::array();
    std::vector<std::string> names{"INBOX", "Sent", "Drafts", "Trash", "Junk"};
    for (const auto &row :
         impl_->query(tx, "SELECT NAME FROM CM_FOLDERS WHERE EMAIL = ? ORDER BY NAME", {Param(email)}))
        names.push_back(row[0]);
    for (const auto &name : names) {
        auto count = impl_->query(tx,
                                  "SELECT COUNT(*), COALESCE(SUM(CASE WHEN SEEN = 0 THEN 1 ELSE 0 END),0) "
                                  "FROM CM_MESSAGES WHERE EMAIL = ? AND FOLDER = ?",
                                  {Param(email), Param(name)});
        result.push_back({{"name", name},
                          {"system", builtin_folder(name)},
                          {"total", std::stoull(count[0][0])},
                          {"unread", std::stoull(count[0][1])}});
    }
    tx.commit();
    return result;
}
void Database::create_folder(const std::string &email, const std::string &name) {
    valid_name(name);
    if (builtin_folder(name))
        throw std::runtime_error("System folder already exists");
    std::lock_guard lock(mutex_);
    Impl::Transaction tx(*impl_);
    impl_->query(tx, "SELECT EMAIL FROM CM_ACCOUNTS WHERE EMAIL = ? WITH LOCK", {Param(email)});
    if (std::stoull(
            impl_->query(tx, "SELECT COUNT(*) FROM CM_FOLDERS WHERE EMAIL = ?", {Param(email)})[0][0]) >= 64)
        throw std::runtime_error("Folder limit exceeded");
    impl_->query(tx, "INSERT INTO CM_FOLDERS (EMAIL, NAME) VALUES (?,?)", {Param(email), Param(name)});
    tx.commit();
}
void Database::rename_folder(const std::string &email, const std::string &name,
                             const std::string &replacement) {
    valid_name(replacement);
    if (builtin_folder(name) || builtin_folder(replacement))
        throw std::runtime_error("System folder is protected");
    std::lock_guard lock(mutex_);
    Impl::Transaction tx(*impl_);
    if (impl_
            ->query(tx, "SELECT NAME FROM CM_FOLDERS WHERE EMAIL = ? AND NAME = ? WITH LOCK",
                    {Param(email), Param(name)})
            .empty())
        throw std::runtime_error("Folder not found");
    impl_->query(tx, "UPDATE CM_FOLDERS SET NAME = ? WHERE EMAIL = ? AND NAME = ?",
                 {Param(replacement), Param(email), Param(name)});
    impl_->query(tx, "UPDATE CM_MESSAGES SET FOLDER = ? WHERE EMAIL = ? AND FOLDER = ?",
                 {Param(replacement), Param(email), Param(name)});
    tx.commit();
}
void Database::remove_folder(const std::string &email, const std::string &name) {
    if (builtin_folder(name))
        throw std::runtime_error("System folder is protected");
    std::lock_guard lock(mutex_);
    Impl::Transaction tx(*impl_);
    if (std::stoull(impl_->query(tx, "SELECT COUNT(*) FROM CM_MESSAGES WHERE EMAIL = ? AND FOLDER = ?",
                                 {Param(email), Param(name)})[0][0]))
        throw std::runtime_error("Folder must be empty");
    impl_->query(tx, "DELETE FROM CM_FOLDERS WHERE EMAIL = ? AND NAME = ?", {Param(email), Param(name)});
    tx.commit();
}
void Database::organize(const std::string &email, std::uint64_t uid, const std::string &folder,
                        const Json &labels, bool flagged) {
    valid_name(folder);
    if (!labels.is_array() || labels.size() > 10)
        throw std::runtime_error("Invalid labels");
    for (const auto &label : labels)
        valid_name(label.get<std::string>(), 64);
    std::lock_guard lock(mutex_);
    Impl::Transaction tx(*impl_);
    if (!builtin_folder(folder) && impl_
                                       ->query(tx, "SELECT NAME FROM CM_FOLDERS WHERE EMAIL = ? AND NAME = ?",
                                               {Param(email), Param(folder)})
                                       .empty())
        throw std::runtime_error("Folder not found");
    if (impl_
            ->query(tx, "SELECT UID FROM CM_MESSAGES WHERE EMAIL = ? AND UID = ? WITH LOCK",
                    {Param(email), Param(std::to_string(uid))})
            .empty())
        throw std::runtime_error("Message not found");
    impl_->query(tx, "UPDATE CM_MESSAGES SET FOLDER = ?, LABELS = ?, FLAGGED = ? WHERE EMAIL = ? AND UID = ?",
                 {Param(folder), Param(labels.dump()), Param(flagged ? "1" : "0"), Param(email),
                  Param(std::to_string(uid))});
    tx.commit();
}
std::vector<QueuedMail> Database::due_mail(std::int64_t now) {
    std::lock_guard lock(mutex_);
    Impl::Transaction tx(*impl_);
    auto rows =
        impl_->query(tx,
                     "SELECT FIRST 1 ID,SENDER,RECIPIENT,ATTEMPTS,CREATED,NEXT_ATTEMPT,PAYLOAD FROM "
                     "CM_OUTBOUND WHERE STATUS = 'pending' AND NEXT_ATTEMPT <= ? ORDER BY NEXT_ATTEMPT",
                     {Param(std::to_string(now))});
    std::vector<QueuedMail> result;
    for (const auto &r : rows) {
        QueuedMail mail;
        mail.id = r[0];
        mail.sender = r[1];
        mail.recipient = r[2];
        mail.attempts = static_cast<unsigned>(std::stoul(r[3]));
        mail.created = std::stoll(r[4]);
        mail.next_attempt = std::stoll(r[5]);
        mail.ciphertext = Bytes(r[6].begin(), r[6].end());
        result.push_back(std::move(mail));
    }
    tx.commit();
    return result;
}
void Database::update_delivery(const QueuedMail &mail) {
    if (mail.status != "pending" && mail.status != "sent" && mail.status != "failed")
        throw std::runtime_error("Invalid queue state");
    std::lock_guard lock(mutex_);
    Impl::Transaction tx(*impl_);
    impl_->query(tx,
                 "UPDATE CM_OUTBOUND SET STATUS = ?, ATTEMPTS = ?, NEXT_ATTEMPT = ?, LAST_ERROR = ? WHERE ID "
                 "= ? AND SENDER = ? AND STATUS = 'pending'",
                 {Param(mail.status), Param(std::to_string(mail.attempts)),
                  Param(std::to_string(mail.next_attempt)), Param(mail.error.substr(0, 500)), Param(mail.id),
                  Param(mail.sender)});
    if (mail.status != "pending")
        impl_->query(tx, "UPDATE CM_OUTBOUND SET PAYLOAD = ? WHERE ID = ? AND SENDER = ?",
                     {Param("", true), Param(mail.id), Param(mail.sender)});
    // Keep terminal delivery metadata for 30 days, without message content.
    impl_->query(tx, "DELETE FROM CM_OUTBOUND WHERE STATUS <> 'pending' AND CREATED < ?",
                 {Param(std::to_string(mail.next_attempt - 30LL * 86400))});
    tx.commit();
}
Json Database::delivery_status(const std::string &email) {
    std::lock_guard lock(mutex_);
    Impl::Transaction tx(*impl_);
    auto rows = impl_->query(tx,
                             "SELECT FIRST 100 ID,RECIPIENT,STATUS,ATTEMPTS,CREATED,NEXT_ATTEMPT,LAST_ERROR "
                             "FROM CM_OUTBOUND WHERE SENDER = ? ORDER BY CREATED DESC",
                             {Param(email)});
    Json result = Json::array();
    for (const auto &r : rows)
        result.push_back({{"id", r[0]},
                          {"recipient", r[1]},
                          {"status", r[2]},
                          {"attempts", std::stoul(r[3])},
                          {"created", std::stoll(r[4])},
                          {"next_attempt", std::stoll(r[5])},
                          {"error", r[6]}});
    tx.commit();
    return result;
}
} // namespace cm
