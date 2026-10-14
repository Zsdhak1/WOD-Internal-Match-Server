#include "competitionoverlay.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPen>
#include <QRadialGradient>
#include <QTimer>
#include <QtMath>

// ReviveIndicator: three self-drawn "mech core" icons. Slot 0 is the last one
// to be consumed; the highest lit slot goes first. Red slots run left->right
// (consumed from the inner/right end); blue is mirrored.

namespace {

constexpr qint64 kNever = -100000;
constexpr int kBreakMs = 400;
constexpr int kLightMs = 320;
constexpr int kLightStaggerMs = 120;
constexpr int kBreathMs = 1200;
constexpr int kFrameMs = 16;

struct Palette {
    QColor base, hot, deep;
};

Palette teamPalette(bool red)
{
    if (red)
        return {QColor(0xe8, 0x41, 0x4f), QColor(0xff, 0xb3, 0xb9), QColor(0x5c, 0x0e, 0x17)};
    return {QColor(0x3a, 0x8d, 0xff), QColor(0xb6, 0xd8, 0xff), QColor(0x0b, 0x26, 0x5c)};
}

QPainterPath hexPath(const QPointF &c, qreal r)
{
    // Pointy-top hexagon, radius = centre to vertex.
    QPainterPath path;
    for (int k = 0; k < 6; ++k) {
        const qreal a = qDegreesToRadians(-90.0 + 60.0 * k);
        const QPointF v(c.x() + r * qCos(a), c.y() + r * qSin(a));
        if (k == 0)
            path.moveTo(v);
        else
            path.lineTo(v);
    }
    path.closeSubpath();
    return path;
}

QPointF hexVertex(const QPointF &c, qreal r, int k)
{
    const qreal a = qDegreesToRadians(-90.0 + 60.0 * k);
    return {c.x() + r * qCos(a), c.y() + r * qSin(a)};
}

qreal easeOut(qreal p) { return 1.0 - (1.0 - p) * (1.0 - p); }

QColor withAlpha(QColor c, qreal a)
{
    c.setAlphaF(qBound<qreal>(0.0, a, 1.0) * c.alphaF());
    return c;
}

void paintUsed(QPainter &p, const QPointF &c, qreal r, qreal opacity)
{
    if (opacity <= 0.0)
        return;
    p.save();
    p.setOpacity(p.opacity() * opacity);
    p.setPen(QPen(QColor(0x5d, 0x66, 0x72), qMax<qreal>(1.0, r * 0.11), Qt::SolidLine,
                  Qt::FlatCap, Qt::MiterJoin));
    p.setBrush(QColor(18, 24, 32, 215));
    p.drawPath(hexPath(c, r));
    p.setPen(QPen(QColor(0x3b, 0x42, 0x4c), qMax<qreal>(1.0, r * 0.07), Qt::SolidLine,
                  Qt::FlatCap, Qt::MiterJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(hexPath(c, r * 0.56));
    p.restore();
}

void paintUnknown(QPainter &p, const QPointF &c, qreal r)
{
    p.save();
    QPen pen(QColor(0x7a, 0x82, 0x8e, 200), qMax<qreal>(1.0, r * 0.09), Qt::DashLine,
             Qt::FlatCap, Qt::MiterJoin);
    pen.setDashPattern({2.2, 1.8});
    p.setPen(pen);
    p.setBrush(QColor(18, 24, 32, 150));
    p.drawPath(hexPath(c, r));
    p.restore();
}

// Lit core. scale shrinks/grows the whole icon, flash adds a white blowout.
void paintLit(QPainter &p, const QPointF &c, qreal r, const Palette &pal, qreal opacity,
              qreal scale = 1.0, qreal flash = 0.0)
{
    if (opacity <= 0.0 || scale <= 0.0)
        return;
    const qreal rs = r * scale;
    p.save();
    p.setOpacity(p.opacity() * opacity);
    // Outer glow.
    QRadialGradient glow(c, rs * 1.45);
    glow.setColorAt(0.0, withAlpha(pal.base, 0.55));
    glow.setColorAt(0.6, withAlpha(pal.base, 0.18));
    glow.setColorAt(1.0, withAlpha(pal.base, 0.0));
    p.setPen(Qt::NoPen);
    p.setBrush(glow);
    p.drawEllipse(c, rs * 1.45, rs * 1.45);
    // Armoured shell.
    QLinearGradient shell(c.x(), c.y() - rs, c.x(), c.y() + rs);
    shell.setColorAt(0.0, pal.deep.lighter(150));
    shell.setColorAt(1.0, pal.deep);
    p.setPen(QPen(pal.base, qMax<qreal>(1.0, rs * 0.13), Qt::SolidLine, Qt::FlatCap,
                  Qt::MiterJoin));
    p.setBrush(shell);
    p.drawPath(hexPath(c, rs));
    // Shell seams: short cuts at the side vertices for a hard-edged look.
    p.setPen(QPen(withAlpha(pal.hot, 0.55), qMax<qreal>(1.0, rs * 0.06)));
    for (int k : {1, 2, 4, 5}) {
        const QPointF v = hexVertex(c, rs * 0.97, k);
        p.drawLine(v, c + (v - c) * 0.74);
    }
    // Inner glowing core.
    QRadialGradient core(c, rs * 0.6);
    core.setColorAt(0.0, QColor(255, 255, 255));
    core.setColorAt(0.35, pal.hot);
    core.setColorAt(1.0, pal.base);
    p.setPen(QPen(withAlpha(QColor(255, 255, 255), 0.5), qMax<qreal>(0.8, rs * 0.04)));
    p.setBrush(core);
    p.drawPath(hexPath(c, rs * 0.56));
    if (flash > 0.0) {
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(QColor(255, 255, 255), flash));
        p.drawPath(hexPath(c, rs));
    }
    p.restore();
}

void paintBreak(QPainter &p, const QPointF &c, qreal r, const Palette &pal, qreal t)
{
    // t in [0,1): used shell fades in, the core collapses, shards and sparks fly.
    paintUsed(p, c, r, easeOut(t));
    const qreal flash = t < 0.18 ? 1.0 - t / 0.18 : 0.0;
    paintLit(p, c, r, pal, 1.0 - t, 1.0 - 0.55 * easeOut(t), flash * 0.8);
    p.save();
    const qreal travel = r * 0.95 * easeOut(t);
    for (int k = 0; k < 6; ++k) {
        const QPointF a = hexVertex(c, r, k);
        const QPointF b = hexVertex(c, r, (k + 1) % 6);
        const QPointF mid = (a + b) / 2.0;
        QPointF dir = mid - c;
        const qreal len = qSqrt(dir.x() * dir.x() + dir.y() * dir.y());
        if (len > 0)
            dir /= len;
        const QPointF off = dir * travel;
        const QPointF inner = c + (mid - c) * 0.62;
        QPainterPath shard;
        shard.moveTo(a + off);
        shard.lineTo(b + off);
        shard.lineTo(inner + off * 1.2);
        shard.closeSubpath();
        p.setOpacity(qMax<qreal>(0.0, 1.0 - t * 1.15));
        p.setPen(Qt::NoPen);
        p.setBrush(k % 2 ? pal.base : pal.hot);
        p.drawPath(shard);
    }
    p.setOpacity(qPow(qMax<qreal>(0.0, 1.0 - t), 1.5));
    p.setPen(QPen(QColor(255, 236, 200), qMax<qreal>(1.0, r * 0.08), Qt::SolidLine,
                  Qt::RoundCap));
    for (int k = 0; k < 8; ++k) {
        const qreal a = qDegreesToRadians(22.5 + 45.0 * k + 9.0 * (k % 3));
        const QPointF dir(qCos(a), qSin(a));
        const qreal from = r * (0.45 + 0.85 * easeOut(t));
        const qreal to = from + r * 0.38 * (1.0 - t);
        p.drawLine(c + dir * from, c + dir * to);
    }
    p.restore();
}

void paintLighting(QPainter &p, const QPointF &c, qreal r, const Palette &pal, qreal t)
{
    paintUsed(p, c, r, 1.0 - t);
    const qreal overshoot = 1.0 + 0.12 * qSin(M_PI * t);
    paintLit(p, c, r, pal, easeOut(t), (0.7 + 0.3 * easeOut(t)) * overshoot,
             t < 0.5 ? 0.0 : (1.0 - t) * 0.6);
    p.save();
    p.setOpacity(1.0 - t);
    p.setPen(QPen(pal.hot, qMax<qreal>(1.0, r * 0.08)));
    p.setBrush(Qt::NoBrush);
    p.drawPath(hexPath(c, r * (1.0 + 0.5 * easeOut(t))));
    p.restore();
}

} // namespace

ReviveIndicator::ReviveIndicator(bool red, QWidget *parent)
    : QLabel(parent), m_red(red)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_TranslucentBackground);
    setFocusPolicy(Qt::NoFocus);
    setMinimumSize(1, 1);
    m_clock.start();
    m_timer = new QTimer(this);
    m_timer->setInterval(kFrameMs);
    m_timer->setTimerType(Qt::PreciseTimer);
    connect(m_timer, &QTimer::timeout, this, [this] {
        update();
        if (!animating())
            m_timer->stop();
    });
    const QString text = tr("剩余复活 未知");
    setText(text);
    setAccessibleName(text);
    setToolTip(text);
}

