#pragma once

#include <QString>

// Represents a foreign key relationship: thisTable.fromColumn -> toTable.toColumn
// Read from PRAGMA foreign_key_list(table).
struct ForeignKey
{
    QString fromTable;
    QString fromColumn;
    QString toTable;
    QString toColumn;

    bool operator==(const ForeignKey& other) const
    {
        return fromTable == other.fromTable && fromColumn == other.fromColumn
            && toTable == other.toTable && toColumn == other.toColumn;
    }
};
