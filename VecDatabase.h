#pragma once
// Thin RAII wrapper over the SQLite C API with sqlite-vec statically registered.
// Use this for all vec0 work. Qt's bundled QSQLITE plugin is built without
// load_extension, so it cannot host sqlite-vec.

#include <QString>
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
    bool fail(const char* what);                // stores sqlite3_errmsg, returns false
    bool error(const QString& msg);             // stores msg, returns false

    sqlite3* m_db = nullptr;
    QString  m_error;
};

} // namespace vec
