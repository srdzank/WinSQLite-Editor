#include "Schema.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QStringList>

namespace {

// Parses the column list out of
//   CREATE VIRTUAL TABLE name USING vec0(embedding float[384], id integer primary key, ...)
// PRAGMA table_info can't be used on a vec0 table (Qt's SQLite has no vec0
// module), so the declaration itself is the only source of column info.
QVector<Column> parseVec0Columns(const QString& createSql)
{
    QVector<Column> columns;

    const int usingPos = createSql.indexOf(QStringLiteral("vec0"), 0, Qt::CaseInsensitive);
    if (usingPos < 0) {
        return columns;
    }
    const int open = createSql.indexOf(QLatin1Char('('), usingPos);
    if (open < 0) {
        return columns;
    }

    // Split the argument list on top-level commas only: "float[384]" and
    // any nested parentheses must not be split apart.
    QStringList parts;
    QString current;
    int depth = 1;
    for (int i = open + 1; i < createSql.size(); ++i) {
        const QChar ch = createSql.at(i);
        if (ch == QLatin1Char('(') || ch == QLatin1Char('[')) {
            ++depth;
        } else if (ch == QLatin1Char(')') || ch == QLatin1Char(']')) {
            --depth;
            if (depth == 0) {
                break;  // closing paren of vec0(...)
            }
        } else if (ch == QLatin1Char(',') && depth == 1) {
            parts << current;
            current.clear();
            continue;
        }
        current += ch;
    }
    if (!current.trimmed().isEmpty()) {
        parts << current;
    }

    for (const QString& rawPart : parts) {
        QString part = rawPart.simplified();
        if (part.isEmpty()) {
            continue;
        }
        // Auxiliary columns are declared as "+name type".
        if (part.startsWith(QLatin1Char('+'))) {
            part.remove(0, 1);
        }
        // Table-level options such as "chunk_size=1024" are not columns.
        const int space = part.indexOf(QLatin1Char(' '));
        const int eq = part.indexOf(QLatin1Char('='));
        if (eq >= 0 && (space < 0 || eq < space)) {
            continue;
        }

        Column col;
        QString name = space < 0 ? part : part.left(space);
        name.remove(QLatin1Char('"')).remove(QLatin1Char('`'));
        name.remove(QLatin1Char('[')).remove(QLatin1Char(']'));
        col.name = name;
        col.type = space < 0 ? QString() : part.mid(space + 1);
        col.isPrimaryKey = part.contains(QStringLiteral("primary key"), Qt::CaseInsensitive);
        columns.push_back(col);
    }
    return columns;
}

}  // namespace

