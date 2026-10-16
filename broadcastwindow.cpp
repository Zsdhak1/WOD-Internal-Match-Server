#include "broadcastwindow.h"
#include "videocanvas.h"
#include "protocol.h"
#include "huddigitslabel.h" // Shared bold, short-bevel angular digits for both outputs.
#include "competitionoverlay.h" // ReviveIndicator + formal CompetitionOverlay.

#include <cmath>

#include <QCloseEvent>
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEasingCurve>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QGraphicsEffect>
#include <QGraphicsOpacityEffect>
#include <QLinearGradient>
#include <QPointer>
#include <QRadialGradient>
#include <QtMath>
#include <QGridLayout>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainterPath>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QLayoutItem>
#include <QPainter>
#include <QPaintEvent>
#include <QPixmap>
#include <QProgressBar>
#include <QPropertyAnimation>
#include <QProcess>
#include <QResizeEvent>
#include <QScreen>
#include <QSettings>
#include <QShowEvent>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QStyle>
#include <QTimer>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QWindow>

#include <functional>

namespace {
qreal windowUiScale(const QWidget *context);
}

class TickerMarqueeWidget final : public QWidget
{
public:
    explicit TickerMarqueeWidget(QWidget *parent = nullptr)
        : QWidget(parent), m_timer(new QTimer(this))
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        // Advance by elapsed time instead of a fixed step per timer tick, so
        // the scroll speed stays constant when the GUI thread is briefly busy
        // and the motion is sub-pixel smooth at 60 Hz.
        connect(m_timer, &QTimer::timeout, this, [this] {
            if (m_text.isEmpty() || width() <= 0)
                return;
            const qint64 elapsed = m_clock.isValid() ? m_clock.restart() : 0;
            if (!m_clock.isValid())
                m_clock.start();
            const QFontMetrics metrics(font());
            const qreal scale = windowUiScale(this);
            const int cycle = qMax(1, metrics.horizontalAdvance(m_text)
                                          + qRound(96 * scale));
            m_offset -= kPixelsPerSecond * scale * qMin<qint64>(elapsed, 100) / 1000.0;
            if (m_offset < -cycle)
                m_offset = width();
            update();
        });
    }

    void setText(const QString &text)
    {
        m_text = text.trimmed();
        m_offset = width();
        if (m_text.isEmpty())
            m_timer->stop();
        else if (isVisible())
            startClock();
        update();
    }

    QString text() const { return m_text; }

protected:
    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        if (!m_text.isEmpty())
            startClock();
    }

    void hideEvent(QHideEvent *event) override
    {
        m_timer->stop();
        QWidget::hideEvent(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        painter.setPen(QColor(QStringLiteral("#f3fbff")));

        if (m_text.isEmpty())
            return;

        const QFontMetrics metrics(font());
        const int baseline = (height() - metrics.height()) / 2 + metrics.ascent();
        painter.save();
        painter.setClipRect(rect());
        const int cycle = qMax(1, metrics.horizontalAdvance(m_text)
                                      + qRound(96 * windowUiScale(this)));
        qreal x = m_offset;
        while (x < width()) {
            painter.drawText(QPointF(x, baseline), m_text);
            x += cycle;
        }
        painter.restore();
    }

private:
    static constexpr qreal kPixelsPerSecond = 110.0;

    void startClock()
    {
        m_clock.start();
        m_timer->start(16);
    }

    QString m_text;
    qreal m_offset = 0;
    QTimer *m_timer = nullptr;
    QElapsedTimer m_clock;
};


// Expandable post-match data panel. It is painted by hand so the layout can
// follow the 1920x1080 reference scale exactly and the collapse/expand motion
// can interpolate one geometry instead of reflowing a widget tree.
class MatchSummaryPanel final : public QWidget
{
public:
    explicit MatchSummaryPanel(QWidget *parent = nullptr)
        : QWidget(parent), m_motion(new QVariantAnimation(this)),
          m_reveal(new QVariantAnimation(this))
    {
        setObjectName(QStringLiteral("matchSummaryPanel"));
        setAttribute(Qt::WA_TranslucentBackground);
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        setAccessibleName(QStringLiteral("赛后数据总结"));
        m_motion->setDuration(320);
        m_motion->setEasingCurve(QEasingCurve::OutCubic);
        connect(m_motion, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
            m_progress = v.toReal();
            applyGeometry();
        });
        m_reveal->setDuration(360);
        m_reveal->setEasingCurve(QEasingCurve::OutCubic);
        connect(m_reveal, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
            m_revealProgress = v.toReal();
            applyGeometry();
        });
        hide();
    }

    std::function<void(bool)> onToggled;

    void setSummary(const QJsonObject &summary, const QString &redName, const QString &blueName)
    {
        m_summary = summary;
        m_redName = redName;
        m_blueName = blueName;
        setAccessibleDescription(winnerText() + QStringLiteral("，")
                                 + summary.value(QStringLiteral("reasonText")).toString());
        update();
    }

    void setFrames(const QRect &collapsed, const QRect &expanded, qreal scale)
    {
        m_collapsed = collapsed;
        m_expanded = expanded;
        m_scale = scale;
        applyGeometry();
    }

    void reveal(bool animated)
    {
        m_reveal->stop();
        m_revealProgress = animated ? 0.0 : 1.0;
        show();
        raise();
        applyGeometry();
        if (animated) {
            m_reveal->setStartValue(0.0);
            m_reveal->setEndValue(1.0);
            m_reveal->start();
        }
    }

    bool expanded() const { return m_expanded_state; }

    void setExpanded(bool expanded, bool animated)
    {
        m_expanded_state = expanded;
        m_motion->stop();
        if (!animated || !isVisible()) {
            m_progress = expanded ? 1.0 : 0.0;
            applyGeometry();
            return;
        }
        m_motion->setStartValue(m_progress);
        m_motion->setEndValue(expanded ? 1.0 : 0.0);
        m_motion->setDuration(qMax(1, qRound(320 * qAbs((expanded ? 1.0 : 0.0) - m_progress))));
        m_motion->start();
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && headerRect().contains(event->pos())) {
            toggle();
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const bool hover = headerRect().contains(event->pos());
        if (hover != m_hover) {
            m_hover = hover;
            setCursor(hover ? Qt::PointingHandCursor : Qt::ArrowCursor);
            update();
        }
        QWidget::mouseMoveEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        m_hover = false;
        update();
        QWidget::leaveEvent(event);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter
            || event->key() == Qt::Key_Space) {
            toggle();
            return;
        }
        if (event->key() == Qt::Key_Escape && m_expanded_state) {
            toggle();
            return;
        }
        QWidget::keyPressEvent(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::TextAntialiasing, true);
        p.setOpacity(m_revealProgress);

        const QColor accent = winnerColor();
        const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.setPen(QPen(QColor(67, 81, 94), px(2)));
        // More opaque when expanded so the settlement art does not show
        // through the statistics.
        p.setBrush(QColor(6, 11, 17, qRound(236 + 16 * m_progress)));
        p.drawRoundedRect(frame, px(10), px(10));
        // Winner accent strip on the left edge.
        p.setPen(Qt::NoPen);
        p.setBrush(accent);
        p.drawRoundedRect(QRectF(0, 0, px(6), height()), px(3), px(3));

        paintHeader(p, accent);
        if (m_progress > 0.01) {
            p.save();
            p.setOpacity(m_revealProgress * qBound(0.0, (m_progress - 0.25) / 0.75, 1.0));
            p.setClipRect(QRect(0, headerRect().bottom() + 1, width(), height()));
            paintBody(p);
            p.restore();
        }
        if (hasFocus()) {
            p.setOpacity(1.0);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(56, 198, 217), px(2), Qt::DashLine));
            p.drawRoundedRect(QRectF(headerRect()).adjusted(px(4), px(4), -px(4), -px(4)),
                              px(6), px(6));
        }
    }

private:
    int px(qreal v) const { return qMax(1, qRound(v * m_scale)); }
    QFont font(qreal pt, bool bold) const
    {
        QFont f = QWidget::font();
        f.setPointSizeF(qMax(1.0, pt * m_scale));
        f.setBold(bold);
        return f;
    }
    QRect headerRect() const { return QRect(0, 0, width(), px(96)); }
    int winner() const { return m_summary.value(QStringLiteral("winner")).toInt(); }
    QColor winnerColor() const
    {
        if (m_summary.value(QStringLiteral("settlement")).toString() == QStringLiteral("termination"))
            return QColor(QStringLiteral("#8d9aa4"));
        return winner() == 1 ? QColor(QStringLiteral("#ff6872"))
             : winner() == 2 ? QColor(QStringLiteral("#72b4ff"))
                             : QColor(QStringLiteral("#f0c860"));
    }
    QString winnerText() const
    {
        if (m_summary.value(QStringLiteral("settlement")).toString() == QStringLiteral("termination"))
            return QStringLiteral("本局终止");
        if (winner() == 1)
            return QStringLiteral("%1 获胜").arg(m_redName);
        if (winner() == 2)
            return QStringLiteral("%1 获胜").arg(m_blueName);
        return QStringLiteral("平局");
    }
    static QString clock(qint64 ms)
    {
        const qint64 s = qMax<qint64>(0, ms / 1000);
        return QStringLiteral("%1:%2").arg(s / 60, 2, 10, QLatin1Char('0'))
                                      .arg(s % 60, 2, 10, QLatin1Char('0'));
    }

    void toggle()
    {
        setExpanded(!m_expanded_state, true);
        if (onToggled)
            onToggled(m_expanded_state);
    }

    void applyGeometry()
    {
        if (!m_collapsed.isValid() || !m_expanded.isValid())
            return;
        const auto lerp = [](int a, int b, qreal t) { return qRound(a + (b - a) * t); };
        const qreal t = m_progress;
        QRect r(lerp(m_collapsed.x(), m_expanded.x(), t),
                lerp(m_collapsed.y(), m_expanded.y(), t),
                lerp(m_collapsed.width(), m_expanded.width(), t),
                lerp(m_collapsed.height(), m_expanded.height(), t));
        // Entrance: rise from slightly below the final position.
        r.translate(0, qRound((1.0 - m_revealProgress) * px(28)));
        setGeometry(r);
        update();
    }

    void paintHeader(QPainter &p, const QColor &accent)
    {
        const QRect h = headerRect();
        p.setPen(QColor(56, 198, 217));
        p.setFont(font(11, true));
        p.drawText(QRect(px(28), px(12), px(360), px(22)), Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("赛后数据 · POST-MATCH"));
        p.setPen(accent);
        p.setFont(font(22, true));
        const QRect titleRect(px(28), px(34), h.width() - px(280), px(36));
        p.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                   QFontMetrics(p.font()).elidedText(winnerText(), Qt::ElideRight, titleRect.width()));
        p.setPen(QColor(QStringLiteral("#c7d0d6")));
        p.setFont(font(12, false));
        const QString sub = QStringLiteral("%1 · 用时 %2")
            .arg(m_summary.value(QStringLiteral("reasonText")).toString(),
                 clock(qint64(m_summary.value(QStringLiteral("durationMs")).toDouble())));
        const QRect subRect(px(28), px(68), h.width() - px(280), px(22));
        p.drawText(subRect, Qt::AlignLeft | Qt::AlignVCenter,
                   QFontMetrics(p.font()).elidedText(sub, Qt::ElideRight, subRect.width()));

        const QRect chip(h.width() - px(212), px(28), px(184), px(40));
        p.setPen(QPen(m_hover ? QColor(114, 180, 255) : QColor(91, 113, 128), px(1)));
        p.setBrush(m_hover ? QColor(45, 74, 93) : QColor(35, 55, 70));
        p.drawRoundedRect(chip, px(6), px(6));
        p.setPen(QColor(QStringLiteral("#f4f7fa")));
        p.setFont(font(12, true));
        const QRect label = chip.adjusted(0, 0, -px(28), 0);
        p.drawText(label, Qt::AlignCenter,
                   m_expanded_state ? QStringLiteral("收起数据") : QStringLiteral("展开数据"));
        // Painted chevron: the broadcast font has no reliable arrow glyphs.
        const QPointF c(chip.right() - px(30), chip.center().y() + 0.5);
        const qreal s = px(5);
        const qreal dir = m_expanded_state ? 1.0 : -1.0;
        QPainterPath chevron;
        chevron.moveTo(c.x() - s, c.y() - dir * s / 2);
        chevron.lineTo(c.x(), c.y() + dir * s / 2);
        chevron.lineTo(c.x() + s, c.y() - dir * s / 2);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(QStringLiteral("#f4f7fa")), px(2), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(chevron);
    }

    void paintBody(QPainter &p)
    {
        const int top = headerRect().bottom() + px(8);
        p.setPen(QPen(QColor(45, 60, 71), px(1)));
        p.drawLine(px(24), top, width() - px(24), top);

        const QJsonObject red = m_summary.value(QStringLiteral("red")).toObject();
        const QJsonObject blue = m_summary.value(QStringLiteral("blue")).toObject();
        const int leftW = qRound(width() * 0.52);
        const QRect stats(px(28), top + px(18), leftW - px(44), px(420));

        // Team names above the comparison rows.
        p.setFont(font(14, true));
        p.setPen(QColor(QStringLiteral("#ff6872")));
        p.drawText(QRect(stats.x(), stats.y(), stats.width() / 2, px(28)),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QFontMetrics(p.font()).elidedText(m_redName, Qt::ElideRight, stats.width() / 2 - px(8)));
        p.setPen(QColor(QStringLiteral("#72b4ff")));
        p.drawText(QRect(stats.center().x(), stats.y(), stats.width() / 2, px(28)),
                   Qt::AlignRight | Qt::AlignVCenter,
                   QFontMetrics(p.font()).elidedText(m_blueName, Qt::ElideRight, stats.width() / 2 - px(8)));

        struct Row { QString label; double r; double b; QString rt; QString bt; bool lowerBetter; };
        const auto num = [](const QJsonObject &o, const char *k) {
            return o.value(QLatin1String(k)).toDouble();
        };
        const auto hpText = [](const QJsonObject &o) {
            const int permille = o.value(QStringLiteral("hpPermille")).toInt(-1);
            return permille < 0 ? QStringLiteral("--")
                                : QStringLiteral("%1%").arg(permille / 10.0, 0, 'f', 1);
        };
        const auto cards = [](const QJsonObject &o) {
            const int y = o.value(QStringLiteral("yellowCards")).toInt();
            const int r = o.value(QStringLiteral("redCards")).toInt();
            return r > 0 ? QStringLiteral("黄%1 红%2").arg(y).arg(r) : QStringLiteral("黄%1").arg(y);
        };
        const auto secs = [](double ms) { return QStringLiteral("%1 s").arg(ms / 1000.0, 0, 'f', 1); };
        const QVector<Row> rows = {
            {QStringLiteral("击杀积分"), num(red, "kills"), num(blue, "kills"),
             QString::number(int(num(red, "kills"))), QString::number(int(num(blue, "kills"))), false},
            {QStringLiteral("剩余血量"), qMax(0.0, num(red, "hpPermille")), qMax(0.0, num(blue, "hpPermille")),
             hpText(red), hpText(blue), false},
            {QStringLiteral("造成伤害"), num(red, "damageDealt"), num(blue, "damageDealt"),
             QString::number(int(num(red, "damageDealt"))), QString::number(int(num(blue, "damageDealt"))), false},
            {QStringLiteral("受击次数"), num(red, "hits"), num(blue, "hits"),
             QString::number(int(num(red, "hits"))), QString::number(int(num(blue, "hits"))), true},
            {QStringLiteral("攻击次数"), num(red, "attacks"), num(blue, "attacks"),
             QString::number(int(num(red, "attacks"))), QString::number(int(num(blue, "attacks"))), false},
            {QStringLiteral("阵亡"), num(red, "deaths"), num(blue, "deaths"),
             QString::number(int(num(red, "deaths"))), QString::number(int(num(blue, "deaths"))), true},
            {QStringLiteral("判罚"), num(red, "yellowCards") + 2 * num(red, "redCards"),
             num(blue, "yellowCards") + 2 * num(blue, "redCards"), cards(red), cards(blue), true},
            {QStringLiteral("离线时长"), num(red, "offlineMs"), num(blue, "offlineMs"),
             secs(num(red, "offlineMs")), secs(num(blue, "offlineMs")), true},
        };
        const int rowH = px(47);
        int y = stats.y() + px(40);
        for (const Row &row : rows) {
            const QRect line(stats.x(), y, stats.width(), rowH);
            const bool redBetter = row.lowerBetter ? row.r < row.b : row.r > row.b;
            const bool blueBetter = row.lowerBetter ? row.b < row.r : row.b > row.r;
            p.setFont(font(15, true));
            p.setPen(redBetter ? QColor(QStringLiteral("#ffffff")) : QColor(QStringLiteral("#aeb9c1")));
            p.drawText(QRect(line.x(), line.y(), line.width() / 3, px(28)), Qt::AlignLeft | Qt::AlignVCenter, row.rt);
            p.setPen(blueBetter ? QColor(QStringLiteral("#ffffff")) : QColor(QStringLiteral("#aeb9c1")));
            p.drawText(QRect(line.right() - line.width() / 3, line.y(), line.width() / 3, px(28)),
                       Qt::AlignRight | Qt::AlignVCenter, row.bt);
            p.setFont(font(12, false));
            p.setPen(QColor(QStringLiteral("#8d9aa4")));
            p.drawText(QRect(line.x(), line.y(), line.width(), px(28)), Qt::AlignCenter, row.label);

            // Split comparison bar growing from the centre towards each side.
            const int barY = line.y() + px(34);
            const int half = line.width() / 2 - px(3);
            // Each side's bar is relative to the larger value, so the leader
            // always fills its half and the other side reads as a fraction.
            const double total = qMax(row.r, row.b);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 18));
            p.drawRoundedRect(QRect(line.x(), barY, line.width(), px(6)), px(3), px(3));
            if (total > 0) {
                const int rw = qRound(half * row.r / total);
                const int bw = qRound(half * row.b / total);
                p.setBrush(QColor(226, 78, 90, redBetter ? 255 : 150));
                p.drawRoundedRect(QRect(line.center().x() - px(3) - rw, barY, rw, px(6)), px(3), px(3));
                p.setBrush(QColor(74, 156, 245, blueBetter ? 255 : 150));
                p.drawRoundedRect(QRect(line.center().x() + px(3), barY, bw, px(6)), px(3), px(3));
            }
            y += rowH;
        }

        const QRect right(leftW + px(8), top + px(18), width() - leftW - px(36), px(460));
        paintTimeline(p, QRect(right.x(), right.y(), right.width(), px(200)));
        paintEvents(p, QRect(right.x(), right.y() + px(222), right.width(), right.height() - px(222)));
    }

    void paintTimeline(QPainter &p, const QRect &area)
    {
        p.setFont(font(12, true));
        p.setPen(QColor(QStringLiteral("#c7d0d6")));
        p.drawText(QRect(area.x(), area.y(), area.width(), px(24)), Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("血量曲线"));
        const QRect plot(area.x() + px(44), area.y() + px(32), area.width() - px(52), area.height() - px(58));
        p.setFont(font(10, false));
        for (int pct : {0, 50, 100}) {
            const int gy = plot.bottom() - plot.height() * pct / 100;
            p.setPen(QPen(QColor(255, 255, 255, pct == 0 ? 60 : 24), px(1)));
            p.drawLine(plot.left(), gy, plot.right(), gy);
            p.setPen(QColor(QStringLiteral("#8d9aa4")));
            p.drawText(QRect(area.x(), gy - px(10), px(40), px(20)), Qt::AlignRight | Qt::AlignVCenter,
                       QStringLiteral("%1%").arg(pct));
        }
        const QJsonArray timeline = m_summary.value(QStringLiteral("timeline")).toArray();
        const double duration = qMax(1000.0, m_summary.value(QStringLiteral("durationMs")).toDouble());
        p.drawText(QRect(plot.left(), plot.bottom() + px(4), px(80), px(20)), Qt::AlignLeft, QStringLiteral("00:00"));
        p.drawText(QRect(plot.right() - px(80), plot.bottom() + px(4), px(80), px(20)), Qt::AlignRight,
                   clock(qint64(duration)));
        for (int side = 1; side <= 2; ++side) {
            QPainterPath path;
            bool drawing = false;
            for (const QJsonValue &value : timeline) {
                const QJsonArray s = value.toArray();
                const int permille = s.at(side).toInt(-1);
                if (permille < 0) {
                    drawing = false;
                    continue;
                }
                const QPointF pt(plot.left() + plot.width() * qMin(1.0, s.at(0).toDouble() / duration),
                                 plot.bottom() - plot.height() * permille / 1000.0);
                if (drawing)
                    path.lineTo(pt);
                else
                    path.moveTo(pt);
                drawing = true;
            }
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(side == 1 ? QColor(QStringLiteral("#ff6872")) : QColor(QStringLiteral("#72b4ff")),
                          px(3), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawPath(path);
        }
    }

    void paintEvents(QPainter &p, const QRect &area)
    {
        p.setFont(font(12, true));
        p.setPen(QColor(QStringLiteral("#c7d0d6")));
        p.drawText(QRect(area.x(), area.y(), area.width(), px(24)), Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("关键事件"));
        QVector<QJsonObject> events;
        for (const QJsonValue &value : m_summary.value(QStringLiteral("events")).toArray()) {
            const QJsonObject e = value.toObject();
            const QString kind = e.value(QStringLiteral("kind")).toString();
            if (kind != QStringLiteral("pause") && kind != QStringLiteral("resume"))
                events.append(e);
        }
        const int lineH = px(28);
        const int capacity = qMax(1, (area.height() - px(32)) / lineH);
        const int first = qMax(0, int(events.size()) - capacity);
        int y = area.y() + px(32);
        p.setFont(font(12, false));
        const QFontMetrics metrics(p.font());
        for (int i = first; i < events.size(); ++i) {
            const QJsonObject &e = events.at(i);
            const int team = e.value(QStringLiteral("team")).toInt();
            p.setPen(Qt::NoPen);
            p.setBrush(team == 1 ? QColor(QStringLiteral("#ff6872"))
                     : team == 2 ? QColor(QStringLiteral("#72b4ff")) : QColor(QStringLiteral("#8d9aa4")));
            p.drawEllipse(QPoint(area.x() + px(6), y + lineH / 2), px(4), px(4));
            p.setPen(QColor(QStringLiteral("#8d9aa4")));
            p.drawText(QRect(area.x() + px(18), y, px(60), lineH), Qt::AlignLeft | Qt::AlignVCenter,
                       clock(qint64(e.value(QStringLiteral("t")).toDouble())));
            const bool verdict = e.value(QStringLiteral("kind")).toString() == QStringLiteral("verdict");
            QFont f = p.font();
            f.setBold(verdict);
            p.setFont(f);
            p.setPen(verdict ? winnerColor() : QColor(QStringLiteral("#e2e9ee")));
            const QRect textRect(area.x() + px(80), y, area.width() - px(80), lineH);
            p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                       metrics.elidedText(e.value(QStringLiteral("text")).toString(), Qt::ElideRight,
                                          textRect.width()));
            f.setBold(false);
            p.setFont(f);
            y += lineH;
        }
    }

    QVariantAnimation *m_motion = nullptr;
    QVariantAnimation *m_reveal = nullptr;
    QJsonObject m_summary;
    QString m_redName;
    QString m_blueName;
    QRect m_collapsed;
    QRect m_expanded;
    qreal m_scale = 1.0;
    qreal m_progress = 0.0;
    qreal m_revealProgress = 1.0;
    bool m_expanded_state = false;
    bool m_hover = false;
};

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
constexpr quint8 kRedTeam = 1;
constexpr quint8 kBlueTeam = 2;
constexpr int kLegacyMaxHp = 5000;
constexpr int kMaxHp = 300;
constexpr int kMaxHeat = 1000;
// The approved 400x118 composite is displayed at 1.5x at 1920x1080.
// All text anchors refer to this ONE artwork, not independent score plates.
constexpr int kCenterHudWidth = 600;
constexpr int kCenterHudHeight = 177;
constexpr int kTeamHudWidth = 642;
constexpr int kTeamHudHeight = 142;
constexpr int kHudGap = 18;
constexpr int kCenterHudNativeYOffset = -34;
// Visual centre of the main trapezoid's face, excluding title and glow.
constexpr int kCenterTimerCenterY = 94;
constexpr int kSettlementFrameWidth = 960;
constexpr int kSettlementFrameHeight = 540;
constexpr int kSettlementFrameBytes = kSettlementFrameWidth * kSettlementFrameHeight * 4;
constexpr int kSettlementFrameIntervalMs = 40;
constexpr int kSettlementFrameQueueLimit = 8;

