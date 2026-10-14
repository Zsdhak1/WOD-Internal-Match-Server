#include "playercombatoverlay.h"

#include <QLinearGradient>
#include <QPolygonF>
#include <QtMath>
#include <QPainter>
#include <QPen>
#include <QTimer>

namespace {
constexpr qint64 kDamageDurationMs = 220;
constexpr qint64 kHitDurationMs = 150;
constexpr qint64 kMinimumPulseIntervalMs = 280;
}

PlayerCombatOverlay::PlayerCombatOverlay(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("playerCombatOverlay"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground);
    setAutoFillBackground(false);
    setFocusPolicy(Qt::NoFocus);
    m_clock.start();
    m_animationTimer = new QTimer(this);
    m_animationTimer->setInterval(16);
    connect(m_animationTimer, &QTimer::timeout, this,
            [this] { advanceFeedback(); });
    hide();
}

void PlayerCombatOverlay::setHudScale(qreal scale)
{
    // Qt paints in logical pixels; do not multiply by devicePixelRatio.
    m_scale = qBound(qreal(0.65), scale, qreal(1.5));
    update();
}

void PlayerCombatOverlay::clearFeedback()
{
    m_animationTimer->stop();
    m_heat.reset();
    m_damageStarted = -1;
    m_hitStarted = -1;
    m_lastDamagePulse = -1000;
    m_lastHitPulse = -1000;
    update();
}

void PlayerCombatOverlay::pulseOwnDamage()
{
    const qint64 now = m_clock.elapsed();
    if (!isVisible() || now - m_lastDamagePulse < kMinimumPulseIntervalMs)
        return;
    m_lastDamagePulse = now;
    m_damageStarted = now;
    setProperty("ownDamagePulseCount", property("ownDamagePulseCount").toInt() + 1);
    m_animationTimer->start();
    update();
}

void PlayerCombatOverlay::pulseOpponentDamage()
{
    const qint64 now = m_clock.elapsed();
    if (!isVisible() || now - m_lastHitPulse < kMinimumPulseIntervalMs)
        return;
    m_lastHitPulse = now;
    m_hitStarted = now;
    setProperty("opponentDamagePulseCount", property("opponentDamagePulseCount").toInt() + 1);
    m_animationTimer->start();
    update();
}

void PlayerCombatOverlay::setHeatTelemetry(int heat, bool canShoot)
{
    m_heat.observe(heat, canShoot, m_clock.elapsed());
    if (m_heat.valid(m_clock.elapsed()))
        m_animationTimer->start();
    update();
}

void PlayerCombatOverlay::advanceFeedback()
{
    const qint64 now = m_clock.elapsed();
    if (m_damageStarted >= 0 && now - m_damageStarted >= kDamageDurationMs)
        m_damageStarted = -1;
    if (m_hitStarted >= 0 && now - m_hitStarted >= kHitDurationMs)
        m_hitStarted = -1;
    if (m_damageStarted < 0 && m_hitStarted < 0 && !m_heat.valid(now))
        m_animationTimer->stop(); // Stop once feedback and telemetry are stale.
    update();
}

