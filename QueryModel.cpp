#include "QueryModel.h"

#include <QSet>

void QueryModel::removeJoin(int index)
{
    if (index < 0 || index >= m_joins.size()) {
        return;
    }

    QSet<QString> removedTables;
    removedTables.insert(m_joins[index].table);
    m_joins.remove(index);

    // A join hangs off the other table of its relation (the "anchor").
    // Keep dropping joins whose anchor was just removed until stable.
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < m_joins.size(); ++i) {
            const JoinClause& join = m_joins[i];
            const QString anchor = (join.onRelation.fromTable == join.table)
                                       ? join.onRelation.toTable
                                       : join.onRelation.fromTable;
            if (removedTables.contains(anchor)) {
                removedTables.insert(join.table);
                m_joins.remove(i);
                changed = true;
                break;
            }
        }
    }

    for (int i = m_conditions.size() - 1; i >= 0; --i) {
        for (const auto& table : removedTables) {
            if (m_conditions[i].column.startsWith(table + ".")) {
                m_conditions.remove(i);
                break;
            }
        }
    }
}

QString QueryModel::toSql() const
{
    if (isEmpty()) {
        return QString();
    }

    QString sql = "SELECT *\nFROM " + m_fromTable;

    for (const auto& join : m_joins) {
        sql += "\nJOIN " + join.table
             + "\n  ON " + join.onRelation.fromTable + "." + join.onRelation.fromColumn
             + " = " + join.onRelation.toTable + "." + join.onRelation.toColumn;
    }

    if (!m_conditions.isEmpty()) {
        sql += "\nWHERE ";
        for (int i = 0; i < m_conditions.size(); ++i) {
            if (i > 0) {
                sql += "\n  AND ";
            }
            const auto& c = m_conditions[i];
            sql += c.column + " " + c.op + " " + c.value;
        }
    }

    sql += ";";
    return sql;
}
