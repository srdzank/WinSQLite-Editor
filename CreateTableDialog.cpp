#include "CreateTableDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QAbstractItemView>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSet>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace {

// Only plain identifiers are accepted for table/column names - keeps the
// generated SQL simple and avoids needing to quote or escape anything.
const QRegularExpression kIdentifierPattern("^[A-Za-z_][A-Za-z0-9_]*$");

const QStringList kColumnTypes = {"INTEGER", "TEXT", "REAL", "NUMERIC", "BLOB"};

enum ColumnIndex { NameColumn, TypeColumn, PrimaryKeyColumn, NotNullColumn, ColumnCount };

// PK/NOT NULL cells hold a centered checkbox rather than a plain
// checkable item, so it lines up visually with the combo box column.
// Clicking anywhere in the cell toggles it - the box itself is only ~16px,
// which made it easy to click "next to" it and see nothing happen.
class CheckCell : public QWidget
{
public:
    CheckCell()
    {
        m_box = new QCheckBox(this);
        auto* checkLayout = new QHBoxLayout(this);
        checkLayout->addWidget(m_box);
        checkLayout->setAlignment(Qt::AlignCenter);
        checkLayout->setContentsMargins(0, 0, 0, 0);
        setCursor(Qt::PointingHandCursor);
    }

    QCheckBox* checkBox() const { return m_box; }

protected:
    // Only reached when the click missed the checkbox's own hit area (a
    // click on the box is handled by the box itself), so no double toggle.
    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && rect().contains(event->pos())) {
            m_box->toggle();
        }
        QWidget::mouseReleaseEvent(event);
    }

private:
    QCheckBox* m_box = nullptr;
};

QWidget* centeredCheckBox()
{
    return new CheckCell();
}

} // namespace

CreateTableDialog::CreateTableDialog(QWidget* parent)
    : QDialog(parent)
    , m_editing(false)
{
    init();
    addColumnRow();  // start with one empty row so the dialog isn't blank
}

CreateTableDialog::CreateTableDialog(const Table& existing, QWidget* parent)
    : QDialog(parent)
    , m_editing(true)
    , m_originalTableName(existing.name)
{
    init();
    m_nameEdit->setText(existing.name);
    for (const auto& col : existing.columns) {
        addColumnRow(&col);
    }
}

void CreateTableDialog::init()
{
    setWindowTitle(m_editing ? "Edit Table" : "New Table");
    resize(480, 360);

    auto* layout = new QVBoxLayout(this);

    auto* nameRow = new QHBoxLayout();
    nameRow->addWidget(new QLabel("Table name:", this));
    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setValidator(new QRegularExpressionValidator(kIdentifierPattern, m_nameEdit));
    nameRow->addWidget(m_nameEdit, 1);
    layout->addLayout(nameRow);

    if (m_editing) {
        auto* hint = new QLabel(
            "You can rename the table and its columns, add columns and remove columns. SQLite cannot "
            "change the type, PRIMARY KEY or NOT NULL of an existing column in place, so those are locked.",
            this);
        hint->setWordWrap(true);
        layout->addWidget(hint);
    }

    m_columnsTable = new QTableWidget(0, ColumnCount, this);
    m_columnsTable->setHorizontalHeaderLabels({"Column", "Type", "PK", "NOT NULL"});
    m_columnsTable->horizontalHeader()->setSectionResizeMode(NameColumn, QHeaderView::Stretch);
    m_columnsTable->horizontalHeader()->setSectionResizeMode(TypeColumn, QHeaderView::ResizeToContents);
    m_columnsTable->horizontalHeader()->setSectionResizeMode(PrimaryKeyColumn, QHeaderView::ResizeToContents);
    m_columnsTable->horizontalHeader()->setSectionResizeMode(NotNullColumn, QHeaderView::ResizeToContents);
    m_columnsTable->verticalHeader()->setVisible(false);
    // A single click on a name cell selects it AND opens it for typing,
    // so there's no separate "double-click to edit" step to discover.
    m_columnsTable->setEditTriggers(QAbstractItemView::AllEditTriggers);
    m_columnsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    // Explicit colours for the selected cell and the in-cell editor. Without
    // them the selection used a default blue that clashed with the theme,
    // and the editor was a bare caret on the same colour as the background,
    // so it was hard to tell which cell you were typing into.
    m_columnsTable->setStyleSheet(
        "QTableWidget::item:selected { background-color: #0C447C; color: #E6F1FB; }"
        "QTableWidget QLineEdit {"
        "  background-color: #201F22; color: #F1EFE8;"
        "  border: 1px solid #378ADD; padding: 0px 4px;"
        "}");
    layout->addWidget(m_columnsTable, 1);

    auto* rowButtons = new QHBoxLayout();
    auto* addRowBtn = new QPushButton("+ Add column", this);
    auto* removeRowBtn = new QPushButton("- Remove column", this);
    rowButtons->addWidget(addRowBtn);
    rowButtons->addWidget(removeRowBtn);
    rowButtons->addStretch();
    layout->addLayout(rowButtons);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(m_editing ? "Save" : "Create");
    layout->addWidget(buttons);

    // "+ Add column" also drops the cursor into the new row's name cell.
    connect(addRowBtn, &QPushButton::clicked, this, [this]() {
        addColumnRow();
        const int row = m_columnsTable->rowCount() - 1;
        m_columnsTable->setCurrentCell(row, NameColumn);
        m_columnsTable->editItem(m_columnsTable->item(row, NameColumn));
    });
    connect(removeRowBtn, &QPushButton::clicked, this, &CreateTableDialog::removeSelectedRow);
    connect(buttons, &QDialogButtonBox::accepted, this, &CreateTableDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &CreateTableDialog::reject);
}

