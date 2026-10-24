#pragma once

#include <QGraphicsScene>
#include <QGraphicsView>
#include <QSet>
#include <QString>
#include <QVector>

#include "ForeignKey.h"
#include "JoinCandidate.h"

// Draws the query that is being built, and what can be added to it next:
//
//   * the tables already in the query sit in a row in the middle (the
//     FROM table first), connected by solid lines for each JOIN;
//   * vec0 (vector) tables get their own colour (see vectorTables);
//   * every table that could still be joined (it has a foreign key to a
//     table in the query) is drawn below as a dashed "+ table" node with
//     dashed lines to the table(s) it connects to. Clicking such a node
//     emits joinCandidateClicked().
//
// The view is a pure renderer: it never changes the query itself. The
// owner reacts to joinCandidateClicked() by extending the query and then
// calls setQueryGraph() again, so the diagram and the query can't drift.
class DiagramView : public QGraphicsView
{
    Q_OBJECT

public:
    explicit DiagramView(QWidget* parent = nullptr);

    // queryTables      tables already in the query; the first is the FROM table.
    // joinedRelations  foreign keys that connect tables inside the query
    //                  (drawn as solid lines).
    // candidates       what could be joined next (drawn dashed).
    void setQueryGraph(const QVector<QString>& queryTables,
                       const QVector<ForeignKey>& joinedRelations,
                       const QVector<JoinCandidate>& candidates,
                       const QSet<QString>& vectorTables = {});

    // Empties the diagram (no database open, or no table chosen yet).
    void clear();

signals:
    // A dashed candidate node was clicked.
    void joinCandidateClicked(const QString& tableName);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void fitScene();

    QGraphicsScene m_scene;
};
