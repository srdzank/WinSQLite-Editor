#include "VecDatabase.h"

#include <sqlite3.h>

#include <QByteArray>
#include <QRegularExpression>
#include <bit>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>

static_assert(sizeof(float) == 4, "sqlite-vec expects IEEE-754 float32");
static_assert(std::endian::native == std::endian::little, "float32 blobs are little-endian");

// Entry point from sqlite-vec.c (compiled with SQLITE_CORE / SQLITE_VEC_STATIC).
struct sqlite3_api_routines;
extern "C" int sqlite3_vec_init(sqlite3*, char**, const sqlite3_api_routines*);

namespace {

struct StmtDeleter { void operator()(sqlite3_stmt* s) const noexcept { sqlite3_finalize(s); } };
using Stmt = std::unique_ptr<sqlite3_stmt, StmtDeleter>;

Stmt prepare(sqlite3* db, const QByteArray& sql)
{
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, sql.constData(), int(sql.size()), &raw, nullptr) != SQLITE_OK)
        return nullptr;
    return Stmt(raw);
}

// Must run before the first sqlite3_open: every new connection then gets vec0 + vec_* functions.
void registerVecOnce()
{
    static std::once_flag once;
    std::call_once(once, [] {
        sqlite3_auto_extension(reinterpret_cast<void (*)()>(&sqlite3_vec_init));
    });
}

QByteArray quoted(const QString& ident) { return '"' + ident.toUtf8() + '"'; }

} // namespace

namespace vec {

Database::~Database() { close(); }

Database::Database(Database&& o) noexcept : m_db(std::exchange(o.m_db, nullptr)), m_error(std::move(o.m_error)) {}

Database& Database::operator=(Database&& o) noexcept
{
    if (this != &o) {
        close();
        m_db    = std::exchange(o.m_db, nullptr);
        m_error = std::move(o.m_error);
    }
    return *this;
}

bool Database::isValidIdentifier(const QString& name)
{
    static const QRegularExpression re(
        QRegularExpression::anchoredPattern(QStringLiteral("[A-Za-z_][A-Za-z0-9_]*")));
    return re.match(name).hasMatch();
}

bool Database::error(const QString& msg) { m_error = msg; return false; }

bool Database::fail(const char* what)
{
    m_error = QStringLiteral("%1: %2").arg(
        QLatin1String(what),
        m_db ? QString::fromUtf8(sqlite3_errmsg(m_db)) : QStringLiteral("no connection"));
    return false;
}

bool Database::exec(const char* sql)
{
    char* err = nullptr;
    if (sqlite3_exec(m_db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        m_error = QString::fromUtf8(err ? err : "unknown error");
        sqlite3_free(err);
        return false;
    }
    return true;
}

bool Database::open(const QString& path, bool readOnly, bool create)
{
    close();
    registerVecOnce();

    const int flags = (readOnly ? SQLITE_OPEN_READONLY
                                : SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0))
                      | SQLITE_OPEN_FULLMUTEX;
    const QByteArray utf8 = path.toUtf8();
    if (sqlite3_open_v2(utf8.constData(), &m_db, flags, nullptr) != SQLITE_OK) {
        fail("open");
        const QString msg = m_error;
        close();
        m_error = msg;
        return false;
    }
    sqlite3_busy_timeout(m_db, 5000); // coexist with the GUI's QSqlDatabase connection

    if (vecVersion().isEmpty()) {
        const QString msg = QStringLiteral("sqlite-vec is not available (%1)").arg(m_error);
        close();
        m_error = msg;
        return false;
    }
    return true;
}

void Database::close() noexcept
{
    if (m_db) {
        sqlite3_close_v2(m_db);
        m_db = nullptr;
    }
}

QString Database::vecVersion()
{
    if (!m_db) return {};
    Stmt st = prepare(m_db, "SELECT vec_version()");
    if (!st) { fail("vec_version/prepare"); return {}; }
    if (sqlite3_step(st.get()) != SQLITE_ROW) { fail("vec_version"); return {}; }
    return QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(st.get(), 0)));
}

bool Database::createVecTable(const QString& table, int dimensions, Metric metric)
{
    if (!m_db || !isValidIdentifier(table) || dimensions <= 0)
        return error(QStringLiteral("createVecTable: invalid arguments"));

    const QByteArray sql = "CREATE VIRTUAL TABLE IF NOT EXISTS " + quoted(table)
        + " USING vec0(embedding float[" + QByteArray::number(dimensions) + "]"
        + (metric == Metric::Cosine ? " distance_metric=cosine" : "") + ")";
    return exec(sql.constData());
}

