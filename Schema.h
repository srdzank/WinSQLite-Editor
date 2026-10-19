#pragma once

#include <QSqlDatabase>
#include <QString>
#include <QVector>

#include "ForeignKey.h"
#include "Index.h"
#include "JoinCandidate.h"
#include "Table.h"

// Loads and holds the full schema (tables, columns, foreign keys, and
// indexes) of an open SQLite database. This is the single source of
// truth that both the table list (left panel) and the diagram (center
// panel) read from.
class Schema
{
public:
    // Loads schema from an already-open QSqlDatabase connection.
    // Returns false and sets errorMessage() if loading fails.
    bool load(QSqlDatabase& db);

    const QVector<Table>& tables() const { return m_tables; }
    const QVector<ForeignKey>& foreignKeys() const { return m_foreignKeys; }
    const QVector<Index>& indexes() const { return m_indexes; }

    const Table* findTable(const QString& tableName) const;

    // Returns foreign keys where fromTable or toTable matches tableName.
    // Used by the focus-based diagram to find a table's direct neighbours.
    QVector<ForeignKey> relationsFor(const QString& tableName) const;

    // Returns every table that could be JOINed to a query that already
    // contains queryTables: one entry per foreign key that links a table
    // inside the query to a table outside it. A table with several
    // foreign keys into the query appears once per key.
    QVector<JoinCandidate> joinCandidates(const QVector<QString>& queryTables) const;

    // Returns user-created indexes defined on tableName.
    QVector<Index> indexesFor(const QString& tableName) const;

    QString errorMessage() const { return m_errorMessage; }

private:
    QVector<Table> m_tables;
    QVector<ForeignKey> m_foreignKeys;
    QVector<Index> m_indexes;
    QString m_errorMessage;
};