// All authored HUD pixel values (sizes, offsets, fonts, stylesheet metrics)
// are expressed against this reference resolution. Every consumer multiplies
// them by windowUiScale() so the whole overlay keeps its proportions on any
// display resolution or aspect ratio.
constexpr int kReferenceWidth = 1920;
constexpr int kReferenceHeight = 1080;
constexpr int kTickerBarNativeHeight = 64;

qreal windowUiScale(const QWidget *context)
{
    const QWidget *host = context ? context->window() : nullptr;
    if (!host || host->width() <= 0 || host->height() <= 0)
        return 1.0;

    const qreal scaleX = qreal(host->width()) / qreal(kReferenceWidth);
    const qreal scaleY = qreal(host->height()) / qreal(kReferenceHeight);
    // The requested test window can be much smaller than a physical output,
    // while high-resolution LED walls can exceed 4K.  A wide clamp would make
    // those cases stop scaling proportionally, so retain only a defensive
    // bound against invalid/tiny values.
    return qBound<qreal>(0.1, qMin(scaleX, scaleY), 8.0);
}

QSize scaledValue(const QSize &size, qreal scale)
{
    return QSize(qMax(1, qRound(size.width() * scale)),
                 qMax(1, qRound(size.height() * scale)));
}

QPoint scaledValue(const QPoint &point, qreal scale)
{
    return QPoint(qRound(point.x() * scale), qRound(point.y() * scale));
}

QString broadcastStyleSheet(qreal scale)
{
    const auto px = [scale](int value) { return qMax(1, qRound(value * scale)); };
    return QStringLiteral(
        "QWidget#broadcastRoot { background: #06090d; color: #f4f7fa; }"
        "QLabel#videoBackground { background: #06090d; color: #c5d0d8; font-size: %1px; }"
        "QFrame#teamHud { background: transparent; border: none; }"
        "QWidget#centerHud { background: transparent; border: none; }"
        "QWidget#interactionLayer { background: transparent; border: none; }"
        "QLabel#redTeam, QLabel#blueTeam { color: #f4f7fa; background: transparent; }"
        "QLabel#timer { color: #f8fbfd; background: transparent; }"
        "QLabel#matchTitle { color: #e8f4f8; background: transparent; }"
        "QLabel#heatText { color: #c7d0d6; background: transparent; }"
        "QLabel#matchState[alert=\"true\"] { color: #ffd08a; border-color: #d08a3c; }"
        "QLabel#score { color: #d7e0e6; }"
        "QLabel#matchState { color: #e2e9ee; background: rgba(8, 14, 21, 218); "
        "border: %2px solid #43515e; border-radius: %3px; padding: %4px %5px; }"
        "QLabel#sourceLabel { color: #f4f7fa; background: rgba(8, 14, 21, 218); "
        "border: %2px solid #43515e; border-radius: %3px; padding: %4px %5px; }"
        "QFrame#tickerBar { background: rgba(5, 11, 17, 236); border: %2px solid #435766; "
        "border-left: %6px solid #38c6d9; border-radius: %7px; }"
        "QLabel#tickerTag { color: #38c6d9; background: transparent; font-weight: 700; "
        "letter-spacing: 1px; }"
        "QLabel#connectionStatus { background: transparent; font-weight: 700; }"
        "QLabel#damageNotice { color: #ffffff; background: transparent; "
        "border: none; padding: 0px; font-weight: 700; }"
        "QProgressBar { background: transparent; border: none; border-radius: %7px; "
        "height: %8px; text-align: center; color: #f5f7f8; }"
        "QProgressBar#redHealth::chunk { background: #e24e5a; border-radius: %9px; }"
        "QProgressBar#blueHealth::chunk { background: #4a9cf5; border-radius: %9px; }"
        "QProgressBar#redHeat::chunk { background: #f0aa36; border-radius: %9px; }"
        "QProgressBar#blueHeat::chunk { background: #55c8d7; border-radius: %9px; }")
        .arg(px(28))
        .arg(px(2))
        .arg(px(6))
        .arg(px(8))
        .arg(px(14))
        .arg(px(5))
        .arg(px(5))
        .arg(px(24))
        .arg(px(3));
}

QRect scaledHudViewport(const QWidget *context, qreal scale)
{
    const QWidget *host = context ? context->window() : nullptr;
    if (!host)
        return QRect();

    const QSize logical(qMax(1, qRound(kReferenceWidth * scale)),
                        qMax(1, qRound(kReferenceHeight * scale)));
    const QSize available = host->size();
    return QRect(qMax(0, (available.width() - logical.width()) / 2),
                 qMax(0, (available.height() - logical.height()) / 2),
                 logical.width(), logical.height());
}

QPoint legacyLayoutOffset(const QString &elementId)
{
    if (elementId == QStringLiteral("center_hud"))
        return QPoint(0, -40);
    if (elementId == QStringLiteral("left_score_panel"))
        return QPoint(140, 10);
    if (elementId == QStringLiteral("right_score_panel"))
        return QPoint(-140, 10);
    if (elementId == QStringLiteral("match_title"))
        return QPoint(0, -12);
    if (elementId == QStringLiteral("match_timer"))
        return QPoint(0, -50);
    return QPoint();
}

QPoint defaultLayoutOffset(const QString &elementId)
{
    // Composite artwork and all text start at their authored local anchors.
    // The top row's -34 native offset cancels the overlay's 34px top margin.
    Q_UNUSED(elementId);
    return QPoint();
}

// Active formal round length from the server snapshot (timing preset).
int formalRoundSeconds(const QJsonObject &state)
{
    const int seconds = state.value(QStringLiteral("roundDurationSeconds")).toInt(0);
    return seconds > 0 ? qMin(seconds, 99 * 60 + 59) : 180;
}

// "3 分钟" / "90 秒" / "2 分 30 秒".
QString durationText(int seconds)
{
    if (seconds % 60 == 0) return QCoreApplication::translate("BroadcastWindow", "%1 分钟").arg(seconds / 60);
    if (seconds < 60) return QCoreApplication::translate("BroadcastWindow", "%1 秒").arg(seconds);
    return QCoreApplication::translate("BroadcastWindow", "%1 分 %2 秒").arg(seconds / 60).arg(seconds % 60);
}

