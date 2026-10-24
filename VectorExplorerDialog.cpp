#include "VectorExplorerDialog.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QSpinBox>
#include <QVBoxLayout>
#include <algorithm>
#include <ranges>

namespace vec {

VectorExplorerDialog::VectorExplorerDialog(const QString& dbPath, QString vecTable,
                                           ScatterView3D::LabelFn labelFn, QWidget* parent)
    : QDialog(parent), m_table(std::move(vecTable))
{
    setWindowTitle(tr("Vector space - %1").arg(m_table));
    resize(900, 680);

    m_view   = new ScatterView3D(this);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_k = new QSpinBox(this);
    m_k->setRange(1, 200);
    m_k->setValue(10);

    auto* bar = new QHBoxLayout;
    bar->addWidget(new QLabel(tr("Nearest neighbours (k):"), this));
    bar->addWidget(m_k);
    bar->addWidget(m_status, 1);

    auto* root = new QVBoxLayout(this);
    root->addWidget(m_view, 1);
    root->addLayout(bar);

    m_view->setLabelProvider(std::move(labelFn));
    connect(m_view, &ScatterView3D::pointClicked, this, &VectorExplorerDialog::onPointClicked);

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = loadData(dbPath);        // NOTE: synchronous; see integration notes for threading
    QApplication::restoreOverrideCursor();
    if (!ok) m_status->setText(tr("Error: %1").arg(m_db.lastError()));
}

bool VectorExplorerDialog::loadData(const QString& dbPath)
{
    if (!m_db.open(dbPath, /*readOnly=*/true)) return false;

    std::vector<Embedding> items = m_db.loadAll(m_table);
    if (items.empty()) {
        // loadAll() returns an empty list both for an empty table and for a
        // failed read - the error text tells them apart.
        if (!m_db.lastError().isEmpty())
            m_status->setText(tr("Error: %1").arg(m_db.lastError()));
        else
            m_status->setText(tr("No vectors found in '%1'.").arg(m_table));
        return true;
    }

    const std::size_t dims = items.front().values.size();
    std::erase_if(items, [dims](const Embedding& e) { return e.values.size() != dims; });

    const std::vector<Vec3> positions = pcaTo3D(items);
    std::vector<qint64> ids;
    ids.reserve(items.size());
    for (const Embedding& e : items) ids.push_back(e.rowid);

    m_view->setPoints(positions, std::move(ids));
    m_status->setText(tr("%1 vectors, %2 dimensions -> PCA 3D. Click a point to inspect it.")
                          .arg(items.size()).arg(dims));
    return true;
}

void VectorExplorerDialog::highlightRowids(const QSet<qint64>& rowids, const QString& note)
{
    m_view->setHighlighted(QSet<qint64>(rowids));
    m_status->setText(note);
}

void VectorExplorerDialog::onPointClicked(qint64 rowid)
{
    emit recordActivated(rowid);

    const auto emb = m_db.embeddingOf(m_table, rowid);
    if (!emb) { m_status->setText(tr("rowid %1: no embedding found").arg(rowid)); return; }

    const int k = m_k->value();
    const std::vector<Hit> hits = m_db.nearest(m_table, *emb, k + 1);   // +1: the point itself

    QSet<qint64> highlighted;
    QStringList parts;
    for (const Hit& h : hits) {
        highlighted.insert(h.rowid);
        if (h.rowid != rowid && parts.size() < 5)
            parts << QStringLiteral("#%1 (%2)").arg(h.rowid).arg(h.distance, 0, 'f', 4);
    }
    m_view->setHighlighted(std::move(highlighted));
    m_status->setText(tr("rowid %1 - closest: %2").arg(rowid).arg(parts.join(QStringLiteral(", "))));
}

} // namespace vec
