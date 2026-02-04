/*
 * Audacity: A Digital Audio Editor
 */
#include "editablepolyline.h"

#include <QPainter>
#include <QPen>
#include <QBrush>
#include <QHoverEvent>
#include <QMouseEvent>

#include <algorithm>
#include <cmath>

namespace {
constexpr double MOVE_THRESHOLD = 3.0;

static inline qreal toPxX(const QQuickItem* item, qreal xN)
{
    return xN * item->width();
}

static inline qreal toPxY(const QQuickItem* item, qreal yN)
{
    return (1.0 - yN) * item->height();
}

static qreal distPointToSegment(const QPointF& p, const QPointF& a, const QPointF& b)
{
    const QPointF ab = b - a;
    const QPointF ap = p - a;

    const qreal ab2 = QPointF::dotProduct(ab, ab);
    if (ab2 <= 1e-9) {
        return std::hypot(p.x() - a.x(), p.y() - a.y());
    }

    qreal t = QPointF::dotProduct(ap, ab) / ab2;
    t = std::max<qreal>(0.0, std::min<qreal>(1.0, t));
    const QPointF c = a + ab * t;

    return std::hypot(p.x() - c.x(), p.y() - c.y());
}

static GhostPoint ghostPointToSegment(const QPointF& p, const QPointF& a, const QPointF& b)
{
    const QPointF ab = b - a;
    const qreal ab2 = QPointF::dotProduct(ab, ab);
    if (ab2 <= 1e-9) {
        const qreal d = std::hypot(p.x() - a.x(), p.y() - a.y());
        return { a, d };
    }

    const QPointF ap = p - a;
    qreal t = QPointF::dotProduct(ap, ab) / ab2;
    t = std::max<qreal>(0.0, std::min<qreal>(1.0, t));

    const QPointF c = a + ab * t;
    const qreal d = std::hypot(p.x() - c.x(), p.y() - c.y());
    return { c, d };
}

static inline double clamp01d(double v) { return std::max(0.0, std::min(1.0, v)); }

static double lerp(double a, double b, double t) { return a + (b - a) * t; }

// Evaluate yAbs at xAbs on the envelope defined by sorted points (xAbs, yAbs)
// With "envelope style": before first point => first y, after last => last y.
static double evalEnvelopeY(const QVector<QPointF>& sortedAbs, double xAbs)
{
    if (sortedAbs.isEmpty()) {
        return 0.0; // caller should handle baseline/default
    }
    if (xAbs <= sortedAbs.front().x()) {
        return sortedAbs.front().y();
    }
    if (xAbs >= sortedAbs.back().x()) {
        return sortedAbs.back().y();
    }

    for (int i = 0; i < sortedAbs.size() - 1; ++i) {
        const auto& a = sortedAbs[i];
        const auto& b = sortedAbs[i + 1];
        if (xAbs >= a.x() && xAbs <= b.x()) {
            const double dx = b.x() - a.x();
            if (std::abs(dx) <= 1e-12) {
                return a.y();
            }
            const double t = (xAbs - a.x()) / dx;
            return lerp(a.y(), b.y(), t);
        }
    }
    return sortedAbs.back().y();
}
}

EditablePolyline::EditablePolyline(QQuickItem* parent)
    : QQuickPaintedItem(parent)
{
    setAcceptHoverEvents(true);
    setAcceptedMouseButtons(Qt::LeftButton);

    setAntialiasing(true);
    setRenderTarget(QQuickPaintedItem::FramebufferObject);
    setOpaquePainting(false);
}

QColor EditablePolyline::lineColor() const
{
    return m_lineColor;
}

void EditablePolyline::setLineColor(const QColor& c)
{
    if (m_lineColor == c) {
        return;
    }
    m_lineColor = c;
    emit lineColorChanged();
    update();
}

qreal EditablePolyline::lineWidth() const
{
    return m_lineWidth;
}

void EditablePolyline::setLineWidth(qreal w)
{
    w = std::max<qreal>(0.5, w);

    if (m_lineWidth == w) {
        return;
    }

    m_lineWidth = w;
    emit lineWidthChanged();

    update();
}

qreal EditablePolyline::baselineN() const
{
    return m_baselineN;
}

void EditablePolyline::setBaselineN(qreal v)
{
    v = clamp01(v);

    if (m_baselineN == v) {
        return;
    }

    m_baselineN = v;

    emit baselineNChanged();
    update();
}

