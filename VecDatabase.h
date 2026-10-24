#pragma once
// Thin RAII wrapper over the SQLite C API with sqlite-vec statically registered.
// Use this for all vec0 work. Qt's bundled QSQLITE plugin is built without
// load_extension, so it cannot host sqlite-vec.

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>
#include <optional>
#include <span>
#include <vector>

struct sqlite3; // opaque; defined in sqlite3.h

namespace vec {

enum class Metric { L2, Cosine };

struct Hit       { qint64 rowid = 0; double distance = 0.0; };
struct Embedding { qint64 rowid = 0; std::vector<float> values; };

class Database {
public:
    Database() = default;
    ~Database();
    Database(const Database&)            = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&& other) noexcept;
    Database& operator=(Database&& other) noexcept;

    // Opens the file and verifies that sqlite-vec is active.
    // create = true makes a new file if it does not exist (ignored when readOnly).
    bool open(const QString& path, bool readOnly = false, bool create = false);
    void close() noexcept;
    bool isOpen() const noexcept { return m_db != nullptr; }
    const QString& lastError() const noexcept { return m_error; }

    QString vecVersion();                       // e.g. "v0.1.x", empty on failure

    // Runs arbitrary user SQL (one or more ';'-separated statements) with
    // sqlite-vec active, so vec0 tables and vec_* functions work. headers/rows
    // receive the result of the last statement that returns columns, already
    // converted to display strings (BLOBs are summarised, not dumped).
    // On failure returns false and lastError() holds SQLite's message.
    bool runSql(const QString& sql, QStringList& headers, QVector<QStringList>& rows);

    // CREATE VIRTUAL TABLE IF NOT EXISTS <table> USING vec0(embedding float[N] ...)
    bool createVecTable(const QString& table, int dimensions, Metric metric = Metric::Cosine);

    bool insert(const QString& table, qint64 rowid, std::span<const float> v);
    bool insertMany(const QString& table, std::span<const Embedding> items); // one transaction

    // KNN: ... WHERE embedding MATCH ? AND k = ? ORDER BY distance
    std::vector<Hit>        nearest(const QString& table, std::span<const float> query, int k);
    std::vector<Embedding>  loadAll(const QString& table, int limit = 0);
    std::optional<std::vector<float>> embeddingOf(const QString& table, qint64 rowid);

    // Table names cannot be bound as parameters, so they are validated + quoted.
    static bool isValidIdentifier(const QString& name);

private:
    bool exec(const char* sql);
    QByteArray idColumn(const QString& table);  // "rowid" or the table's own "id" column
    bool fail(const char* what);                // stores sqlite3_errmsg, returns false
    bool error(const QString& msg);             // stores msg, returns false

    sqlite3* m_db = nullptr;
    QString  m_error;
};

} // namespace vec
