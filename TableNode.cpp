#include "TableNode.h"

#include <QBrush>
#include <QCursor>
#include <QFont>
#include <QFontMetrics>
#include <QGraphicsSimpleTextItem>
#include <QPen>

namespace {
constexpr int kTableNameRole = 0;
}

TableNode::TableNode(const QString& tableName, Style style, QGraphicsItem* parent)
    : QGraphicsRectItem(0, 0, kWidth, kHeight, parent)
    , m_style(style)
{
    setData(kTableNameRole, tableName);

    // Only candidates react to the mouse; tables already in the query are
    // plain labels (they're removed from the step list on the right).
    if (m_style == Candidate) {
        setCursor(Qt::PointingHandCursor);
        setAcceptHoverEvents(true);
    }

    applyColors(false);

    const bool isCandidate = (m_style == Candidate);
    const QString shown = isCandidate ? QStringLiteral("+  ") + tableName : tableName;

    auto* label = new QGraphicsSimpleTextItem(this);
    QFont font = label->font();
    font.setPointSize(9);
    font.setBold(m_style == Base);
    label->setFont(font);
    label->setText(QFontMetrics(font).elidedText(shown, Qt::ElideRight, int(kWidth) - 12));
    label->setBrush(QBrush(QColor(m_style == Base ? "#E1F5EE"
                                  : m_style == Joined ? "#CFEDE3"
                                                      : "#B4B2A9")));

    const QRectF textBounds = label->boundingRect();
    label->setPos((kWidth - textBounds.width()) / 2.0,
                  (kHeight - textBounds.height()) / 2.0);

    setToolTip(tableName);
}

QString TableNode::tableName() const
{
    return data(kTableNameRole).toString();
}

void TableNode::applyColors(bool hovered)
{
    switch (m_style) {
    case Base:
        setBrush(QBrush(QColor("#0F6E56")));
        setPen(QPen(QColor("#1D9E75"), 1.5));
        break;
    case Joined:
        setBrush(QBrush(QColor("#0B4A3C")));
        setPen(QPen(QColor("#1D9E75"), 1.2));
        break;
    case Candidate:
        setBrush(QBrush(QColor(hovered ? "#2A3A35" : "#1F1F22")));
        setPen(QPen(QColor(hovered ? "#1D9E75" : "#888780"), 1.2, Qt::DashLine));
        break;
    }
}

void TableNode::hoverEnterEvent(QGraphicsSceneHoverEvent* event)
{
    applyColors(true);
    QGraphicsRectItem::hoverEnterEvent(event);
}

void TableNode::hoverLeaveEvent(QGraphicsSceneHoverEvent* event)
{
    applyColors(false);
    QGraphicsRectItem::hoverLeaveEvent(event);
}