qreal EditablePolyline::pointRadius() const
{
    return m_pointRadius;
}

void EditablePolyline::setPointRadius(qreal r)
{
    r = std::max<qreal>(1.0, r);

    if (m_pointRadius == r) {
        return;
    }

    m_pointRadius = r;
    emit pointRadiusChanged();

    update();
}

qreal EditablePolyline::hitRadius() const
{
    return m_hitRadius;
}

void EditablePolyline::setHitRadius(qreal r)
{
    r = std::max<qreal>(2.0, r);

    if (m_hitRadius == r) {
        return;
    }

    m_hitRadius = r;
    emit hitRadiusChanged();
}

QVector<QPointF> EditablePolyline::points() const
{
    return m_points;
}

void EditablePolyline::setPointsN(const QVector<QPointF>& pts)
{
    if (m_pointsN == pts) {
        return;
    }
    m_pointsN = pts;

    // if first point exists, keep baseline aligned with it
    if (m_pointsN.size() == 1) {
        m_baselineN = clamp01(m_pointsN[0].y());
        emit baselineNChanged();
    }

    emit pointsNChanged();
    update();
}

qreal EditablePolyline::defaultValue() const
{
    return m_defaultValue;
}

void EditablePolyline::setXRangeFrom(qreal v)
{
    if (m_xFrom == v) {
        return;
    }
    m_xFrom = v;
    emit xRangeFromChanged();
    rebuildNormalizedFromDomain();
    rebuildVisiblePoints();
}

qreal EditablePolyline::xRangeTo() const
{
    return m_xTo;
}

void EditablePolyline::setXRangeTo(qreal v)
{
    if (m_xTo == v) {
        return;
    }
    m_xTo = v;
    emit xRangeToChanged();
    rebuildNormalizedFromDomain();
    rebuildVisiblePoints();
}

qreal EditablePolyline::yRangeFrom() const
{
    return m_yFrom;
}

void EditablePolyline::setYRangeFrom(qreal v)
{
    if (m_yFrom == v) {
        return;
    }
    m_yFrom = v;
    emit yRangeFromChanged();
    rebuildNormalizedFromDomain();
    rebuildVisiblePoints();
}

qreal EditablePolyline::yRangeTo() const
{
    return m_yTo;
}

void EditablePolyline::setYRangeTo(qreal v)
{
    if (m_yTo == v) {
        return;
    }
    m_yTo = v;
    emit yRangeToChanged();

    rebuildNormalizedFromDomain();
    rebuildVisiblePoints();
}

bool EditablePolyline::yAxisInverse() const
{
    return m_yAxisInverse;
}

void EditablePolyline::setYAxisInverse(bool v)
{
    if (m_yAxisInverse == v) {
        return;
    }
    m_yAxisInverse = v;
    emit yAxisInverseChanged();
    rebuildNormalizedFromDomain();
}

qreal EditablePolyline::dragX() const
{
    return m_dragX;
}

void EditablePolyline::setDragX(qreal v)
{
    if (m_dragX == v) {
        return;
    }

    m_dragX = v;
    emit dragXChanged();
}

qreal EditablePolyline::dragY() const
{
    return m_dragY;
}

void EditablePolyline::setDragY(qreal v)
{
    if (m_dragY == v) {
        return;
    }

    m_dragY = v;
    emit dragYChanged();
}

void EditablePolyline::setPoints(const QVector<QPointF>& pts)
{
    if (m_points == pts) {
        return;
    }
    m_points = pts;
    emit pointsChanged();

    if (m_points.isEmpty()) {
        m_baselineN = normalizedFromDomain(QPointF(m_xFrom, m_defaultValue)).y();
        emit baselineNChanged();
    }

    rebuildNormalizedFromDomain();
    rebuildVisiblePoints();
}

QVector<QPointF> EditablePolyline::pointsN() const
{
    return m_pointsN;
}

void EditablePolyline::setDefaultValue(qreal v)
{
    if (m_defaultValue == v) {
        return;
    }

    m_defaultValue = v;
    emit defaultValueChanged();

    // If there are no points, baseline should reflect defaultY immediately
    if (m_points.isEmpty()) {
        // Convert domain baseline to normalized baseline
        m_baselineN = normalizedFromDomain(QPointF(m_xFrom, m_defaultValue)).y();
        emit baselineNChanged();
        rebuildVisiblePoints();
    }
}

qreal EditablePolyline::xRangeFrom() const
{
    return m_xFrom;
}

