#include "VectorEditDialog.h"

#include "VecDatabase.h"

#include <QBrush>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>
#include <cmath>

namespace vec {

namespace {

constexpr int kIndexCol = 0;
constexpr int kValueCol = 1;

// 9 significant digits round-trip a float32 exactly.
QString floatText(float v) { return QString::number(double(v), 'g', 9); }

// Locale-independent parse; rejects empty text, NaN and infinity.
bool parseFloat(const QString& text, float& out)
{
    bool ok = false;
    const double d = QString(text).trimmed().toDouble(&ok);
    if (!ok || !std::isfinite(d) || std::fabs(d) > double(3.4e38))
        return false;
    out = float(d);
    return true;
}

} // namespace

VectorEditDialog::VectorEditDialog(const QString& title, std::vector<float> values, QWidget* parent)
    : QDialog(parent), m_original(std::move(values))
{
    setWindowTitle(title);
    resize(520, 640);

    const int dims = int(m_original.size());

    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(tr("Filter by index or value…"));
    m_filter->setClearButtonEnabled(true);

    m_gotoSpin = new QSpinBox(this);
    m_gotoSpin->setRange(0, qMax(0, dims - 1));
    auto* gotoBtn = new QPushButton(tr("Go to index"), this);

    auto* pasteBtn = new QPushButton(tr("Paste list…"), this);
    pasteBtn->setToolTip(tr("Replace all %1 values with a pasted list of numbers").arg(dims));
    auto* rangeBtn = new QPushButton(tr("Set range…"), this);
    rangeBtn->setToolTip(tr("Give the same value to a range of elements"));
    auto* resetBtn = new QPushButton(tr("Reset"), this);
    resetBtn->setToolTip(tr("Go back to the stored values"));

    auto* topRow = new QHBoxLayout;
    topRow->addWidget(m_filter, 1);
    topRow->addWidget(m_gotoSpin);
    topRow->addWidget(gotoBtn);

    auto* toolRow = new QHBoxLayout;
    toolRow->addWidget(pasteBtn);
    toolRow->addWidget(rangeBtn);
    toolRow->addWidget(resetBtn);
    toolRow->addStretch();

    m_table = new QTableWidget(dims, 2, this);
    m_table->setHorizontalHeaderLabels({tr("Index"), tr("Value")});
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed
                             | QAbstractItemView::AnyKeyPressed);

    m_populating = true;
    for (int i = 0; i < dims; ++i) {
        auto* idx = new QTableWidgetItem(QString::number(i));
        idx->setFlags(idx->flags() & ~Qt::ItemIsEditable);
        idx->setForeground(QBrush(QColor("#888780")));
        m_table->setItem(i, kIndexCol, idx);
        m_table->setItem(i, kValueCol, new QTableWidgetItem(floatText(m_original[std::size_t(i)])));
    }
    m_populating = false;

    m_info = new QLabel(this);
    m_info->setWordWrap(true);
    updateInfo();

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* root = new QVBoxLayout(this);
    root->addLayout(topRow);
    root->addLayout(toolRow);
    root->addWidget(m_table, 1);
    root->addWidget(m_info);
    root->addWidget(buttons);

    connect(m_filter, &QLineEdit::textChanged, this, &VectorEditDialog::applyFilter);
    connect(gotoBtn, &QPushButton::clicked, this, &VectorEditDialog::goToIndex);
    connect(m_gotoSpin, &QSpinBox::editingFinished, this, &VectorEditDialog::goToIndex);
    connect(pasteBtn, &QPushButton::clicked, this, &VectorEditDialog::pasteList);
    connect(rangeBtn, &QPushButton::clicked, this, &VectorEditDialog::setRange);
    connect(resetBtn, &QPushButton::clicked, this, &VectorEditDialog::resetAll);
    connect(m_table, &QTableWidget::itemChanged, this, &VectorEditDialog::onItemChanged);
}

bool VectorEditDialog::parseRow(int row, float& out) const
{
    const QTableWidgetItem* item = m_table->item(row, kValueCol);
    return item && parseFloat(item->text(), out);
}

void VectorEditDialog::markRow(int row)
{
    QTableWidgetItem* item = m_table->item(row, kValueCol);
    if (!item)
        return;
    float v = 0.f;
    const bool valid = parseFloat(item->text(), v);
    const bool changed = valid && v != m_original[std::size_t(row)];

    QBrush brush;  // default = no background
    if (!valid)
        brush = QBrush(QColor("#6B1F23"));       // red: not a number
    else if (changed)
        brush = QBrush(QColor("#5C4A12"));       // amber: modified
    item->setBackground(brush);
}

void VectorEditDialog::updateInfo()
{
    int changed = 0, invalid = 0;
    for (int i = 0; i < m_table->rowCount(); ++i) {
        float v = 0.f;
        if (!parseRow(i, v))
            ++invalid;
        else if (v != m_original[std::size_t(i)])
            ++changed;
    }
    QString text = tr("%1 dimensions (fixed). Changed: %2.").arg(m_table->rowCount()).arg(changed);
    if (invalid > 0)
        text += tr("  Invalid: %1 (marked red).").arg(invalid);
    m_info->setText(text);
}

void VectorEditDialog::onItemChanged(QTableWidgetItem* item)
{
    if (m_populating || !item || item->column() != kValueCol)
        return;
    markRow(item->row());
    updateInfo();
}

void VectorEditDialog::applyFilter(const QString& text)
{
    const QString needle = text.trimmed();
    for (int i = 0; i < m_table->rowCount(); ++i) {
        bool show = needle.isEmpty();
        if (!show) {
            const QTableWidgetItem* v = m_table->item(i, kValueCol);
            show = QString::number(i).contains(needle) || (v && v->text().contains(needle));
        }
        m_table->setRowHidden(i, !show);
    }
}