void CreateTableDialog::addColumnRow(const Column* existing)
{
    const int row = m_columnsTable->rowCount();
    m_columnsTable->insertRow(row);

    auto* nameItem = new QTableWidgetItem(existing ? existing->name : QString());
    if (existing) {
        // Remembered so MainWindow can tell a rename from a drop + add.
        nameItem->setData(Qt::UserRole, existing->name);
    }
    m_columnsTable->setItem(row, NameColumn, nameItem);

    auto* typeCombo = new QComboBox();
    typeCombo->addItems(kColumnTypes);
    if (existing) {
        // Existing columns can have any declared type (VARCHAR(50), empty, ...).
        int typeIndex = typeCombo->findText(existing->type, Qt::MatchFixedString);  // case-insensitive
        if (typeIndex < 0) {
            typeCombo->addItem(existing->type);
            typeIndex = typeCombo->count() - 1;
        }
        typeCombo->setCurrentIndex(typeIndex);
        typeCombo->setEnabled(false);
    }
    m_columnsTable->setCellWidget(row, TypeColumn, typeCombo);

    QWidget* pkCell = centeredCheckBox();
    QWidget* notNullCell = centeredCheckBox();
    if (existing) {
        pkCell->findChild<QCheckBox*>()->setChecked(existing->isPrimaryKey);
        notNullCell->findChild<QCheckBox*>()->setChecked(existing->isNotNull);
    }
    if (m_editing) {
        // Disabling the cell widget also stops its click-anywhere-to-toggle.
        pkCell->setEnabled(false);
        notNullCell->setEnabled(false);
    }
    m_columnsTable->setCellWidget(row, PrimaryKeyColumn, pkCell);
    m_columnsTable->setCellWidget(row, NotNullColumn, notNullCell);
}

void CreateTableDialog::removeSelectedRow()
{
    const int row = m_columnsTable->currentRow();
    if (row >= 0) {
        m_columnsTable->removeRow(row);
    }
}

QString CreateTableDialog::tableName() const
{
    return m_nameEdit->text().trimmed();
}

QVector<CreateTableDialog::ColumnDef> CreateTableDialog::columns() const
{
    QVector<ColumnDef> result;
    for (int row = 0; row < m_columnsTable->rowCount(); ++row) {
        ColumnDef col;
        col.name = m_columnsTable->item(row, NameColumn)->text().trimmed();
        col.originalName = m_columnsTable->item(row, NameColumn)->data(Qt::UserRole).toString();
        col.type = static_cast<QComboBox*>(m_columnsTable->cellWidget(row, TypeColumn))->currentText();
        col.primaryKey = m_columnsTable->cellWidget(row, PrimaryKeyColumn)->findChild<QCheckBox*>()->isChecked();
        col.notNull = m_columnsTable->cellWidget(row, NotNullColumn)->findChild<QCheckBox*>()->isChecked();
        result.push_back(col);
    }
    return result;
}

void CreateTableDialog::accept()
{
    // A name cell that is still being edited hasn't written its text to the
    // item yet. Taking focus away from the editor commits it, so the last
    // typed name is never lost when "Create" is pressed.
    m_columnsTable->setFocus();

    const QString title = windowTitle();

    // In edit mode an untouched name is accepted as-is, even if it isn't a
    // plain identifier (e.g. a column called "my col" created elsewhere).
    const bool tableNameUntouched = m_editing && tableName() == m_originalTableName;
    if (!tableNameUntouched && !kIdentifierPattern.match(tableName()).hasMatch()) {
        QMessageBox::warning(this, title,
                              "Table name must start with a letter or underscore, and contain only "
                              "letters, numbers, and underscores.");
        return;
    }

    const auto cols = columns();
    if (cols.isEmpty()) {
        QMessageBox::warning(this, title, "Add at least one column.");
        return;
    }

    QSet<QString> seenNames;
    for (const auto& col : cols) {
        const bool untouched = m_editing && !col.originalName.isEmpty() && col.name == col.originalName;
        if (!untouched && !kIdentifierPattern.match(col.name).hasMatch()) {
            QMessageBox::warning(this, title,
                                  "Every column name must start with a letter or underscore, and "
                                  "contain only letters, numbers, and underscores.");
            return;
        }
        const QString key = col.name.toLower();
        if (seenNames.contains(key)) {
            QMessageBox::warning(this, title,
                                  "Column names must be unique: \"" + col.name + "\" is repeated.");
            return;
        }
        seenNames.insert(key);
    }

    QDialog::accept();
}