#ifdef Q_OS_WIN
void coverTaskbarOnScreen(QWidget *window, const QRect &geometry)
{
    if (!window)
        return;

    const HWND handle = reinterpret_cast<HWND>(window->winId());
    if (!handle)
        return;

    LONG_PTR extendedStyle = GetWindowLongPtr(handle, GWL_EXSTYLE);
    extendedStyle |= WS_EX_TOOLWINDOW;
    extendedStyle &= ~WS_EX_APPWINDOW;
    SetWindowLongPtr(handle, GWL_EXSTYLE, extendedStyle);
    SetWindowPos(handle,
                 HWND_TOPMOST,
                 geometry.x(),
                 geometry.y(),
                 geometry.width(),
                 geometry.height(),
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

void releaseTaskbarCover(QWidget *window)
{
    if (!window || !window->winId())
        return;

    const HWND handle = reinterpret_cast<HWND>(window->winId());
    SetWindowPos(handle,
                 HWND_NOTOPMOST,
                 0,
                 0,
                 0,
                 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}
#endif

QPixmap assetCrop(const QString &resource, const QRect &rect)
{
    const QPixmap atlas(resource);
    return atlas.isNull() ? QPixmap() : atlas.copy(rect);
}

QLabel *makeLabel(QWidget *parent, Qt::Alignment alignment = Qt::AlignLeft)
{
    auto *label = new QLabel(parent);
    label->setAlignment(alignment);
    return label;
}

void setTextColor(QLabel *label, const QColor &color, bool bold = false)
{
    label->setStyleSheet(QStringLiteral("color: %1;%2 background: transparent; border: none;")
                             .arg(color.name(), bold ? QStringLiteral(" font-weight: 700;")
                                                     : QString()));
}

QString defaultTeamName(quint8 team)
{
    return team == kRedTeam ? QStringLiteral("红方") : QStringLiteral("蓝方");
}

// Transparent state wash restricted to the HP ribbon, never an enclosing
// panel frame. Winner state remains available to the settlement machinery.
class HudStateLayer final : public QWidget
{
public:
    HudStateLayer(const QColor &teamColor, QWidget *parent)
        : QWidget(parent), m_teamColor(teamColor),
          m_downAnimation(new QVariantAnimation(this)),
          m_hitAnimation(new QVariantAnimation(this))
    {
        setObjectName(QStringLiteral("hudStateLayer"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_TranslucentBackground);
        m_downAnimation->setDuration(280);
        m_downAnimation->setEasingCurve(QEasingCurve::OutCubic);
        connect(m_downAnimation, &QVariantAnimation::valueChanged, this,
                [this](const QVariant &value) { m_down = value.toReal(); update(); });
        m_hitAnimation->setDuration(460);
        m_hitAnimation->setEasingCurve(QEasingCurve::OutQuad);
        connect(m_hitAnimation, &QVariantAnimation::valueChanged, this,
                [this](const QVariant &value) { m_hit = value.toReal(); update(); });
    }

    void setDown(bool down)
    {
        if (down == m_downTarget)
            return;
        m_downTarget = down;
        m_downAnimation->stop();
        m_downAnimation->setStartValue(m_down);
        m_downAnimation->setEndValue(down ? 1.0 : 0.0);
        m_downAnimation->start();
    }

    void flashHit()
    {
        m_hitAnimation->stop();
        m_hitAnimation->setStartValue(1.0);
        m_hitAnimation->setEndValue(0.0);
        m_hitAnimation->start();
    }

    void setWinner(bool winner)
    {
        if (winner == m_winner)
            return;
        m_winner = winner;
        update();
    }

    bool isDown() const { return m_downTarget; }
    bool isWinner() const { return m_winner; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        if (m_down <= 0.001 && m_hit <= 0.001 && !m_winner)
            return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const qreal scale = windowUiScale(this);
        const qreal radius = qMax<qreal>(1.0, 8 * scale);
        const QRectF frame = QRectF(rect()).adjusted(1, 1, -1, -1);
        if (m_down > 0.001) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(4, 7, 11, qRound(150 * m_down)));
            painter.drawRoundedRect(frame, radius, radius);
        }
        if (m_hit > 0.001) {
            QColor glow = m_teamColor.lighter(135);
            glow.setAlpha(qRound(220 * m_hit));
            glow.setAlpha(qRound(45 * m_hit));
            painter.setPen(Qt::NoPen);
            painter.setBrush(glow);
            painter.drawRoundedRect(frame, radius, radius);
        }
    }

private:
    QColor m_teamColor;
    QVariantAnimation *m_downAnimation = nullptr;
    QVariantAnimation *m_hitAnimation = nullptr;
    qreal m_down = 0.0;
    qreal m_hit = 0.0;
    bool m_downTarget = false;
    bool m_winner = false;
};

class AtlasProgressBar final : public QProgressBar
{
public:
    AtlasProgressBar(const QPixmap &asset,
                     bool reverse,
                     bool mirrorAsset,
                     QWidget *parent = nullptr,
                     bool healthRibbon = false)
        : QProgressBar(parent),
          m_asset(mirrorAsset ? QPixmap::fromImage(asset.toImage().mirrored(true, false))
                              : asset),
          m_reverse(reverse), m_healthRibbon(healthRibbon)
    {
        setTextVisible(false);
    }

    void setDamageBufferValue(int bufferValue)
    {
        m_damageBufferValue = qBound(minimum(), bufferValue, maximum());
        update();
    }

    int damageBufferValue() const
    {
        return m_damageBufferValue >= minimum() ? m_damageBufferValue : value();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        const QPixmap scaled = m_asset.scaled(size(), Qt::IgnoreAspectRatio,
                                              Qt::SmoothTransformation);

        painter.setOpacity(0.22);
        painter.drawPixmap(rect(), scaled);

        const int span = maximum() - minimum();
        const qreal ratio = span > 0
                                ? qBound<qreal>(0.0, qreal(value() - minimum()) / span, 1.0)
                                : 0.0;
        const int fillWidth = qRound(width() * ratio);
        const QRect fillRect = m_reverse
                                   ? QRect(width() - fillWidth, 0, fillWidth, height())
                                   : QRect(0, 0, fillWidth, height());
        painter.save();
        painter.setClipRect(fillRect);
        painter.setOpacity(1.0);
        painter.drawPixmap(rect(), scaled);
        if (m_healthRibbon) {
            // Fine hexagonal texture like the reference HP ribbon. It is
            // clipped both to the live fill and the original atlas silhouette.
            const qreal s = height() / 38.0;
            QPainterPath silhouette;
            silhouette.moveTo(8*s, 5*s);
            silhouette.lineTo(width()-8*s, 5*s);
            silhouette.lineTo(width()-26*s, height()-5*s);
            silhouette.lineTo(8*s, height()-5*s);
            silhouette.closeSubpath();
            if (m_reverse) {
                QTransform mirror; mirror.translate(width(),0); mirror.scale(-1,1);
                silhouette = mirror.map(silhouette);
            }
            painter.setClipPath(silhouette, Qt::IntersectClip);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(QPen(QColor(255,255,255,48), qMax(qreal(0.5),s)));
            const qreal r = 6*s;
            for (int row = 0; row < 4; ++row) {
                const qreal cy = 9*s + row*10.4*s;
                for (qreal cx = (row%2 ? 9 : 0)*s; cx < width()+r; cx += 18*s) {
                    QPolygonF hex;
                    hex << QPointF(cx-r,cy) << QPointF(cx-r/2,cy-5.2*s)
                        << QPointF(cx+r/2,cy-5.2*s) << QPointF(cx+r,cy)
                        << QPointF(cx+r/2,cy+5.2*s) << QPointF(cx-r/2,cy+5.2*s);
                    painter.drawPolygon(hex);
                }
            }
        }
        painter.restore();

        const int bufferValue = qMax(value(), damageBufferValue());
        const qreal bufferRatio = span > 0
                                      ? qBound<qreal>(0.0,
                                                     qreal(bufferValue - minimum()) / span,
                                                     1.0)
                                      : 0.0;
        const int bufferWidth = qRound(width() * bufferRatio);
        if (bufferWidth > fillWidth) {
            const QRect bufferRect = m_reverse
                                         ? QRect(width() - bufferWidth,
                                                 3,
                                                 bufferWidth - fillWidth,
                                                 qMax(1, height() - 6))
                                         : QRect(fillWidth,
                                                 3,
                                                 bufferWidth - fillWidth,
                                                 qMax(1, height() - 6));
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(255, 255, 255, 228));
            painter.drawRect(bufferRect);
            painter.setBrush(QColor(255, 255, 255, 80));
            const QRect highlight = bufferRect.adjusted(0, 0, 0, -qMax(1, bufferRect.height() / 2));
            painter.drawRect(highlight);
        }

    }

private:
    QPixmap m_asset;
    bool m_reverse = false;
    int m_damageBufferValue = -1;
    bool m_healthRibbon = false;
};

// HUD overlay root. Paints a soft-edged dark backing under the top HUD so the
// glass artwork, countdown and team text stay readable over overexposed or
// white program video (sunlit field, white walls). No hard edge: the band
// fades to fully transparent well above the play area.
class HudOverlayWidget final : public QWidget
{
public:
    using QWidget::QWidget;
    void setTopShadow(bool on) { if (m_topShadow != on) { m_topShadow = on; update(); } }
protected:
    void paintEvent(QPaintEvent *event) override
    {
        if (!m_topShadow) return;
        // Repaints happen on every video frame underneath; draw the gradients
        // once per size/viewport and blit the cached band afterwards.
        const QRect vp = property("hudViewport").toRect().isValid() ? property("hudViewport").toRect() : rect();
        const qreal dpr = devicePixelRatioF();
        if (m_band.isNull() || m_bandSize != size() || m_bandViewport != vp || !qFuzzyCompare(m_band.devicePixelRatio(), dpr)) {
            m_bandSize = size(); m_bandViewport = vp;
            const int bandHeight = qMin(height(), vp.top() + qCeil(260 * vp.height() / 1080.0) + 1);
            QImage img(QSize(width(), qMax(1, bandHeight)) * dpr, QImage::Format_ARGB32_Premultiplied);
            img.setDevicePixelRatio(dpr);
            img.fill(Qt::transparent);
            QPainter painter(&img);
            paintBand(painter, vp);
            painter.end();
            m_band = QPixmap::fromImage(img);
        }
        QPainter p(this);
        p.setClipRegion(event->region());
        p.drawPixmap(0, 0, m_band);
    }
private:
    void paintBand(QPainter &p, const QRect &vp)
    {
        const qreal s = vp.height() / 1080.0;
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        // Full-width vertical fade (top 260 authored px).
        const qreal band = 260 * s;
        QLinearGradient fade(0, vp.top(), 0, vp.top() + band);
        fade.setColorAt(0.00, QColor(4, 10, 16, 178));
        fade.setColorAt(0.45, QColor(4, 10, 16, 115));
        fade.setColorAt(0.75, QColor(4, 10, 16, 38));
        fade.setColorAt(1.00, QColor(4, 10, 16, 0));
        p.fillRect(QRectF(0, vp.top(), width(), band), fade);
        // Extra soft pool behind the centre countdown artwork.
        const QPointF c(vp.center().x(), vp.top() + 95 * s);
        QRadialGradient pool(c, 1.0);
        pool.setColorAt(0.0, QColor(2, 8, 14, 105));
        pool.setColorAt(0.5, QColor(2, 8, 14, 55));
        pool.setColorAt(1.0, QColor(2, 8, 14, 0));
        p.save();
        p.translate(c);
        p.scale(500 * s, 140 * s);
        p.translate(-c);
        p.setBrush(pool);
        p.drawEllipse(c, 1.0, 1.0);
        p.restore();
    }
    bool m_topShadow = true;
    QPixmap m_band;
    QSize m_bandSize;
    QRect m_bandViewport;
};

// Three-pass box blur of the alpha channel (≈ Gaussian), in place.
static void blurAlpha(QImage &img, int radius)
{
    if (radius < 1) return;
    const int w = img.width(), h = img.height();
    QVector<int> line(qMax(w, h)), out(qMax(w, h));
    const auto pass = [&](int n, auto get, auto set) {
        int sum = 0; const int win = 2 * radius + 1;
        for (int i = -radius; i <= radius; ++i) sum += get(qBound(0, i, n - 1));
        for (int i = 0; i < n; ++i) {
            out[i] = sum / win;
            sum += get(qMin(n - 1, i + radius + 1)) - get(qMax(0, i - radius));
        }
        for (int i = 0; i < n; ++i) set(i, out[i]);
    };
    for (int iter = 0; iter < 3; ++iter) {
        for (int y = 0; y < h; ++y) {
            QRgb *row = reinterpret_cast<QRgb *>(img.scanLine(y));
            pass(w, [&](int x) { return qAlpha(row[x]); }, [&](int x, int a) { row[x] = qRgba(0, 0, 0, a); });
        }
        for (int x = 0; x < w; ++x)
            pass(h, [&](int y) { return qAlpha(reinterpret_cast<const QRgb *>(img.constScanLine(y))[x]); },
                 [&](int y, int a) { reinterpret_cast<QRgb *>(img.scanLine(y))[x] = qRgba(0, 0, 0, a); });
    }
}

// Soft drop shadow for HUD text that is blurred only when the label's own
// content changes (text, size, colour, font). The HUD sits over live video,
// so its labels repaint on every video frame; QGraphicsDropShadowEffect would
// re-blur each time, this effect just blits the cached result.
class CachedTextShadow final : public QGraphicsEffect
{
public:
    explicit CachedTextShadow(QWidget *target) : QGraphicsEffect(target), m_target(target) {}
    void setParams(qreal blur, qreal dy, const QColor &color)
    {
        m_blur = blur; m_dy = dy; m_color = color; m_key.clear();
        updateBoundingRect(); update();
    }
    QRectF boundingRectFor(const QRectF &rect) const override
    {
        const qreal m = m_blur + qAbs(m_dy);
        return rect.adjusted(-m, -m, m, m);
    }
protected:
    void draw(QPainter *painter) override
    {
        auto *label = qobject_cast<QLabel *>(m_target.data());
        const QString key = label ? label->text() + QChar(1) + QString::number(label->width()) + QChar('x')
                                        + QString::number(label->height()) + QChar(1)
                                        + label->palette().color(QPalette::WindowText).name(QColor::HexArgb)
                                        + QChar(1) + label->font().toString() + QChar(1) + label->styleSheet()
                                  : QString();
        if (key.isEmpty() || key != m_key || m_cache.isNull()) {
            QPoint offset;
            const QPixmap source = sourcePixmap(Qt::LogicalCoordinates, &offset, QGraphicsEffect::PadToEffectiveBoundingRect);
            if (source.isNull()) return;
            const qreal dpr = source.devicePixelRatio();
            QImage shadow = source.toImage().convertToFormat(QImage::Format_ARGB32);
            blurAlpha(shadow, qMax(1, qRound(m_blur * dpr / 3.0)));
            QImage result(source.size(), QImage::Format_ARGB32_Premultiplied);
            result.fill(Qt::transparent);
            {
                QPainter p(&result);
                // Tint the blurred alpha with the shadow colour.
                QImage tinted(shadow.size(), QImage::Format_ARGB32_Premultiplied);
                tinted.fill(m_color);
                { QPainter t(&tinted); t.setCompositionMode(QPainter::CompositionMode_DestinationIn); t.drawImage(0, 0, shadow); }
                p.drawImage(QPointF(0, m_dy * dpr), tinted);
                p.drawPixmap(0, 0, source);
            }
            m_cache = QPixmap::fromImage(result);
            m_cache.setDevicePixelRatio(dpr);
            m_offset = offset;
            m_key = key;
        }
        painter->drawPixmap(m_offset, m_cache);
    }
private:
    QPointer<QWidget> m_target;
    qreal m_blur = 14, m_dy = 2;
    QColor m_color{0, 6, 12, 230};
    QString m_key;
    QPixmap m_cache;
    QPoint m_offset;
};

// Soft dark halo behind HUD text (countdown, scores, title, team names).
static void applyTextShadow(QWidget *w, qreal scale, qreal blur = 14, qreal dy = 2, int alpha = 230)
{
    if (!w) return;
    auto *e = dynamic_cast<CachedTextShadow *>(w->graphicsEffect());
    if (!e) { e = new CachedTextShadow(w); w->setGraphicsEffect(e); }
    e->setParams(blur * scale, dy * scale, QColor(0, 6, 12, alpha));
}

class CenterHudCanvas final : public QWidget
{
public:
    explicit CenterHudCanvas(QWidget *parent = nullptr)
        : QWidget(parent),
          m_artwork(QStringLiteral(":/broadcast/center_hud_combined.png"))
    {
        setObjectName(QStringLiteral("centerHud"));
        setAttribute(Qt::WA_TranslucentBackground);
        setAutoFillBackground(false);
        setMinimumSize(1, 1);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        setProperty("compositeAssetLoaded", !m_artwork.isNull());
        m_content = new QWidget(this);
        m_content->setAttribute(Qt::WA_TranslucentBackground);
        m_content->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_content->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
        for (QLabel **label : {&m_leftScore, &m_rightScore}) {
            *label = new HudDigitsLabel(QStringLiteral("0"), this);
            (*label)->setObjectName(label == &m_leftScore
                                       ? QStringLiteral("redRoundScore")
                                       : QStringLiteral("blueRoundScore"));
            (*label)->setAlignment(Qt::AlignCenter);
            (*label)->setStyleSheet(QStringLiteral("color: #effcff; background: transparent;"));
            (*label)->setAttribute(Qt::WA_TransparentForMouseEvents);
        }
    }

    QSize sizeHint() const override
    {
        return scaledValue(QSize(kCenterHudWidth, kCenterHudHeight), windowUiScale(this));
    }
    QWidget *contentWidget() const { return m_content; }
    QLabel *leftScoreLabel() const { return m_leftScore; }
    QLabel *rightScoreLabel() const { return m_rightScore; }
    void setElementOffset(const QString &id, const QPoint &offset)
    {
        m_elementOffsets.insert(id, offset);
        updateElementGeometry();
    }
    void setElementSize(const QString &id, const QSize &size)
    {
        if (size.isValid()) m_elementSizes.insert(id, size);
        else m_elementSizes.remove(id);
        updateElementGeometry();
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        updateElementGeometry();
        QWidget::resizeEvent(event);
    }
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        // One draw call: the relative N15/N16 positions cannot drift apart.
        if (!m_artwork.isNull()) painter.drawPixmap(rect(), m_artwork);
    }

private:
    void updateElementGeometry()
    {
        const qreal sx = width() / qreal(kCenterHudWidth);
        const qreal sy = height() / qreal(kCenterHudHeight);
        const auto placeScore = [this, sx, sy](QLabel *label, const QString &id,
                                              const QRect &anchor) {
            const QPoint offset = m_elementOffsets.value(id);
            const QSize size = m_elementSizes.value(id, anchor.size());
            label->setGeometry(qRound((anchor.x() + offset.x()) * sx),
                               qRound((anchor.y() + offset.y()) * sy),
                               qMax(1, qRound(size.width() * sx)),
                               qMax(1, qRound(size.height() * sy)));
            QFont font = label->font();
            font.setBold(true);
            font.setPointSizeF(qMax(1.0, 32.0 * qMin(sx, sy)));
            label->setFont(font);
        };
        // Visual plate centers in the approved composite, in 600x177 units.
        placeScore(m_leftScore, QStringLiteral("red_round_score"), QRect(36, 61, 117, 62));
        placeScore(m_rightScore, QStringLiteral("blue_round_score"), QRect(447, 61, 117, 62));
        m_content->setGeometry(rect());
        m_content->raise();
        m_leftScore->raise();
        m_rightScore->raise();
    }
    QPixmap m_artwork;
    QWidget *m_content = nullptr;
    QLabel *m_leftScore = nullptr;
    QLabel *m_rightScore = nullptr;
    QHash<QString, QPoint> m_elementOffsets;
    QHash<QString, QSize> m_elementSizes;
};

class HudElementLayout : public QLayout
{
public:
    explicit HudElementLayout(QWidget *parent = nullptr)
        : QLayout(parent)
    {
        setContentsMargins(0, 0, 0, 0);
    }

    ~HudElementLayout() override
    {
        while (QLayoutItem *item = takeAt(0))
            delete item;
    }

    void addElement(const QString &elementId, QWidget *widget)
    {
        if (elementId.isEmpty() || !widget)
            return;

        addChildWidget(widget);
        m_entries.append({elementId, new QWidgetItem(widget)});
        invalidate();
    }

    void addItem(QLayoutItem *item) override
    {
        if (!item)
            return;
        m_entries.append({QString(), item});
        invalidate();
    }

    void setElementOffset(const QString &elementId, const QPoint &offset)
    {
        m_offsets.insert(elementId, offset);
        invalidate();
    }

    void setElementSize(const QString &elementId, const QSize &size)
    {
        if (size.isValid() && size.width() > 0 && size.height() > 0)
            m_sizes.insert(elementId, size);
        else
            m_sizes.remove(elementId);
        invalidate();
    }

    bool hasElementSize(const QString &elementId) const
    {
        return m_sizes.contains(elementId);
    }

    QLayoutItem *itemAt(int index) const override
    {
        return index >= 0 && index < m_entries.size() ? m_entries.at(index).item : nullptr;
    }

    QLayoutItem *takeAt(int index) override
    {
        if (index < 0 || index >= m_entries.size())
            return nullptr;

        const Entry entry = m_entries.takeAt(index);
        m_offsets.remove(entry.elementId);
        m_sizes.remove(entry.elementId);
        return entry.item;
    }

    int count() const override { return m_entries.size(); }

protected:
    struct Entry {
        QString elementId;
        QLayoutItem *item = nullptr;
    };

    const Entry *entry(const QString &elementId) const
    {
        for (const Entry &candidate : m_entries) {
            if (candidate.elementId == elementId)
                return &candidate;
        }
        return nullptr;
    }

    QSize naturalSize(const QString &elementId) const
    {
        const Entry *candidate = entry(elementId);
        if (!candidate || !candidate->item)
            return QSize(1, 1);

        const QSize hint = candidate->item->sizeHint();
        const QSize minimum = candidate->item->minimumSize();
        return QSize(qMax(1, qMax(hint.width(), minimum.width())),
                     qMax(1, qMax(hint.height(), minimum.height())));
    }

    QSize minimumElementSize(const QString &elementId) const
    {
        const Entry *candidate = entry(elementId);
        if (!candidate || !candidate->item)
            return QSize(1, 1);
        const QSize minimum = candidate->item->minimumSize();
        return QSize(qMax(1, minimum.width()), qMax(1, minimum.height()));
    }

    QSize elementSize(const QString &elementId) const
    {
        return m_sizes.contains(elementId) ? m_sizes.value(elementId)
                                            : naturalSize(elementId);
    }

    QPoint elementOffset(const QString &elementId) const
    {
        return m_offsets.value(elementId, QPoint());
    }

    void setItemGeometry(const QString &elementId, const QRect &geometry)
    {
        const Entry *candidate = entry(elementId);
        if (!candidate || !candidate->item)
            return;

        // QWidgetItem clamps an item to the layout rect.  The original HUD
        // deliberately lets the center artwork extend above that rect, so
        // set the child geometry directly and retain the signed coordinates.
        if (QWidget *widget = candidate->item->widget())
            widget->setGeometry(geometry);
        else
            candidate->item->setGeometry(geometry);
    }

private:
    QVector<Entry> m_entries;
    QHash<QString, QPoint> m_offsets;
    QHash<QString, QSize> m_sizes;
};

class HudTopLayout final : public HudElementLayout
{
public:
    explicit HudTopLayout(QWidget *parent = nullptr)
        : HudElementLayout(parent)
    {
    }

    QSize sizeHint() const override
    {
        const qreal scale = windowUiScale(parentWidget());
        const QSize red = effectiveSize(QStringLiteral("red_team_panel"),
                                        QSize(kTeamHudWidth, kTeamHudHeight), scale);
        const QSize center = effectiveSize(QStringLiteral("center_hud"),
                                           QSize(kCenterHudWidth, kCenterHudHeight), scale);
        const QSize blue = effectiveSize(QStringLiteral("blue_team_panel"),
                                         QSize(kTeamHudWidth, kTeamHudHeight), scale);
        return QSize(red.width() + center.width() + blue.width()
                         + qRound(kHudGap * scale) * 2,
                     qMax(red.height(), qMax(center.height(), blue.height())));
    }

    QSize minimumSize() const override { return sizeHint(); }

    Qt::Orientations expandingDirections() const override { return Qt::Horizontal; }

protected:
    void setGeometry(const QRect &rect) override
    {
        QLayout::setGeometry(rect);

        const QString redId = QStringLiteral("red_team_panel");
        const QString centerId = QStringLiteral("center_hud");
        const QString blueId = QStringLiteral("blue_team_panel");
        // Everything is authored against the reference resolution and scaled
        // uniformly, so the header keeps its proportions on any display.
        const qreal scale = windowUiScale(parentWidget());
        QSize center = effectiveSize(centerId, QSize(kCenterHudWidth, kCenterHudHeight),
                                     scale);
        QSize redSize = effectiveSize(redId, QSize(kTeamHudWidth, kTeamHudHeight), scale);
        QSize blueSize = effectiveSize(blueId, QSize(kTeamHudWidth, kTeamHudHeight), scale);

        // Side panels occupy the remaining logical-viewport width beside the
        // actual composite, preserving an 18px scaled join even after resizing
        // or moving the center. Explicit custom side-panel sizes still win.
        QRect horizontalBounds = parentWidget()
                                     ? parentWidget()->property("hudViewport").toRect()
                                     : QRect();
        if (!horizontalBounds.isValid())
            horizontalBounds = parentWidget() ? parentWidget()->contentsRect() : rect;
        const int leftEdge = horizontalBounds.x();
        const int rightEdge = horizontalBounds.x() + horizontalBounds.width();
        const int centerLeft = leftEdge + (horizontalBounds.width() - center.width()) / 2;

        const QPoint redOffset = scaledValue(elementOffset(redId), scale);
        const QPoint centerOffset = scaledValue(elementOffset(centerId), scale);
        const QPoint blueOffset = scaledValue(elementOffset(blueId), scale);
        const int gap = qRound(kHudGap * scale);
        if (!hasElementSize(redId))
            redSize.setWidth(qMax(1, centerLeft + centerOffset.x() - gap
                                      - leftEdge - redOffset.x()));
        if (!hasElementSize(blueId))
            blueSize.setWidth(qMax(1, rightEdge + blueOffset.x()
                                       - centerLeft - centerOffset.x() - center.width() - gap));
        setItemGeometry(redId,
                        QRect(leftEdge + redOffset.x(),
                              rect.y() + redOffset.y(),
                              redSize.width(),
                              redSize.height()));
        setItemGeometry(centerId,
                        QRect(centerLeft + centerOffset.x(),
                              rect.y() + qRound(kCenterHudNativeYOffset * scale)
                                  + centerOffset.y(),
                              center.width(),
                              center.height()));
        setItemGeometry(blueId,
                        QRect(rightEdge - blueSize.width() + blueOffset.x(),
                              rect.y() + blueOffset.y(),
                              blueSize.width(),
                              blueSize.height()));
    }

private:
    QSize effectiveSize(const QString &elementId, const QSize &nativeSize,
                        qreal scale) const
    {
        if (hasElementSize(elementId))
            return scaledValue(elementSize(elementId), scale);
        // Natural hints of the panels reflect the currently applied fonts and
        // the center canvas already scales its own size hint.
        const QSize natural = naturalSize(elementId);
        return QSize(qMax(1, qRound(nativeSize.width() * scale)),
                     qMax(qRound(nativeSize.height() * scale), natural.height()));
    }
};

class HudCenterContentLayout final : public HudElementLayout
{
public:
    explicit HudCenterContentLayout(QWidget *parent = nullptr)
        : HudElementLayout(parent)
    {
    }

    QSize sizeHint() const override
    {
        const QSize title = elementSize(QStringLiteral("match_title"));
        const QSize timer = elementSize(QStringLiteral("match_timer"));
        return QSize(qMax(title.width(), timer.width()), title.height() + timer.height() + 2);
    }

    QSize minimumSize() const override { return sizeHint(); }

protected:
    void setGeometry(const QRect &rect) override
    {
        QLayout::setGeometry(rect);

        const QString titleId = QStringLiteral("match_title");
        const QString timerId = QStringLiteral("match_timer");
        // Title and timer use fixed anchors in the center HUD coordinate
        // system. Their positions must not depend on the other label's text
        // metrics.
        const qreal scaleX = rect.width() / qreal(kCenterHudWidth);
        const qreal scaleY = rect.height() / qreal(kCenterHudHeight);
        const auto resolvedSize = [this, scaleX, scaleY](const QString &elementId) {
            // Saved sizes are reference-resolution pixels; scale them into the
            // current center HUD coordinate system.
            if (hasElementSize(elementId)) {
                const QSize saved = elementSize(elementId);
                return QSize(qMax(1, qRound(saved.width() * scaleX)),
                             qMax(1, qRound(saved.height() * scaleY)));
            }
            return elementSize(elementId);
        };
        QSize title = resolvedSize(titleId);
        QSize timer = resolvedSize(timerId);
        if (!hasElementSize(titleId)) title.setHeight(qMax(1, qRound(28 * scaleY)));
        if (!hasElementSize(timerId)) timer.setHeight(qMax(1, qRound(78 * scaleY)));
        const int titleWidth = hasElementSize(titleId) ? title.width() : qRound(570 * scaleX);
        const int timerWidth = hasElementSize(timerId) ? timer.width() : qRound(216 * scaleX);
        const auto scaledOffset = [scaleX, scaleY](const QPoint &offset) {
            return QPoint(qRound(offset.x() * scaleX), qRound(offset.y() * scaleY));
        };
        const QPoint titleOffset = scaledOffset(elementOffset(titleId));
        const QPoint timerOffset = scaledOffset(elementOffset(timerId));

        // The title is a child of centerHud, so a negative Y would place it
        // outside the parent's paint region and clip the glyphs. Keep the
        // requested offset when it is visible, but clamp its top edge.
        const int titleY = qMax(rect.y(), rect.y() + qRound(3 * scaleY) + titleOffset.y());
        setItemGeometry(titleId,
                        QRect(rect.x() + (rect.width() - titleWidth) / 2 + titleOffset.x(),
                              titleY,
                              titleWidth,
                              title.height()));
        setItemGeometry(timerId,
                        QRect(rect.x() + (rect.width() - timerWidth) / 2 + timerOffset.x(),
                              rect.y() + qRound(kCenterTimerCenterY * scaleY)
                                  - timer.height() / 2 + timerOffset.y(),
                              timerWidth,
                              timer.height()));
    }
};

