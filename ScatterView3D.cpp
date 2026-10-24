#include "ScatterView3D.h"

#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace vec {
namespace {

constexpr float kCamDist    = 3.5f;   // camera sits at z = +kCamDist looking at the origin
constexpr float kBaseRadius = 3.5f;   // px at z = 0
constexpr float kPickRadius = 9.0f;   // px
constexpr float kMaxPitch   = std::numbers::pi_v<float> / 2.f - 0.05f;

struct Camera { float cy, sy, cp, sp, focal, cx, cyScreen; };

Camera makeCamera(const QSize& size, float yaw, float pitch, float zoom)
{
    return {std::cos(yaw), std::sin(yaw), std::cos(pitch), std::sin(pitch),
            0.4f * float(std::min(size.width(), size.height())) * kCamDist * zoom,
            size.width() * 0.5f, size.height() * 0.5f};
}

// Rotate (yaw around Y, then pitch around X) and perspective-divide.
bool toScreen(const Camera& c, const Vec3& p, float& sx, float& sy, float& depth)
{
    const float x1 =  c.cy * p.x + c.sy * p.z;
    const float z1 = -c.sy * p.x + c.cy * p.z;
    const float y2 =  c.cp * p.y - c.sp * z1;
    const float z2 =  c.sp * p.y + c.cp * z1;
    depth = kCamDist - z2;
    if (depth < 0.1f) return false;               // behind the camera
    const float k = c.focal / depth;
    sx = c.cx + x1 * k;
    sy = c.cyScreen - y2 * k;
    return true;
}

} // namespace

ScatterView3D::ScatterView3D(QWidget* parent) : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(false);
}

void ScatterView3D::setPoints(std::vector<Vec3> positions, std::vector<qint64> rowids)
{
    Q_ASSERT(positions.size() == rowids.size());
    m_pos = std::move(positions);
    m_ids = std::move(rowids);
    m_indexById.clear();
    m_indexById.reserve(int(m_ids.size()));
    for (int i = 0; i < int(m_ids.size()); ++i) m_indexById.insert(m_ids[std::size_t(i)], i);
    m_highlighted.clear();
    m_selected.reset();
    m_proj.clear();
    m_hover = -1;
    update();
}

void ScatterView3D::setHighlighted(QSet<qint64> rowids) { m_highlighted = std::move(rowids); update(); }
void ScatterView3D::setSelected(std::optional<qint64> rowid) { m_selected = rowid; update(); }

void ScatterView3D::resetCamera() { m_yaw = 0.6f; m_pitch = 0.35f; m_zoom = 1.0f; update(); }

void ScatterView3D::project()
{
    const Camera cam = makeCamera(size(), m_yaw, m_pitch, m_zoom);
    m_proj.clear();
    m_proj.reserve(m_pos.size());
    for (std::size_t i = 0; i < m_pos.size(); ++i) {
        Projected pr{};
        if (toScreen(cam, m_pos[i], pr.sx, pr.sy, pr.depth)) {
            pr.index = int(i);
            m_proj.push_back(pr);
        }
    }
    // painter's algorithm: far first, so near points end up on top
    std::ranges::sort(m_proj, [](const Projected& a, const Projected& b) { return a.depth > b.depth; });
}

int ScatterView3D::pickAt(const QPointF& pos) const
{
    int   best   = -1;
    float bestD2 = kPickRadius * kPickRadius;
    for (auto it = m_proj.rbegin(); it != m_proj.rend(); ++it) {   // nearest first
        const float dx = it->sx - float(pos.x());
        const float dy = it->sy - float(pos.y());
        const float d2 = dx * dx + dy * dy;
        if (d2 < bestD2) { bestD2 = d2; best = it->index; }
    }
    return best;
}

void ScatterView3D::drawAxes(QPainter& p)
{
    const Camera cam = makeCamera(size(), m_yaw, m_pitch, m_zoom);
    float ox, oy, od;
    if (!toScreen(cam, {}, ox, oy, od)) return;

    const std::array<std::pair<Vec3, const char*>, 3> axes{{
        {{1.1f, 0.f, 0.f}, "PC1"}, {{0.f, 1.1f, 0.f}, "PC2"}, {{0.f, 0.f, 1.1f}, "PC3"}}};

    p.setPen(QPen(palette().mid().color(), 1));
    for (const auto& [end, name] : axes) {
        float sx, sy, d;
        if (!toScreen(cam, end, sx, sy, d)) continue;
        p.drawLine(QPointF(ox, oy), QPointF(sx, sy));
        p.drawText(QPointF(sx + 4, sy - 4), QString::fromLatin1(name));
    }
}

