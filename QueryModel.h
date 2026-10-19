#pragma once

#include <QString>
#include <QVector>

#include "ForeignKey.h"

// A single WHERE condition. Kept deliberately simple for phase 4 —
// one column, one operator, one value. Extend later for AND/OR grouping.
struct Condition
{
    QString column;
    QString op;     // "=", "LIKE", ">", etc.
    QString value;
};

// A single JOIN clause, built from a ForeignKey the user clicked in the diagram.
struct JoinClause
{
    QString table;
    ForeignKey onRelation;
};

// The in-memory representation of a query being built visually.
// This is the single source of truth: both the step panel (right side)
// and the SQL text box render FROM this model. Editing raw SQL directly
// is phase 5's job (escape hatch) — this model does not parse SQL back.
class QueryModel
{
public:
    void setFromTable(const QString& tableName) { m_fromTable = tableName; }
    const QString& fromTable() const { return m_fromTable; }

    void addJoin(const QString& table, const ForeignKey& relation)
    {
        if (involvedTables().contains(table)) {
            return;  // never join the same table twice
        }
        m_joins.push_back({table, relation});
    }

    // Removes the join at index, plus everything that depended on it:
    // joins that hang off the removed table (and so on, transitively) and
    // WHERE conditions on any removed table. Without the cascade, removing
    // a middle join would leave the SQL referencing a table that is no
    // longer in the query.
    void removeJoin(int index);
    const QVector<JoinClause>& joins() const { return m_joins; }

    void addCondition(const Condition& condition) { m_conditions.push_back(condition); }
    void removeCondition(int index)
    {
        if (index >= 0 && index < m_conditions.size()) {
            m_conditions.remove(index);
        }
    }
    const QVector<Condition>& conditions() const { return m_conditions; }

    bool isEmpty() const { return m_fromTable.isEmpty(); }

    // Every table already referenced by the query (FROM + all JOINs).
    // Used to figure out which relations/columns are still available to
    // add as a new step.
    QVector<QString> involvedTables() const
    {
        QVector<QString> result;
        if (!m_fromTable.isEmpty()) {
            result.push_back(m_fromTable);
        }
        for (const auto& join : m_joins) {
            result.push_back(join.table);
        }
        return result;
    }

    // Renders the model to a formatted SQL string.
    QString toSql() const;

private:
    QString m_fromTable;
    QVector<JoinClause> m_joins;
    QVector<Condition> m_conditions;
};