class HudBottomLayout final : public HudElementLayout
{
public:
    explicit HudBottomLayout(QWidget *parent = nullptr)
        : HudElementLayout(parent)
    {
    }

    QSize sizeHint() const override
    {
        const qreal scale = windowUiScale(parentWidget());
        const QSize state = scaledElementSize(QStringLiteral("match_state"), scale);
        const QSize source = scaledElementSize(QStringLiteral("source_label"), scale);
        return QSize(state.width() + source.width(), qMax(state.height(), source.height()));
    }

    QSize minimumSize() const override { return sizeHint(); }

    Qt::Orientations expandingDirections() const override { return Qt::Horizontal; }

protected:
    void setGeometry(const QRect &rect) override
    {
        QLayout::setGeometry(rect);

        const QString stateId = QStringLiteral("match_state");
        const QString sourceId = QStringLiteral("source_label");
        const qreal scale = windowUiScale(parentWidget());
        const QSize state = scaledElementSize(stateId, scale);
        const QSize source = scaledElementSize(sourceId, scale);
        const QPoint stateOffset = scaledValue(elementOffset(stateId), scale);
        const QPoint sourceOffset = scaledValue(elementOffset(sourceId), scale);
        const int bottom = rect.y() + rect.height();

        setItemGeometry(stateId,
                        QRect(rect.x() + stateOffset.x(),
                              bottom - state.height() + stateOffset.y(),
                              state.width(),
                              state.height()));
        setItemGeometry(sourceId,
                        QRect(rect.x() + rect.width() - source.width() + sourceOffset.x(),
                              bottom - source.height() + sourceOffset.y(),
                              source.width(),
                              source.height()));
    }

private:
    QSize scaledElementSize(const QString &elementId, qreal scale) const
    {
        // Natural hints already reflect the scaled fonts; only saved custom
        // sizes are reference-resolution pixels that need scaling here.
        if (hasElementSize(elementId))
            return scaledValue(elementSize(elementId), scale);
        return elementSize(elementId);
    }
};

// Team panels use authored row coordinates. A flow layout redistributes rows
// whenever a label's text changes, which is exactly the y-drift visible when
// the match starts or a robot changes connection state.
class HudTeamLayout final : public QLayout
{
public:
    explicit HudTeamLayout(QWidget *parent = nullptr)
        : QLayout(parent)
    {
        setContentsMargins(0, 0, 0, 0);
    }

    ~HudTeamLayout() override
    {
        while (QLayoutItem *item = takeAt(0))
            delete item;
    }

    void addElement(const QString &elementId, QWidget *widget)
    {
        if (elementId.isEmpty() || !widget)
            return;
        addChildWidget(widget);
        m_entries.append({elementId, new QWidgetItem(widget)});
        invalidate();
    }

    void addItem(QLayoutItem *item) override
    {
        if (!item)
            return;
        m_entries.append({QString(), item});
        invalidate();
    }

    QLayoutItem *itemAt(int index) const override
    {
        return index >= 0 && index < m_entries.size() ? m_entries.at(index).item : nullptr;
    }

    QLayoutItem *takeAt(int index) override
    {
        if (index < 0 || index >= m_entries.size())
            return nullptr;
        return m_entries.takeAt(index).item;
    }

    int count() const override { return m_entries.size(); }
    QSize sizeHint() const override
    {
        const qreal scale = windowUiScale(parentWidget());
        return QSize(qMax(1, qRound(kTeamHudWidth * scale)),
                     qMax(1, qRound(kTeamHudHeight * scale)));
    }
    QSize minimumSize() const override { return sizeHint(); }

protected:
    void setGeometry(const QRect &rect) override
    {
        QLayout::setGeometry(rect);
        const qreal scaleX = qMax<qreal>(0.01, rect.width() / qreal(kTeamHudWidth));
        const qreal scaleY = qMax<qreal>(0.01, rect.height() / qreal(kTeamHudHeight));
        const auto scaleRect = [rect, scaleX, scaleY](const QRect &native) {
            return QRect(rect.x() + qRound(native.x() * scaleX),
                         rect.y() + qRound(native.y() * scaleY),
                         qMax(1, qRound(native.width() * scaleX)),
                         qMax(1, qRound(native.height() * scaleY)));
        };

        // Reference-style compact header: white team name, wide textured HP
        // ribbon with its number inset. No telemetry rows or enclosing frame.
        setItemGeometry(QStringLiteral("team_row"), scaleRect(QRect(28, 0, 586, 32)));
        setItemGeometry(QStringLiteral("health_bar"), scaleRect(QRect(28, 37, 586, 64)));
        const bool red = parentWidget()->property("team").toString() == QStringLiteral("red");
        setItemGeometry(QStringLiteral("health_text"), scaleRect(QRect(red ? 42 : 486, 37, 112, 64)));
        const bool formal = parentWidget()->property("competitionMode").toBool();
        // Mirror per side inside the 28..614 ribbon span: revive cores hug the
        // outer HP-bar edge (red left, blue right), cards take the inner end.
        setItemGeometry(QStringLiteral("revive_count"),
                        scaleRect(red ? QRect(28, 105, 402, 34) : QRect(212, 105, 402, 34)));
        setItemGeometry(QStringLiteral("card_row"),
                        scaleRect(formal ? (red ? QRect(440, 108, 174, 25) : QRect(28, 108, 174, 25))
                                         : QRect(28, 106, 586, 25)));
        setItemGeometry(QStringLiteral("state_layer"), scaleRect(QRect(28, 37, 586, 64)));
        if (const Entry *layer = entry(QStringLiteral("state_layer")); layer && layer->item->widget())
            layer->item->widget()->raise();
    }

private:
    struct Entry {
        QString elementId;
        QLayoutItem *item = nullptr;
    };

    const Entry *entry(const QString &elementId) const
    {
        for (const Entry &candidate : m_entries) {
            if (candidate.elementId == elementId)
                return &candidate;
        }
        return nullptr;
    }

    void setItemGeometry(const QString &elementId, const QRect &geometry)
    {
        const Entry *candidate = entry(elementId);
        if (!candidate || !candidate->item)
            return;
        if (QWidget *widget = candidate->item->widget())
            widget->setGeometry(geometry);
        else
            candidate->item->setGeometry(geometry);
    }

    QVector<Entry> m_entries;
};
} // namespace

BroadcastWindow::BroadcastWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("赛事转播画面"));
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_DeleteOnClose, false);

    m_teamNames.insert(kRedTeam, tr("红方"));
    m_teamNames.insert(kBlueTeam, tr("蓝方"));

    m_matchTimer = new QTimer(this);
    m_matchTimer->setInterval(1000);
    connect(m_matchTimer, &QTimer::timeout, this, [this] {
        if (m_remainingSeconds > 0)
            --m_remainingSeconds;

        if (m_remainingSeconds > 0) {
            updateTimerDisplay();
        } else if (m_autoFinishOnTimeout) {
            finishMatch(settlementTypeForScores());
        } else {
            // The owner (MatchReferee or the server for player clients)
            // decides the result; just freeze the clock at 00:00.
            m_matchTimer->stop();
            m_matchRunning = false;
            updateTimerDisplay();
            emit matchStateChanged(false);
            emit timeExpired();
        }
    });

    m_summaryRevealTimer = new QTimer(this);
    m_summaryRevealTimer->setSingleShot(true);
    m_summaryRevealTimer->setInterval(350);
    connect(m_summaryRevealTimer, &QTimer::timeout, this, &BroadcastWindow::revealSummaryIfReady);

    // Final-ten-seconds pulse: each tick flashes the clock red and fades it
    // back, giving a clear rhythm without moving any layout.
    m_timerPulse = new QVariantAnimation(this);
    m_timerPulse->setDuration(900);
    m_timerPulse->setStartValue(0.0);
    m_timerPulse->setEndValue(1.0);
    m_timerPulse->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_timerPulse, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        if (!m_timerLabel)
            return;
        const qreal t = value.toReal();
        const QColor hot(0xff, 0x4d, 0x5e);
        const QColor rest(0xff, 0xc4, 0xc9);
        const QColor color(qRound(hot.red() + (rest.red() - hot.red()) * t),
                           qRound(hot.green() + (rest.green() - hot.green()) * t),
                           qRound(hot.blue() + (rest.blue() - hot.blue()) * t));
        m_timerLabel->setStyleSheet(QStringLiteral("color: %1;").arg(color.name()));
    });

    m_programRenderTimer = new QTimer(this);
    m_programRenderTimer->setSingleShot(true);
    // Do not impose a fixed fullscreen FPS. The zero-delay single-shot timer
    // still coalesces bursts, but otherwise renders the newest frame as soon
    // as the event loop is available.
    m_programRenderTimer->setInterval(0);
    connect(m_programRenderTimer, &QTimer::timeout,
            this, &BroadcastWindow::renderProgramFrame);

    m_settlementFrameTimer = new QTimer(this);
    m_settlementFrameTimer->setInterval(kSettlementFrameIntervalMs);
    connect(m_settlementFrameTimer, &QTimer::timeout, this, [this] {
        if (!m_settlementFrames.isEmpty()) {
            renderSettlementFrame(m_settlementFrames.dequeue());
        }

        if (m_settlementProcess && m_settlementFrames.size() < kSettlementFrameQueueLimit)
            consumeSettlementOutput();

        if (m_settlementFinished && m_settlementFrames.isEmpty()
            && m_settlementBuffer.size() < kSettlementFrameBytes) {
            m_settlementFrameTimer->stop();
            QProcess *process = m_settlementProcess;
            m_settlementProcess = nullptr;
            if (process)
                process->deleteLater();
            // The clip is over: the formal verdict card may now appear.
            if (m_competitionOverlay) {
                m_competitionOverlay->setSettlementBlocking(false);
                m_competitionOverlay->raise(); // above the held final clip frame
            }
            // Let the final settlement frame hold briefly before the data
            // panel slides in over it.
            if (m_summaryRevealTimer)
                m_summaryRevealTimer->start();
        }
    });

    buildUi();
    resetMatch();
}

void BroadcastWindow::buildUi()
{
    auto *central = new QWidget(this);
    central->setObjectName(QStringLiteral("broadcastRoot"));
    // The HUD is intentionally allowed to compress below its natural hint;
    // its custom layouts scale authored coordinates to the actual output.
    // Leaving the layout-derived minimum in place would make a 1280x720
    // window grow to 1364x720 before the first resize pass (the unscaled
    // 42 px overlay margins are included in that hint).
    central->setMinimumSize(1, 1);
    central->setStyleSheet(broadcastStyleSheet(1.0));

    auto *stack = new QGridLayout(central);
    stack->setContentsMargins(0, 0, 0, 0);
    stack->setSpacing(0);

    m_programView = new QLabel(tr("等待全场实时转播画面\n请在服务端选择本地摄像头并启动采集"), central);
    m_programView->setObjectName(QStringLiteral("videoBackground"));
    m_programView->setAlignment(Qt::AlignCenter);
    m_programView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    stack->addWidget(m_programView, 0, 0);

    auto *overlay = new HudOverlayWidget(central);
    overlay->setObjectName(QStringLiteral("hudOverlay"));
    overlay->setAttribute(Qt::WA_TransparentForMouseEvents);
    overlay->setStyleSheet(QStringLiteral("background: transparent;"));
    m_hudOverlay = overlay;

    auto *overlayLayout = new QVBoxLayout(overlay);
    overlayLayout->setContentsMargins(42, 34, 42, 34);
    overlayLayout->setSpacing(0);
    m_overlayLayout = overlayLayout;

    auto *topRow = new HudTopLayout;
    overlayLayout->addLayout(topRow);

    RobotOverlay redOverlay;
    auto *redPanel = createRobotOverlay(m_teamNames.value(kRedTeam),
                                        kRedTeam, &redOverlay);
    topRow->addElement(QStringLiteral("red_team_panel"), redPanel);
    m_overlays.insert(kRedTeam, redOverlay);
    registerLayoutElement(QStringLiteral("red_team_panel"), redPanel);

    auto *centerHud = new CenterHudCanvas(overlay);
    auto *centerContent = centerHud->contentWidget();
    auto *centerLayout = new HudCenterContentLayout;
    centerContent->setLayout(centerLayout);

    // Keep the labels inside fixed-height layers. The custom layout owns their
    // geometry, so changing the displayed text cannot move either layer.
    auto *titleLayer = new QWidget(centerContent);
    titleLayer->setObjectName(QStringLiteral("matchTitleLayer"));
    titleLayer->setAttribute(Qt::WA_TranslucentBackground);
    titleLayer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_titleLayer = titleLayer;
    titleLayer->setMinimumSize(1, 1);

    m_matchTitle = makeLabel(titleLayer, Qt::AlignCenter);
    m_matchTitle->setObjectName(QStringLiteral("matchTitle"));
    m_matchTitle->setText(tr("1v1 对抗赛 · 1v1 Соревнование"));
    setBaseFont(m_matchTitle, 15, true);
    titleLayer->setMinimumSize(1, 1);
    titleLayer->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    auto *titleLayerLayout = new QVBoxLayout(titleLayer);
    titleLayerLayout->setSizeConstraint(QLayout::SetNoConstraint);
    titleLayerLayout->setContentsMargins(0, 0, 0, 0);
    titleLayerLayout->addWidget(m_matchTitle);
    centerLayout->addElement(QStringLiteral("match_title"), titleLayer);

    auto *timerLayer = new QWidget(centerContent);
    timerLayer->setObjectName(QStringLiteral("matchTimerLayer"));
    timerLayer->setAttribute(Qt::WA_TranslucentBackground);
    timerLayer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_timerLayer = timerLayer;

    m_timerLabel = new HudDigitsLabel(timerLayer);
    m_timerLabel->setAlignment(Qt::AlignCenter);
    m_timerLabel->setObjectName(QStringLiteral("timer"));
    setBaseFont(m_timerLabel, 40, true);
    m_timerLabel->setMinimumSize(1, 1);
    timerLayer->setMinimumSize(1, 1);
    auto *timerLayerLayout = new QVBoxLayout(timerLayer);
    timerLayerLayout->setSizeConstraint(QLayout::SetNoConstraint);
    timerLayerLayout->setContentsMargins(0, 0, 0, 0);
    timerLayerLayout->addWidget(m_timerLabel, 0, Qt::AlignCenter);
    centerLayout->addElement(QStringLiteral("match_timer"), timerLayer);
    m_redRoundScoreLabel = centerHud->leftScoreLabel();
    m_blueRoundScoreLabel = centerHud->rightScoreLabel();
    topRow->addElement(QStringLiteral("center_hud"), centerHud);
    registerLayoutElement(QStringLiteral("center_hud"), centerHud);
    registerLayoutElement(QStringLiteral("match_title"), titleLayer);
    registerLayoutElement(QStringLiteral("match_timer"), timerLayer);
    registerLayoutElement(QStringLiteral("red_round_score"), m_redRoundScoreLabel);
    registerLayoutElement(QStringLiteral("blue_round_score"), m_blueRoundScoreLabel);

    RobotOverlay blueOverlay;
    auto *bluePanel = createRobotOverlay(m_teamNames.value(kBlueTeam),
                                         kBlueTeam, &blueOverlay);
    topRow->addElement(QStringLiteral("blue_team_panel"), bluePanel);
    m_overlays.insert(kBlueTeam, blueOverlay);
    registerLayoutElement(QStringLiteral("blue_team_panel"), bluePanel);
    overlayLayout->addStretch(1);

    auto *bottomRow = new HudBottomLayout;
    overlayLayout->addLayout(bottomRow);
    m_matchState = makeLabel(overlay, Qt::AlignLeft | Qt::AlignVCenter);
    m_matchState->setObjectName(QStringLiteral("matchState"));
    setBaseFont(m_matchState, 20, false);
    bottomRow->addElement(QStringLiteral("match_state"), m_matchState);
    m_sourceLabel = makeLabel(overlay, Qt::AlignRight | Qt::AlignVCenter);
    m_sourceLabel->setObjectName(QStringLiteral("sourceLabel"));
    m_sourceLabel->setText(tr("LIVE · %1").arg(m_activeSourceTitle));
    setBaseFont(m_sourceLabel, 20, true);
    bottomRow->addElement(QStringLiteral("source_label"), m_sourceLabel);
    registerLayoutElement(QStringLiteral("match_state"), m_matchState);
    registerLayoutElement(QStringLiteral("source_label"), m_sourceLabel);

    m_layoutManagers.insert(QStringLiteral("red_team_panel"), topRow);
    m_layoutManagers.insert(QStringLiteral("center_hud"), topRow);
    m_layoutManagers.insert(QStringLiteral("blue_team_panel"), topRow);
    m_layoutManagers.insert(QStringLiteral("match_title"), centerLayout);
    m_layoutManagers.insert(QStringLiteral("match_timer"), centerLayout);
    m_layoutManagers.insert(QStringLiteral("match_state"), bottomRow);
    m_layoutManagers.insert(QStringLiteral("source_label"), bottomRow);

    m_tickerBar = new QFrame(overlay);
    m_tickerBar->setObjectName(QStringLiteral("tickerBar"));
    m_tickerBar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_tickerBar->setMinimumHeight(0);
    m_tickerBar->setMaximumHeight(0);
    auto *tickerLayout = new QHBoxLayout(m_tickerBar);
    tickerLayout->setContentsMargins(18, 3, 18, 3);
    tickerLayout->setSpacing(14);

    auto *tickerTag = new QLabel(tr("TICKER"), m_tickerBar);
    tickerTag->setObjectName(QStringLiteral("tickerTag"));
    tickerTag->setAlignment(Qt::AlignCenter);
    tickerTag->setMinimumWidth(68);
    tickerLayout->addWidget(tickerTag, 0, Qt::AlignVCenter);
    m_tickerTagLabel = tickerTag;
    setBaseFont(tickerTag, tickerTag->font().pointSizeF(), true);

    m_tickerTextView = new TickerMarqueeWidget(m_tickerBar);
    setBaseFont(m_tickerTextView, 22, true);
    tickerLayout->addWidget(m_tickerTextView, 1);
    m_tickerText = tr("欢迎来到赛事转播现场 · 比赛即将开始");
    m_tickerTextView->setText(m_tickerText);

    m_tickerOpacity = new QGraphicsOpacityEffect(m_tickerBar);
    m_tickerOpacity->setOpacity(0.0);
    m_tickerBar->setGraphicsEffect(m_tickerOpacity);
    m_tickerHeightAnimation = new QPropertyAnimation(m_tickerBar, "maximumHeight", this);
    m_tickerHeightAnimation->setDuration(180);
    m_tickerHeightAnimation->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_tickerHeightAnimation, &QPropertyAnimation::valueChanged, this,
            [this](const QVariant &value) {
                if (!m_tickerBar)
                    return;
                const int height = qMax(0, value.toInt());
                m_tickerBar->setMinimumHeight(height);
            });
    m_tickerOpacityAnimation = new QPropertyAnimation(m_tickerOpacity, "opacity", this);
    m_tickerOpacityAnimation->setDuration(150);
    m_tickerOpacityAnimation->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_tickerHeightAnimation, &QPropertyAnimation::finished, this, [this] {
        if (!m_tickerVisible && m_tickerBar) {
            m_tickerBar->hide();
            m_tickerBar->setMinimumHeight(0);
            m_tickerBar->setMaximumHeight(0);
        }
    });
    overlayLayout->addWidget(m_tickerBar);
    m_tickerBar->hide();

    stack->addWidget(overlay, 0, 0);

    m_interactionLayer = new QWidget(central);
    m_interactionLayer->setObjectName(QStringLiteral("interactionLayer"));
    m_interactionLayer->setAttribute(Qt::WA_TranslucentBackground);
    m_interactionLayer->setAutoFillBackground(false);
    m_interactionLayer->hide();
    stack->addWidget(m_interactionLayer, 0, 0);

    m_settlementView = new QLabel(central);
    m_settlementView->setObjectName(QStringLiteral("settlementView"));
    m_settlementView->setAlignment(Qt::AlignCenter);
    m_settlementView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_settlementView->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_settlementView->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
    m_settlementView->setGeometry(central->rect());
    m_settlementView->hide();

    m_summaryPanel = new MatchSummaryPanel(central);
    m_summaryPanel->onToggled = [this](bool expanded) { emit summaryExpandedChanged(expanded); };

    // Formal phases have their own self-drawn presentation layer (countdown,
    // launch, verdict card, intermission, BO3 final). It covers the whole
    // central widget, is mouse-transparent and sits above the settlement
    // view; it hides itself while the clip plays so the clip is never covered.
    m_competitionOverlay = new CompetitionOverlay(central);
    m_competitionOverlay->setObjectName(QStringLiteral("competitionOverlay"));
    m_competitionOverlay->setGeometry(central->rect());
    m_competitionOverlay->clear();

    setCentralWidget(central);
    setMinimumSize(1, 1);
}

