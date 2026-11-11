#include "CreateIndexDialog.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QVBoxLayout>

#include "Schema.h"

namespace {
const QRegularExpression kIdentifierPattern("^[A-Za-z_][A-Za-z0-9_]*$");
}

CreateIndexDialog::CreateIndexDialog(const Schema& schema, const QString& initialTable, QWidget* parent)
    : QDialog(parent)
    , m_schema(schema)
    , m_editing(false)
{
    init(initialTable);
}

CreateIndexDialog::CreateIndexDialog(const Schema& schema, const Index& existing, QWidget* parent)
    : QDialog(parent)
    , m_schema(schema)
    , m_editing(true)
    , m_originalName(existing.name)
{
    init(existing.table);

    // The table can't change while editing: the column list belongs to it.
    m_tableCombo->setEnabled(false);
    // Must come after init(): setting the table there fires onTableChanged(),
    // which would otherwise overwrite the name/columns filled in here.
    populateForEdit(existing);
}

void CreateIndexDialog::init(const QString& initialTable)
{
    setWindowTitle(m_editing ? "Edit Index" : "New Index");
    resize(360, 420);

    auto* layout = new QVBoxLayout(this);

    layout->addWidget(new QLabel("Table:", this));
    m_tableCombo = new QComboBox(this);
    for (const auto& table : m_schema.tables()) {
        m_tableCombo->addItem(table.name);
    }
    layout->addWidget(m_tableCombo);

    layout->addWidget(new QLabel("Index name:", this));
    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setValidator(new QRegularExpressionValidator(kIdentifierPattern, m_nameEdit));
    layout->addWidget(m_nameEdit);

    layout->addWidget(new QLabel("Columns (drag to change the order):", this));
    m_columnsList = new QListWidget(this);
    // Column order matters in an index, so let the user rearrange it.
    m_columnsList->setDragDropMode(QAbstractItemView::InternalMove);
    m_columnsList->setDefaultDropAction(Qt::MoveAction);
    layout->addWidget(m_columnsList, 1);

    m_uniqueCheck = new QCheckBox("UNIQUE", this);
    layout->addWidget(m_uniqueCheck);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(m_editing ? "Save" : "Create");
    layout->addWidget(buttons);

    connect(m_tableCombo, &QComboBox::currentTextChanged, this, &CreateIndexDialog::onTableChanged);
    connect(buttons, &QDialogButtonBox::accepted, this, &CreateIndexDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &CreateIndexDialog::reject);

    const int initialIndex = m_tableCombo->findText(initialTable);
    if (initialIndex >= 0) {
        m_tableCombo->setCurrentIndex(initialIndex);
    }
    // Populate explicitly rather than relying on currentTextChanged firing
    // for the initial selection, since Qt doesn't guarantee that signal
    // fires just from the combo box being filled or left at index 0.
    if (m_tableCombo->count() > 0) {
        onTableChanged(m_tableCombo->currentText());
    }
}

void CreateIndexDialog::addColumnItem(const QString& name, bool checked)
{
    auto* item = new QListWidgetItem(name, m_columnsList);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
}

void CreateIndexDialog::onTableChanged(const QString& table)
{
    m_columnsList->clear();

    const Table* t = m_schema.findTable(table);
    if (!t) {
        return;
    }

    for (const auto& col : t->columns) {
        addColumnItem(col.name, false);
    }

    m_nameEdit->setText("idx_" + table);
}

void CreateIndexDialog::populateForEdit(const Index& existing)
{
    m_columnsList->clear();

    const Table* t = m_schema.findTable(existing.table);
    if (!t) {
        return;
    }

    // The index's own columns come first, checked and in index order, so
    // saving without touching the list keeps the order exactly as it was.
    for (const QString& name : existing.columns) {
        addColumnItem(name, true);
    }
    for (const auto& col : t->columns) {
        if (!existing.columns.contains(col.name, Qt::CaseInsensitive)) {
            addColumnItem(col.name, false);
        }
    }

    m_nameEdit->setText(existing.name);
    m_uniqueCheck->setChecked(existing.isUnique);
}

QString CreateIndexDialog::indexName() const
{
    return m_nameEdit->text().trimmed();
}

QString CreateIndexDialog::tableName() const
{
    return m_tableCombo->currentText();
}

QStringList CreateIndexDialog::selectedColumns() const
{
    QStringList result;
    for (int i = 0; i < m_columnsList->count(); ++i) {
        const auto* item = m_columnsList->item(i);
        if (item->checkState() == Qt::Checked) {
            result << item->text();
        }
    }
    return result;
}

bool CreateIndexDialog::isUnique() const
{
    return m_uniqueCheck->isChecked();
}

void CreateIndexDialog::accept()
{
    const QString title = windowTitle();

    if (tableName().isEmpty()) {
        QMessageBox::warning(this, title, "This database has no tables to index.");
        return;
    }
    if (!kIdentifierPattern.match(indexName()).hasMatch()) {
        QMessageBox::warning(this, title,
                              "Index name must start with a letter or underscore, and contain only "
                              "letters, numbers, and underscores.");
        return;
    }
    if (selectedColumns().isEmpty()) {
        QMessageBox::warning(this, title, "Select at least one column.");
        return;
    }

    // Index names are unique per database (not per table). When editing,
    // keeping the index's own current name is of course fine.
    for (const auto& other : m_schema.indexes()) {
        if (other.name.compare(indexName(), Qt::CaseInsensitive) != 0) {
            continue;
        }
        if (m_editing && other.name.compare(m_originalName, Qt::CaseInsensitive) == 0) {
            continue;
        }
        QMessageBox::warning(this, title, "An index named \"" + other.name + "\" already exists.");
        return;
    }

    QDialog::accept();
}