bool Schema::load(QSqlDatabase& db)
{
    m_tables.clear();
    m_vectorTables.clear();
    m_foreignKeys.clear();
    m_indexes.clear();
    m_errorMessage.clear();
    m_hasVectorTables = false;

    // sqlite-vec (vec0) virtual tables cannot be inspected through Qt's bundled
    // SQLite (the vec0 module is not loaded there, so PRAGMA table_info fails with
    // "no such module: vec0"). They (and their shadow tables) are kept out of
    // m_tables, but each vec0 table is still recorded in m_vectorTables, with
    // its columns parsed from the CREATE statement, so the table list can show
    // it. Browsing the vectors themselves is done via Tools > Vector space (3D).
    QStringList vecTables;
    {
        QSqlQuery vecQuery(db);
        if (vecQuery.exec(
                "SELECT name, sql FROM sqlite_master "
                "WHERE type='table' AND sql LIKE 'CREATE VIRTUAL TABLE%' "
                "AND lower(sql) LIKE '%using vec0%' "
                "ORDER BY name;")) {
            while (vecQuery.next()) {
                Table vecTable;
                vecTable.name = vecQuery.value(0).toString();
                vecTable.columns = parseVec0Columns(vecQuery.value(1).toString());
                vecTables << vecTable.name;
                m_vectorTables.push_back(vecTable);
            }
        }
    }
    m_hasVectorTables = !vecTables.isEmpty();
    static const QStringList kVecShadowSuffixes = {"_chunks", "_rowids", "_vector_chunks", "_info", "_auxiliary"};
    const auto isVecObject = [&vecTables](const QString& name) {
        for (const QString& vt : vecTables) {
            if (name == vt) {
                return true;
            }
            for (const QString& suffix : kVecShadowSuffixes) {
                if (name.startsWith(vt + suffix)) {
                    return true;
                }
            }
        }
        return false;
    };

    QSqlQuery tableListQuery(db);
    if (!tableListQuery.exec(
            "SELECT name FROM sqlite_master "
            "WHERE type='table' AND name NOT LIKE 'sqlite_%' "
            "ORDER BY name;")) {
        m_errorMessage = tableListQuery.lastError().text();
        return false;
    }

    while (tableListQuery.next()) {
        Table table;
        table.name = tableListQuery.value(0).toString();
        if (isVecObject(table.name)) {
            continue;
        }

        // PRAGMA statements can't be parameterised, so we build the string
        // directly. Table names come from sqlite_master, not user input,
        // so this is safe.
        QSqlQuery columnQuery(db);
        if (!columnQuery.exec(QString("PRAGMA table_info(%1);").arg(table.name))) {
            m_errorMessage = columnQuery.lastError().text();
            return false;
        }
        while (columnQuery.next()) {
            Column col;
            col.name = columnQuery.value("name").toString();
            col.type = columnQuery.value("type").toString();
            col.isNotNull = columnQuery.value("notnull").toInt() != 0;
            col.isPrimaryKey = columnQuery.value("pk").toInt() != 0;
            table.columns.push_back(col);
        }

        QSqlQuery fkQuery(db);
        if (!fkQuery.exec(QString("PRAGMA foreign_key_list(%1);").arg(table.name))) {
            m_errorMessage = fkQuery.lastError().text();
            return false;
        }
        while (fkQuery.next()) {
            ForeignKey fk;
            fk.fromTable = table.name;
            fk.fromColumn = fkQuery.value("from").toString();
            fk.toTable = fkQuery.value("table").toString();
            fk.toColumn = fkQuery.value("to").toString();
            m_foreignKeys.push_back(fk);
        }

        QSqlQuery indexListQuery(db);
        if (!indexListQuery.exec(QString("PRAGMA index_list(%1);").arg(table.name))) {
            m_errorMessage = indexListQuery.lastError().text();
            return false;
        }
        while (indexListQuery.next()) {
            const QString indexName = indexListQuery.value("name").toString();
            // SQLite creates these implicitly to back PRIMARY KEY/UNIQUE
            // constraints; they can't be dropped directly and aren't
            // something the user created, so skip them.
            if (indexName.startsWith("sqlite_autoindex_")) {
                continue;
            }

            Index index;
            index.name = indexName;
            index.table = table.name;
            index.isUnique = indexListQuery.value("unique").toInt() != 0;

            QSqlQuery indexInfoQuery(db);
            if (!indexInfoQuery.exec(QString("PRAGMA index_info(%1);").arg(index.name))) {
                m_errorMessage = indexInfoQuery.lastError().text();
                return false;
            }
            while (indexInfoQuery.next()) {
                index.columns << indexInfoQuery.value("name").toString();
            }

            m_indexes.push_back(index);
        }

        m_tables.push_back(table);
    }

    // "REFERENCES other" without a column list leaves PRAGMA's "to" empty,
    // meaning "the primary key of other". Resolve it here so every
    // consumer (JOIN ... ON ...) gets a usable column name.
    for (auto& fk : m_foreignKeys) {
        if (!fk.toColumn.isEmpty()) {
            continue;
        }
        if (const Table* target = findTable(fk.toTable)) {
            for (const auto& col : target->columns) {
                if (col.isPrimaryKey) {
                    fk.toColumn = col.name;
                    break;
                }
            }
        }
    }

    addVectorRelations();
    return true;
}

