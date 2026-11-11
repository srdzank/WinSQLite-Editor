#pragma once

#include <QDialog>
#include <QString>
#include <QVector>

#include "Table.h"

class QLineEdit;
class QTableWidget;

// Modal dialog for defining a brand-new table OR editing an existing one:
// a name plus a list of columns (name / type / PRIMARY KEY / NOT NULL).
// MainWindow turns the result into SQL - this dialog only collects and
// validates the definition, it never touches the database itself.
//
// Edit mode is limited to what SQLite's ALTER TABLE can do without
// rebuilding the table: rename the table, rename columns, add columns,
// remove columns. Type / PRIMARY KEY / NOT NULL of existing columns are
// shown but locked (and PK / NOT NULL can't be set on newly added ones).
class CreateTableDialog : public QDialog
{
    Q_OBJECT

public:
    struct ColumnDef
    {
        QString name;
        // Edit mode: the column's name before editing (empty for a column
        // that was added in the dialog). Always empty in "new table" mode.
        QString originalName;
        QString type;
        bool notNull = false;
        bool primaryKey = false;
    };

    explicit CreateTableDialog(QWidget* parent = nullptr);

    // Edit mode: fields start out filled from existing.
    explicit CreateTableDialog(const Table& existing, QWidget* parent = nullptr);

    QString tableName() const;
    QVector<ColumnDef> columns() const;

private slots:
    void addColumnRow(const Column* existing = nullptr);
    void removeSelectedRow();

private:
    void init();
    void accept() override;

    const bool m_editing;
    QString m_originalTableName;  // only set in edit mode

    QLineEdit* m_nameEdit = nullptr;
    QTableWidget* m_columnsTable = nullptr;
};