QWidget *BroadcastWindow::createRobotOverlay(const QString &teamTitle,
                                             quint8 team,
                                             RobotOverlay *overlay)
{
    auto *frame = new QFrame(this);
    frame->setObjectName(QStringLiteral("teamHud"));
    frame->setFrameShape(QFrame::NoFrame);
    frame->setProperty("team", team == kRedTeam ? QStringLiteral("red")
                                                 : QStringLiteral("blue"));
    auto *layout = new HudTeamLayout(frame);

    overlay->teamId = team;
    overlay->team = makeLabel(frame, team == kRedTeam ? Qt::AlignRight : Qt::AlignLeft);
    overlay->team->setObjectName(team == kRedTeam ? QStringLiteral("redTeam")
                                                  : QStringLiteral("blueTeam"));
    overlay->team->setText(teamTitle);
    setBaseFont(overlay->team, 20, true);

    auto *teamRow = new QWidget(frame);
    auto *teamRowLayout = new QHBoxLayout(teamRow);
    teamRowLayout->setContentsMargins(0, 0, 0, 0);
    teamRowLayout->setSpacing(10);
    overlay->connection = makeLabel(
        frame,
        team == kRedTeam ? Qt::AlignRight | Qt::AlignVCenter
                         : Qt::AlignLeft | Qt::AlignVCenter);
    overlay->connection->setObjectName(QStringLiteral("connectionStatus"));
    overlay->connection->setMinimumWidth(68);
    setBaseFont(overlay->connection, 18, true);
    teamRowLayout->setSizeConstraint(QLayout::SetNoConstraint);
    teamRowLayout->addWidget(overlay->team);
    overlay->connection->hide();
    layout->addElement(QStringLiteral("team_row"), teamRow);

    auto *cardRow = new QWidget(frame);
    auto *cardRowLayout = new QHBoxLayout(cardRow);
    cardRowLayout->setContentsMargins(0, 0, 0, 0);
    cardRowLayout->setSpacing(8);
    const auto addCardIndicator = [frame](const QRect &sourceRect,
                                           const QString &labelText,
                                           QLabel **countLabel,
                                           QLabel **iconLabel) {
        auto *indicator = new QWidget(frame);
        auto *indicatorLayout = new QHBoxLayout(indicator);
        indicatorLayout->setContentsMargins(0, 0, 0, 0);
        indicatorLayout->setSpacing(3);

        auto *icon = new QLabel(indicator);
        icon->setFixedSize(24, 30);
        icon->setProperty("atlasRect", sourceRect);
        const QPixmap pixmap = assetCrop(QStringLiteral(":/broadcast/statusbar_atlas.png"),
                                         sourceRect);
        icon->setPixmap(pixmap.scaled(icon->size(), Qt::KeepAspectRatio,
                                      Qt::SmoothTransformation));
        icon->setAlignment(Qt::AlignCenter);
        indicatorLayout->addWidget(icon);
        if (iconLabel)
            *iconLabel = icon;

        auto *count = new QLabel(QStringLiteral("0"), indicator);
        count->setAlignment(Qt::AlignCenter);
        count->setToolTip(labelText);
        count->setStyleSheet(QStringLiteral("color: #f4f7fa; background: transparent;"));
        indicatorLayout->addWidget(count);
        if (countLabel)
            *countLabel = count;
        return indicator;
    };
    cardRowLayout->addStretch(1);
    cardRowLayout->addWidget(addCardIndicator(QRect(4, 269, 53, 66), tr("红牌"),
                                              &overlay->redCard,
                                              &overlay->redCardIcon));
    cardRowLayout->addWidget(addCardIndicator(QRect(897, 472, 53, 66), tr("黄牌"),
                                              &overlay->yellowCard,
                                              &overlay->yellowCardIcon));
    cardRowLayout->addStretch(1);
    layout->addElement(QStringLiteral("card_row"), cardRow);
    // Self-drawn "mech core" revive icons; text() keeps the plain description.
    overlay->reviveCount = new ReviveIndicator(team == kRedTeam, frame);
    overlay->reviveCount->setObjectName(team == kRedTeam ? QStringLiteral("redReviveCount")
                                                        : QStringLiteral("blueReviveCount"));
    layout->addElement(QStringLiteral("revive_count"), overlay->reviveCount);
    overlay->reviveCount->hide();
    setBaseFont(overlay->redCard, 18, true);
    setBaseFont(overlay->yellowCard, 18, true);

    auto *healthBar = new AtlasProgressBar(
        assetCrop(QStringLiteral(":/broadcast/statusbar_atlas.png"),
                  team == kRedTeam ? QRect(2, 851, 373, 56)
                                   : QRect(386, 851, 373, 56)),
        team == kRedTeam,
        team == kRedTeam,
        frame, true);
    healthBar->setRange(0, kMaxHp);
    healthBar->setObjectName(QStringLiteral("healthBar"));
    healthBar->setMinimumSize(1, 1);
    overlay->healthBar = healthBar;
    layout->addElement(QStringLiteral("health_bar"), healthBar);

    auto *heatBar = new AtlasProgressBar(
        assetCrop(QStringLiteral(":/broadcast/statusbar_atlas.png"),
                  QRect(211, 410, 276, 24)),
        team == kBlueTeam,
        false,
        frame);
    heatBar->setRange(0, kMaxHeat);
    heatBar->setObjectName(QStringLiteral("heatBar"));
    heatBar->setFixedHeight(20);
    overlay->heatBar = heatBar;
    heatBar->hide(); // Retain the model binding, not a second visible ribbon.

    overlay->damageNotice = makeLabel(frame, Qt::AlignCenter);
    overlay->damageNotice->setObjectName(QStringLiteral("damageNotice"));
    setBaseFont(overlay->damageNotice, 25, true);
    overlay->damageNotice->setFixedSize(160, 36);
    overlay->damageNotice->move(0, 0);
    overlay->damageNotice->setVisible(false);
    overlay->damageNotice->setAttribute(Qt::WA_TransparentForMouseEvents);
    overlay->damageOpacity = new QGraphicsOpacityEffect(overlay->damageNotice);
    overlay->damageOpacity->setOpacity(1.0);
    overlay->damageNotice->setGraphicsEffect(overlay->damageOpacity);
    overlay->damageAnimation = new QPropertyAnimation(overlay->damageOpacity, "opacity",
                                                      overlay->damageNotice);
    overlay->damageAnimation->setDuration(900);
    QLabel *damageNotice = overlay->damageNotice;
    QGraphicsOpacityEffect *damageOpacity = overlay->damageOpacity;
    connect(overlay->damageAnimation, &QPropertyAnimation::finished, this,
            [damageNotice, damageOpacity] {
        damageNotice->setVisible(false);
        damageOpacity->setOpacity(1.0);
    });
    overlay->damageDropAnimation = new QPropertyAnimation(overlay->damageNotice,
                                                           "pos",
                                                           overlay->damageNotice);
    overlay->damageDropAnimation->setDuration(760);
    overlay->damageDropAnimation->setEasingCurve(QEasingCurve::OutCubic);

    auto *atlasHealthBar = static_cast<AtlasProgressBar *>(healthBar);
    overlay->damageBufferAnimation = new QVariantAnimation(frame);
    overlay->damageBufferAnimation->setDuration(680);
    overlay->damageBufferAnimation->setEasingCurve(QEasingCurve::OutCubic);
    connect(overlay->damageBufferAnimation, &QVariantAnimation::valueChanged, this,
            [atlasHealthBar](const QVariant &value) {
        atlasHealthBar->setDamageBufferValue(value.toInt());
    });
    connect(overlay->damageBufferAnimation, &QVariantAnimation::finished, this,
            [atlasHealthBar] {
        atlasHealthBar->setDamageBufferValue(atlasHealthBar->value());
    });

    auto *statusRow = new QGridLayout;
    statusRow->setContentsMargins(0, 0, 0, 0);
    statusRow->setHorizontalSpacing(0);
    statusRow->setVerticalSpacing(0);
    for (int column = 0; column < 4; ++column)
        statusRow->setColumnStretch(column, 1);
    statusRow->setRowMinimumHeight(0, 28);
    overlay->health = makeLabel(frame);
    overlay->state = makeLabel(frame, Qt::AlignCenter);
    overlay->shoot = makeLabel(frame, Qt::AlignRight | Qt::AlignVCenter);
    for (QLabel *label : {overlay->health, overlay->state, overlay->shoot}) {
        setBaseFont(label, 21, false);
    }
    overlay->heat = makeLabel(frame);
    overlay->health->setObjectName(QStringLiteral("healthText"));
    overlay->heat->setObjectName(QStringLiteral("heatText"));
    overlay->state->setObjectName(QStringLiteral("lifeState"));
    overlay->shoot->setObjectName(QStringLiteral("shootState"));
    overlay->redCard->setObjectName(QStringLiteral("redCardCount"));
    overlay->yellowCard->setObjectName(QStringLiteral("yellowCardCount"));
    setBaseFont(overlay->heat, 18, false);
    overlay->health->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    overlay->heat->setAlignment(Qt::AlignCenter);
    overlay->state->setAlignment(Qt::AlignCenter);
    overlay->shoot->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    overlay->health->setAlignment(Qt::AlignCenter);
    overlay->health->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->addElement(QStringLiteral("health_text"), overlay->health);
    statusRow->addWidget(overlay->heat, 0, 1);
    statusRow->addWidget(overlay->state, 0, 2);
    statusRow->addWidget(overlay->shoot, 0, 3);
    auto *statusWidget = new QWidget(frame);
    statusWidget->setLayout(statusRow);
    statusWidget->hide();
    overlay->diagnostics = makeLabel(frame, Qt::AlignCenter);
    overlay->diagnostics->setObjectName(QStringLiteral("deviceDiagnostics"));
    setBaseFont(overlay->diagnostics, 13, false);
    overlay->diagnostics->hide();

    auto *stateLayer = new HudStateLayer(team == kRedTeam ? QColor(QStringLiteral("#e24e5a"))
                                                          : QColor(QStringLiteral("#4a9cf5")),
                                         frame);
    overlay->stateLayer = stateLayer;
    layout->addElement(QStringLiteral("state_layer"), stateLayer);

    updateOverlay(nullptr, *overlay, RobotManager::keyOf(team, 0));
    return frame;
}

void BroadcastWindow::updateOverlay(const RobotManager::RobotInfo *robot,
                                    RobotOverlay &overlay,
                                    quint64 robotKey)
{
    overlay.team->setText(m_teamNames.value(overlay.teamId, defaultTeamName(overlay.teamId)));
    const int redCardCount = m_redCards.value(overlay.teamId, 0);
    const int yellowCardCount = m_yellowCards.value(overlay.teamId, 0);
    overlay.redCard->setText(QString::number(redCardCount));
    overlay.yellowCard->setText(QString::number(yellowCardCount));
    overlay.redCard->parentWidget()->setVisible(redCardCount > 0);
    overlay.yellowCard->parentWidget()->setVisible(yellowCardCount > 0);
    if (overlay.robotKey != robotKey) {
        overlay.damageBufferAnimation->stop();
        overlay.damageAnimation->stop();
        overlay.damageDropAnimation->stop();
        overlay.damageNotice->hide();
        m_lastHp.remove(robotKey);
        overlay.robotKey = robotKey;
    }

    if (!robot) {
        if (overlay.damageBufferAnimation)
            overlay.damageBufferAnimation->stop();
        if (overlay.damageDropAnimation)
            overlay.damageDropAnimation->stop();
        if (overlay.damageAnimation)
            overlay.damageAnimation->stop();
        overlay.healthBar->setValue(0);
        static_cast<AtlasProgressBar *>(overlay.healthBar)->setDamageBufferValue(0);
        overlay.heatBar->setValue(0);
        overlay.connection->setText(tr("未连接"));
        overlay.health->setText(QStringLiteral("--"));
        overlay.heat->setText(tr("热量 --"));
        overlay.state->setText(tr("等待状态"));
        overlay.shoot->setText(tr("射击 --"));
        overlay.diagnostics->setText(tr("功率 -- · 供电 -- · 链路 --"));
        setTextColor(overlay.diagnostics, QColor(QStringLiteral("#c7d0d6")));
        overlay.damageNotice->setVisible(false);
        overlay.damageOpacity->setOpacity(1.0);
        setTextColor(overlay.connection, QColor(QStringLiteral("#c7d0d6")), true);
        setTextColor(overlay.health, QColor(QStringLiteral("#c7d0d6")));
        setTextColor(overlay.state, QColor(QStringLiteral("#c7d0d6")));
        setTextColor(overlay.shoot, QColor(QStringLiteral("#c7d0d6")));
        setTextColor(overlay.heat, QColor(QStringLiteral("#8d9aa4")));
        static_cast<HudStateLayer *>(overlay.stateLayer)->setDown(false);
        return;
    }

    const int maxHp = robot->protocolVersion == proto::kVersion1 ? kLegacyMaxHp : kMaxHp;
    const bool rangeChanged = overlay.healthBar->maximum() != maxHp;
    overlay.healthBar->setRange(0, maxHp);
    if (robot->hp >= 0) {
        const int hp = qBound(0, robot->hp, maxHp);
        const int previousHp = rangeChanged ? -1 : m_lastHp.value(robotKey, -1);
        auto *atlasHealthBar = static_cast<AtlasProgressBar *>(overlay.healthBar);
        const bool isDamage = previousHp >= 0 && hp < previousHp;
        if (isDamage) {
            triggerDamageEffect(overlay, previousHp - hp, previousHp, hp);
        } else if (rangeChanged || hp > previousHp) {
            overlay.damageBufferAnimation->stop();
            overlay.damageAnimation->stop();
            overlay.damageDropAnimation->stop();
            overlay.damageNotice->hide();
            atlasHealthBar->setDamageBufferValue(hp);
        } else if (!overlay.damageBufferAnimation
                   || overlay.damageBufferAnimation->state() != QAbstractAnimation::Running) {
            atlasHealthBar->setDamageBufferValue(hp);
        }
        m_lastHp.insert(robotKey, hp);
        overlay.healthBar->setValue(hp);
        overlay.health->setText(QString::number(robot->hp));
    } else {
        if (overlay.damageBufferAnimation)
            overlay.damageBufferAnimation->stop();
        overlay.healthBar->setValue(0);
        static_cast<AtlasProgressBar *>(overlay.healthBar)->setDamageBufferValue(0);
        overlay.health->setText(QStringLiteral("--"));
    }

    if (robot->heat >= 0) {
        overlay.heatBar->setValue(qBound(0, robot->heat, kMaxHeat));
        overlay.heat->setText(tr("热量 %1").arg(robot->heat));
    } else {
        overlay.heatBar->setValue(0);
        overlay.heat->setText(tr("热量 --"));
    }

    if (!robot->online) {
        overlay.damageBufferAnimation->stop();
        overlay.damageAnimation->stop();
        overlay.damageDropAnimation->stop();
        overlay.damageNotice->hide();
        static_cast<AtlasProgressBar *>(overlay.healthBar)->setDamageBufferValue(overlay.healthBar->value());
        overlay.diagnostics->setText(tr("功率 -- · 供电 -- · 链路 --"));
        setTextColor(overlay.diagnostics, QColor(QStringLiteral("#c7d0d6")));
        overlay.connection->setText(tr("离线"));
        setTextColor(overlay.connection, QColor(QStringLiteral("#ffb35c")), true);
        overlay.state->setText(tr("状态未知"));
        overlay.shoot->setText(tr("射击 --"));
        setTextColor(overlay.state, QColor(QStringLiteral("#c7d0d6")), true);
        setTextColor(overlay.shoot, QColor(QStringLiteral("#c7d0d6")));
        setTextColor(overlay.heat, QColor(QStringLiteral("#8d9aa4")));
        // Stale data: keep the dim wash so an offline robot reads as inactive.
        static_cast<HudStateLayer *>(overlay.stateLayer)->setDown(true);
        return;
    }

    overlay.connection->setText(tr("在线"));
    if (robot->protocolVersion == proto::kVersion2) {
        overlay.diagnostics->setText(tr("功率 %1 W · %2 · 链路%3")
            .arg(robot->power >= 0 ? QString::number(robot->power) : QStringLiteral("--"))
            .arg(robot->powerOn ? tr("已通电") : tr("已断电"))
            .arg(robot->linkUp ? tr("正常") : tr("断开")));
        setTextColor(overlay.diagnostics, robot->powerOn && robot->linkUp
            ? QColor(QStringLiteral("#c7d0d6")) : QColor(QStringLiteral("#ffb35c")));
    } else {
        overlay.diagnostics->setText(tr("V1 · 功率/供电/链路未上报"));
        setTextColor(overlay.diagnostics, QColor(QStringLiteral("#c7d0d6")));
    }
    setTextColor(overlay.connection, QColor(QStringLiteral("#72d39a")), true);
    overlay.state->setText(robot->alive ? tr("存活") : tr("阵亡"));
    setTextColor(overlay.heat, overlay.teamId == kRedTeam ? QColor(QStringLiteral("#f5c98c"))
                                                          : QColor(QStringLiteral("#9fe2ec")));
    static_cast<HudStateLayer *>(overlay.stateLayer)->setDown(!robot->alive || robot->hp == 0);
    overlay.shoot->setText(robot->shootEnabled ? tr("射击 开放") : tr("射击 禁止"));
    setTextColor(overlay.health, QColor(QStringLiteral("#f4f7fa")), true);
    setTextColor(overlay.state,
                 robot->alive ? QColor(QStringLiteral("#72d39a"))
                              : QColor(QStringLiteral("#ff6872")),
                 true);
    setTextColor(overlay.shoot,
                 robot->shootEnabled ? QColor(QStringLiteral("#72d39a"))
                                     : QColor(QStringLiteral("#ff6872")),
                 true);
}

void BroadcastWindow::triggerDamageEffect(RobotOverlay &overlay,
                                           int damage,
                                           int previousHp,
                                           int currentHp)
{
    if (!overlay.damageNotice || !overlay.damageAnimation || !overlay.damageDropAnimation
        || !overlay.damageBufferAnimation || !overlay.healthBar || !overlay.heatBar)
        return;

    auto *atlasHealthBar = static_cast<AtlasProgressBar *>(overlay.healthBar);
    const int bufferStart = qMax(previousHp, atlasHealthBar->damageBufferValue());
    atlasHealthBar->setValue(currentHp);
    atlasHealthBar->setDamageBufferValue(bufferStart);
    overlay.damageBufferAnimation->stop();
    overlay.damageBufferAnimation->setStartValue(bufferStart);
    overlay.damageBufferAnimation->setEndValue(currentHp);
    overlay.damageBufferAnimation->start();

    if (overlay.stateLayer)
        static_cast<HudStateLayer *>(overlay.stateLayer)->flashHit();
    // Keep the clean header free of floating damage/debug text; the ribbon
    // damage trail and player combat overlay provide the visual feedback.
    overlay.damageNotice->hide();
    Q_UNUSED(damage);
}