void VectorEditDialog::goToIndex()
{
    const int row = m_gotoSpin->value();
    if (row < 0 || row >= m_table->rowCount())
        return;
    m_filter->clear();  // a filter could hide the target row
    m_table->selectRow(row);
    m_table->scrollToItem(m_table->item(row, kValueCol), QAbstractItemView::PositionAtCenter);
    m_table->setCurrentCell(row, kValueCol);
    m_table->setFocus();
}

void VectorEditDialog::pasteList()
{
    bool ok = false;
    const QString text = QInputDialog::getMultiLineText(
        this, tr("Paste list"),
        tr("Numbers separated by commas, spaces or new lines (exactly %1 values; [ ] are ignored):")
            .arg(m_table->rowCount()),
        QString(), &ok);
    if (!ok || text.trimmed().isEmpty())
        return;

    static const QRegularExpression splitter(QStringLiteral("[\\s,;\\[\\]]+"));
    const QStringList parts = text.split(splitter, Qt::SkipEmptyParts);

    if (parts.size() != m_table->rowCount()) {
        QMessageBox::warning(this, tr("Paste list"),
                             tr("Got %1 values, but this vector has exactly %2 dimensions.")
                                 .arg(parts.size()).arg(m_table->rowCount()));
        return;
    }
    std::vector<float> parsed(parts.size());
    for (int i = 0; i < parts.size(); ++i) {
        if (!parseFloat(parts[i], parsed[std::size_t(i)])) {
            QMessageBox::warning(this, tr("Paste list"),
                                 tr("Value #%1 is not a valid number: \"%2\".").arg(i).arg(parts[i]));
            return;
        }
    }

    m_populating = true;
    for (int i = 0; i < parts.size(); ++i) {
        m_table->item(i, kValueCol)->setText(floatText(parsed[std::size_t(i)]));
        markRow(i);
    }
    m_populating = false;
    updateInfo();
}

void VectorEditDialog::setRange()
{
    const int last = m_table->rowCount() - 1;

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Set range"));
    auto* from = new QSpinBox(&dlg);
    from->setRange(0, last);
    auto* to = new QSpinBox(&dlg);
    to->setRange(0, last);
    to->setValue(last);
    auto* value = new QDoubleSpinBox(&dlg);
    value->setDecimals(9);
    value->setRange(-3.4e38, 3.4e38);

    auto* form = new QFormLayout;
    form->addRow(tr("From index:"), from);
    form->addRow(tr("To index (inclusive):"), to);
    form->addRow(tr("Value:"), value);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto* lay = new QVBoxLayout(&dlg);
    lay->addLayout(form);
    lay->addWidget(buttons);

    if (dlg.exec() != QDialog::Accepted)
        return;

    const int a = qMin(from->value(), to->value());
    const int b = qMax(from->value(), to->value());
    const QString text = floatText(float(value->value()));

    m_populating = true;
    for (int i = a; i <= b; ++i) {
        m_table->item(i, kValueCol)->setText(text);
        markRow(i);
    }
    m_populating = false;
    updateInfo();
}

void VectorEditDialog::resetAll()
{
    m_populating = true;
    for (int i = 0; i < m_table->rowCount(); ++i) {
        m_table->item(i, kValueCol)->setText(floatText(m_original[std::size_t(i)]));
        markRow(i);
    }
    m_populating = false;
    updateInfo();
}

std::vector<float> VectorEditDialog::values() const
{
    std::vector<float> out(std::size_t(m_table->rowCount()), 0.f);
    for (int i = 0; i < m_table->rowCount(); ++i)
        parseRow(i, out[std::size_t(i)]);
    return out;
}

void VectorEditDialog::accept()
{
    for (int i = 0; i < m_table->rowCount(); ++i) {
        float v = 0.f;
        if (!parseRow(i, v)) {
            m_filter->clear();
            m_table->selectRow(i);
            m_table->scrollToItem(m_table->item(i, kValueCol), QAbstractItemView::PositionAtCenter);
            QMessageBox::warning(this, windowTitle(),
                                 tr("Element %1 is not a valid number. Fix it (or press Reset) first.").arg(i));
            return;
        }
    }
    QDialog::accept();
}

bool VectorEditDialog::editRecord(QWidget* parent, const QString& dbPath, const QString& table, qint64 rowid)
{
    Database db;
    if (!db.open(dbPath, /*readOnly=*/false, /*create=*/false)) {
        QMessageBox::critical(parent, tr("Edit vector"), tr("Cannot open the database:\n%1").arg(db.lastError()));
        return false;
    }
    const auto current = db.embeddingOf(table, rowid);
    if (!current) {
        QMessageBox::warning(parent, tr("Edit vector"),
                             tr("No vector found for id %1 in '%2'.").arg(rowid).arg(table));
        return false;
    }

    VectorEditDialog dialog(tr("Edit vector - %1 (id %2)").arg(table).arg(rowid), *current, parent);
    if (dialog.exec() != QDialog::Accepted)
        return false;

    const std::vector<float> edited = dialog.values();
    if (edited == *current)
        return false;  // nothing changed

    if (!db.updateEmbedding(table, rowid, edited)) {
        QMessageBox::critical(parent, tr("Edit vector"),
                              tr("The vector could not be saved:\n%1\n\n"
                                 "(This needs a sqlite-vec version that supports UPDATE on vector columns.)")
                                  .arg(db.lastError()));
        return false;
    }
    return true;
}

} // namespace vec