qint64 ReviveIndicator::now() const
{
    return m_manualClock ? m_manualNow : m_clock.elapsed();
}

namespace {
// Slot that is reserved/being confirmed while a revive is pending.
int pendingSlot(int remaining)
{
    if (remaining < 0)
        return -1;
    return remaining < ReviveIndicator::kLimit ? remaining : remaining - 1;
}
} // namespace

void ReviveIndicator::setResourceState(int remaining, bool pending, bool animate)
{
    if (remaining < 0 || remaining > kLimit)
        remaining = -1;
    const QString text = remaining < 0
        ? tr("剩余复活 未知")
        : tr("剩余复活 %1 / %2").arg(remaining).arg(kLimit);
    const QString full = pending ? text + tr(" · 确认中") : text;
    if (QLabel::text() != full) {
        setText(full);
        setAccessibleName(full);
        setToolTip(full);
    }
    if (m_initialized && remaining == m_remaining && pending == m_pending)
        return;

    const qint64 t = now();
    const bool snap = !animate || !m_initialized || remaining < 0 || m_remaining < 0;
    if (snap) {
        for (int i = 0; i < kLimit; ++i) {
            m_breakStart[i] = kNever;
            m_lightStart[i] = kNever;
        }
    } else {
        // "Shown lit" = full core or pending (reserved but not yet consumed).
        const auto shownLit = [](int rem, bool pend, int slot) {
            return slot < rem || (pend && slot == pendingSlot(rem));
        };
        int lightOrder = 0;
        for (int i = 0; i < kLimit; ++i) {
            const bool before = shownLit(m_remaining, m_pending, i);
            const bool after = shownLit(remaining, pending, i);
            if (before && !after) {
                m_breakStart[i] = t;
                m_lightStart[i] = kNever;
            } else if (!before && after) {
                m_breakStart[i] = kNever;
                m_lightStart[i] = t + qint64(lightOrder++) * kLightStaggerMs;
            }
        }
    }
    m_initialized = true;
    m_remaining = remaining;
    m_pending = pending;
    update();
    ensureTicking();
}