const RobotManager::RobotInfo *BroadcastWindow::robotForTeam(
    quint8 team,
    const QHash<quint64, RobotManager::RobotInfo> &robots) const
{
    const RobotManager::RobotInfo *selected = nullptr;
    for (auto it = robots.constBegin(); it != robots.constEnd(); ++it) {
        if (it.value().team != team)
            continue;
        if (!selected || (it.value().online && !selected->online)
            || (it.value().online == selected->online && it.value().robotId < selected->robotId)) {
            selected = &it.value();
        }
    }
    return selected;
}

void BroadcastWindow::updateRobots(const QVector<RobotManager::RobotInfo> &robots)
{
    QHash<quint64, RobotManager::RobotInfo> current;
    for (const auto &robot : robots)
        current.insert(RobotManager::keyOf(robot.team, robot.robotId), robot);

    m_onlineCount = 0;
    for (const quint8 team : {kRedTeam, kBlueTeam}) {
        const RobotManager::RobotInfo *selected = robotForTeam(team, current);
        if (selected && selected->online)
            ++m_onlineCount;
    }

    for (auto it = m_overlays.begin(); it != m_overlays.end(); ++it) {
        const quint8 team = static_cast<quint8>(it.key());
        const RobotManager::RobotInfo *selected = robotForTeam(team, current);
        const quint64 key = selected ? RobotManager::keyOf(team, selected->robotId)
                                     : RobotManager::keyOf(team, 0);
        updateOverlay(selected, it.value(), key);
    }

    updateMatchState();
}

void BroadcastWindow::updateMatchState()
{
    if (m_competitionMode) {
        updateCompetitionPresentation();
        return;
    }
    QString phase;
    if (m_roundEnded)
        phase = tr("比赛结束");
    else if (m_suddenDeath)
        phase = m_matchRunning ? tr("SUDDEN DEATH · 一命定胜负") : tr("SUDDEN DEATH · 已暂停");
    else if (m_remainingSeconds == 0)
        phase = tr("时间到 · 判定中");
    else if (m_matchRunning)
        phase = tr("比赛进行中");
    else if (m_remainingSeconds < m_clockDuration)
        phase = tr("比赛暂停");
    else
        phase = tr("准备开始");

    const bool alert = !m_roundEnded && !m_refereeNotice.isEmpty();
    const QString verdict = m_roundEnded
                                ? m_matchSummary.value(QStringLiteral("reasonText")).toString()
                                : QString();
    if (alert && m_refereeNotice.startsWith(phase))
        m_matchState->setText(m_refereeNotice);
    else if (alert)
        m_matchState->setText(tr("%1 · %2").arg(phase, m_refereeNotice));
    else if (!verdict.isEmpty())
        m_matchState->setText(tr("%1 · %2").arg(phase, verdict));
    else
        m_matchState->setText(phase);
    if (m_matchState->property("alert").toBool() != alert) {
        m_matchState->setProperty("alert", alert);
        m_matchState->style()->unpolish(m_matchState);
        m_matchState->style()->polish(m_matchState);
    }
}

void BroadcastWindow::setRefereeNotice(const QString &notice)
{
    if (notice == m_refereeNotice)
        return;
    m_refereeNotice = notice;
    updateMatchState();
    emit presentationStateChanged();
}

void BroadcastWindow::updateTimerDisplay()
{
    const int minutes = m_remainingSeconds / 60;
    const int seconds = m_remainingSeconds % 60;
    const QString clockText = QStringLiteral("%1:%2")
                                  .arg(minutes, 2, 10, QLatin1Char('0'))
                                  .arg(seconds, 2, 10, QLatin1Char('0'));
    const bool clockChanged = m_timerLabel->text() != clockText;
    m_timerLabel->setText(clockText);

    // Clock colour follows the phase: pulsing red in the final ten seconds,
    // softly cool-white while paused (not dark/occluded), default otherwise.
    const bool finalCountdown = m_matchRunning && m_remainingSeconds > 0
                                && m_remainingSeconds <= 10;
    if (finalCountdown) {
        if (!m_competitionMode || clockChanged) {
            m_timerPulse->stop();
            m_timerPulse->start();
        }
    } else {
        m_timerPulse->stop();
        const bool paused = !m_matchRunning && !m_roundEnded && m_remainingSeconds > 0
                            && m_remainingSeconds < m_clockDuration;
        m_timerLabel->setStyleSheet(paused ? QStringLiteral("color: #dcebf1; background: transparent;")
                                   : m_remainingSeconds == 0 ? QStringLiteral("color: #ff6872;")
                                                             : QString());
    }
    updateMatchState();
    emit presentationStateChanged();
}

void BroadcastWindow::startMatch()
{
    if (m_competitionMode)
        return; // Formal clocks are owned exclusively by the server flow.
    // Starting a finished round resets presentation; the owner resets the
    // authoritative per-round kill score before sending GAME_START.
    if (m_roundEnded || (m_remainingSeconds == 0 && !m_suddenDeath))
        resetRound();
    if (m_matchRunning)
        return;

    stopSettlement();
    m_roundEnded = false;
    m_settlementType.clear();
    m_matchRunning = true;
    if (!m_suddenDeath) m_matchTimer->start();
    updateTimerDisplay();
    emit matchStateChanged(true);
}

void BroadcastWindow::pauseMatch()
{
    if (m_competitionMode)
        return;
    if (!m_matchRunning)
        return;
    m_matchRunning = false;
    m_matchTimer->stop();
    updateTimerDisplay();
    emit matchStateChanged(false);
}

void BroadcastWindow::resetRound()
{
    leaveCompetitionPresentation();
    stopSettlement();
    clearMatchSummary();
    m_matchTimer->stop();
    m_matchRunning = false;
    m_roundEnded = false;
    m_settlementType.clear();
    m_refereeNotice.clear();
    m_suddenDeath = false;
    m_remainingSeconds = m_clockDuration;
    clearCards();
    m_lastHp.clear();
    updateTimerDisplay();
    emit matchStateChanged(false);
}

void BroadcastWindow::resetMatch()
{
    resetRound();
    m_redScore = 0;
    m_blueScore = 0;
    setScores(0, 0);
}

void BroadcastWindow::setMatchState(int remainingSeconds, bool running, bool suddenDeath)
{
    leaveCompetitionPresentation();
    stopSettlement();
    m_suddenDeath = suddenDeath;
    m_remainingSeconds = qBound(0, remainingSeconds, m_clockDuration);
    m_matchRunning = running && (m_remainingSeconds > 0 || m_suddenDeath);
    m_roundEnded = false;
    m_settlementType.clear();
    if (m_matchRunning && !m_suddenDeath)
        m_matchTimer->start();
    else
        m_matchTimer->stop();

    updateTimerDisplay();
    emit matchStateChanged(m_matchRunning);
}

void BroadcastWindow::setClockDuration(int seconds)
{
    const int duration = qMax(1, seconds);
    if (duration == m_clockDuration)
        return;
    const bool atStart = !m_competitionMode && !m_matchRunning && !m_roundEnded
                         && m_remainingSeconds == m_clockDuration;
    m_clockDuration = duration;
    if (!m_competitionMode) {
        m_remainingSeconds = atStart ? duration : qMin(m_remainingSeconds, duration);
        updateTimerDisplay();
    }
}

void BroadcastWindow::leaveCompetitionPresentation()
{
    if (!m_competitionMode)
        return;
    m_competitionMode = false;
    m_competitionState = QJsonObject();
    m_competitionPhase.clear();
    m_competitionReason.clear();
    m_competitionSettlementKey.clear();
    if (m_competitionOverlay)
        m_competitionOverlay->clear(); // Legacy mode: no formal presentation.
    m_matchTitle->setText(tr("1v1 对抗赛 · 1v1 Соревнование"));
    for (auto it = m_overlays.begin(); it != m_overlays.end(); ++it) {
        it.value().reviveCount->hide();
        QWidget *panel = it.value().reviveCount->parentWidget();
        panel->setProperty("competitionMode", false);
        panel->layout()->invalidate();
        panel->layout()->activate();
    }
    clearMatchSummary();
}

void BroadcastWindow::setCompetitionState(const QJsonObject &state)
{
    if (state.value(QStringLiteral("scoringRule")).toString()
        != QStringLiteral("remaining_resources"))
        return;
    const QString phase = state.value(QStringLiteral("phase")).toString();
    const bool boundary = !m_competitionMode
        || state.value(QStringLiteral("matchId")) != m_competitionState.value(QStringLiteral("matchId"))
        || state.value(QStringLiteral("roundId")) != m_competitionState.value(QStringLiteral("roundId"));
    const bool wasRunning = m_matchRunning;
    if (boundary) {
        stopSettlement();
        clearMatchSummary();
        m_competitionReason.clear();
        m_competitionSettlementKey.clear();
        m_lastHp.clear();
        clearCards();
    } else if (phase != m_competitionPhase
               && (phase == QStringLiteral("ready") || phase == QStringLiteral("countdown")
                   || phase == QStringLiteral("starting") || phase == QStringLiteral("running"))) {
        stopSettlement();
    }
    m_competitionMode = true;
    m_competitionState = state;
    m_competitionPhase = phase;
    m_matchTimer->stop(); // Never locally advance/expire an authoritative snapshot.
    m_suddenDeath = false;
    m_remainingSeconds = qMax(0, state.value(QStringLiteral("remainingSeconds")).toInt());
    // After a round ends the main clock freezes on that round's final time; the
    // 120 s preparation countdown belongs only to the overlay's intermission bar,
    // so the HUD never reads "02:00" as if the next round were already running.
    // CompetitionFlow reports roundRemainingSeconds=180 outside Running, so the
    // frozen value is derived from the authoritative round duration instead.
    if (phase == QStringLiteral("intermission") || phase == QStringLiteral("match_finished")
        || phase == QStringLiteral("cancelled")) {
        const QJsonValue duration = state.value(QStringLiteral("summary")).toObject()
                                        .value(QStringLiteral("durationMs"));
        const double ms = duration.isDouble() ? duration.toDouble() : -1.0;
        if (ms >= 0.0 && std::isfinite(ms)) {
            // The round length comes from the active timing preset; older
            // servers that do not send it ran fixed 180 s rounds.
            const int kFormalRoundSeconds = formalRoundSeconds(state);
            const int elapsed = int(qMin(ms, 24.0 * 3600 * 1000) / 1000.0);
            m_remainingSeconds = qBound(0, kFormalRoundSeconds - elapsed, kFormalRoundSeconds);
        } else {
            m_remainingSeconds = 0; // Unknown duration: never imply a running clock.
        }
    }
    m_matchRunning = phase == QStringLiteral("running");
    m_roundEnded = state.value(QStringLiteral("ended")).toBool();
    m_refereeNotice = state.value(QStringLiteral("notice")).toString();
    const QString redName = state.value(QStringLiteral("redName")).toString();
    const QString blueName = state.value(QStringLiteral("blueName")).toString();
    if (!redName.isEmpty() || !blueName.isEmpty())
        setTeamNames(redName.isEmpty() ? redTeamName() : redName,
                     blueName.isEmpty() ? blueTeamName() : blueName);
    const int red = state.value(QStringLiteral("redScore")).toInt();
    const int blue = state.value(QStringLiteral("blueScore")).toInt();
    if (boundary || red != m_redScore || blue != m_blueScore)
        setScores(red, blue);

    const QJsonObject summary = state.value(QStringLiteral("summary")).toObject();
    if (!summary.isEmpty() && summary.value(QStringLiteral("valid")).toBool(true)) {
        m_matchSummary = summary;
        const QString reason = summary.value(QStringLiteral("reasonText")).toString().trimmed();
        if (!reason.isEmpty())
            m_competitionReason = reason;
    }
    // Repeated 100 ms snapshots never clear summary or restart a clip. The
    // normal legacy summary API remains available outside formal presentation.
    m_summaryRevealTimer->stop();
    m_summaryPanel->hide();
    const QString settlement = state.value(QStringLiteral("settlement")).toString().trimmed().toLower();
    const QString settlementKey = state.value(QStringLiteral("matchId")).toString()
        + QLatin1Char('/') + state.value(QStringLiteral("roundId")).toString()
        + QLatin1Char('/') + settlement;
    if (m_roundEnded && !settlement.isEmpty()
        && m_competitionSettlementKey != settlementKey) {
        m_competitionSettlementKey = settlementKey;
        playSettlement(settlement); // Use the explicit server verdict only.
    } else if (!settlementPlaying()) {
        m_settlementType = settlement;
    }
    // After the settlement decision so a clip that just started already
    // blocks the verdict card (no one-frame flash over the video).
    if (m_competitionOverlay) {
        m_competitionOverlay->setSettlementBlocking(settlementPlaying());
        m_competitionOverlay->setCompetitionState(state);
    }
    updateWinnerHighlight();
    updateTimerDisplay();
    if (wasRunning != m_matchRunning)
        emit matchStateChanged(m_matchRunning);
}

void BroadcastWindow::updateCompetitionPresentation()
{
    QString phaseText;
    if (m_competitionPhase == QStringLiteral("ready"))
        phaseText = tr("准备就绪 · 等待裁判开赛");
    else if (m_competitionPhase == QStringLiteral("countdown"))
        phaseText = tr("开赛倒计时");
    else if (m_competitionPhase == QStringLiteral("starting"))
        phaseText = tr("设备确认中 · 等待双方开赛确认");
    else if (m_competitionPhase == QStringLiteral("running"))
        phaseText = tr("正式局进行中 · %1").arg(durationText(formalRoundSeconds(m_competitionState)));
    else if (m_competitionPhase == QStringLiteral("awaiting_decision"))
        phaseText = tr("等待裁判判定");
    else if (m_competitionPhase == QStringLiteral("intermission"))
        // m_remainingSeconds is the frozen round clock here; the preparation
        // countdown is the raw snapshot value.
        phaseText = m_competitionState.value(QStringLiteral("remainingSeconds")).toInt() > 0
                        ? tr("局间准备") : tr("下局准备完 · 等待裁判");
    else if (m_competitionPhase == QStringLiteral("match_finished"))
        phaseText = tr("BO3 比赛结束");
    else if (m_competitionPhase == QStringLiteral("cancelled"))
        phaseText = tr("比赛已取消");
    else
        phaseText = tr("等待赛事状态");
    const int round = qMax(1, m_competitionState.value(QStringLiteral("roundIndex")).toInt(1));
    m_matchTitle->setText(tr("BO3 · 第 %1 小局 · 胜局 %2 : %3")
                             .arg(round).arg(m_redScore).arg(m_blueScore));
    // The server notice often repeats the phase name ("开赛倒计时"); show it once.
    const QString notice = m_refereeNotice == phaseText ? QString() : m_refereeNotice;
    m_matchState->setText(notice.isEmpty() ? phaseText : tr("%1 · %2").arg(phaseText, notice));
    const bool alert = !notice.isEmpty();
    if (m_matchState->property("alert").toBool() != alert) {
        m_matchState->setProperty("alert", alert);
        m_matchState->style()->unpolish(m_matchState);
        m_matchState->style()->polish(m_matchState);
    }
    for (const quint8 team : {kRedTeam, kBlueTeam}) {
        RobotOverlay &overlay = m_overlays[team];
        const QString prefix = team == kRedTeam ? QStringLiteral("red") : QStringLiteral("blue");
        const QJsonValue value = m_competitionState.value(prefix + QStringLiteral("RemainingRevives"));
        const int count = value.isDouble() ? value.toInt(-1) : -1; // out of 0..3 = unknown
        const bool pending = m_competitionState.value(prefix + QStringLiteral("RevivePending")).toBool();
        // Snap while hidden (first formal snapshot); animate consumption/refill after.
        overlay.reviveCount->setResourceState(count, pending, !overlay.reviveCount->isHidden());
        overlay.reviveCount->show();
        QWidget *panel = overlay.reviveCount->parentWidget();
        if (!panel->property("competitionMode").toBool()) {
            panel->setProperty("competitionMode", true);
            panel->layout()->invalidate();
            panel->layout()->activate();
        }
    }
    layoutCompetitionPresentation();
}

void BroadcastWindow::layoutCompetitionPresentation()
{
    if (!m_competitionOverlay || !centralWidget())
        return;
    QWidget *central = centralWidget();
    if (m_competitionOverlay->geometry() != central->rect())
        m_competitionOverlay->setGeometry(central->rect());
    if (!m_competitionMode)
        return;
    const qreal scale = m_uiScale;
    const auto px = [scale](int value) { return qMax(1, qRound(value * scale)); };
    const QRect viewport = scaledHudViewport(this, scale);
    // The compact intermission bar / verdict card must stay above the bottom
    // status line and the ticker.
    int safeBottom = viewport.bottom() - px(m_tickerVisible ? 164 : 100);
    if (m_matchState && m_matchState->isVisible()) {
        const int stateTop = m_matchState->mapTo(central, QPoint()).y();
        if (stateTop > viewport.y() + px(500))
            safeBottom = qMin(safeBottom, stateTop - px(14));
    }
    if (m_tickerBar && m_tickerBar->isVisible() && m_tickerBar->height() > 0) {
        const int tickerTop = m_tickerBar->mapTo(central, QPoint()).y();
        if (tickerTop > viewport.y() + px(500))
            safeBottom = qMin(safeBottom, tickerTop - px(14));
    }
    m_competitionOverlay->setLayoutMetrics(viewport, scale, safeBottom);
    // Above the settlement view; the overlay hides itself while the clip plays.
    m_competitionOverlay->raise();
}
void BroadcastWindow::terminateMatch()
{
    if (m_competitionMode || m_roundEnded)
        return;
    finishMatch(QStringLiteral("termination"));
}

void BroadcastWindow::concludeRound(const QString &settlementType)
{
    if (m_competitionMode || m_roundEnded || settlementType.trimmed().isEmpty())
        return;
    finishMatch(settlementType.trimmed().toLower());
}

void BroadcastWindow::finishMatch(const QString &settlementType)
{
    m_refereeNotice.clear();
    m_matchTimer->stop();
    m_matchRunning = false;
    m_roundEnded = true;
    m_settlementType = settlementType;
    updateWinnerHighlight();
    updateTimerDisplay();
    emit matchStateChanged(false);
    playSettlement(settlementType);
    emit roundFinished(settlementType);
    emit presentationStateChanged();
}

QString BroadcastWindow::settlementTypeForScores() const
{
    if (m_redScore > m_blueScore)
        return QStringLiteral("redwin");
    if (m_blueScore > m_redScore)
        return QStringLiteral("bluewin");
    return QStringLiteral("draw");
}

QString BroadcastWindow::settlementAssetDirectory() const
{
    const QStringList candidates = {
        QDir(QCoreApplication::applicationDirPath()).filePath(
            QStringLiteral("Assets/gamefinishvideo")),
        QDir(QCoreApplication::applicationDirPath()).filePath(
            QStringLiteral("../Assets/gamefinishvideo")),
        QDir::cleanPath(QDir::current().filePath(QStringLiteral("Assets/gamefinishvideo"))),
        QDir(QFileInfo(QStringLiteral(__FILE__)).absolutePath()).filePath(
            QStringLiteral("Assets/gamefinishvideo"))
    };

    for (const QString &candidate : candidates) {
        if (QFileInfo::exists(QDir(candidate).filePath(QStringLiteral("draw_rgb.mp4"))))
            return QDir(candidate).absolutePath();
    }
    return QString();
}

