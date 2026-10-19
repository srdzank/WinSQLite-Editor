#pragma once

#include <QGraphicsRectItem>
#include <QString>

// A single table box on the diagram. Stores the table name as item data
// so DiagramView can identify which table was clicked without a separate
// QObject signal path (QGraphicsItem doesn't support signals directly).
class TableNode : public QGraphicsRectItem
{
public:
    static constexpr qreal kWidth = 110.0;
    static constexpr qreal kHeight = 44.0;

    enum Style {
        Base,       // the FROM table: solid teal, bold
        Joined,     // a table already JOINed into the query: teal outline
        Candidate,  // a table that could be joined next: dashed, clickable
    };

    TableNode(const QString& tableName, Style style, QGraphicsItem* parent = nullptr);

    QString tableName() const;
    Style style() const { return m_style; }

protected:
    void hoverEnterEvent(QGraphicsSceneHoverEvent* event) override;
    void hoverLeaveEvent(QGraphicsSceneHoverEvent* event) override;

private:
    void applyColors(bool hovered);

    Style m_style;
};
