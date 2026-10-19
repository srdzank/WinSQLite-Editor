#include "Schema.h"

#include <QSqlQuery>
#include <QSqlError>

bool Schema::load(QSqlDatabase& db)
{
    m_tables.clear();
    m_foreignKeys.clear();
    m_indexes.clear();
    m_errorMessage.clear();

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

    return true;
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
        if (!findTable(candidate.table)) {
            continue;  // FK pointing at a table that doesn't exist
        }
        result.push_back(candidate);
    }
    return result;
}