QString BroadcastWindow::settlementAssetPath(const QString &type, const QString &suffix) const
{
    static const QStringList validTypes = {
        QStringLiteral("redwin"), QStringLiteral("bluewin"),
        QStringLiteral("defeated"), QStringLiteral("draw"),
        QStringLiteral("termination")
    };
    if (!validTypes.contains(type) || (suffix != QStringLiteral("rgb")
                                       && suffix != QStringLiteral("alpha")))
        return QString();

    const QString directory = settlementAssetDirectory();
    if (directory.isEmpty())
        return QString();
    const QString path = QDir(directory).filePath(
        QStringLiteral("%1_%2.mp4").arg(type, suffix));
    return QFileInfo::exists(path) ? path : QString();
}

QString BroadcastWindow::ffmpegExecutable() const
{
    const QDir applicationDir(QCoreApplication::applicationDirPath());
    const QStringList candidates = {
        applicationDir.filePath(QStringLiteral("ffmpeg.exe")),
        applicationDir.filePath(QStringLiteral("tools/ffmpeg/ffmpeg.exe")),
        applicationDir.filePath(QStringLiteral("tools/ffmpeg.exe")),
        QDir::cleanPath(applicationDir.filePath(QStringLiteral("../tools/ffmpeg/ffmpeg.exe")))
    };

    for (const QString &candidate : candidates) {
        if (QFileInfo::exists(candidate))
            return QFileInfo(candidate).absoluteFilePath();
    }
    return QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
}

void BroadcastWindow::playSettlement(const QString &type)
{
    const QString normalizedType = type.trimmed().toLower();
    if (normalizedType.isEmpty())
        return;
    if (m_settlementProcess && m_settlementType == normalizedType)
        return;

    stopSettlement();
    m_roundEnded = true;
    m_settlementType = normalizedType;
    const QString rgbPath = settlementAssetPath(normalizedType, QStringLiteral("rgb"));
    const QString alphaPath = settlementAssetPath(normalizedType, QStringLiteral("alpha"));
    const QString executable = ffmpegExecutable();
    if (rgbPath.isEmpty() || alphaPath.isEmpty() || executable.isEmpty()) {
        // A missing optional clip must not obscure the formal reason/reticle;
        // the verdict card is shown directly instead.
        if (m_competitionMode) {
            m_settlementView->hide();
            if (m_competitionOverlay)
                m_competitionOverlay->setSettlementBlocking(false);
            return;
        }
        if (m_settlementView) {
            m_settlementView->setPixmap(QPixmap());
            m_settlementView->setText(tr("结算动画资源或 FFmpeg 不可用"));
            m_settlementView->show();
            m_settlementView->raise();
        }
        return;
    }

    m_settlementBuffer.clear();
    m_settlementFrames.clear();
    m_lastSettlementFrame = QImage();
    m_settlementFinished = false;
    if (m_settlementView) {
        m_settlementView->setPixmap(QPixmap());
        m_settlementView->setText(QString());
        m_settlementView->show();
        m_settlementView->raise();
    }
    // The clip really plays now: hide the verdict card until it ends.
    if (m_competitionOverlay)
        m_competitionOverlay->setSettlementBlocking(true);
    m_settlementFrameTimer->start();

    m_settlementProcess = new QProcess(this);
    QProcess *process = m_settlementProcess;
    process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(process, &QProcess::readyReadStandardOutput,
            this, &BroadcastWindow::consumeSettlementOutput);
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process](int, QProcess::ExitStatus) {
                if (m_settlementProcess != process)
                    return;
                consumeSettlementOutput();
                m_settlementFinished = true;
            });
    connect(process, &QProcess::errorOccurred, this,
            [this, process](QProcess::ProcessError) {
                if (m_settlementProcess != process)
                    return;
                consumeSettlementOutput();
                m_settlementFinished = true;
                if (m_lastSettlementFrame.isNull() && m_settlementFrames.isEmpty()
                    && m_settlementView) {
                    if (m_competitionMode) {
                        // FFmpeg failed before any frame: reveal the card now.
                        m_settlementView->hide();
                        QProcess *failed = m_settlementProcess;
                        m_settlementProcess = nullptr;
                        if (m_settlementFrameTimer)
                            m_settlementFrameTimer->stop();
                        if (failed)
                            failed->deleteLater();
                        if (m_competitionOverlay)
                            m_competitionOverlay->setSettlementBlocking(false);
                        return;
                    }
                    m_settlementView->setText(tr("结算动画播放失败"));
                    m_settlementView->show();
                    m_settlementView->raise();
                }
            });

    process->start(executable, {
        QStringLiteral("-hide_banner"),
        QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-re"),
        QStringLiteral("-i"), rgbPath,
        QStringLiteral("-re"),
        QStringLiteral("-i"), alphaPath,
        QStringLiteral("-filter_complex"),
        QStringLiteral("[1:v]format=gray[mask];[0:v][mask]alphamerge,"
                       "scale=960:540:flags=lanczos,format=bgra[v]"),
        QStringLiteral("-map"), QStringLiteral("[v]"),
        QStringLiteral("-an"),
        QStringLiteral("-f"), QStringLiteral("rawvideo"),
        QStringLiteral("-pix_fmt"), QStringLiteral("bgra"),
        QStringLiteral("pipe:1")
    });
}

void BroadcastWindow::stopSettlement()
{
    QProcess *process = m_settlementProcess;
    m_settlementProcess = nullptr;
    m_settlementBuffer.clear();
    m_settlementFrames.clear();
    m_lastSettlementFrame = QImage();
    m_settlementFinished = false;
    if (m_settlementFrameTimer)
        m_settlementFrameTimer->stop();
    if (m_settlementView)
        m_settlementView->hide();
    if (m_competitionOverlay)
        m_competitionOverlay->setSettlementBlocking(false);
    if (!process)
        return;

    process->disconnect(this);
    if (process->state() == QProcess::NotRunning) { delete process; return; }
    // Do not block the GUI thread waiting for FFmpeg to exit; delete it from
    // its own finished signal. Reparent so the window dtor does not wait on it.
    process->setParent(nullptr);
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), process, &QObject::deleteLater);
    process->kill();
}

void BroadcastWindow::playSettlementPreview(const QString &type)
{
    const bool previousRoundEnded = m_roundEnded;
    const QString previousSettlementType = m_settlementType;
    playSettlement(type);

    // A preview must not finish or rename the active match. The animation
    // itself stays owned by the normal settlement pipeline and can be stopped
    // by starting/resetting the match as usual.
    m_roundEnded = previousRoundEnded;
    m_settlementType = previousSettlementType;
}

void BroadcastWindow::clearSettlement()
{
    stopSettlement();
    m_roundEnded = false;
    m_settlementType.clear();
    clearMatchSummary();
}

void BroadcastWindow::setMatchSummary(const QJsonObject &summary)
{
    if (!summary.value(QStringLiteral("valid")).toBool()) {
        clearMatchSummary();
        return;
    }
    const bool same = summary == m_matchSummary;
    m_matchSummary = summary;
    if (m_summaryPanel)
        m_summaryPanel->setSummary(summary, redTeamName(), blueTeamName());
    updateWinnerHighlight();
    updateMatchState();
    if (same && summaryVisible())
        return;
    // Wait for the settlement clip; show immediately when none is playing
    // (for example when the clip assets or FFmpeg are unavailable).
    if (!settlementPlaying())
        m_summaryRevealTimer->start();
}

void BroadcastWindow::clearMatchSummary()
{
    m_matchSummary = QJsonObject();
    if (m_summaryRevealTimer)
        m_summaryRevealTimer->stop();
    if (m_summaryPanel) {
        m_summaryPanel->setExpanded(false, false);
        m_summaryPanel->hide();
    }
    updateWinnerHighlight();
}

bool BroadcastWindow::summaryVisible() const
{
    return m_summaryPanel && m_summaryPanel->isVisible();
}

bool BroadcastWindow::summaryExpanded() const
{
    return m_summaryPanel && m_summaryPanel->expanded();
}

void BroadcastWindow::setSummaryExpanded(bool expanded, bool animated)
{
    if (!m_summaryPanel || m_summaryPanel->expanded() == expanded)
        return;
    m_summaryPanel->setExpanded(expanded, animated);
}

void BroadcastWindow::revealSummaryIfReady()
{
    if (m_competitionMode || !m_summaryPanel || !m_roundEnded || settlementPlaying()
        || !m_matchSummary.value(QStringLiteral("valid")).toBool()) {
        return;
    }
    layoutSummaryPanel();
    if (m_summaryPanel->isVisible()) {
        m_summaryPanel->raise();
        return;
    }
    m_summaryPanel->reveal(isVisible());
}

void BroadcastWindow::layoutSummaryPanel()
{
    if (!m_summaryPanel || !centralWidget())
        return;
    const qreal scale = m_uiScale;
    const auto px = [scale](int value) { return qMax(1, qRound(value * scale)); };
    const QRect viewport = scaledHudViewport(this, scale);

    // Collapsed bar sits just above the bottom status row (and the ticker,
    // which is below that row), so it never covers the live HUD.
    int bottom = viewport.bottom() - px(170);
    if (m_matchState && m_matchState->isVisible()) {
        const int stateTop = m_matchState->mapTo(centralWidget(), QPoint(0, 0)).y();
        if (stateTop > viewport.y() + px(300))
            bottom = stateTop - px(14);
    }
    const int collapsedWidth = qMin(px(900), viewport.width() - px(80));
    const QRect collapsed(viewport.x() + (viewport.width() - collapsedWidth) / 2,
                          bottom - px(96), collapsedWidth, px(96));

    const int expandedWidth = qMin(px(1320), viewport.width() - px(80));
    // Below the centre HUD artwork (its canvas ends at ~255 reference px).
    const int expandedTop = viewport.y() + px(262);
    const int expandedHeight = qMax(px(96), qMin(px(580), collapsed.bottom() - expandedTop));
    const QRect expanded(viewport.x() + (viewport.width() - expandedWidth) / 2,
                         expandedTop, expandedWidth, expandedHeight);
    m_summaryPanel->setFrames(collapsed, expanded, scale);
}

void BroadcastWindow::updateWinnerHighlight()
{
    const bool valid = m_roundEnded && !m_matchSummary.isEmpty()
        && m_matchSummary.value(QStringLiteral("valid")).toBool(m_competitionMode);
    const int winner = valid ? m_matchSummary.value(QStringLiteral("winner")).toInt() : 0;
    for (auto it = m_overlays.begin(); it != m_overlays.end(); ++it) {
        if (auto *layer = static_cast<HudStateLayer *>(it.value().stateLayer))
            layer->setWinner(winner != 0 && it.value().teamId == winner);
    }
}

void BroadcastWindow::flashScore(QLabel *label)
{
    if (!label)
        return;
    QVariantAnimation *animation = m_scoreFlashes.value(label);
    if (!animation) {
        animation = new QVariantAnimation(this);
        animation->setDuration(900);
        animation->setStartValue(0.0);
        animation->setEndValue(1.0);
        animation->setEasingCurve(QEasingCurve::OutCubic);
        connect(animation, &QVariantAnimation::valueChanged, label, [label](const QVariant &v) {
            // Gold on change, easing back to the atlas' icy white.
            const qreal t = v.toReal();
            const QColor from(0xff, 0xd2, 0x5c);
            const QColor to(0xef, 0xfc, 0xff);
            const QColor c(qRound(from.red() + (to.red() - from.red()) * t),
                           qRound(from.green() + (to.green() - from.green()) * t),
                           qRound(from.blue() + (to.blue() - from.blue()) * t));
            label->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(c.name()));
        });
        m_scoreFlashes.insert(label, animation);
    }
    animation->stop();
    animation->start();
}

void BroadcastWindow::consumeSettlementOutput()
{
    if (!m_settlementProcess)
        return;

    const int freeFrames = kSettlementFrameQueueLimit - m_settlementFrames.size();
    if (freeFrames <= 0)
        return;

    const qint64 bytesNeeded = qint64(freeFrames) * kSettlementFrameBytes
                               - m_settlementBuffer.size();
    if (bytesNeeded > 0)
        m_settlementBuffer.append(m_settlementProcess->read(bytesNeeded));

    while (m_settlementFrames.size() < kSettlementFrameQueueLimit
           && m_settlementBuffer.size() >= kSettlementFrameBytes) {
        const QByteArray frameBytes = m_settlementBuffer.left(kSettlementFrameBytes);
        m_settlementBuffer.remove(0, kSettlementFrameBytes);
        const QImage frame(reinterpret_cast<const uchar *>(frameBytes.constData()),
                           kSettlementFrameWidth,
                           kSettlementFrameHeight,
                           kSettlementFrameWidth * 4,
                           QImage::Format_ARGB32);
        m_settlementFrames.enqueue(frame.copy());
    }
}

void BroadcastWindow::renderSettlementFrame(const QImage &frame)
{
    if (!m_settlementView || frame.isNull())
        return;

    m_lastSettlementFrame = frame;
    const QSize targetSize = m_settlementView->size();
    if (targetSize.isEmpty())
        return;
    const QPixmap pixmap = QPixmap::fromImage(frame).scaled(
        targetSize, Qt::KeepAspectRatioByExpanding, Qt::FastTransformation);
    m_settlementView->setText(QString());
    m_settlementView->setPixmap(pixmap);
}

void BroadcastWindow::setProgramFrame(const QImage &frame)
{
    setSourceFrame(QStringLiteral("field"), frame);
}

void BroadcastWindow::setSourceFrame(const QString &sourceId, const QImage &frame)
{
    const QString id = sourceId.isEmpty() ? QStringLiteral("field") : sourceId;
    const bool firstFrame = !m_sourceFrames.contains(id);
    if (frame.isNull())
        m_sourceFrames.remove(id);
    else
        m_sourceFrames.insert(id, frame);
    // Non-program sources remain cached for an instant switch, but must not
    // trigger a full-window scale and repaint on every incoming frame.
    if (id == m_activeSource)
        scheduleProgramRender();

    // Throttled notification for the control-panel preview strip. Emitting on
    // every frame would make the preview QLabel pay a scale + pixmap upload
    // at the incoming frame rate, which is wasteful when the strip is only a
    // few hundred pixels wide.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 last = m_sourceFrameSignalTimes.value(id, 0);
    if (firstFrame || frame.isNull() || now - last >= 100) {
        m_sourceFrameSignalTimes.insert(id, now);
        emit sourceFrameUpdated(id);
    }
}

QImage BroadcastWindow::sourceFrame(const QString &sourceId) const
{
    const QString id = sourceId.isEmpty() ? QStringLiteral("field") : sourceId;
    return m_sourceFrames.value(id);
}

QStringList BroadcastWindow::knownSourceIds() const
{
    return m_sourceFrames.keys();
}

void BroadcastWindow::setActiveSource(const QString &sourceId, const QString &sourceTitle)
{
    m_activeSource = sourceId.isEmpty() ? QStringLiteral("field") : sourceId;
    m_activeSourceTitle = sourceTitle.isEmpty()
                              ? (m_activeSource == QStringLiteral("field") ? tr("全场视角")
                                                                             : m_activeSource)
                              : sourceTitle;
    if (m_sourceLabel)
        m_sourceLabel->setText(tr("LIVE · %1").arg(m_activeSourceTitle));
    scheduleProgramRender();
}

void BroadcastWindow::setTeamNames(const QString &redName, const QString &blueName)
{
    if (!redName.trimmed().isEmpty())
        m_teamNames.insert(kRedTeam, redName.trimmed());
    if (!blueName.trimmed().isEmpty())
        m_teamNames.insert(kBlueTeam, blueName.trimmed());

    for (auto it = m_overlays.begin(); it != m_overlays.end(); ++it)
        it.value().team->setText(m_teamNames.value(static_cast<quint8>(it.key())));
    if (m_summaryPanel && !m_matchSummary.isEmpty())
        m_summaryPanel->setSummary(m_matchSummary, redTeamName(), blueTeamName());
    if (m_competitionOverlay)
        m_competitionOverlay->setTeamNameFallback(redTeamName(), blueTeamName());

    emit presentationStateChanged();
}

void BroadcastWindow::setScores(int redScore, int blueScore)
{
    const int previousRed = m_redScore;
    const int previousBlue = m_blueScore;
    m_redScore = qMax(0, redScore);
    m_blueScore = qMax(0, blueScore);
    if (m_redScore > previousRed)
        flashScore(m_redRoundScoreLabel);
    if (m_blueScore > previousBlue)
        flashScore(m_blueRoundScoreLabel);
    if (m_redRoundScoreLabel)
        m_redRoundScoreLabel->setText(QString::number(m_redScore));
    if (m_blueRoundScoreLabel)
        m_blueRoundScoreLabel->setText(QString::number(m_blueScore));
    if (m_scoreLabel)
        m_scoreLabel->setText(tr("击杀积分 %1  :  %2").arg(m_redScore).arg(m_blueScore));

    emit presentationStateChanged();
}

void BroadcastWindow::setTickerText(const QString &text)
{
    const QString normalized = text.trimmed();
    if (normalized.isEmpty())
        return;

    m_tickerText = normalized;
    if (m_tickerTextView)
        m_tickerTextView->setText(m_tickerText);
    emit presentationStateChanged();
}

void BroadcastWindow::setTickerVisible(bool visible, bool animated)
{
    m_tickerVisible = visible;
    if (!m_tickerBar || !m_tickerHeightAnimation || !m_tickerOpacityAnimation)
        return;

    if (!animated) {
        m_tickerHeightAnimation->stop();
        m_tickerOpacityAnimation->stop();
        const int targetHeight = visible ? tickerBarHeight() : 0;
        m_tickerBar->setMinimumHeight(targetHeight);
        m_tickerBar->setMaximumHeight(targetHeight);
        m_tickerOpacity->setOpacity(visible ? 1.0 : 0.0);
        m_tickerBar->setVisible(visible);
        return;
    }

    animateTicker();
    emit presentationStateChanged();
}

void BroadcastWindow::animateTicker()
{
    if (!m_tickerBar || !m_tickerHeightAnimation || !m_tickerOpacityAnimation
        || !m_tickerOpacity) {
        return;
    }

    const int targetHeight = m_tickerVisible ? tickerBarHeight() : 0;
    if (m_tickerVisible && !m_tickerBar->isVisible()) {
        m_tickerBar->setMinimumHeight(0);
        m_tickerBar->setMaximumHeight(0);
        m_tickerOpacity->setOpacity(0.0);
        m_tickerBar->show();
    }

    m_tickerHeightAnimation->stop();
    m_tickerOpacityAnimation->stop();
    const int currentHeight = qMax(0, m_tickerBar->maximumHeight());
    const qreal currentOpacity = m_tickerOpacity->opacity();
    m_tickerHeightAnimation->setStartValue(currentHeight);
    m_tickerHeightAnimation->setEndValue(targetHeight);
    m_tickerOpacityAnimation->setStartValue(currentOpacity);
    m_tickerOpacityAnimation->setEndValue(m_tickerVisible ? 1.0 : 0.0);
    m_tickerHeightAnimation->start();
    m_tickerOpacityAnimation->start();
}

QStringList BroadcastWindow::layoutElementIds() const
{
    return {
        QStringLiteral("red_team_panel"),
        QStringLiteral("center_hud"),
        QStringLiteral("blue_team_panel"),
        QStringLiteral("match_title"),
        QStringLiteral("match_timer"),
        QStringLiteral("red_round_score"),
        QStringLiteral("blue_round_score"),
        QStringLiteral("match_state"),
        QStringLiteral("source_label")
    };
}

QPoint BroadcastWindow::layoutElementPosition(const QString &elementId) const
{
    return m_layoutOffsets.value(elementId, QPoint());
}

QSize BroadcastWindow::layoutElementSize(const QString &elementId) const
{
    if (m_layoutSizes.contains(elementId))
        return m_layoutSizes.value(elementId);

    QWidget *widget = m_layoutElements.value(elementId);
    if (!widget)
        return QSize();

    const QSize currentSize = widget->size();
    return currentSize.isValid() && currentSize.width() > 0 && currentSize.height() > 0
               ? currentSize
               : widget->sizeHint();
}

void BroadcastWindow::setLayoutElementPosition(const QString &elementId,
                                                const QPoint &position)
{
    if (!m_layoutElements.contains(elementId))
        return;

    m_layoutOffsets.insert(elementId, position);
    if (isVisible())
        applyLayoutPositions();
}