bool Database::insert(const QString& table, qint64 rowid, std::span<const float> v)
{
    if (!m_db || !isValidIdentifier(table) || v.empty())
        return error(QStringLiteral("insert: invalid arguments"));

    Stmt st = prepare(m_db, "INSERT INTO " + quoted(table) + "(rowid, embedding) VALUES (?1, ?2)");
    if (!st) return fail("insert/prepare");

    sqlite3_bind_int64(st.get(), 1, rowid);
    sqlite3_bind_blob(st.get(), 2, v.data(), int(v.size_bytes()), SQLITE_TRANSIENT);
    return sqlite3_step(st.get()) == SQLITE_DONE ? true : fail("insert");
}

bool Database::insertMany(const QString& table, std::span<const Embedding> items)
{
    if (!m_db || !isValidIdentifier(table))
        return error(QStringLiteral("insertMany: invalid arguments"));
    if (!exec("BEGIN IMMEDIATE")) return false;

    Stmt st = prepare(m_db, "INSERT INTO " + quoted(table) + "(rowid, embedding) VALUES (?1, ?2)");
    if (!st) { fail("insertMany/prepare"); exec("ROLLBACK"); return false; }

    for (const Embedding& e : items) {
        sqlite3_reset(st.get());
        sqlite3_bind_int64(st.get(), 1, e.rowid);
        sqlite3_bind_blob(st.get(), 2, e.values.data(),
                          int(e.values.size() * sizeof(float)), SQLITE_TRANSIENT);
        if (sqlite3_step(st.get()) != SQLITE_DONE) {
            fail("insertMany");
            const QString msg = m_error;
            exec("ROLLBACK");
            m_error = msg;
            return false;
        }
    }
    return exec("COMMIT");
}

std::vector<Hit> Database::nearest(const QString& table, std::span<const float> query, int k)
{
    std::vector<Hit> hits;
    if (!m_db || !isValidIdentifier(table) || query.empty() || k <= 0) {
        error(QStringLiteral("nearest: invalid arguments"));
        return hits;
    }
    Stmt st = prepare(m_db,
        "SELECT rowid, distance FROM " + quoted(table) +
        " WHERE embedding MATCH ?1 AND k = ?2 ORDER BY distance");
    if (!st) { fail("nearest/prepare"); return hits; }

    sqlite3_bind_blob(st.get(), 1, query.data(), int(query.size_bytes()), SQLITE_TRANSIENT);
    sqlite3_bind_int(st.get(), 2, k);

    int rc;
    while ((rc = sqlite3_step(st.get())) == SQLITE_ROW)
        hits.push_back({sqlite3_column_int64(st.get(), 0), sqlite3_column_double(st.get(), 1)});
    if (rc != SQLITE_DONE) { fail("nearest"); hits.clear(); }
    return hits;
}

std::vector<Embedding> Database::loadAll(const QString& table, int limit)
{
    std::vector<Embedding> out;
    if (!m_db || !isValidIdentifier(table)) {
        error(QStringLiteral("loadAll: invalid arguments"));
        return out;
    }
    QByteArray sql = "SELECT rowid, embedding FROM " + quoted(table);
    if (limit > 0) sql += " LIMIT " + QByteArray::number(limit);

    Stmt st = prepare(m_db, sql);
    if (!st) { fail("loadAll/prepare"); return out; }

    int rc;
    while ((rc = sqlite3_step(st.get())) == SQLITE_ROW) {
        const void* blob = sqlite3_column_blob(st.get(), 1);
        const int   bytes = sqlite3_column_bytes(st.get(), 1);
        if (!blob || bytes <= 0 || bytes % int(sizeof(float)) != 0) continue;

        Embedding e;
        e.rowid = sqlite3_column_int64(st.get(), 0);
        e.values.resize(std::size_t(bytes) / sizeof(float));
        std::memcpy(e.values.data(), blob, std::size_t(bytes));
        out.push_back(std::move(e));
    }
    if (rc != SQLITE_DONE) { fail("loadAll"); out.clear(); }
    return out;
}

std::optional<std::vector<float>> Database::embeddingOf(const QString& table, qint64 rowid)
{
    if (!m_db || !isValidIdentifier(table)) { error(QStringLiteral("embeddingOf: invalid arguments")); return std::nullopt; }

    Stmt st = prepare(m_db, "SELECT embedding FROM " + quoted(table) + " WHERE rowid = ?1");
    if (!st) { fail("embeddingOf/prepare"); return std::nullopt; }
    sqlite3_bind_int64(st.get(), 1, rowid);

    if (sqlite3_step(st.get()) != SQLITE_ROW) return std::nullopt;
    const void* blob  = sqlite3_column_blob(st.get(), 0);
    const int   bytes = sqlite3_column_bytes(st.get(), 0);
    if (!blob || bytes <= 0 || bytes % int(sizeof(float)) != 0) return std::nullopt;

    std::vector<float> v(std::size_t(bytes) / sizeof(float));
    std::memcpy(v.data(), blob, std::size_t(bytes));
    return v;
}

} // namespace vec