qreal EditablePolyline::clamp01(qreal v) const
{
    return std::max<qreal>(0.0, std::min<qreal>(1.0, v));
}

QPointF EditablePolyline::clamp01(const QPointF& p) const
{
    return QPointF(clamp01(p.x()), clamp01(p.y()));
}

bool EditablePolyline::hasValidXRange() const
{
    return std::isfinite(m_xFrom) && std::isfinite(m_xTo) && std::abs(m_xTo - m_xFrom) > 0;
}

bool EditablePolyline::hasValidYRange() const
{
    return std::isfinite(m_yFrom) && std::isfinite(m_yTo) && std::abs(m_yTo - m_yFrom) > 0;
}

QPointF EditablePolyline::normalizedFromDomain(const QPointF& p) const
{
    if (!hasValidXRange() || !hasValidYRange()) {
        return clamp01(QPointF(0.0, 0.0));
    }

    const qreal xN = (p.x() - m_xFrom) / (m_xTo - m_xFrom);
    qreal yN = (p.y() - m_yFrom) / (m_yTo - m_yFrom);

    if (m_yAxisInverse) {
        yN = 1.0 - yN;
    }

    return clamp01(QPointF(xN, yN));
}

QPointF EditablePolyline::domainFromNormalized(const QPointF& pN) const
{
    if (!hasValidXRange() || !hasValidYRange()) {
        return QPointF(m_xFrom, m_yFrom);
    }

    const qreal x = m_xFrom + clamp01(pN.x()) * (m_xTo - m_xFrom);

    qreal yT = clamp01(pN.y());
    if (m_yAxisInverse) {
        yT = 1.0 - yT;
    }
    const qreal y = m_yFrom + yT * (m_yTo - m_yFrom);

    return QPointF(x, y);
}

QVector<QPointF> EditablePolyline::normalizedFromDomain(const QVector<QPointF>& pts) const
{
    QVector<QPointF> out;
    out.reserve(pts.size());
    for (const auto& p : pts) {
        out.push_back(normalizedFromDomain(p));
    }
    return out;
}

QVector<QPointF> EditablePolyline::domainFromNormalized(const QVector<QPointF>& ptsN) const
{
    QVector<QPointF> out;
    out.reserve(ptsN.size());
    for (const auto& pN : ptsN) {
        out.push_back(domainFromNormalized(pN));
    }
    return out;
}

void EditablePolyline::rebuildNormalizedFromDomain()
{
    // Update internal normalized cache used for drawing and hit-tests
    const auto newPtsN = normalizedFromDomain(m_points);
    setPointsN(newPtsN); // uses your existing setter (emits pointsNChanged/update)
}

void EditablePolyline::rebuildVisiblePoints()
{
    m_pointsNVisible.clear();
    m_visibleToDomainIndex.clear();

    const double x0 = m_xFrom;
    const double x1 = m_xTo;
    const double y0 = m_yFrom;
    const double y1 = m_yTo;

    const double xDen = (x1 - x0);
    const double yDen = (y1 - y0);

    if (width() <= 0 || height() <= 0 || std::abs(xDen) <= 0 || std::abs(yDen) <= 0) {
        update();
        return;
    }

    if (m_points.isEmpty()) {
        update();
        return;
    }

    struct P {
        QPointF p;
        int idx;
    };

    QVector<P> sorted;
    sorted.reserve(m_points.size());
    for (int i = 0; i < m_points.size(); ++i) {
        sorted.push_back({ m_points[i], i });
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const P& a, const P& b) { return a.p.x() < b.p.x(); });

    auto normY = [&](double yAbs) {
        double yn = (yAbs - y0) / yDen;
        if (m_yAxisInverse) {
            yn = 1.0 - yn;
        }
        return clamp01d(yn);
    };

    // interpolate at window edges using the sortedAbs vector (no indices needed there)
    QVector<QPointF> sortedAbs;
    sortedAbs.reserve(sorted.size());
    for (const auto& it : sorted) {
        sortedAbs.push_back(it.p);
    }

    const double yAt0 = evalEnvelopeY(sortedAbs, x0);
    const double yAt1 = evalEnvelopeY(sortedAbs, x1);

    // left boundary (synthetic)
    m_pointsNVisible.push_back(QPointF(-0.1, normY(yAt0)));
    m_visibleToDomainIndex.push_back(-1);

    // interior real points
    for (const auto& it : sorted) {
        const auto& p = it.p;
        if (p.x() <= x0 || p.x() >= x1) {
            continue;
        }
        const double xN = (p.x() - x0) / xDen;
        m_pointsNVisible.push_back(QPointF(clamp01d(xN), normY(p.y())));
        m_visibleToDomainIndex.push_back(it.idx);
    }

    // right boundary (synthetic)
    m_pointsNVisible.push_back(QPointF(1.1, normY(yAt1)));
    m_visibleToDomainIndex.push_back(-1);

    update();
}