void ScatterView3D::drawRing(QPainter& p, int index, const QColor& color, qreal width)
{
    if (index < 0) return;
    const auto it = std::ranges::find(m_proj, index, &Projected::index);
    if (it == m_proj.end()) return;
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(color, width));
    const qreal r = kBaseRadius * (kCamDist / it->depth) + 5.0;
    p.drawEllipse(QPointF(it->sx, it->sy), r, r);
}

void ScatterView3D::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), palette().base());

    project();
    drawAxes(p);

    p.setPen(Qt::NoPen);
    for (const Projected& pr : m_proj) {
        const bool  hl = m_highlighted.contains(m_ids[std::size_t(pr.index)]);
        const float s  = kCamDist / pr.depth;                       // perspective scale
        const float r  = kBaseRadius * s * (hl ? 1.7f : 1.0f);
        QColor c = hl ? QColor(255, 140, 0) : QColor(70, 130, 200);
        c.setAlphaF(std::clamp(s * 0.9f - 0.1f, 0.25f, 1.0f));      // depth fog
        p.setBrush(c);
        p.drawEllipse(QPointF(pr.sx, pr.sy), r, r);
    }

    drawRing(p, m_hover, palette().text().color(), 1.5);
    if (m_selected)
        drawRing(p, m_indexById.value(*m_selected, -1), QColor(220, 40, 40), 2.5);

    p.setPen(palette().mid().color());
    p.drawText(8, height() - 8,
               tr("%1 points   |   drag: rotate   wheel: zoom   double-click: reset").arg(m_ids.size()));
}

void ScatterView3D::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) return;
    m_dragging = true;
    m_moved    = false;
    m_pressPos = m_lastPos = e->position();
}

void ScatterView3D::mouseMoveEvent(QMouseEvent* e)
{
    const QPointF pos = e->position();
    if (m_dragging && (e->buttons() & Qt::LeftButton)) {
        const QPointF delta = pos - m_lastPos;
        m_lastPos = pos;
        m_yaw   += float(delta.x()) * 0.01f;
        m_pitch  = std::clamp(m_pitch + float(delta.y()) * 0.01f, -kMaxPitch, kMaxPitch);
        if ((pos - m_pressPos).manhattanLength() > 4) m_moved = true;
        QToolTip::hideText();
        update();
    } else {
        updateHover(pos);
    }
}

void ScatterView3D::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton || !m_dragging) return;
    m_dragging = false;
    if (!m_moved) {                                    // a click, not a drag
        const int idx = pickAt(e->position());
        if (idx >= 0) {
            m_selected = m_ids[std::size_t(idx)];
            update();
            emit pointClicked(*m_selected);
        }
    }
    m_moved = false;
}

void ScatterView3D::mouseDoubleClickEvent(QMouseEvent*) { resetCamera(); }

void ScatterView3D::wheelEvent(QWheelEvent* e)
{
    m_zoom = std::clamp(m_zoom * std::pow(1.0015f, float(e->angleDelta().y())), 0.2f, 20.0f);
    update();
}

void ScatterView3D::leaveEvent(QEvent*)
{
    if (m_hover != -1) { m_hover = -1; QToolTip::hideText(); update(); }
}

void ScatterView3D::updateHover(const QPointF& pos)
{
    const int idx = pickAt(pos);
    if (idx == m_hover) return;
    m_hover = idx;
    setCursor(idx >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    update();

    if (idx < 0) { QToolTip::hideText(); return; }
    const qint64 id = m_ids[std::size_t(idx)];
    emit pointHovered(id);
    const QString text = m_label ? m_label(id) : tr("rowid %1").arg(id);
    QToolTip::showText(mapToGlobal(pos.toPoint()), text, this);
}

} // namespace vec