bool Schema::isVectorTable(const QString& tableName) const
{
    for (const auto& t : m_vectorTables) {
        if (t.name == tableName) {
            return true;
        }
    }
    return false;
}

const Table* Schema::findAnyTable(const QString& tableName) const
{
    if (const Table* t = findTable(tableName)) {
        return t;
    }
    for (const auto& t : m_vectorTables) {
        if (t.name == tableName) {
            return &t;
        }
    }
    return nullptr;
}

void Schema::addVectorRelations()
{
    static const QStringList kPrefixes = {"vec_", "vss_", "embeddings_", "embedding_", "vec"};
    static const QStringList kSuffixes = {"_vec", "_vectors", "_embeddings", "_embedding", "_emb"};

    for (const auto& vt : m_vectorTables) {
        QString vecPk;
        for (const auto& col : vt.columns) {
            if (col.isPrimaryKey) {
                vecPk = col.name;
                break;
            }
        }
        if (vecPk.isEmpty()) {
            vecPk = QStringLiteral("rowid");
        }

        // Candidate base-table names: the vec table's name with a known
        // prefix/suffix removed.
        QStringList bases;
        for (const QString& p : kPrefixes) {
            if (vt.name.startsWith(p, Qt::CaseInsensitive) && vt.name.size() > p.size()) {
                bases << vt.name.mid(p.size());
            }
        }
        for (const QString& s : kSuffixes) {
            if (vt.name.endsWith(s, Qt::CaseInsensitive) && vt.name.size() > s.size()) {
                bases << vt.name.left(vt.name.size() - s.size());
            }
        }

        for (const QString& base : bases) {
            const Table* target = nullptr;
            for (const auto& t : m_tables) {
                if (t.name.compare(base, Qt::CaseInsensitive) == 0) {
                    target = &t;
                    break;
                }
            }
            if (!target) {
                continue;
            }
            QString toColumn;
            for (const auto& col : target->columns) {
                if (col.isPrimaryKey) {
                    toColumn = col.name;
                    break;
                }
            }
            if (toColumn.isEmpty()) {
                continue;
            }
            ForeignKey fk;
            fk.fromTable = vt.name;
            fk.fromColumn = vecPk;
            fk.toTable = target->name;
            fk.toColumn = toColumn;
            m_foreignKeys.push_back(fk);
            break;
        }
    }
}

const Table* Schema::findTable(const QString& tableName) const
{
    for (const auto& t : m_tables) {
        if (t.name == tableName) {
            return &t;
        }
    }
    return nullptr;
}

QVector<ForeignKey> Schema::relationsFor(const QString& tableName) const
{
    QVector<ForeignKey> result;
    for (const auto& fk : m_foreignKeys) {
        if (fk.fromTable == tableName || fk.toTable == tableName) {
            result.push_back(fk);
        }
    }
    return result;
}

QVector<Index> Schema::indexesFor(const QString& tableName) const
{
    QVector<Index> result;
    for (const auto& index : m_indexes) {
        if (index.table == tableName) {
            result.push_back(index);
        }
    }
    return result;
}

QVector<JoinCandidate> Schema::joinCandidates(const QVector<QString>& queryTables) const
{
    QVector<JoinCandidate> result;
    for (const auto& fk : m_foreignKeys) {
        if (fk.fromTable == fk.toTable) {
            continue;  // self-references would need aliases
        }
        const bool fromInQuery = queryTables.contains(fk.fromTable);
        const bool toInQuery = queryTables.contains(fk.toTable);
        if (fromInQuery == toInQuery) {
            continue;  // both already joined, or unrelated to the query
        }

        JoinCandidate candidate;
        candidate.relation = fk;
        candidate.table = fromInQuery ? fk.toTable : fk.fromTable;
        candidate.anchorTable = fromInQuery ? fk.fromTable : fk.toTable;
        if (!findAnyTable(candidate.table)) {
            continue;  // FK pointing at a table that doesn't exist
        }
        result.push_back(candidate);
    }
    return result;
}