QVector<QPointF> EditablePolyline::polylinePx() const
{
    QVector<QPointF> pts;

    if (width() <= 0 || height() <= 0) {
        return pts;
    }

    // 0 or 1 point -> horizontal baseline
    if (m_pointsNVisible.size() < 2) {
        qreal yN = m_baselineN;
        if (m_pointsNVisible.size() == 1) {
            yN = m_pointsNVisible[0].y();
        }
        yN = clamp01(yN);

        const qreal y = toPxY(this, yN);
        pts.push_back(QPointF(0.0, y));
        pts.push_back(QPointF(width(), y));
        return pts;
    }

    // 2+ points -> envelope behavior:
    QVector<QPointF> sorted = m_pointsNVisible;
    std::sort(sorted.begin(), sorted.end(),
              [](const QPointF& a, const QPointF& b) { return a.x() < b.x(); });

    const QPointF firstN = sorted.front();
    const QPointF lastN  = sorted.back();

    const qreal firstYpx = toPxY(this, clamp01(firstN.y()));
    const qreal lastYpx  = toPxY(this, clamp01(lastN.y()));

    pts.reserve(sorted.size() + 2);

    // left horizontal segment start
    pts.push_back(QPointF(0.0, firstYpx));

    // actual points
    for (const auto& pN : sorted) {
        pts.push_back(QPointF(toPxX(this, clamp01(pN.x())),
                              toPxY(this, clamp01(pN.y()))));
    }

    // right horizontal segment end
    pts.push_back(QPointF(width(), lastYpx));

    return pts;
}

bool EditablePolyline::isNearLinePx(const QPointF& px) const
{
    const auto pts = polylinePx();
    if (pts.size() < 2) {
        return false;
    }

    qreal best = 1e18;
    for (int i = 0; i < pts.size() - 1; ++i) {
        best = std::min(best, distPointToSegment(px, pts[i], pts[i + 1]));
    }
    return best <= m_hitRadius;
}

int EditablePolyline::pointIndexAtPx(const QPointF& px) const
{
    // search in visible points but only those with real indices
    for (int i = 0; i < m_pointsNVisible.size(); ++i) {
        const int domainIdx = (i < m_visibleToDomainIndex.size()) ? m_visibleToDomainIndex[i] : -1;
        if (domainIdx < 0) {
            continue; // skip synthetic boundary points
        }

        QPointF pN = m_pointsNVisible[i];
        // apply preview override if this is the dragged one
        if (m_hasPreview && domainIdx == m_previewDomainIndex) {
            pN = m_previewPointN;
        }

        const qreal x = toPxX(this, pN.x());
        const qreal y = toPxY(this, pN.y());
        const qreal dx = px.x() - x;
        const qreal dy = px.y() - y;
        if ((dx * dx + dy * dy) <= (m_hitRadius * m_hitRadius)) {
            return domainIdx;
        }
    }
    return -1;
}

GhostPoint EditablePolyline::ghostPointToPolylinePx(const QPointF& px) const
{
    GhostPoint best;

    const auto pts = polylinePx();
    if (pts.size() < 2) {
        best.point = px;
        best.dist = 1e18;
        return best;
    }

    for (int i = 0; i < pts.size() - 1; ++i) {
        const auto res = ghostPointToSegment(px, pts[i], pts[i + 1]);
        if (res.dist < best.dist) {
            best = res;
        }
    }

    return best;
}

void EditablePolyline::updateCursor()
{
    const bool interactive = m_hoveredOnLine || m_pressed || m_draggingLine || (m_pressedPointIndex >= 0);

    if (interactive) {
        setCursor(Qt::ArrowCursor);
    } else {
        unsetCursor();
    }
}

void EditablePolyline::resetGestureState()
{
    m_pressed = false;
    m_pressedOnLine = false;
    m_pressedOnPoint = false;
    m_pressedPointIndex = -1;
    m_draggingLine = false;

    m_movedSincePress = false;
    m_pressPx = QPointF(0.0, 0.0);

    m_hasPreview = false;
    m_previewDomainIndex = -1;

    updateCursor();
    update();
}