bool ReviveIndicator::animating() const
{
    if (!m_initialized || m_remaining < 0)
        return false;
    if (m_pending && pendingSlot(m_remaining) >= 0)
        return true;
    const qint64 t = now();
    for (int i = 0; i < kLimit; ++i) {
        if (t - m_breakStart[i] < kBreakMs && t >= m_breakStart[i])
            return true;
        if (t - m_lightStart[i] < kLightMs)
            return true; // includes not-yet-started staggered slots
    }
    return false;
}

void ReviveIndicator::ensureTicking()
{
    if (m_manualClock || !animating()) {
        m_timer->stop();
        return;
    }
    if (!m_timer->isActive())
        m_timer->start();
}

void ReviveIndicator::setManualClockForTest(bool manual)
{
    if (manual == m_manualClock)
        return;
    if (manual) {
        // Rebase in-flight animations onto a manual clock starting at 0 so
        // test frames (including the pending breathing phase) are reproducible.
        const qint64 offset = m_clock.elapsed();
        for (int i = 0; i < kLimit; ++i) {
            if (m_breakStart[i] > kNever)
                m_breakStart[i] -= offset;
            if (m_lightStart[i] > kNever)
                m_lightStart[i] -= offset;
        }
        m_manualNow = 0;
    } else {
        // Back to wall-clock time: settle everything instead of jumping.
        for (int i = 0; i < kLimit; ++i) {
            m_breakStart[i] = kNever;
            m_lightStart[i] = kNever;
        }
    }
    m_manualClock = manual;
    ensureTicking();
    update();
}

