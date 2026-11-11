#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

#include "Index.h"

class QCheckBox;
class QComboBox;
class QLineEdit;
class QListWidget;
class Schema;

// Modal dialog for defining a new index OR editing an existing one: pick a
// table, an index name, which of that table's columns to include (drag to
// change their order), and whether it's UNIQUE.
// Like CreateTableDialog, this only collects and validates input -
// MainWindow turns the result into the CREATE INDEX statement (and, when
// editing, the DROP INDEX that precedes it).
class CreateIndexDialog : public QDialog
{
    Q_OBJECT

public:
    // "New index" mode. schema must outlive the dialog - MainWindow only
    // ever constructs this on the stack right before calling exec(), so
    // that's always true.
    CreateIndexDialog(const Schema& schema, const QString& initialTable, QWidget* parent = nullptr);

    // "Edit index" mode: fields start out filled from existing, the table
    // can't be changed, and the dialog is titled "Edit Index".
    CreateIndexDialog(const Schema& schema, const Index& existing, QWidget* parent = nullptr);

    QString indexName() const;
    QString tableName() const;
    // In the order the columns are listed in the dialog (= index order).
    QStringList selectedColumns() const;
    bool isUnique() const;

private slots:
    void onTableChanged(const QString& table);

private:
    void init(const QString& initialTable);
    void populateForEdit(const Index& existing);
    void addColumnItem(const QString& name, bool checked);
    void accept() override;

    const Schema& m_schema;
    const bool m_editing;
    QString m_originalName;  // only set in edit mode
    QLineEdit* m_nameEdit = nullptr;
    QComboBox* m_tableCombo = nullptr;
    QListWidget* m_columnsList = nullptr;
    QCheckBox* m_uniqueCheck = nullptr;
};