void EditablePolyline::geometryChange(const QRectF& newG, const QRectF& oldG)
{
    QQuickPaintedItem::geometryChange(newG, oldG);
    if (newG.size() != oldG.size()) {
        rebuildVisiblePoints();
    }
}

void EditablePolyline::paint(QPainter* painter)
{
    if (!painter) {
        return;
    }

    painter->setRenderHint(QPainter::Antialiasing, antialiasing());

    // draw line/polyline
    {
        QPen pen(m_lineColor);
        pen.setWidthF(m_lineWidth);
        pen.setCapStyle(Qt::FlatCap);
        pen.setJoinStyle(Qt::MiterJoin);
        painter->setPen(pen);
        painter->setBrush(Qt::NoBrush);

        const auto pts = polylinePx();
        if (pts.size() >= 2) {
            for (int i = 0; i < pts.size() - 1; ++i) {
                painter->drawLine(pts[i], pts[i + 1]);
            }
        }
    }

    // draw permanent dots (with preview override)
    painter->setPen(Qt::NoPen);
    painter->setBrush(m_lineColor);

    const int n = m_pointsNVisible.size();
    for (int i = 0; i < n; ++i) {
        QPointF pN = m_pointsNVisible[i];

        if (m_hasPreview
            && i < m_visibleToDomainIndex.size()
            && m_visibleToDomainIndex[i] == m_previewDomainIndex) {
            pN = m_previewPointN; // paint-only
        }

        const QPointF c(toPxX(this, pN.x()), toPxY(this, pN.y()));
        painter->drawEllipse(c, m_pointRadius, m_pointRadius);
    }

    // hover point
    if (m_hoveredOnLine && !m_draggingLine && m_pressedPointIndex < 0) {
        QPointF hp = m_hoverGhostPx;

        // keep hover point on baseline when 0/1 point
        if (m_pointsNVisible.size() < 2) {
            const qreal yN
                =(m_pointsNVisible.size() == 1)
                  ? m_pointsNVisible[0].y()
                  : (m_hasPreview ? m_previewBaselineN : m_baselineN);   // <-- change #1

            hp.setY(toPxY(this, yN));
        }

        if (pointIndexAtPx(hp) < 0 && isNearLinePx(hp)) {
            const qreal eraseRadius = m_pointRadius + m_lineWidth * 0.75;

            // erase underlying line
            painter->save();
            painter->setCompositionMode(QPainter::CompositionMode_Clear);
            painter->setPen(Qt::NoPen);
            painter->setBrush(Qt::transparent);
            painter->drawEllipse(hp, eraseRadius, eraseRadius);
            painter->restore();

            // draw hollow outline
            QPen hoverPen(m_lineColor);
            hoverPen.setWidthF(1.0);
            hoverPen.setCapStyle(Qt::RoundCap);
            hoverPen.setJoinStyle(Qt::RoundJoin);

            painter->setPen(hoverPen);
            painter->setBrush(Qt::NoBrush);
            painter->drawEllipse(hp, m_pointRadius, m_pointRadius);
        }
    }
}

// ------------------------- Hover events -------------------------

void EditablePolyline::hoverMoveEvent(QHoverEvent* e)
{
    m_hoverPx = e->position();

    // interactive if near point or near line
    const bool nearPoint = (pointIndexAtPx(m_hoverPx) >= 0);

    auto proj = ghostPointToPolylinePx(m_hoverPx);
    const bool nearLine = (proj.dist <= m_hitRadius);

    m_hoveredOnLine = (nearPoint || nearLine);
    updateCursor();

    // for 2+ points, hover circle should follow the polyline
    // for 0/1 point, hover circle can follow the baseline
    if (m_pointsNVisible.size() >= 2) {
        m_hoverGhostPx = proj.point;
    } else {
        m_hoverGhostPx = m_hoverPx;
    }

    update();
    e->accept();
}

void EditablePolyline::hoverLeaveEvent(QHoverEvent* e)
{
    Q_UNUSED(e);
    m_hoveredOnLine = false;
    updateCursor();
    update();
}