void ReviveIndicator::advanceForTest(int milliseconds)
{
    if (!m_manualClock)
        setManualClockForTest(true);
    m_manualNow += qMax(0, milliseconds);
    update();
}

void ReviveIndicator::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal h = height();
    const qreal r = qMax<qreal>(3.0, h * 0.8 / 2.0);       // icon height = 2r = 80% h
    const qreal halfW = r * 0.8660254;                       // pointy-top hex half width
    const qreal step = halfW * 2.0 + r * 0.42;
    const qreal margin = r * 0.32 + halfW;
    const qreal cy = h / 2.0;
    const auto centre = [&](int slot) {
        const qreal x = m_red ? margin + slot * step : width() - margin - slot * step;
        return QPointF(x, cy);
    };

    if (!m_initialized || m_remaining < 0) {
        for (int i = 0; i < kLimit; ++i)
            paintUnknown(p, centre(i), r);
        return;
    }

    const Palette pal = teamPalette(m_red);
    const qint64 t = now();
    const int breathing = m_pending ? pendingSlot(m_remaining) : -1;
    for (int i = 0; i < kLimit; ++i) {
        const QPointF c = centre(i);
        const bool lit = i < m_remaining || i == breathing;
        const qint64 sinceBreak = t - m_breakStart[i];
        const qint64 sinceLight = t - m_lightStart[i];
        if (!lit && sinceBreak >= 0 && sinceBreak < kBreakMs) {
            paintBreak(p, c, r, pal, qreal(sinceBreak) / kBreakMs);
        } else if (!lit) {
            paintUsed(p, c, r, 1.0);
        } else if (sinceLight < 0) {
            paintUsed(p, c, r, 1.0); // staggered: waiting its turn
        } else if (sinceLight < kLightMs) {
            paintLighting(p, c, r, pal, qreal(sinceLight) / kLightMs);
        } else if (i == breathing) {
            // Pending approval: the reserved core pulses ~1.2 s per cycle.
            const qreal phase = qreal(t % kBreathMs) / kBreathMs;
            const qreal wave = 0.5 - 0.5 * qCos(2.0 * M_PI * phase);
            paintUsed(p, c, r, 1.0);
            paintLit(p, c, r, pal, 0.28 + 0.72 * wave, 0.9 + 0.1 * wave, 0.25 * wave);
        } else {
            paintLit(p, c, r, pal, 1.0);
        }
    }
}
