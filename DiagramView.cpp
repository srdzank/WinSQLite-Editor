#include "DiagramView.h"

#include <algorithm>

#include <QGraphicsPathItem>
#include <QHash>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainterPath>
#include <QPen>
#include <QPointF>
#include <QResizeEvent>
#include <QSet>

#include "TableNode.h"

namespace {
constexpr qreal kColSpacing = 150.0;  // centre-to-centre, nodes are 110 wide
constexpr qreal kRowSpacing = 95.0;   // centre-to-centre, nodes are 44 tall
constexpr qreal kMaxZoom = 1.25;      // don't blow a 2-node diagram up to fill the pane

// Order-independent key for a pair of table names, used to draw one line
// per pair even when several foreign keys connect the same two tables.
QString pairKey(const QString& a, const QString& b)
{
    return a < b ? a + QChar('\n') + b : b + QChar('\n') + a;
}
}  // namespace

DiagramView::DiagramView(QWidget* parent)
    : QGraphicsView(parent)
{
    setScene(&m_scene);
    setRenderHint(QPainter::Antialiasing);
    // Scoped to DiagramView itself. An unscoped rule is inherited by every
    // child widget - including the tooltip popup, which then came out with
    // the diagram's near-black background, no border and dark text.
    setStyleSheet("DiagramView { background-color: #17171A; border: none; }");
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scene.setBackgroundBrush(QColor("#17171A"));
}

void DiagramView::clear()
{
    m_scene.clear();
    m_scene.setSceneRect(0, 0, 330, 240);
}