void BroadcastWindow::setLayoutElementSize(const QString &elementId, const QSize &size)
{
    if (!m_layoutElements.contains(elementId) || !size.isValid()
        || size.width() <= 0 || size.height() <= 0) {
        return;
    }

    m_layoutSizes.insert(elementId, size);
    if (isVisible())
        applyLayoutPositions();
}

void BroadcastWindow::resetLayoutPositions()
{
    for (const QString &elementId : layoutElementIds()) {
        const QPoint defaultOffset = defaultLayoutOffset(elementId);
        m_layoutOffsets.insert(elementId, defaultOffset);
        m_layoutSizes.remove(elementId);

        QWidget *widget = m_layoutElements.value(elementId);
        if (!widget)
            continue;
        if (m_layoutMinimumSizes.contains(elementId))
            widget->setMinimumSize(m_layoutMinimumSizes.value(elementId));
        if (m_layoutMaximumSizes.contains(elementId))
            widget->setMaximumSize(m_layoutMaximumSizes.value(elementId));
        widget->updateGeometry();
    }
    if (isVisible())
        applyLayoutPositions();
}

bool BroadcastWindow::saveLayout(const QString &filePath) const
{
    QSettings settings(filePath, QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("broadcast_layout"));
    settings.setValue(QStringLiteral("version"), 4);
    settings.remove(QStringLiteral("left_score_panel"));
    settings.remove(QStringLiteral("right_score_panel"));
    for (const QString &elementId : layoutElementIds()) {
        const QPoint position = m_layoutOffsets.value(elementId, QPoint());
        settings.setValue(elementId + QStringLiteral("/x"), position.x());
        settings.setValue(elementId + QStringLiteral("/y"), position.y());
        const QString widthKey = elementId + QStringLiteral("/width");
        const QString heightKey = elementId + QStringLiteral("/height");
        if (m_layoutSizes.contains(elementId)) {
            const QSize size = m_layoutSizes.value(elementId);
            settings.setValue(widthKey, size.width());
            settings.setValue(heightKey, size.height());
        } else {
            settings.remove(widthKey);
            settings.remove(heightKey);
        }
    }
    settings.endGroup();
    settings.sync();
    return settings.status() == QSettings::NoError;
}

bool BroadcastWindow::loadLayout(const QString &filePath)
{
    QSettings settings(filePath, QSettings::IniFormat);
    if (!settings.contains(QStringLiteral("broadcast_layout/center_hud/x")))
        return false;

    const int layoutVersion = settings.value(QStringLiteral("broadcast_layout/version"), 1).toInt();
    resetLayoutPositions();
    settings.beginGroup(QStringLiteral("broadcast_layout"));
    for (const QString &elementId : layoutElementIds()) {
        // V1/V2 centered text and separately sized plates belonged to the old
        // three-part canvas. Keep unrelated custom positions, but migrate the
        // central artwork/text and automatic side widths to the new defaults.
        if (layoutVersion < 3
            && (elementId == QStringLiteral("center_hud")
                || elementId == QStringLiteral("match_title")
                || elementId == QStringLiteral("match_timer")
                || elementId == QStringLiteral("red_round_score")
                || elementId == QStringLiteral("blue_round_score")))
            continue;
        if (layoutVersion < 4
            && (elementId == QStringLiteral("red_team_panel")
                || elementId == QStringLiteral("blue_team_panel")))
            continue; // Old tall telemetry panels must not return via saved sizes.
        const QString xKey = elementId + QStringLiteral("/x");
        const QString yKey = elementId + QStringLiteral("/y");
        if (settings.contains(xKey) && settings.contains(yKey)) {
            QPoint position(settings.value(xKey).toInt(), settings.value(yKey).toInt());
            if (layoutVersion < 2)
                position -= legacyLayoutOffset(elementId);
            m_layoutOffsets.insert(elementId, position);
        }

        const QString widthKey = elementId + QStringLiteral("/width");
        const QString heightKey = elementId + QStringLiteral("/height");
        if (settings.contains(widthKey) && settings.contains(heightKey)
            && !(layoutVersion < 3
                 && (elementId == QStringLiteral("red_team_panel")
                     || elementId == QStringLiteral("blue_team_panel")))) {
            const QSize size(settings.value(widthKey).toInt(),
                             settings.value(heightKey).toInt());
            if (size.isValid() && size.width() > 0 && size.height() > 0)
                m_layoutSizes.insert(elementId, size);
        }
    }
    settings.endGroup();
    if (isVisible())
        applyLayoutPositions();
    return settings.status() == QSettings::NoError;
}

void BroadcastWindow::awardCard(quint8 team, bool redCard)
{
    if (team != kRedTeam && team != kBlueTeam)
        return;

    auto &cards = redCard ? m_redCards : m_yellowCards;
    cards[team] = qMin(9, cards.value(team, 0) + 1);
    for (auto it = m_overlays.begin(); it != m_overlays.end(); ++it) {
        if (static_cast<quint8>(it.key()) != team)
            continue;
        it.value().redCard->setText(QString::number(m_redCards.value(team, 0)));
        it.value().yellowCard->setText(QString::number(m_yellowCards.value(team, 0)));
        it.value().redCard->parentWidget()->setVisible(m_redCards.value(team, 0) > 0);
        it.value().yellowCard->parentWidget()->setVisible(m_yellowCards.value(team, 0) > 0);
    }
}

void BroadcastWindow::clearCards()
{
    m_redCards.clear();
    m_yellowCards.clear();
    for (auto it = m_overlays.begin(); it != m_overlays.end(); ++it) {
        it.value().redCard->setText(QStringLiteral("0"));
        it.value().yellowCard->setText(QStringLiteral("0"));
        it.value().redCard->parentWidget()->setVisible(false);
        it.value().yellowCard->parentWidget()->setVisible(false);
    }
}

void BroadcastWindow::clearProgramFrame()
{
    m_sourceFrames.remove(QStringLiteral("field"));
    scheduleProgramRender();
}

void BroadcastWindow::scheduleProgramRender()
{
    if (!m_programRenderTimer)
        return;
    if (!isVisible())
        return;
    if (!m_programRenderTimer->isActive())
        m_programRenderTimer->start();
}

void BroadcastWindow::renderProgramFrame()
{
    if (!m_programView)
        return;

    const QImage frame = m_sourceFrames.value(m_activeSource);
    if (frame.isNull()) {
        if (m_videoCanvas) {
            m_videoCanvas->clearFrame();
            m_videoCanvas->hide();
        }
        m_programView->show();
        m_programView->setPixmap(QPixmap());
        m_programView->setText(tr("等待 %1\n视频流未接入").arg(m_activeSourceTitle));
        return;
    }

    const QSize targetSize = m_programView->size();
    if (targetSize.isEmpty())
        return;
    // Qt's offscreen/minimal platforms cannot create a QOpenGLWidget context.
    // Keep the exact same real widget HUD and use the raster background path
    // for QA (also useful when a venue machine explicitly disables GPU video).
    const QString platform = QGuiApplication::platformName();
    if (platform == QStringLiteral("offscreen") || platform == QStringLiteral("minimal")
        || qEnvironmentVariableIntValue("SCOMP_BROADCAST_SOFTWARE_VIDEO") != 0) {
        if (m_videoCanvas)
            m_videoCanvas->hide();
        m_programView->setText(QString());
        m_programView->setPixmap(QPixmap::fromImage(frame).scaled(
            targetSize, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
        m_programView->show();
        return;
    }
    // A status-only HUD does not need an OpenGL surface. Create it only
    // when video arrives, so software/offscreen HUD rendering also works.
    if (!m_videoCanvas) {
        m_videoCanvas = new VideoCanvas(centralWidget());
        static_cast<QGridLayout *>(centralWidget()->layout())->addWidget(m_videoCanvas, 0, 0);
        m_videoCanvas->stackUnder(m_hudOverlay);
    }
    m_programView->setText(QString());
    m_programView->setPixmap(QPixmap());
    m_programView->hide();
    m_videoCanvas->setFrame(frame);
    m_videoCanvas->show();
}

void BroadcastWindow::registerLayoutElement(const QString &elementId, QWidget *widget)
{
    if (elementId.isEmpty() || !widget)
        return;

    m_layoutElements.insert(elementId, widget);
    const QPoint defaultOffset = defaultLayoutOffset(elementId);
    m_layoutOffsets.insert(elementId, defaultOffset);
    m_layoutMinimumSizes.insert(elementId, widget->minimumSize());
    m_layoutMaximumSizes.insert(elementId, widget->maximumSize());
}

void BroadcastWindow::setBaseFont(QWidget *widget, double pointSize, bool bold)
{
    if (!widget)
        return;

    m_baseFontPoints.insert(widget, pointSize);
    QFont font = widget->font();
    font.setBold(bold);
    font.setPointSizeF(qMax(1.0, pointSize * m_uiScale));
    widget->setFont(font);
}

int BroadcastWindow::tickerBarHeight() const
{
    return qMax(1, qRound(kTickerBarNativeHeight * m_uiScale));
}

void BroadcastWindow::applyUiScale()
{
    const qreal scale = windowUiScale(this);
    if (m_uiScaleReady && qAbs(scale - m_uiScale) < 0.01)
        return;
    m_uiScale = scale;
    m_uiScaleReady = true;

    if (QWidget *central = centralWidget())
        central->setStyleSheet(broadcastStyleSheet(scale));

    for (auto it = m_baseFontPoints.constBegin(); it != m_baseFontPoints.constEnd(); ++it) {
        QWidget *widget = it.key();
        if (!widget)
            continue;
        QFont font = widget->font();
        font.setPointSizeF(qMax(1.0, it.value() * scale));
        widget->setFont(font);
    }

    const auto px = [scale](int value) { return qMax(1, qRound(value * scale)); };
    // Bright-venue readability: soft dark halo behind top HUD text.
    applyTextShadow(m_timerLabel, scale, 18, 3, 240);
    applyTextShadow(m_redRoundScoreLabel, scale, 14, 2, 230);
    applyTextShadow(m_blueRoundScoreLabel, scale, 14, 2, 230);
    applyTextShadow(m_matchTitle, scale, 10, 1.5, 230);
    for (auto it = m_overlays.begin(); it != m_overlays.end(); ++it)
        applyTextShadow(it->team, scale, 12, 2, 230);
    if (m_hudOverlay) m_hudOverlay->update();
    if (m_matchTitle && m_titleLayer) {
        QFont font = m_matchTitle->font();
        font.setPixelSize(px(20));
        m_matchTitle->setFont(font);
        m_titleLayer->setMinimumSize(1, 1);
        m_titleLayer->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    }
    if (m_timerLabel && m_timerLayer) {
        QFont font = m_timerLabel->font();
        font.setPixelSize(px(54));
        m_timerLabel->setFont(font);
        // Authored logical heights avoid stale font/style size hints on the
        // first resize of a hidden client or an offscreen render.
        m_timerLabel->setMinimumSize(1, 1);
        m_timerLabel->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        m_timerLayer->setMinimumSize(1, 1);
        m_timerLayer->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    }
    if (m_tickerTagLabel)
        m_tickerTagLabel->setMinimumWidth(px(68));
    if (m_overlayLayout) {
        // Keep the complete 1920x1080 composition centered inside a
        // differently shaped output.  All authored coordinates are scaled by
        // the same factor; any excess aspect-ratio space becomes a symmetric
        // letterbox instead of stretching the HUD vertically or horizontally.
        const QRect viewport = scaledHudViewport(this, scale);
        if (m_hudOverlay)
            m_hudOverlay->setProperty("hudViewport", viewport);
        const int left = viewport.x() + px(42);
        const int top = viewport.y() + px(34);
        const int right = qMax(0, width() - viewport.right() - 1) + px(42);
        const int bottom = qMax(0, height() - viewport.bottom() - 1) + px(34);
        m_overlayLayout->setContentsMargins(left, top, right, bottom);
    }
    if (m_tickerBar) {
        if (QLayout *tickerLayout = m_tickerBar->layout()) {
            tickerLayout->setContentsMargins(px(18), px(3), px(18), px(3));
            tickerLayout->setSpacing(px(14));
        }
        if (m_tickerVisible
            && (!m_tickerHeightAnimation
                || m_tickerHeightAnimation->state() != QAbstractAnimation::Running)) {
            m_tickerBar->setMinimumHeight(tickerBarHeight());
            m_tickerBar->setMaximumHeight(tickerBarHeight());
        }
    }

    for (auto it = m_overlays.begin(); it != m_overlays.end(); ++it) {
        RobotOverlay &overlay = it.value();
        if (overlay.connection)
            overlay.connection->setMinimumWidth(px(68));
        if (overlay.healthBar) {
            overlay.healthBar->setMinimumSize(1, 1);
            overlay.healthBar->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        }
        if (overlay.heatBar)
            overlay.heatBar->setFixedHeight(px(20));
        if (overlay.damageNotice)
            overlay.damageNotice->setFixedSize(px(160), px(36));
        for (QLabel *icon : {overlay.redCardIcon, overlay.yellowCardIcon}) {
            if (!icon)
                continue;
            icon->setFixedSize(px(24), px(30));
            const QRect sourceRect = icon->property("atlasRect").toRect();
            const QPixmap pixmap = assetCrop(
                QStringLiteral(":/broadcast/statusbar_atlas.png"), sourceRect);
            if (!pixmap.isNull())
                icon->setPixmap(pixmap.scaled(icon->size(), Qt::KeepAspectRatio,
                                              Qt::SmoothTransformation));
        }
    }

    applyLayoutPositions();
}

void BroadcastWindow::applyLayoutPositions()
{
    // The main HUD entries are owned by custom layouts.  Feed their saved
    // values into those layouts instead of moving layout-managed children.
    // This keeps a text change from changing the coordinate system underneath
    // a manually edited element.
    auto *centerHud = dynamic_cast<CenterHudCanvas *>(
        m_layoutElements.value(QStringLiteral("center_hud")));
    const auto isCenterElement = [](const QString &elementId) {
        return elementId == QStringLiteral("red_round_score")
               || elementId == QStringLiteral("blue_round_score");
    };
    if (centerHud) {
        for (const QString &elementId : {QStringLiteral("red_round_score"),
                                          QStringLiteral("blue_round_score")}) {
            centerHud->setElementOffset(elementId,
                                         m_layoutOffsets.value(elementId, QPoint()));
            centerHud->setElementSize(elementId,
                                       m_layoutSizes.value(elementId, QSize()));
        }
    }

    for (const QString &elementId : layoutElementIds()) {
        QWidget *widget = m_layoutElements.value(elementId);
        if (!widget)
            continue;

        if (centerHud && isCenterElement(elementId)) {
            if (m_layoutSizes.contains(elementId)) {
                // Saved sizes are reference-resolution pixels; scale them to
                // the current output before constraining the widget.
                const QSize requestedSize = scaledValue(m_layoutSizes.value(elementId),
                                                        m_uiScale);
                if (widget->minimumSize() != requestedSize)
                    widget->setMinimumSize(requestedSize);
                if (widget->maximumSize() != requestedSize)
                    widget->setMaximumSize(requestedSize);
            } else {
                if (m_layoutMinimumSizes.contains(elementId))
                    widget->setMinimumSize(m_layoutMinimumSizes.value(elementId));
                if (m_layoutMaximumSizes.contains(elementId))
                    widget->setMaximumSize(m_layoutMaximumSizes.value(elementId));
            }
            continue;
        }

        if (QLayout *manager = m_layoutManagers.value(elementId)) {
            auto *hudLayout = static_cast<HudElementLayout *>(manager);
            hudLayout->setElementOffset(elementId,
                                        m_layoutOffsets.value(elementId, QPoint()));
            hudLayout->setElementSize(elementId,
                                       m_layoutSizes.value(elementId, QSize()));

            if (m_layoutSizes.contains(elementId)) {
                const QSize requestedSize = scaledValue(m_layoutSizes.value(elementId),
                                                        m_uiScale);
                if (widget->minimumSize() != requestedSize)
                    widget->setMinimumSize(requestedSize);
                if (widget->maximumSize() != requestedSize)
                    widget->setMaximumSize(requestedSize);
            } else {
                if (m_layoutMinimumSizes.contains(elementId))
                    widget->setMinimumSize(m_layoutMinimumSizes.value(elementId));
                if (m_layoutMaximumSizes.contains(elementId))
                    widget->setMaximumSize(m_layoutMaximumSizes.value(elementId));
            }
            continue;
        }

        // Every registered element is assigned to either a stable layout or
        // CenterHudCanvas above. Keeping this branch explicit prevents a new
        // QWidget::move() fallback from reintroducing the original bug.
    }

    QHash<QLayout *, bool> activatedLayouts;
    for (QLayout *manager : m_layoutManagers) {
        if (manager && !activatedLayouts.contains(manager)) {
            manager->activate();
            activatedLayouts.insert(manager, true);

            // A configured height changes the size hint of the nested HUD
            // layout. Re-activate its owning layout so the surrounding
            // overlay also receives the new row height immediately.
            if (QWidget *parent = manager->parentWidget()) {
                if (QLayout *parentLayout = parent->layout(); parentLayout != manager)
                    parentLayout->activate();
            }
        }
    }
}

void BroadcastWindow::showOnScreen(QScreen *screen)
{
    QScreen *target = screen ? screen : QGuiApplication::primaryScreen();
    if (!target)
        return;

    // Test hook: SCOMP_BROADCAST_SIZE=1280x720 shows the output as a regular
    // window at the requested size so layout scaling can be verified on any
    // display without changing monitor settings.
    const QByteArray forcedSize = qgetenv("SCOMP_BROADCAST_SIZE");
    const int separator = forcedSize.indexOf('x');
    if (separator > 0) {
        const int width = forcedSize.left(separator).toInt();
        const int height = forcedSize.mid(separator + 1).toInt();
        if (width >= 320 && height >= 240) {
            if (isVisible())
                hide();
            setWindowState(windowState() & ~Qt::WindowFullScreen);
            if (windowHandle())
                windowHandle()->setScreen(target);
            const QRect available = target->availableGeometry();
            const int x = available.x() + qMax(0, (available.width() - width) / 2);
            const int y = available.y() + qMax(0, (available.height() - height) / 2);
            setGeometry(x, y, width, height);
            setAttribute(Qt::WA_ShowWithoutActivating);
            show();
            raise();
            return;
        }
    }

    // Reuse the same native window when toggling or moving the output. Clearing
    // the fullscreen state while hidden avoids stale Windows fullscreen state.
    if (isVisible())
        hide();
    setWindowState(windowState() & ~Qt::WindowFullScreen);
    if (windowHandle())
        windowHandle()->setScreen(target);
    setGeometry(target->geometry());
    showFullScreen();
#ifdef Q_OS_WIN
    coverTaskbarOnScreen(this, target->geometry());
#endif
    raise();
    activateWindow();
}

BroadcastWindow::~BroadcastWindow()
{
    // A settlement FFmpeg child may still be running. ~QObject would destroy it
    // after this class's members are gone, and its finished/errorOccurred
    // lambdas would then run on a half-destroyed window. Detach and stop it now.
    if (m_settlementFrameTimer)
        m_settlementFrameTimer->stop();
    if (QProcess *process = m_settlementProcess) {
        m_settlementProcess = nullptr;
        process->disconnect(this);
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(1000);
        }
        delete process;
    }
}

void BroadcastWindow::closeEvent(QCloseEvent *event)
{
    event->ignore();
    hide();
}

void BroadcastWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    if (m_settlementView && centralWidget())
        m_settlementView->setGeometry(centralWidget()->rect());
    applyUiScale();
    applyLayoutPositions();
    layoutSummaryPanel();
    layoutCompetitionPresentation();
    scheduleProgramRender();
    emit visibilityChanged(true);
}

void BroadcastWindow::hideEvent(QHideEvent *event)
{
#ifdef Q_OS_WIN
    releaseTaskbarCover(this);
#endif
    QMainWindow::hideEvent(event);
    emit visibilityChanged(false);
}

void BroadcastWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    if (m_settlementView && centralWidget())
        m_settlementView->setGeometry(centralWidget()->rect());
    applyUiScale();
    if (isVisible())
        applyLayoutPositions();
    layoutSummaryPanel();
    layoutCompetitionPresentation();
    scheduleProgramRender();
    if (!m_lastSettlementFrame.isNull())
        renderSettlementFrame(m_lastSettlementFrame);
}