void EditablePolyline::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) {
        e->ignore();
        return;
    }

    const int pointIndex = pointIndexAtPx(e->position());
    const bool onPoint = pointIndex >= 0;
    const bool onLine  = isNearLinePx(e->position());

    // NOTE: allow clicks on the points and lines only
    if (!onPoint && !onLine) {
        e->ignore();
        return;
    }

    resetGestureState();

    e->accept();
    setDragX(e->position().rx());
    setDragY(e->position().ry());
    updateCursor();

    m_pressed = true;
    m_pressPx = e->position();

    if (onPoint) {
        m_pressedOnPoint = true;
        m_pressedPointIndex = pointIndex;
        return;
    }

    if (onLine) {
        m_pressedOnLine = true;
        m_pressBaselineN = m_baselineN;
        return;
    }
}

void EditablePolyline::mouseMoveEvent(QMouseEvent* e)
{
    if (!m_pressed) {
        e->ignore();
        return;
    }

    e->accept();
    setDragX(e->position().rx());
    setDragY(e->position().ry());

    const QPointF pos = e->position();
    if (!m_movedSincePress && (pos - m_pressPx).manhattanLength() > MOVE_THRESHOLD) {
        m_movedSincePress = true;

        // start line dragging only after it becomes a drag gesture
        if (m_pressedOnLine && m_pointsN.size() <= 1) {
            m_draggingLine = true;
            m_hasPreview = true;
            m_previewBaselineN = m_baselineN;
        }
    }

    // drag point (2+ points only)
    // Drag a real point (DOMAIN index)
    if (m_pressedPointIndex >= 0) {
        if (width() <= 0 || height() <= 0) {
            return;
        }

        QPointF pN(pos.x() / width(), 1.0 - (pos.y() / height()));
        pN = clamp01(pN);

        // preview only (no mutation of m_points/m_pointsN)
        m_hasPreview = true;
        m_previewDomainIndex = m_pressedPointIndex;
        m_previewPointN = pN;

        const QPointF pDomain = domainFromNormalized(pN);
        emit pointMoved(m_pressedPointIndex, pDomain.x(), pDomain.y(), /*completed*/ false);

        update();
        return;
    }

    // Drag baseline / single-point line: emit flatten request (DOMAIN y)
    if (m_draggingLine && m_points.size() <= 1) {
        const qreal dyPx = pos.y() - m_pressPx.y();
        const qreal dyN = dyPx / height();
        const qreal newBaselineN = clamp01(m_pressBaselineN - dyN);

        m_hasPreview = true;
        m_previewBaselineN = newBaselineN;

        // Convert baseline to DOMAIN y:
        const QPointF domainAtBaseline = domainFromNormalized(QPointF(0.0, newBaselineN));
        emit polylineFlattenRequested(domainAtBaseline.y(), /*completed*/ false);

        update();
        return;
    }
}

void EditablePolyline::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton || !m_pressed) {
        e->ignore();
        return;
    }
    e->accept();

    const bool isClick = !m_movedSincePress;
    const QPointF rel = e->position();

    // Click on point => request removal (DOMAIN index)
    if (isClick && m_pressedOnPoint && m_pressedPointIndex >= 0) {
        emit pointRemoved(m_pressedPointIndex, /*completed*/ true);
        emit interactionFinished();
        resetGestureState();
        return;
    }

    // Click on line => request add point
    if (isClick && m_pressedOnLine) {
        if (width() > 0 && height() > 0) {
            const auto proj = ghostPointToPolylinePx(rel);

            // Convert projected px to normalized, then to domain:
            QPointF pN(clamp01(proj.point.x() / width()),
                       1.0 - clamp01(proj.point.y() / height()));

            const QPointF pDomain = domainFromNormalized(pN);
            emit pointAdded(pDomain.x(), pDomain.y(), /*completed*/ true);
            emit interactionFinished();
        }

        resetGestureState();
        return;
    }

    // Drag commit: point
    if (!isClick && m_pressedPointIndex >= 0 && m_hasPreview) {
        const QPointF pDomain = domainFromNormalized(m_previewPointN);
        emit pointMoved(m_pressedPointIndex, pDomain.x(), pDomain.y(), /*completed*/ true);
        emit interactionFinished();
        resetGestureState();
        return;
    }

    // Drag commit: baseline/flatten
    if (!isClick && m_draggingLine && m_hasPreview) {
        const QPointF domainAtBaseline = domainFromNormalized(QPointF(0.0, m_previewBaselineN));
        emit polylineFlattenRequested(domainAtBaseline.y(), /*completed*/ true);
        emit interactionFinished();
        resetGestureState();
        return;
    }

    resetGestureState();
}