void DiagramView::setQueryGraph(const QVector<QString>& queryTables,
                                const QVector<ForeignKey>& joinedRelations,
                                const QVector<JoinCandidate>& candidates)
{
    m_scene.clear();
    if (queryTables.isEmpty()) {
        m_scene.setSceneRect(0, 0, 330, 240);
        return;
    }

    // ---- Layout (all positions are node centres) -----------------------
    QHash<QString, QPointF> centers;
    QHash<QString, int> queryIndex;
    const int queryCount = queryTables.size();
    for (int i = 0; i < queryCount; ++i) {
        centers.insert(queryTables[i], QPointF(i * kColSpacing, 0));
        queryIndex.insert(queryTables[i], i);
    }
    const qreal rowMidX = (queryCount - 1) * kColSpacing / 2.0;

    // One node per candidate table, remembering every query table it
    // could connect to and the relations behind that.
    struct Candidate
    {
        QString table;
        int leftmostAnchor = 0;
        QVector<JoinCandidate> relations;
    };
    QVector<Candidate> unique;
    QHash<QString, int> uniqueIndex;
    for (const auto& c : candidates) {
        if (!queryIndex.contains(c.anchorTable) || queryIndex.contains(c.table)) {
            continue;
        }
        const int anchor = queryIndex.value(c.anchorTable);
        auto it = uniqueIndex.find(c.table);
        if (it == uniqueIndex.end()) {
            uniqueIndex.insert(c.table, unique.size());
            unique.push_back({c.table, anchor, {c}});
        } else {
            Candidate& existing = unique[it.value()];
            existing.leftmostAnchor = std::min(existing.leftmostAnchor, anchor);
            existing.relations.push_back(c);
        }
    }
    // Candidates hanging off tables further left are placed further left,
    // which keeps the dashed lines from crossing in the common case.
    std::stable_sort(unique.begin(), unique.end(), [](const Candidate& a, const Candidate& b) {
        if (a.leftmostAnchor != b.leftmostAnchor) {
            return a.leftmostAnchor < b.leftmostAnchor;
        }
        return a.table < b.table;
    });

    // Candidate rows below the query row, centred under it, wrapping so
    // a table with many relations doesn't make the diagram a mile wide.
    const int columns = std::max(queryCount, 3);
    for (int k = 0; k < unique.size(); ++k) {
        const int row = k / columns;
        const int col = k % columns;
        const int inThisRow = std::min(columns, int(unique.size()) - row * columns);
        const qreal x = rowMidX + (col - (inThisRow - 1) / 2.0) * kColSpacing;
        centers.insert(unique[k].table, QPointF(x, (row + 1) * kRowSpacing));
    }

    // ---- Edges (added first so the nodes paint over their ends) --------
    const QPen joinedPen(QColor("#1D9E75"), 1.6);
    QPen candidatePen(QColor("#888780"), 1.1, Qt::DashLine);

    QSet<QString> drawnPairs;
    for (const auto& fk : joinedRelations) {
        if (fk.fromTable == fk.toTable || !centers.contains(fk.fromTable) || !centers.contains(fk.toTable)) {
            continue;
        }
        if (!queryIndex.contains(fk.fromTable) || !queryIndex.contains(fk.toTable)) {
            continue;
        }
        const QString key = pairKey(fk.fromTable, fk.toTable);
        if (drawnPairs.contains(key)) {
            continue;
        }
        drawnPairs.insert(key);

        const int ia = queryIndex.value(fk.fromTable);
        const int ib = queryIndex.value(fk.toTable);
        const QPointF a = centers.value(fk.fromTable);
        const QPointF b = centers.value(fk.toTable);
        const int span = std::abs(ia - ib);

        QPainterPath path;
        if (span == 1) {
            // Neighbours in the row: a plain line across the gap.
            path.moveTo(a);
            path.lineTo(b);
        } else {
            // Further apart: arc over the top so the line doesn't run
            // underneath the tables in between.
            const qreal top = -TableNode::kHeight / 2.0;
            path.moveTo(a.x(), top);
            path.quadTo((a.x() + b.x()) / 2.0, top - 36.0 * span, b.x(), top);
        }
        auto* edge = m_scene.addPath(path, joinedPen);
        edge->setToolTip(QString("%1.%2 = %3.%4").arg(fk.fromTable, fk.fromColumn, fk.toTable, fk.toColumn));
    }

    for (const auto& cand : unique) {
        QSet<QString> anchors;
        for (const auto& rel : cand.relations) {
            if (anchors.contains(rel.anchorTable)) {
                continue;
            }
            anchors.insert(rel.anchorTable);
            m_scene.addLine(QLineF(centers.value(cand.table), centers.value(rel.anchorTable)), candidatePen);
        }
    }

    // ---- Nodes ---------------------------------------------------------
    auto addNode = [this, &centers](const QString& name, TableNode::Style style) {
        auto* node = new TableNode(name, style);
        const QPointF c = centers.value(name);
        node->setPos(c.x() - TableNode::kWidth / 2.0, c.y() - TableNode::kHeight / 2.0);
        m_scene.addItem(node);
        return node;
    };

    for (int i = 0; i < queryCount; ++i) {
        addNode(queryTables[i], i == 0 ? TableNode::Base : TableNode::Joined);
    }
    for (const auto& cand : unique) {
        TableNode* node = addNode(cand.table, TableNode::Candidate);

        QStringList lines;
        for (const auto& rel : cand.relations) {
            const auto& fk = rel.relation;
            lines << QString("%1.%2 = %3.%4").arg(fk.fromTable, fk.fromColumn, fk.toTable, fk.toColumn);
        }
        node->setToolTip(QString("Click to JOIN %1\n%2").arg(cand.table, lines.join("\n")));
    }

    m_scene.setSceneRect(m_scene.itemsBoundingRect().adjusted(-24, -24, 24, 24));
    fitScene();
}

void DiagramView::fitScene()
{
    const QRectF rect = m_scene.sceneRect();
    if (rect.isEmpty()) {
        return;
    }
    fitInView(rect, Qt::KeepAspectRatio);
    if (transform().m11() > kMaxZoom) {
        resetTransform();
        scale(kMaxZoom, kMaxZoom);
        centerOn(rect.center());
    }
}

void DiagramView::resizeEvent(QResizeEvent* event)
{
    QGraphicsView::resizeEvent(event);
    fitScene();
}

void DiagramView::mousePressEvent(QMouseEvent* event)
{
    QString clickedCandidate;
    if (event->button() == Qt::LeftButton) {
        if (QGraphicsItem* item = itemAt(event->pos())) {
            if (auto* node = dynamic_cast<TableNode*>(item->topLevelItem())) {
                if (node->style() == TableNode::Candidate) {
                    clickedCandidate = node->tableName();
                }
            }
        }
    }

    QGraphicsView::mousePressEvent(event);

    if (!clickedCandidate.isEmpty()) {
        // Queued: the owner will rebuild the whole scene in response,
        // which destroys the node this press event came from. Doing that
        // from inside the event would be use-after-free territory.
        QMetaObject::invokeMethod(this, [this, clickedCandidate]() {
            emit joinCandidateClicked(clickedCandidate);
        }, Qt::QueuedConnection);
    }
}