void PlayerCombatOverlay::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const qint64 now = m_clock.elapsed();

    if (m_damageStarted >= 0) {
        const qreal progress = qBound(qreal(0),
            qreal(now - m_damageStarted) / kDamageDurationMs, qreal(1));
        const QColor edge(220, 48, 58, qRound(42 * (1.0 - progress)));
        const QColor clear(220, 48, 58, 0);
        const qreal band = qMin(qMin(width(), height()) * 0.06, 34.0 * m_scale);
        // Four narrow gradients leave the entire central view untouched.
        QLinearGradient top(0, 0, 0, band);
        top.setColorAt(0, edge);
        top.setColorAt(1, clear);
        painter.fillRect(QRectF(0, 0, width(), band), top);
        QLinearGradient bottom(0, height(), 0, height() - band);
        bottom.setColorAt(0, edge);
        bottom.setColorAt(1, clear);
        painter.fillRect(QRectF(0, height() - band, width(), band), bottom);
        QLinearGradient left(0, 0, band, 0);
        left.setColorAt(0, edge);
        left.setColorAt(1, clear);
        painter.fillRect(QRectF(0, band, band, qMax(qreal(0), height() - 2 * band)), left);
        QLinearGradient right(width(), 0, width() - band, 0);
        right.setColorAt(0, edge);
        right.setColorAt(1, clear);
        painter.fillRect(QRectF(width() - band, band, band,
                               qMax(qreal(0), height() - 2 * band)), right);
    }

    painter.translate(width() / 2.0, height() / 2.0);
    painter.scale(m_scale, m_scale);
    // Cooling arcs, ammunition budget, reticle and hit marker share one uniform
    // enlargement of their original geometry.
    constexpr qreal kCombatUiScale = 2.0;
    painter.save();
    painter.scale(kCombatUiScale, kCombatUiScale);
    // Open side arcs keep the centre and diagonal hit marker unobstructed.
    const bool heatValid = m_heat.valid(now);
    const int shots = m_heat.availableShots(now);
    const int maxShots = playercombat::kHeatLimit / playercombat::kHeatPerShot;
    const QRectF arc(-34, -34, 68, 68);
    const QColor ready(166, 226, 239, 210);
    const QColor warning(255, 184, 103, 225);
    const QColor blocked(246, 110, 113, 220);
    const QColor ammoColor = !heatValid ? QColor(162, 178, 185, 125)
                              : shots == 0 ? blocked : shots <= 3 ? warning : ready;
    const auto drawArc = [&](qreal start, qreal span, QColor color, qreal thickness) {
        if (qAbs(span) < 0.01) return;
        painter.setPen(QPen(QColor(0, 0, 0, 145), thickness + 2, Qt::SolidLine, Qt::RoundCap));
        painter.drawArc(arc, qRound(start * 16), qRound(span * 16));
        painter.setPen(QPen(color, thickness, Qt::SolidLine, Qt::RoundCap));
        painter.drawArc(arc, qRound(start * 16), qRound(span * 16));
    };
    drawArc(120, 120, QColor(202, 221, 229, 60), 2.0);
    drawArc(60, -120, QColor(202, 221, 229, 60), 2.0);
    if (heatValid) {
        drawArc(240, -120.0 * shots / maxShots, ammoColor, 2.4);
        drawArc(60, -120.0 * m_heat.coolingProgress(now), ready, 2.4);
    }
    const QString budget = heatValid ? QStringLiteral("%1/%2").arg(shots).arg(maxShots)
                                     : QStringLiteral("--/%1").arg(maxShots);
    const qreal glyphW = 6.4;
    const qreal advance = 8.0;
    const qreal totalW = budget.size() * advance - 1.6;
    // Keep the budget number clear of the left arc; kCombatUiScale scales it too.
    constexpr qreal kBudgetNumberOffset = -5.0;
    const qreal numberLeft = -91.0 + 48.0 - totalW + kBudgetNumberOffset;
    const auto drawBudgetGlyph = [&](QChar c, qreal x) {
        static constexpr unsigned masks[] = {0x3f,0x06,0x5b,0x4f,0x66,0x6d,0x7d,0x07,0x7f,0x6f};
        if (c == QLatin1Char('/')) {
            painter.setPen(QPen(ammoColor, 1.7, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(QPointF(x + 1.0, 3.8), QPointF(x + 5.0, -3.8));
            return;
        }
        if (c == QLatin1Char('-')) {
            painter.setPen(QPen(ammoColor, 1.8, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(QPointF(x + 1.0, 0), QPointF(x + 5.4, 0));
            return;
        }
        const int digit = c.digitValue();
        if (digit < 0 || digit > 9) return;
        painter.setPen(Qt::NoPen);
        painter.setBrush(ammoColor);
        const auto h = [&](qreal y) {
            QPolygonF s; s << QPointF(x+0.5,y) << QPointF(x+1.2,y-0.8)
                            << QPointF(x+glyphW-1.2,y-0.8) << QPointF(x+glyphW-0.5,y)
                            << QPointF(x+glyphW-1.2,y+0.8) << QPointF(x+1.2,y+0.8);
            painter.drawPolygon(s);
        };
        const auto v = [&](qreal xx, qreal top, qreal bottom) {
            QPolygonF s; s << QPointF(x+xx,top) << QPointF(x+xx+0.8,top+0.6)
                            << QPointF(x+xx+0.8,bottom-0.6) << QPointF(x+xx,bottom)
                            << QPointF(x+xx-0.8,bottom-0.6) << QPointF(x+xx-0.8,top+0.6);
            painter.drawPolygon(s);
        };
        const unsigned mask = masks[digit];
        if (mask & 1) h(-4.5);
        if (mask & 2) v(glyphW-1.0,-3.8,-.7);
        if (mask & 4) v(glyphW-1.0,.7,3.8);
        if (mask & 8) h(4.5);
        if (mask & 16) v(1.0,.7,3.8);
        if (mask & 32) v(1.0,-3.8,-.7);
        if (mask & 64) h(0);
    };
    for (int i = 0; i < budget.size(); ++i)
        drawBudgetGlyph(budget.at(i), numberLeft + i * advance);
    // Small clock ticks distinguish the right arc from the ammunition budget.
    painter.setPen(QPen(QColor(202, 221, 229, heatValid ? 120 : 60), 1));
    for (int angle : {-60, 0, 60}) {
        const qreal radians = qDegreesToRadians(qreal(angle));
        painter.drawLine(QPointF(38 * qCos(radians), -38 * qSin(radians)),
                         QPointF(41 * qCos(radians), -41 * qSin(radians)));
    }

    const QLineF reticle[] = {
        QLineF(-7, 0, -3, 0), QLineF(3, 0, 7, 0),
        QLineF(0, -7, 0, -3), QLineF(0, 3, 0, 7)
    };
    painter.setPen(QPen(QColor(0, 0, 0, 85), 2.2, Qt::SolidLine, Qt::FlatCap));
    painter.drawLines(reticle, 4);
    painter.setPen(QPen(QColor(234, 244, 247, 150), 1.0, Qt::SolidLine, Qt::FlatCap));
    painter.drawLines(reticle, 4);

    if (m_hitStarted >= 0) {
        const qreal progress = qBound(qreal(0),
            qreal(now - m_hitStarted) / kHitDurationMs, qreal(1));
        painter.setPen(QPen(QColor(242, 247, 240, qRound(185 * (1.0 - progress))),
                            1.15, Qt::SolidLine, Qt::FlatCap));
        const QLineF marker[] = {
            QLineF(-10, -10, -6, -6), QLineF(6, 6, 10, 10),
            QLineF(-10, 10, -6, 6), QLineF(6, -6, 10, -10)
        };
        painter.drawLines(marker, 4);
    }
    painter.restore(); // The 2x combat UI scale ends here.
}
