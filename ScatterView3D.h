#pragma once
// Lightweight interactive 3D scatter plot: plain QWidget + QPainter, software
// perspective projection. No OpenGL / Qt3D dependency; picking is exact because
// we hit-test the same screen positions we draw.
// Comfortable up to ~50k points. Beyond that, port paintEvent to QOpenGLWidget
// (GL_POINTS) and keep project()/pickAt() for hit-testing.

#include "Projection.h"

#include <QHash>
#include <QSet>
#include <QWidget>
#include <functional>
#include <optional>
#include <vector>

namespace vec {

class ScatterView3D : public QWidget {
    Q_OBJECT
public:
    using LabelFn = std::function<QString(qint64 rowid)>;

    explicit ScatterView3D(QWidget* parent = nullptr);

    // positions[i] belongs to rowids[i]; both must have the same size.
    void setPoints(std::vector<Vec3> positions, std::vector<qint64> rowids);
    void setHighlighted(QSet<qint64> rowids);       // e.g. KNN results
    void setSelected(std::optional<qint64> rowid);
    void setLabelProvider(LabelFn fn) { m_label = std::move(fn); } // tooltip text, GUI thread
    void resetCamera();

    QSize sizeHint() const override { return {720, 540}; }

signals:
    void pointHovered(qint64 rowid);
    void pointClicked(qint64 rowid);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    struct Projected { float sx, sy, depth; int index; };

    void project();                     // fills m_proj, sorted far -> near
    int  pickAt(const QPointF& pos) const;
    void updateHover(const QPointF& pos);
    void drawAxes(QPainter& p);
    void drawRing(QPainter& p, int index, const QColor& color, qreal width);

    std::vector<Vec3>    m_pos;
    std::vector<qint64>  m_ids;
    QHash<qint64, int>   m_indexById;
    QSet<qint64>         m_highlighted;
    std::optional<qint64> m_selected;
    std::vector<Projected> m_proj;
    LabelFn              m_label;

    float  m_yaw = 0.6f, m_pitch = 0.35f, m_zoom = 1.0f;
    int    m_hover = -1;
    bool   m_dragging = false, m_moved = false;
    QPointF m_pressPos, m_lastPos;
};

} // namespace vec
