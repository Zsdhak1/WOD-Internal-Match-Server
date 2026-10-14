#include "competitionoverlay.h"

#include <QFont>
#include <QFontMetricsF>
#include <QJsonValue>
#include <QLinearGradient>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QStringList>
#include <QTimer>
#include <QtGlobal>
#include <QtMath>

#include <cmath>

// CompetitionOverlay only. ReviveIndicator lives in reviveindicator.cpp.

namespace {
constexpr qreal kRefW = 1920.0;
constexpr qreal kRefH = 1080.0;
constexpr qint64 kFadeMs = 320;        // cross-fade of the previous mode
constexpr qint64 kDigitMs = 420;       // countdown digit entrance
constexpr qint64 kGoMs = 860;          // launch flash total
constexpr qint64 kHoldMs = 7000;       // result card before the intermission bar
constexpr qint64 kFlipMs = 340;        // intermission clock flip
constexpr qint64 kResultAnimMs = 1500; // HP fill + score flip
constexpr qint64 kFinishAnimMs = 1800; // final sweep
constexpr qint64 kFar = -1000000;

const QColor kRed(0xe8, 0x41, 0x4f);
const QColor kBlue(0x3a, 0x8d, 0xff);
const QColor kAmber(0xf0, 0xb4, 0x29);
const QColor kInk(0x0b, 0x0f, 0x16);
const QColor kText(0xf4, 0xf6, 0xfa);
const QColor kMuted(0xa9, 0xb3, 0xc2);

qreal clamp01(qreal v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
qreal outCubic(qreal t) { t = clamp01(t); const qreal f = 1 - t; return 1 - f * f * f; }
qreal outBack(qreal t)
{
    t = clamp01(t);
    const qreal c1 = 1.70158, c3 = c1 + 1;
    return 1 + c3 * std::pow(t - 1, 3) + c1 * std::pow(t - 1, 2);
}
qreal progress(qint64 now, qint64 start, qint64 duration)
{
    if (duration <= 0) return 1;
    return clamp01(qreal(now - start) / qreal(duration));
}

int jsonInt(const QJsonValue &v, int lo, int hi, int fallback)
{
    if (!v.isDouble()) {
        if (v.isString()) {
            bool ok = false;
            const double d = v.toString().toDouble(&ok);
            if (ok && std::isfinite(d)) return int(qBound<double>(lo, d, hi));
        }
        return fallback;
    }
    const double d = v.toDouble();
    if (!std::isfinite(d)) return fallback;
    return int(qBound<double>(lo, std::floor(d + 0.5), hi));
}

QFont uiFont(qreal px, QFont::Weight weight, bool digits = false)
{
    QFont f;
    const QStringList families = digits
        ? QStringList{QStringLiteral("Bahnschrift"), QStringLiteral("Microsoft YaHei UI"),
                      QStringLiteral("Microsoft YaHei"), QStringLiteral("Arial")}
        : QStringList{QStringLiteral("Microsoft YaHei UI"), QStringLiteral("Microsoft YaHei"),
                      QStringLiteral("Bahnschrift"), QStringLiteral("Arial")};
    f.setFamily(families.first());
#if QT_VERSION >= QT_VERSION_CHECK(5, 13, 0)
    f.setFamilies(families);
#endif
    f.setPixelSize(qMax(1, qRound(px)));
    f.setWeight(weight);
    f.setStyleStrategy(QFont::PreferAntialias);
    return f;
}

QString clockText(int seconds)
{
    seconds = qBound(0, seconds, 99 * 60 + 59);
    return QStringLiteral("%1:%2").arg(seconds / 60, 2, 10, QLatin1Char('0'))
                                  .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

QColor withAlpha(QColor c, qreal a) { c.setAlphaF(clamp01(a)); return c; }
QColor sideColor(int winner) { return winner == 1 ? kRed : winner == 2 ? kBlue : kAmber; }

QString methodText(const QString &m)
{
    if (m == QLatin1String("time_decision")) return QStringLiteral("时间到 · 按剩余资源判定");
    if (m == QLatin1String("revive_exhausted")) return QStringLiteral("复活次数耗尽");
    if (m == QLatin1String("violation")) return QStringLiteral("违规判定");
    if (m == QLatin1String("safety_stop")) return QStringLiteral("安全停止");
    if (m == QLatin1String("normal")) return QStringLiteral("裁判判定");
    return QString();
}

// Text drawn inside a rect, elided to fit (never wraps, never overflows).
void drawText(QPainter &p, const QRectF &r, const QString &text, const QFont &font,
              const QColor &color, Qt::Alignment align = Qt::AlignCenter)
{
    if (text.isEmpty() || r.width() <= 1 || r.height() <= 1) return;
    p.setFont(font);
    p.setPen(color);
    const QFontMetricsF fm(font);
    p.drawText(r, int(align) | Qt::TextSingleLine, fm.elidedText(text, Qt::ElideRight, r.width()));
}

void fillRound(QPainter &p, const QRectF &r, qreal radius, const QBrush &brush)
{
    p.setPen(Qt::NoPen);
    p.setBrush(brush);
    p.drawRoundedRect(r, radius, radius);
}

// Small hexagonal "mech core" (self drawn; no glyph dependencies).
void drawCore(QPainter &p, const QPointF &c, qreal r, const QColor &color, bool filled, bool unknown)
{
    QPainterPath path;
    for (int i = 0; i < 6; ++i) {
        const qreal a = M_PI / 6 + i * M_PI / 3;
        const QPointF q(c.x() + r * std::cos(a), c.y() + r * std::sin(a));
        if (i == 0) path.moveTo(q); else path.lineTo(q);
    }
    path.closeSubpath();
    p.setBrush(Qt::NoBrush);
    if (filled) {
        QRadialGradient g(c, r);
        g.setColorAt(0, color.lighter(150));
        g.setColorAt(1, color);
        p.setBrush(g);
        p.setPen(QPen(color.lighter(130), qMax<qreal>(1, r * 0.14)));
    } else {
        p.setPen(QPen(unknown ? withAlpha(kMuted, 0.55) : withAlpha(color, 0.45),
                      qMax<qreal>(1, r * 0.12), unknown ? Qt::DashLine : Qt::SolidLine));
        p.setBrush(withAlpha(kInk, 0.6));
    }
    p.drawPath(path);
    if (filled) {
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(Qt::white, 0.85));
        p.drawEllipse(c, r * 0.28, r * 0.28);
    }
}
} // namespace

CompetitionOverlay::CompetitionOverlay(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground);
    setAutoFillBackground(false);
    setFocusPolicy(Qt::NoFocus);
    m_clock.start();
    m_viewport = QRect(0, 0, int(kRefW), int(kRefH));

    m_frameTimer = new QTimer(this);
    m_frameTimer->setInterval(16);
    m_frameTimer->setTimerType(Qt::PreciseTimer);
    connect(m_frameTimer, &QTimer::timeout, this, [this] { tick(); });

    m_holdTimer = new QTimer(this);
    m_holdTimer->setSingleShot(true);
    connect(m_holdTimer, &QTimer::timeout, this, [this] { evaluateTimedMode(); });

    m_prevStart = kFar;
    m_goStart = kFar;
    m_clockFlipStart = kFar;
    hide();
}

qint64 CompetitionOverlay::now() const
{
    return m_manualClock ? m_manualNow : m_clock.elapsed();
}

QPointF CompetitionOverlay::pt(qreal x, qreal y) const
{
    return QPointF(m_viewport.left() + x * m_scale, m_viewport.top() + y * m_scale);
}

QRectF CompetitionOverlay::rr(qreal x, qreal y, qreal w, qreal h) const
{
    return QRectF(pt(x, y), QSizeF(qMax<qreal>(0, w) * m_scale, qMax<qreal>(0, h) * m_scale));
}

QRect CompetitionOverlay::toDevice(const QRectF &ref) const
{
    return rr(ref.x(), ref.y(), ref.width(), ref.height()).toAlignedRect();
}

void CompetitionOverlay::setManualClockForTest(bool manual)
{
    if (manual && !m_manualClock) m_manualNow = m_clock.elapsed();
    m_manualClock = manual;
    if (manual) { m_frameTimer->stop(); m_holdTimer->stop(); }
    else ensureTicking();
}

void CompetitionOverlay::advanceForTest(int milliseconds)
{
    if (!m_manualClock) return;
    const qint64 step = qBound(qint64(0), qint64(milliseconds), qint64(24) * 3600 * 1000);
    m_manualNow += step;
    tick();
}

// ---------------------------------------------------------------------------
// Snapshot intake
// ---------------------------------------------------------------------------

void CompetitionOverlay::setCompetitionState(const QJsonObject &state)
{
    const QString rule = state.value(QStringLiteral("scoringRule")).toString();
    if (!rule.isEmpty() && rule != QLatin1String("remaining_resources")) {
        clear();
        return;
    }
    const QString prevPhase = m_phase;
    m_phase = state.value(QStringLiteral("phase")).toString().trimmed().toLower();
    if (m_phase != prevPhase) m_previousPhase = prevPhase;
    m_remaining = jsonInt(state.value(QStringLiteral("remainingSeconds")), 0, 24 * 3600, 0);
    m_round = jsonInt(state.value(QStringLiteral("roundIndex")), 1, 99, 1);
    m_matchId = state.value(QStringLiteral("matchId")).toString();
    m_roundId = state.value(QStringLiteral("roundId")).toString();
    m_redWins = jsonInt(state.value(QStringLiteral("redScore")), 0, 9, 0);
    m_blueWins = jsonInt(state.value(QStringLiteral("blueScore")), 0, 9, 0);
    m_snapRedRevives = jsonInt(state.value(QStringLiteral("redRemainingRevives")), -1, 3, -1);
    m_snapBlueRevives = jsonInt(state.value(QStringLiteral("blueRemainingRevives")), -1, 3, -1);
    m_snapRedPending = state.value(QStringLiteral("redRevivePending")).toBool();
    m_snapBluePending = state.value(QStringLiteral("blueRevivePending")).toBool();
    m_redName = state.value(QStringLiteral("redName")).toString().trimmed();
    m_blueName = state.value(QStringLiteral("blueName")).toString().trimmed();
    m_notice = state.value(QStringLiteral("notice")).toString();
    m_ended = state.value(QStringLiteral("ended")).toBool();

    const QJsonObject summary = state.value(QStringLiteral("summary")).toObject();
    if (!summary.isEmpty()) {
        m_summary = summary;
    } else if (m_phase == QLatin1String("ready") || m_phase == QLatin1String("countdown")
               || m_phase == QLatin1String("starting") || m_phase == QLatin1String("running")) {
        m_summary = QJsonObject();
    }
    m_active = true;

    // Launch flash: once per round, when the countdown reaches 0 (or the
    // server already moved on from the countdown).
    const QString goKey = m_matchId + QLatin1Char('/') + m_roundId;
    const bool reachedZero = m_phase == QLatin1String("countdown") && m_remaining <= 0;
    const bool leftCountdown = (m_phase == QLatin1String("starting") || m_phase == QLatin1String("running"))
                               && m_previousPhase == QLatin1String("countdown") && prevPhase != m_phase;
    if ((reachedZero || leftCountdown) && goKey != m_goKey) {
        m_goKey = goKey;
        m_goStart = now();
        m_goFadeStart = -1;
    }
    refresh();
}

void CompetitionOverlay::setSettlementBlocking(bool blocking)
{
    if (m_blocking == blocking) return;
    m_blocking = blocking;
    const qint64 t = now();
    if (!blocking) {
        // Entrance animations start when the card is actually revealed.
        m_modeStart = t;
        m_prevStart = kFar;
    }
    refresh();
}

void CompetitionOverlay::setTeamNameFallback(const QString &redName, const QString &blueName)
{
    if (m_fallbackRed == redName.trimmed() && m_fallbackBlue == blueName.trimmed()) return;
    m_fallbackRed = redName.trimmed();
    m_fallbackBlue = blueName.trimmed();
    if (m_active) refresh();
}

void CompetitionOverlay::setLayoutMetrics(const QRect &viewport, qreal scale, int safeBottom)
{
    QRect vp = viewport;
    if (!vp.isValid() || vp.width() < 16 || vp.height() < 9) vp = rect();
    if (!vp.isValid() || vp.width() < 16 || vp.height() < 9) vp = QRect(0, 0, int(kRefW), int(kRefH));
    qreal s = scale;
    if (!std::isfinite(s) || s <= 0.01) s = qreal(vp.width()) / kRefW;
    s = qBound<qreal>(0.05, s, 8.0);
    if (vp == m_viewport && qFuzzyCompare(s, m_scale) && safeBottom == m_safeBottom) return;
    m_viewport = vp;
    m_scale = s;
    m_safeBottom = safeBottom;
    rebuildGlyphs();
    m_prevCache = QPixmap();
    m_prevStart = kFar;
    m_cur.staticKey.clear(); // force a static rebuild for the new geometry
    if (m_active) refresh();
    update();
}

void CompetitionOverlay::clear()
{
    m_frameTimer->stop();
    m_holdTimer->stop();
    m_active = false;
    m_phase.clear();
    m_previousPhase.clear();
    m_summary = QJsonObject();
    m_resultKey.clear();
    m_revealMs = -1;
    m_cur = Content();
    m_prev = Content();
    m_curCache = QPixmap();
    m_prevCache = QPixmap();
    m_prevStart = kFar;
    m_goStart = kFar;
    m_goFadeStart = -1;
    m_clockFlipStart = kFar;
    m_prevSeconds = -1;
    m_prevDigit = -1;
    hide();
}

// ---------------------------------------------------------------------------
// Test hooks / queries
// ---------------------------------------------------------------------------

bool CompetitionOverlay::resultVisible() const
{
    return m_active && !m_blocking && m_cur.mode == Mode::Result && m_cur.hasResult;
}

bool CompetitionOverlay::launchFlashActive() const
{
    return m_active && !m_blocking && m_cur.mode == Mode::Launch && now() < m_goStart + kGoMs;
}

QString CompetitionOverlay::intermissionClockText() const
{
    if (m_cur.mode == Mode::Intermission) return clockText(m_cur.seconds);
    if (m_active && m_phase == QLatin1String("intermission")) return clockText(m_remaining);
    return QString();
}

bool CompetitionOverlay::animating() const
{
    if (!m_active || m_blocking) return false;
    const qint64 t = now();
    if (m_prev.mode != Mode::Hidden && t < m_prevStart + kFadeMs) return true;
    switch (m_cur.mode) {
    case Mode::Hidden: return false;
    case Mode::Launch: return true;
    case Mode::Starting: return true; // loading dots
    case Mode::Countdown: return t < m_digitStart + 1100; // entrance + draining ring segment
    case Mode::Result: return t < m_modeStart + kResultAnimMs;
    case Mode::MatchFinished: return t < m_modeStart + kFinishAnimMs;
    case Mode::Intermission:
        return t < m_modeStart + kFadeMs + 120 || t < m_clockFlipStart + kFlipMs;
    case Mode::AwaitingDecision:
    case Mode::Cancelled:
        return t < m_modeStart + kFadeMs + 120;
    }
    return false;
}

QRect CompetitionOverlay::occupiedRect() const
{
    if (!m_active || m_blocking) return QRect();
    QRect r = paintedRect(m_cur.mode);
    if (m_prev.mode != Mode::Hidden && now() < m_prevStart + kFadeMs) r |= paintedRect(m_prev.mode);
    return r;
}

// ---------------------------------------------------------------------------
// Content (all JSON reading happens here, never in paintEvent)
// ---------------------------------------------------------------------------

void CompetitionOverlay::fillResult(Content &c) const
{
    const QJsonObject &s = m_summary;
    c.hasResult = !s.isEmpty();
    const bool valid = s.value(QStringLiteral("valid")).toBool();
    c.method = s.value(QStringLiteral("endingMethod")).toString().trimmed();
    const int winner = jsonInt(s.value(QStringLiteral("winner")), -1, 99, 0);
    c.winner = (valid && c.method != QLatin1String("safety_stop") && (winner == 1 || winner == 2)) ? winner : 0;

    const QString sRed = s.value(QStringLiteral("redName")).toString().trimmed();
    const QString sBlue = s.value(QStringLiteral("blueName")).toString().trimmed();
    if (!sRed.isEmpty()) c.red.name = sRed;
    if (!sBlue.isEmpty()) c.blue.name = sBlue;

    auto side = [](const QJsonObject &o, Side &out) {
        if (o.isEmpty()) return;
        out.pending = o.value(QStringLiteral("revivePending")).toBool();
        const bool known = !o.contains(QStringLiteral("quotaKnown"))
                           || o.value(QStringLiteral("quotaKnown")).toBool();
        const int rev = jsonInt(o.value(QStringLiteral("remainingRevives")), -1, 3, -1);
        out.revives = (known && !out.pending) ? rev : -1;
        out.maxHp = jsonInt(o.value(QStringLiteral("maxHp")), -1, 1000000, -1);
        if (out.maxHp <= 0) out.maxHp = -1;
        out.hp = jsonInt(o.value(QStringLiteral("hp")), -1, 1000000, -1);
        if (out.hp >= 0 && out.maxHp > 0) out.hp = qMin(out.hp, out.maxHp);
    };
    side(s.value(QStringLiteral("red")).toObject(), c.red);
    side(s.value(QStringLiteral("blue")).toObject(), c.blue);

    c.round = jsonInt(s.value(QStringLiteral("roundIndex")), 1, 99, c.round);
    c.redWins = jsonInt(s.value(QStringLiteral("redSeriesWins")), 0, 9, c.redWins);
    c.blueWins = jsonInt(s.value(QStringLiteral("blueSeriesWins")), 0, 9, c.blueWins);
    c.beforeRed = qMax(0, c.redWins - (c.winner == 1 ? 1 : 0));
    c.beforeBlue = qMax(0, c.blueWins - (c.winner == 2 ? 1 : 0));

    const QJsonValue dur = s.value(QStringLiteral("durationMs"));
    c.durationMs = dur.isDouble() && std::isfinite(dur.toDouble()) && dur.toDouble() > 0
                       ? qint64(qMin(dur.toDouble(), 24.0 * 3600 * 1000)) : -1;

    c.reason = s.value(QStringLiteral("reasonText")).toString().trimmed();
    if (c.reason.isEmpty()) c.reason = m_notice.trimmed();
    c.title = QStringLiteral("BO3 · 第 %1 局 · 结果").arg(c.round);
    if (c.winner == 1 || c.winner == 2) {
        c.headline = QStringLiteral("%1 获胜").arg(c.winner == 1 ? c.red.name : c.blue.name);
        c.kicker = methodText(c.method);
    } else {
        c.headline = QStringLiteral("本局终止");
        c.kicker = QStringLiteral("等待裁判判定");
    }
}

CompetitionOverlay::Content CompetitionOverlay::buildContent(qint64 t)
{
    Content c;
    c.round = m_round;
    c.redWins = m_redWins;
    c.blueWins = m_blueWins;
    c.beforeRed = m_redWins;
    c.beforeBlue = m_blueWins;
    c.red.name = !m_redName.isEmpty() ? m_redName : !m_fallbackRed.isEmpty() ? m_fallbackRed : QStringLiteral("红方");
    c.blue.name = !m_blueName.isEmpty() ? m_blueName : !m_fallbackBlue.isEmpty() ? m_fallbackBlue : QStringLiteral("蓝方");
    c.red.revives = m_snapRedPending ? -1 : m_snapRedRevives;
    c.blue.revives = m_snapBluePending ? -1 : m_snapBlueRevives;
    c.red.pending = m_snapRedPending;
    c.blue.pending = m_snapBluePending;
    if (!m_active) return c;

    const bool flash = t < m_goStart + kGoMs && m_goStart > kFar;
    const QString &ph = m_phase;
    const bool resultPhase = ph == QLatin1String("intermission") || ph == QLatin1String("match_finished")
                             || ph == QLatin1String("cancelled");

    if (ph == QLatin1String("countdown")) {
        if (m_remaining <= 0) c.mode = flash ? Mode::Launch : Mode::Hidden;
        else { c.mode = Mode::Countdown; c.digit = qBound(1, m_remaining, 5); }
    } else if (ph == QLatin1String("starting")) {
        c.mode = flash ? Mode::Launch : Mode::Starting;
    } else if (ph == QLatin1String("running")) {
        c.mode = flash ? Mode::Launch : Mode::Hidden;
    } else if (ph == QLatin1String("awaiting_decision")) {
        c.mode = Mode::AwaitingDecision;
    } else if (resultPhase) {
        // A cancelled match has no verdict to reveal: go straight to its own
        // consistent "比赛已取消" card instead of a generic "本局终止" result card.
        bool showCard = !m_summary.isEmpty() && ph != QLatin1String("cancelled");
        if (showCard) {
            QString key = m_summary.value(QStringLiteral("roundId")).toString();
            if (key.isEmpty()) key = m_roundId;
            key = m_matchId + QLatin1Char('/') + key;
            if (key != m_resultKey) { m_resultKey = key; m_revealMs = -1; }
            if (!m_blocking && m_revealMs < 0) m_revealMs = t;
            if (m_revealMs >= 0 && t >= m_revealMs + kHoldMs) showCard = false;
        }
        if (showCard) c.mode = Mode::Result;
        else if (ph == QLatin1String("intermission")) c.mode = Mode::Intermission;
        else if (ph == QLatin1String("match_finished")) c.mode = Mode::MatchFinished;
        else c.mode = Mode::Cancelled;
    } else {
        c.mode = Mode::Hidden; // ready / unknown
    }

    if (!m_summary.isEmpty() && (c.mode == Mode::Result || c.mode == Mode::MatchFinished
                                 || c.mode == Mode::Intermission || c.mode == Mode::Cancelled))
        fillResult(c);

    switch (c.mode) {
    case Mode::Hidden:
        break;
    case Mode::Countdown:
        c.title = QStringLiteral("BO3 · 第 %1 局").arg(c.round);
        c.headline = QStringLiteral("比赛即将开始");
        break;
    case Mode::Launch:
        c.title = QStringLiteral("开赛");
        c.headline = QStringLiteral("开赛");
        c.kicker = QStringLiteral("FIGHT");
        break;
    case Mode::Starting:
        c.title = QStringLiteral("等待双方设备确认");
        c.headline = c.title;
        c.kicker = QStringLiteral("BO3 · 第 %1 局").arg(c.round);
        break;
    case Mode::AwaitingDecision:
        c.title = QStringLiteral("等待裁判判定");
        c.headline = c.title;
        c.kicker = QStringLiteral("BO3 · 第 %1 局 · 比赛暂停计时").arg(c.round);
        c.reason = m_notice.trimmed();
        break;
    case Mode::Result:
        break; // fillResult
    case Mode::Intermission: {
        c.seconds = qBound(0, m_remaining, 99 * 60 + 59);
        // roundIndex only advances when the next countdown begins.
        const int next = qBound(1, c.round + 1, 99);
        c.round = next;
        c.headline = QStringLiteral("局间准备");
        c.title = c.seconds > 0 ? QStringLiteral("局间准备") : QStringLiteral("准备结束 · 等待裁判开赛");
        c.footer = QStringLiteral("下一局：第 %1 局").arg(next);
        c.reason.clear();
        break;
    }
    case Mode::MatchFinished: {
        const int w = c.redWins > c.blueWins ? 1 : c.blueWins > c.redWins ? 2 : 0;
        c.winner = w;
        c.title = QStringLiteral("比赛结束");
        if (w) {
            c.headline = QStringLiteral("%1 赢得比赛").arg(w == 1 ? c.red.name : c.blue.name);
            c.kicker = QStringLiteral("%1:%2").arg(qMax(c.redWins, c.blueWins)).arg(qMin(c.redWins, c.blueWins));
        } else {
            c.headline = QStringLiteral("比赛结束");
            c.kicker = QStringLiteral("等待裁判确认最终结果");
        }
        c.footer = QStringLiteral("BO3 · 总比分 %1 : %2").arg(c.redWins).arg(c.blueWins);
        break;
    }
    case Mode::Cancelled:
        c.winner = 0;
        c.title = QStringLiteral("本局已取消");
        // Phase "cancelled" is match-level: every line must say the same thing.
        c.headline = QStringLiteral("比赛已取消");
        c.kicker = QStringLiteral("本局不计胜负 · 等待裁判处理");
        if (c.reason.isEmpty()) c.reason = m_notice.trimmed();
        break;
    }
    if (c.mode == Mode::Result && c.title.isEmpty()) c.title = c.headline;

    auto sideKey = [](const Side &s) {
        return s.name + QLatin1Char('|') + QString::number(s.revives) + QLatin1Char('|')
               + QString::number(int(s.pending)) + QLatin1Char('|') + QString::number(s.hp)
               + QLatin1Char('|') + QString::number(s.maxHp);
    };
    // Plain concatenation (user text may contain '%').
    const QChar sep(0x1f);
    c.staticKey = QString::number(int(c.mode)) + sep + c.title + sep + c.headline + sep + c.kicker + sep
                  + c.reason + sep + c.method + sep + c.footer + sep + sideKey(c.red) + sep + sideKey(c.blue)
                  + sep + QString::number(c.round) + sep + QString::number(c.redWins) + sep
                  + QString::number(c.blueWins) + sep + QString::number(c.beforeRed) + sep
                  + QString::number(c.beforeBlue) + sep + QString::number(c.winner) + sep
                  + QString::number(c.durationMs);
    return c;
}

// ---------------------------------------------------------------------------
// Transitions and timers
// ---------------------------------------------------------------------------

void CompetitionOverlay::refresh()
{
    const qint64 t = now();
    if (!m_active) { syncVisibility(); return; }
    rebuildGlyphs(); // no-op unless the scale changed / first use
    apply(buildContent(t), t);

    // Result card hold: real timer in live mode, tick()/advanceForTest in manual mode.
    if (!m_manualClock && m_cur.mode == Mode::Result && m_revealMs >= 0 && !m_blocking) {
        const qint64 left = qMax<qint64>(1, m_revealMs + kHoldMs - t);
        m_holdTimer->start(int(qMin<qint64>(left, 600000)));
    } else {
        m_holdTimer->stop();
    }
    syncVisibility();
    ensureTicking();
}

void CompetitionOverlay::evaluateTimedMode()
{
    if (m_active) refresh();
}

void CompetitionOverlay::apply(const Content &next, qint64 t)
{
    const QRect before = occupiedRect();
    if (next.mode != m_cur.mode) {
        ++m_transitions;
        m_prev = m_cur;
        m_prevCache = m_curCache;
        m_prevStart = m_prev.mode == Mode::Hidden ? kFar : t;
        m_cur = next;
        m_modeStart = t;
        m_curCache = QPixmap();
        if (m_cur.mode != Mode::Hidden) rebuildStatic(m_cur, &m_curCache);
        if (m_cur.mode == Mode::Countdown) {
            m_prevDigit = -1;
            m_digitStart = t;
        }
        if (m_cur.mode == Mode::Intermission) {
            m_prevSeconds = m_cur.seconds;
            m_clockFlipStart = kFar;
        }
        if (m_cur.mode == Mode::Launch) m_goFadeStart = m_goStart + kGoMs - 260;
    } else {
        if (next.mode == Mode::Countdown && next.digit != m_cur.digit) {
            ++m_transitions;
            m_prevDigit = m_cur.digit;
            m_digitStart = t;
        }
        if (next.mode == Mode::Intermission && next.seconds != m_cur.seconds) {
            ++m_transitions;
            m_prevSeconds = m_cur.seconds;
            m_clockFlipStart = t;
        }
        const bool rebuild = next.staticKey != m_cur.staticKey || (m_curCache.isNull() && next.mode != Mode::Hidden);
        m_cur = next;
        if (rebuild) {
            m_curCache = QPixmap();
            if (m_cur.mode != Mode::Hidden) rebuildStatic(m_cur, &m_curCache);
        }
        if (!rebuild && before == occupiedRect() && !animating())
            return; // identical snapshot: nothing to repaint
    }
    const QRect area = before | occupiedRect();
    if (!area.isEmpty()) update(area);
}

void CompetitionOverlay::ensureTicking()
{
    if (m_manualClock) { m_frameTimer->stop(); return; }
    if (animating() && !isHidden()) {
        if (!m_frameTimer->isActive()) m_frameTimer->start();
    } else {
        m_frameTimer->stop();
    }
}

void CompetitionOverlay::tick()
{
    if (!m_active) { m_frameTimer->stop(); m_holdTimer->stop(); return; }
    const qint64 t = now();
    const QRect before = occupiedRect() | paintedRect(m_prev.mode);
    bool reevaluate = false;
    if (m_cur.mode == Mode::Launch && t >= m_goStart + kGoMs) reevaluate = true;
    if (m_cur.mode == Mode::Result && m_revealMs >= 0 && !m_blocking && t >= m_revealMs + kHoldMs)
        reevaluate = true;
    if (reevaluate) {
        refresh();
    } else {
        if (m_prev.mode != Mode::Hidden && t >= m_prevStart + kFadeMs) {
            m_prev = Content();
            m_prevCache = QPixmap();
        }
        syncVisibility();
        ensureTicking();
    }
    const QRect area = before | occupiedRect();
    if (!area.isEmpty() && !isHidden()) update(area);
}

void CompetitionOverlay::syncVisibility()
{
    const qint64 t = now();
    const bool fading = m_prev.mode != Mode::Hidden && t < m_prevStart + kFadeMs;
    const bool want = m_active && !m_blocking && (m_cur.mode != Mode::Hidden || fading);
    if (want == isHidden()) {
        setVisible(want);
        if (want) raise();
    }
    if (!want) m_frameTimer->stop();
}

// ---------------------------------------------------------------------------
// Geometry (1920x1080 reference coordinates)
// ---------------------------------------------------------------------------

QRectF CompetitionOverlay::modeRef(Mode mode) const
{
    switch (mode) {
    case Mode::Hidden: return QRectF();
    case Mode::Countdown: return QRectF(380, 120, 1160, 800);
    case Mode::Launch: return goRef();
    case Mode::Starting: return QRectF(610, 112, 700, 84);
    case Mode::AwaitingDecision:
    case Mode::Cancelled: return QRectF(510, 110, 900, 230);
    case Mode::Result: return QRectF(340, 150, 1240, 720);
    case Mode::MatchFinished: return QRectF(290, 130, 1340, 780);
    case Mode::Intermission: {
        const qreal h = 92, w = 940;
        qreal bottom = kRefH - 36;
        if (m_safeBottom >= 0 && m_scale > 0) {
            const qreal safeRef = (m_safeBottom - m_viewport.top()) / m_scale;
            if (std::isfinite(safeRef)) bottom = qMin(bottom, safeRef - 18);
        }
        // Never rise into the player reticle zone (centre 400x400 reference
        // px, y 340..740); a tall status line must not push the bar upward.
        // 16 px gap plus the 28 px glow margin paintedRect() adds around it.
        const qreal reticleClear = kRefH / 2 + 200 + 16 + 28;
        const qreal y = qBound<qreal>(reticleClear, bottom - h, kRefH - h);
        return QRectF((kRefW - w) / 2, y, w, h);
    }
    }
    return QRectF();
}

QRectF CompetitionOverlay::goRef() const
{
    return QRectF(0, 360, kRefW, 360);
}

QRect CompetitionOverlay::paintedRect(Mode mode) const
{
    const QRectF ref = modeRef(mode);
    if (ref.isEmpty()) return QRect();
    // Margin covers glow, entrance overshoot and anti-aliasing.
    const int m = qMax(4, qRound(u(28)));
    return toDevice(ref).adjusted(-m, -m, m, m).intersected(rect().isEmpty() ? QRect(-100000, -100000, 200000, 200000) : rect());
}

void CompetitionOverlay::rebuildGlyphs()
{
    if (qFuzzyCompare(m_glyphScale, m_scale) && !m_goPix.isNull()) return;
    m_glyphScale = m_scale;
    const qreal dpr = qMax<qreal>(1, devicePixelRatioF());

    const qreal px = u(400);
    const QFont f = uiFont(px, QFont::Black, true);
    const QFontMetricsF fm(f);
    const QSizeF cell(qMax<qreal>(8, fm.horizontalAdvance(QStringLiteral("8")) + u(80)),
                      qMax<qreal>(8, fm.height() + u(40)));
    for (int d = 0; d <= 5; ++d) {
        QPixmap pix((cell * dpr).toSize());
        pix.setDevicePixelRatio(dpr);
        pix.fill(Qt::transparent);
        QPainter p(&pix);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::TextAntialiasing);
        const QRectF r(QPointF(0, 0), cell);
        QPainterPath path;
        const QString s = QString::number(d);
        const qreal x = (cell.width() - fm.horizontalAdvance(s)) / 2;
        const qreal y = (cell.height() - fm.height()) / 2 + fm.ascent();
        path.addText(QPointF(x, y), f, s);
        // Soft outer glow: a few widening translucent strokes.
        for (int i = 4; i >= 1; --i) {
            p.setPen(QPen(withAlpha(QColor(0xff, 0xd0, 0x7a), 0.05 * (5 - i)), u(10) * i,
                          Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
            p.drawPath(path);
        }
        QLinearGradient g(r.topLeft(), r.bottomLeft());
        g.setColorAt(0, QColor(0xff, 0xff, 0xff));
        g.setColorAt(0.55, QColor(0xff, 0xf1, 0xd6));
        g.setColorAt(1, QColor(0xff, 0xb8, 0x4a));
        p.setPen(QPen(withAlpha(kInk, 0.75), qMax<qreal>(1, u(6))));
        p.setBrush(g);
        p.drawPath(path);
        m_digitPix[d] = pix;
    }

    // Launch word: "开赛" + "FIGHT".
    const QSizeF goSize(u(1100), u(320));
    QPixmap go((goSize * dpr).toSize());
    go.setDevicePixelRatio(dpr);
    go.fill(Qt::transparent);
    {
        QPainter p(&go);
        p.setRenderHint(QPainter::Antialiasing);
        const QFont big = uiFont(u(190), QFont::Black);
        const QFont small = uiFont(u(64), QFont::Bold, true);
        QPainterPath path;
        const QString word = QStringLiteral("开赛");
        const QFontMetricsF bm(big);
        path.addText(QPointF((goSize.width() - bm.horizontalAdvance(word)) / 2, u(30) + bm.ascent()), big, word);
        for (int i = 3; i >= 1; --i) {
            p.setPen(QPen(withAlpha(QColor(0xff, 0xc8, 0x5a), 0.08 * (4 - i)), u(12) * i,
                          Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawPath(path);
        }
        QLinearGradient g(0, 0, goSize.width(), 0);
        g.setColorAt(0, QColor(0xff, 0x8a, 0x8f));
        g.setColorAt(0.5, Qt::white);
        g.setColorAt(1, QColor(0x8c, 0xc0, 0xff));
        p.setPen(QPen(withAlpha(kInk, 0.8), qMax<qreal>(1, u(5))));
        p.setBrush(g);
        p.drawPath(path);
        QFont sf = small;
        sf.setLetterSpacing(QFont::AbsoluteSpacing, u(26));
        drawText(p, QRectF(0, u(232), goSize.width(), u(80)), QStringLiteral("FIGHT"), sf,
                 QColor(0xff, 0xe0, 0xa0));
    }
    m_goPix = go;
}

// ---------------------------------------------------------------------------
// Static layer: everything that only changes with staticKey. The pixmap
// covers paintedRect(mode) and is drawn at that rect's top-left.
// ---------------------------------------------------------------------------

void CompetitionOverlay::rebuildStatic(const Content &c, QPixmap *target) const
{
    if (!target) return;
    const QRect area = paintedRect(c.mode);
    if (area.isEmpty() || area.width() > 16384 || area.height() > 16384) { *target = QPixmap(); return; }
    const qreal dpr = qMax<qreal>(1, devicePixelRatioF());
    QPixmap pix(area.size() * dpr);
    pix.setDevicePixelRatio(dpr);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.translate(-area.topLeft());

    const QColor accent = sideColor(c.winner);
    auto teamBlock = [&](const QRectF &r, const Side &s, int wins, bool red, Qt::Alignment al) {
        const QColor col = red ? kRed : kBlue;
        QLinearGradient g(r.topLeft(), r.topRight());
        g.setColorAt(red ? 0 : 1, withAlpha(col, 0.85));
        g.setColorAt(red ? 1 : 0, withAlpha(col.darker(260), 0.55));
        fillRound(p, r, u(10), g);
        const QRectF nameR = r.adjusted(u(22), 0, -u(22), 0);
        drawText(p, nameR.adjusted(red ? 0 : u(70), 0, red ? -u(70) : 0, 0), s.name,
                 uiFont(u(34), QFont::Bold), kText, al | Qt::AlignVCenter);
        drawText(p, nameR, QString::number(wins), uiFont(u(46), QFont::Black, true), kText,
                 (red ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter);
    };

    switch (c.mode) {
    case Mode::Hidden:
    case Mode::Launch:
        break;
    case Mode::Countdown: {
        const QRectF title = rr(760, 150, 400, 56);
        fillRound(p, title, u(28), withAlpha(kInk, 0.78));
        p.setPen(QPen(withAlpha(kAmber, 0.8), qMax<qreal>(1, u(2))));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(title, u(28), u(28));
        drawText(p, title, c.title, uiFont(u(28), QFont::Bold), kText);
        // Team blocks sit outside the 250px ring (center x 960).
        teamBlock(rr(390, 470, 300, 92), c.red, c.redWins, true, Qt::AlignLeft);
        teamBlock(rr(1230, 470, 300, 92), c.blue, c.blueWins, false, Qt::AlignRight);
        // Ring track + dark disc behind the digit.
        const QPointF center = pt(960, 516);
        QRadialGradient disc(center, u(300));
        disc.setColorAt(0, withAlpha(kInk, 0.72));
        disc.setColorAt(0.8, withAlpha(kInk, 0.55));
        disc.setColorAt(1, withAlpha(kInk, 0));
        p.setPen(Qt::NoPen);
        p.setBrush(disc);
        p.drawEllipse(center, u(300), u(300));
        p.setPen(QPen(withAlpha(Qt::white, 0.14), u(12), Qt::SolidLine, Qt::FlatCap));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(center, u(250), u(250));
        drawText(p, rr(660, 820, 600, 50), c.headline, uiFont(u(30), QFont::DemiBold), kMuted);
        break;
    }
    case Mode::Starting: {
        const QRectF r = modeRef(c.mode);
        const QRectF cap = rr(r.x(), r.y(), r.width(), r.height());
        fillRound(p, cap, cap.height() / 2, withAlpha(kInk, 0.82));
        p.setPen(QPen(withAlpha(kAmber, 0.7), qMax<qreal>(1, u(2))));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(cap, cap.height() / 2, cap.height() / 2);
        drawText(p, rr(r.x() + 40, r.y(), 170, r.height()), c.kicker, uiFont(u(24), QFont::DemiBold),
                 kAmber, Qt::AlignLeft | Qt::AlignVCenter);
        drawText(p, rr(r.x() + 210, r.y(), r.width() - 330, r.height()), c.title,
                 uiFont(u(32), QFont::Bold), kText, Qt::AlignLeft | Qt::AlignVCenter);
        break;
    }
    case Mode::AwaitingDecision:
    case Mode::Cancelled: {
        const QRectF r = modeRef(c.mode);
        const QRectF card = rr(r.x(), r.y(), r.width(), r.height());
        QLinearGradient g(card.topLeft(), card.bottomLeft());
        g.setColorAt(0, withAlpha(QColor(0x1d, 0x1a, 0x10), 0.92));
        g.setColorAt(1, withAlpha(kInk, 0.9));
        fillRound(p, card, u(18), g);
        fillRound(p, rr(r.x(), r.y(), r.width(), 8), u(4), kAmber);
        drawText(p, rr(r.x() + 40, r.y() + 26, r.width() - 80, 40), c.kicker, uiFont(u(24), QFont::DemiBold), kAmber);
        drawText(p, rr(r.x() + 40, r.y() + 70, r.width() - 80, 80), c.headline, uiFont(u(58), QFont::Black), kText);
        drawText(p, rr(r.x() + 40, r.y() + 158, r.width() - 80, 46),
                 c.reason.isEmpty() ? QStringLiteral("请以裁判宣布为准") : c.reason,
                 uiFont(u(26), QFont::Normal), kMuted);
        break;
    }
    case Mode::Intermission: {
        const QRectF r = modeRef(c.mode);
        const QRectF bar = rr(r.x(), r.y(), r.width(), r.height());
        QLinearGradient g(bar.topLeft(), bar.topRight());
        g.setColorAt(0, withAlpha(kRed.darker(320), 0.9));
        g.setColorAt(0.5, withAlpha(kInk, 0.92));
        g.setColorAt(1, withAlpha(kBlue.darker(320), 0.9));
        fillRound(p, bar, u(16), g);
        fillRound(p, rr(r.x(), r.y(), 10, r.height()), u(4), kRed);
        fillRound(p, rr(r.right() - 10, r.y(), 10, r.height()), u(4), kBlue);
        const bool done = c.seconds <= 0;
        drawText(p, done ? rr(r.x() + 44, r.y(), r.width() - 88, r.height()) : rr(r.x() + 44, r.y(), 230, r.height()),
                 done ? c.title : c.headline,
                 uiFont(u(done ? 30 : 34), QFont::Bold), done ? kAmber : kText,
                 (done ? Qt::AlignHCenter : Qt::AlignLeft) | Qt::AlignVCenter);
        if (!done) {
            drawText(p, rr(r.x() + 540, r.y(), 360, r.height() / 2 + 6), c.footer, uiFont(u(28), QFont::DemiBold),
                     kText, Qt::AlignRight | Qt::AlignBottom);
            drawText(p, rr(r.x() + 540, r.y() + r.height() / 2 + 6, 360, r.height() / 2 - 12),
                     QStringLiteral("BO3 · %1 : %2").arg(c.redWins).arg(c.blueWins),
                     uiFont(u(22), QFont::Normal, true), kMuted, Qt::AlignRight | Qt::AlignTop);
        }
        break;
    }
    case Mode::Result:
    case Mode::MatchFinished:
        break; // continued below
    }
    if (c.mode != Mode::Result && c.mode != Mode::MatchFinished) { p.end(); *target = pix; return; }

    const bool finished = c.mode == Mode::MatchFinished;
    const QRectF ref = modeRef(c.mode);
    const QRectF card = rr(ref.x(), ref.y(), ref.width(), ref.height());
    {
        QLinearGradient g(card.topLeft(), card.bottomLeft());
        g.setColorAt(0, withAlpha(accent.darker(finished ? 260 : 330), 0.95));
        g.setColorAt(0.45, withAlpha(kInk, 0.93));
        g.setColorAt(1, withAlpha(kInk, 0.95));
        fillRound(p, card, u(24), g);
        QRadialGradient glow(pt(960, ref.y() + 150), u(finished ? 760 : 620));
        glow.setColorAt(0, withAlpha(accent, finished ? 0.42 : 0.32));
        glow.setColorAt(1, withAlpha(accent, 0));
        fillRound(p, card, u(24), glow);
        p.setPen(QPen(withAlpha(accent, 0.75), qMax<qreal>(1, u(3))));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(card, u(24), u(24));
        fillRound(p, rr(ref.x() + 120, ref.y(), ref.width() - 240, 8), u(4), accent);
    }
    const qreal x0 = ref.x() + 40, w0 = ref.width() - 80;

    if (finished) {
        drawText(p, rr(x0, ref.y() + 40, w0, 44), c.title + QStringLiteral(" · BO3"),
                 uiFont(u(30), QFont::Bold), accent.lighter(130));
        drawText(p, rr(x0, ref.y() + 100, w0, 140), c.headline, uiFont(u(104), QFont::Black), kText);
        drawText(p, rr(x0, ref.y() + 250, w0, 190), c.kicker, uiFont(u(c.winner ? 160 : 44), QFont::Black, c.winner != 0),
                 c.winner ? accent.lighter(135) : kAmber);
        teamBlock(rr(ref.x() + 120, ref.y() + 470, 500, 96), c.red, c.redWins, true, Qt::AlignLeft);
        teamBlock(rr(ref.right() - 620, ref.y() + 470, 500, 96), c.blue, c.blueWins, false, Qt::AlignRight);
        if (!c.reason.isEmpty())
            drawText(p, rr(x0, ref.y() + 600, w0, 44), QStringLiteral("决胜局：") + c.reason,
                     uiFont(u(26), QFont::Normal), kMuted);
        drawText(p, rr(x0, ref.y() + 660, w0, 44), c.footer, uiFont(u(26), QFont::DemiBold, true), kText);
        p.end();
        *target = pix;
        return;
    }

    // ---- Result card ----
    drawText(p, rr(x0, 176, w0, 36), c.title, uiFont(u(26), QFont::DemiBold), kMuted);
    drawText(p, rr(x0, 214, w0, 110), c.headline, uiFont(u(88), QFont::Black), kText);
    drawText(p, rr(x0, 326, w0, 38), c.kicker, uiFont(u(28), QFont::Bold), accent.lighter(125));
    drawText(p, rr(x0, 370, w0, 44), c.reason.isEmpty() ? QStringLiteral("—") : c.reason,
             uiFont(u(27), QFont::Normal), kText);
    fillRound(p, rr(x0, 432, w0, 2), 0, withAlpha(Qt::white, 0.12));

    auto panel = [&](qreal px, const Side &s, bool red) {
        const QColor col = red ? kRed : kBlue;
        const bool win = (red && c.winner == 1) || (!red && c.winner == 2);
        const QRectF pr = rr(px, 450, 560, 250);
        fillRound(p, pr, u(14), withAlpha(col.darker(win ? 230 : 380), win ? 0.8 : 0.6));
        if (win) {
            p.setPen(QPen(withAlpha(col, 0.9), qMax<qreal>(1, u(2))));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(pr, u(14), u(14));
        }
        fillRound(p, rr(px, 450, 8, 250), u(4), col);
        drawText(p, rr(px + 30, 466, 500, 44), s.name, uiFont(u(32), QFont::Bold), kText,
                 Qt::AlignLeft | Qt::AlignVCenter);
        drawText(p, rr(px + 30, 530, 200, 44), QStringLiteral("剩余复活"), uiFont(u(24), QFont::Normal), kMuted,
                 Qt::AlignLeft | Qt::AlignVCenter);
        const bool known = s.revives >= 0 && !s.pending;
        for (int i = 0; i < 3; ++i)
            drawCore(p, pt(px + 300 + i * 62, 552), u(22), col, known && i < s.revives, !known);
        drawText(p, rr(px + 470, 530, 70, 44),
                 known ? QStringLiteral("%1/3").arg(s.revives) : (s.pending ? QStringLiteral("确认中") : QStringLiteral("未知")),
                 uiFont(u(24), QFont::Bold, true), known ? kText : kMuted, Qt::AlignRight | Qt::AlignVCenter);
        drawText(p, rr(px + 30, 596, 200, 44), QStringLiteral("剩余 HP"), uiFont(u(24), QFont::Normal), kMuted,
                 Qt::AlignLeft | Qt::AlignVCenter);
        const QString hp = s.hp < 0 ? QStringLiteral("—")
                           : s.maxHp > 0 ? QStringLiteral("%1 / %2").arg(s.hp).arg(s.maxHp) : QString::number(s.hp);
        drawText(p, rr(px + 230, 596, 310, 44), hp, uiFont(u(30), QFont::Bold, true), kText,
                 Qt::AlignRight | Qt::AlignVCenter);
        fillRound(p, rr(px + 30, 656, 510, 16), u(8), withAlpha(Qt::white, 0.1));
    };
    panel(380, c.red, true);
    panel(980, c.blue, false);

    drawText(p, rr(x0, 722, 360, 110), QStringLiteral("BO3 比分"), uiFont(u(26), QFont::DemiBold), kMuted,
             Qt::AlignLeft | Qt::AlignVCenter);
    drawText(p, rr(940, 722, 40, 110), QStringLiteral(":"), uiFont(u(70), QFont::Black, true), kMuted);
    const qint64 sec = c.durationMs >= 0 ? (c.durationMs + 500) / 1000 : -1;
    drawText(p, rr(1180, 722, 360, 110),
             QStringLiteral("本局用时  ") + (sec >= 0 ? clockText(int(qMin<qint64>(sec, 5999))) : QStringLiteral("—")),
             uiFont(u(26), QFont::DemiBold, true), kMuted, Qt::AlignRight | Qt::AlignVCenter);
    p.end();
    *target = pix;
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void CompetitionOverlay::paintEvent(QPaintEvent *event)
{
    if (!m_active || m_blocking) return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setClipRect(event->rect());
    const qint64 t = now();
    if (m_prev.mode != Mode::Hidden && t < m_prevStart + kFadeMs) {
        const qreal k = progress(t, m_prevStart, kFadeMs);
        paintMode(p, m_prev, m_prevCache, t, 1 - outCubic(k), true);
    }
    if (m_cur.mode != Mode::Hidden) {
        const qint64 fadeIn = m_cur.mode == Mode::Result || m_cur.mode == Mode::MatchFinished ? 420 : 280;
        const qreal k = m_cur.mode == Mode::Launch ? 1.0 : outCubic(progress(t, m_modeStart, fadeIn));
        paintMode(p, m_cur, m_curCache, t, k, false);
    }
}

void CompetitionOverlay::paintMode(QPainter &p, const Content &c, const QPixmap &cache, qint64 t,
                                   qreal opacity, bool exiting) const
{
    if (opacity <= 0.003) return;
    p.save();
    p.setOpacity(clamp01(opacity));
    const QRect area = paintedRect(c.mode);
    const qint64 start = exiting ? m_prevStart : m_modeStart;
    const qreal in = exiting ? 1.0 : progress(t, start, 420);

    // Entrance motion per mode (scale for cards, slide for bars).
    QTransform tf;
    const QPointF center = QRectF(area).center();
    switch (c.mode) {
    case Mode::Result:
    case Mode::MatchFinished: {
        const qreal s = exiting ? 1.0 - 0.04 * (1 - opacity) : 0.92 + 0.08 * outBack(in);
        tf.translate(center.x(), center.y());
        tf.scale(s, s);
        tf.translate(-center.x(), -center.y());
        break;
    }
    case Mode::Intermission:
    case Mode::Starting:
    case Mode::AwaitingDecision:
    case Mode::Cancelled: {
        const qreal dy = exiting ? 0 : u(c.mode == Mode::Intermission ? 30 : -24) * (1 - outCubic(in));
        tf.translate(0, dy);
        break;
    }
    default:
        break;
    }
    p.setTransform(tf, true);

    if (c.mode != Mode::Launch && c.mode != Mode::Hidden && !cache.isNull())
        p.drawPixmap(area.topLeft(), cache);

    switch (c.mode) {
    case Mode::Countdown: paintCountdown(p, c, t); break;
    case Mode::Launch: {
        qreal env = 1;
        const qint64 fadeAt = m_goStart + kGoMs - 260;
        if (t >= fadeAt) env = 1 - progress(t, fadeAt, 260);
        paintLaunch(p, t, env);
        break;
    }
    case Mode::Starting: paintStarting(p, c, t); break;
    case Mode::Result: paintResultLive(p, c, t, exiting); break;
    case Mode::Intermission: paintIntermissionLive(p, c); break;
    case Mode::MatchFinished: paintFinishedLive(p, c, t, exiting); break;
    default: break;
    }
    p.restore();
}

void CompetitionOverlay::paintCountdown(QPainter &p, const Content &c, qint64 t) const
{
    const QPointF center = pt(960, 516);
    const qreal k = progress(t, m_digitStart, kDigitMs);
    const qreal secK = clamp01(qreal(t - m_digitStart) / 1000.0);

    // Segmented ring: 5 segments, the current second drains.
    const qreal radius = u(250);
    const QRectF ring(center.x() - radius, center.y() - radius, 2 * radius, 2 * radius);
    const int digit = qBound(1, c.digit, 5);
    for (int i = 0; i < 5; ++i) {
        const qreal startDeg = 90 - i * 72 - 3;
        qreal spanDeg = -66;
        QColor col = withAlpha(kAmber, 0.9);
        if (i >= digit) continue;                // consumed seconds
        if (i == digit - 1) spanDeg *= (1 - secK * 0.85);
        p.setPen(QPen(col, u(12), Qt::SolidLine, Qt::FlatCap));
        p.setBrush(Qt::NoBrush);
        p.drawArc(ring, int(startDeg * 16), int(spanDeg * 16));
    }

    // Outgoing digit.
    if (m_prevDigit >= 0 && m_prevDigit <= 5 && k < 1) {
        const QPixmap &old = m_digitPix[m_prevDigit];
        if (!old.isNull()) {
            const qreal dpr = old.devicePixelRatio();
            const QSizeF sz = QSizeF(old.size()) / dpr;
            const qreal s = 1.0 - 0.25 * outCubic(k);
            p.save();
            p.setOpacity(p.opacity() * (1 - outCubic(k * 1.6)));
            p.translate(center);
            p.scale(s, s);
            p.drawPixmap(QPointF(-sz.width() / 2, -sz.height() / 2), old);
            p.restore();
        }
    }
    // Incoming digit: 1.35 -> 1.0 with fade.
    const QPixmap &pix = m_digitPix[digit];
    if (!pix.isNull()) {
        const qreal dpr = pix.devicePixelRatio();
        const QSizeF sz = QSizeF(pix.size()) / dpr;
        const qreal s = 1.35 - 0.35 * outCubic(k);
        p.save();
        p.setOpacity(p.opacity() * clamp01(k * 2.2));
        p.translate(center);
        p.scale(s, s);
        p.drawPixmap(QPointF(-sz.width() / 2, -sz.height() / 2), pix);
        p.restore();
    }
    // Light pulse ring + diagonal sweep at each new second.
    if (k < 1) {
        p.save();
        const qreal r = radius * (0.9 + 0.35 * outCubic(k));
        p.setPen(QPen(withAlpha(QColor(0xff, 0xe2, 0xa8), 0.55 * (1 - k)), u(6)));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(center, r, r);
        p.setClipRect(QRectF(center.x() - radius, center.y() - radius, 2 * radius, 2 * radius),
                      Qt::IntersectClip);
        const qreal sx = center.x() - radius * 1.4 + 2.8 * radius * outCubic(k);
        QLinearGradient g(sx - u(60), 0, sx + u(60), 0);
        g.setColorAt(0, withAlpha(Qt::white, 0));
        g.setColorAt(0.5, withAlpha(Qt::white, 0.22 * (1 - k)));
        g.setColorAt(1, withAlpha(Qt::white, 0));
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawEllipse(center, radius - u(8), radius - u(8));
        p.restore();
    }
}

void CompetitionOverlay::paintLaunch(QPainter &p, qint64 t, qreal envelope) const
{
    if (envelope <= 0.003) return;
    const qint64 el = qMax<qint64>(0, t - m_goStart);
    const QRectF band = rr(0, 360, kRefW, 360);
    p.save();
    p.setOpacity(p.opacity() * clamp01(envelope));
    // Dark band behind the word (opens vertically).
    const qreal open = outCubic(progress(t, m_goStart, 220));
    const QRectF dark(band.left(), band.center().y() - band.height() * 0.32 * open, band.width(),
                      band.height() * 0.64 * open);
    QLinearGradient bg(dark.topLeft(), dark.topRight());
    bg.setColorAt(0, withAlpha(kRed.darker(300), 0));
    bg.setColorAt(0.2, withAlpha(kRed.darker(300), 0.75));
    bg.setColorAt(0.5, withAlpha(kInk, 0.85));
    bg.setColorAt(0.8, withAlpha(kBlue.darker(300), 0.75));
    bg.setColorAt(1, withAlpha(kBlue.darker(300), 0));
    p.fillRect(dark, bg);
    // Horizontal light band sweeping once.
    const qreal sk = outCubic(progress(t, m_goStart + 60, 560));
    const qreal sx = band.left() - u(400) + (band.width() + u(800)) * sk;
    QLinearGradient sw(sx - u(260), 0, sx + u(260), 0);
    sw.setColorAt(0, withAlpha(Qt::white, 0));
    sw.setColorAt(0.5, withAlpha(QColor(0xff, 0xf0, 0xd0), 0.55 * (1 - sk * 0.6)));
    sw.setColorAt(1, withAlpha(Qt::white, 0));
    p.fillRect(QRectF(sx - u(260), dark.top(), u(520), dark.height()), sw);
    p.fillRect(QRectF(band.left(), dark.top(), band.width() * sk, qMax<qreal>(1, u(3))), withAlpha(kAmber, 0.9));
    p.fillRect(QRectF(band.right() - band.width() * sk, dark.bottom() - qMax<qreal>(1, u(3)), band.width() * sk,
                      qMax<qreal>(1, u(3))), withAlpha(kAmber, 0.9));
    // Impact word: 1.6 -> 1.0.
    if (!m_goPix.isNull()) {
        const qreal ik = progress(t, m_goStart, 300);
        const qreal s = 1.6 - 0.6 * outBack(ik) + 0.03 * clamp01(qreal(el - 300) / 500.0);
        const QSizeF sz = QSizeF(m_goPix.size()) / m_goPix.devicePixelRatio();
        p.setOpacity(p.opacity() * clamp01(ik * 3));
        p.translate(pt(960, 540));
        p.scale(s, s);
        p.drawPixmap(QPointF(-sz.width() / 2, -sz.height() / 2), m_goPix);
    }
    p.restore();
}

void CompetitionOverlay::paintStarting(QPainter &p, const Content &c, qint64 t) const
{
    Q_UNUSED(c);
    const QRectF r = modeRef(Mode::Starting);
    const qint64 el = qMax<qint64>(0, t - m_modeStart);
    p.setPen(Qt::NoPen);
    for (int i = 0; i < 3; ++i) {
        const qreal phase = std::fmod(el / 900.0 - i * 0.18 + 10.0, 1.0);
        const qreal a = 0.25 + 0.75 * std::pow(std::sin(phase * M_PI), 2);
        p.setBrush(withAlpha(kAmber, a));
        p.drawEllipse(pt(r.right() - 100 + i * 26, r.center().y()), u(6) * (0.8 + 0.3 * a), u(6) * (0.8 + 0.3 * a));
    }
}

void CompetitionOverlay::paintResultLive(QPainter &p, const Content &c, qint64 t, bool exiting) const
{
    const qreal k = exiting ? 1.0 : outCubic(progress(t, m_modeStart + 250, 900));
    auto bar = [&](qreal px, const Side &s, bool red) {
        if (s.hp < 0 || s.maxHp <= 0) return;
        const qreal ratio = clamp01(qreal(s.hp) / qreal(s.maxHp)) * k;
        if (ratio <= 0) return;
        const QColor col = red ? kRed : kBlue;
        const QRectF r = rr(px + 30, 656, 510 * ratio, 16);
        QLinearGradient g(r.topLeft(), r.topRight());
        g.setColorAt(0, col.darker(120));
        g.setColorAt(1, col.lighter(130));
        fillRound(p, r, u(8), g);
    };
    bar(380, c.red, true);
    bar(980, c.blue, false);

    // BO3 score flip before -> after.
    const qreal fk = exiting ? 1.0 : progress(t, m_modeStart + 600, 520);
    auto score = [&](const QRectF &ref, int before, int after, const QColor &col) {
        const QRectF r = rr(ref.x(), ref.y(), ref.width(), ref.height());
        const QFont f = uiFont(u(84), QFont::Black, true);
        // Solid tile behind each score so a narrow "1" reads as a number on a
        // scoreboard plate, never as a stray bar next to the separator.
        {
            const QRectF plate = r.adjusted(u(14), u(6), -u(14), -u(6));
            p.save();
            p.setPen(QPen(QColor(col.red(), col.green(), col.blue(), 150), qMax<qreal>(1.0, u(2))));
            p.setBrush(QColor(12, 16, 26, 215));
            p.drawRoundedRect(plate, u(14), u(14));
            p.restore();
        }
        if (before == after || fk >= 1) {
            drawText(p, r, QString::number(after), f, col);
            return;
        }
        const qreal e = outCubic(fk);
        p.save();
        p.setClipRect(r, Qt::IntersectClip);
        p.translate(0, -r.height() * e);
        drawText(p, r, QString::number(before), f, withAlpha(col, 1 - e));
        drawText(p, r.translated(0, r.height()), QString::number(after), f, col);
        p.restore();
    };
    score(QRectF(800, 722, 130, 110), c.beforeRed, c.redWins, c.winner == 1 ? kRed.lighter(125) : kText);
    score(QRectF(990, 722, 130, 110), c.beforeBlue, c.blueWins, c.winner == 2 ? kBlue.lighter(125) : kText);

    // One-time sheen across the headline.
    const qreal sk = exiting ? 1.0 : progress(t, m_modeStart + 150, 800);
    if (sk < 1) {
        const QRectF hr = rr(380, 214, 1160, 110);
        const qreal sx = hr.left() + hr.width() * outCubic(sk);
        QLinearGradient g(sx - u(120), 0, sx + u(120), 0);
        g.setColorAt(0, withAlpha(Qt::white, 0));
        g.setColorAt(0.5, withAlpha(Qt::white, 0.16 * (1 - sk)));
        g.setColorAt(1, withAlpha(Qt::white, 0));
        p.fillRect(QRectF(sx - u(120), hr.top(), u(240), hr.height()), g);
    }
}

void CompetitionOverlay::paintIntermissionLive(QPainter &p, const Content &c) const
{
    if (c.seconds <= 0) return;
    const QRectF r = modeRef(Mode::Intermission);
    paintClock(p, rr(r.x() + 280, r.y(), 240, r.height()), u(56), c.seconds, kText, Qt::AlignCenter);
}

void CompetitionOverlay::paintFinishedLive(QPainter &p, const Content &c, qint64 t, bool exiting) const
{
    if (exiting) return;
    const QRectF ref = modeRef(Mode::MatchFinished);
    const QColor accent = sideColor(c.winner);
    const qreal k = progress(t, m_modeStart, kFinishAnimMs);
    if (k >= 1) return;
    // Two light sweeps and an expanding accent line.
    const QRectF card = rr(ref.x(), ref.y(), ref.width(), ref.height());
    p.save();
    QPainterPath clip;
    clip.addRoundedRect(card, u(24), u(24));
    p.setClipPath(clip, Qt::IntersectClip);
    for (int i = 0; i < 2; ++i) {
        const qreal sk = outCubic(clamp01(k * 1.4 - i * 0.3));
        if (sk <= 0 || sk >= 1) continue;
        const qreal sx = card.left() - u(300) + (card.width() + u(600)) * sk;
        QLinearGradient g(sx - u(200), 0, sx + u(200), 0);
        g.setColorAt(0, withAlpha(Qt::white, 0));
        g.setColorAt(0.5, withAlpha(accent.lighter(160), 0.22 * (1 - sk)));
        g.setColorAt(1, withAlpha(Qt::white, 0));
        p.fillRect(QRectF(sx - u(200), card.top(), u(400), card.height()), g);
    }
    p.restore();
    const qreal lk = outCubic(k * 1.5);
    const qreal w = u(ref.width() - 240) * lk;
    fillRound(p, QRectF(card.center().x() - w / 2, card.bottom() - u(8), w, u(8)), u(4), withAlpha(accent, 0.9));
}

void CompetitionOverlay::paintClock(QPainter &p, const QRectF &devRect, qreal pixel, int seconds,
                                    const QColor &color, Qt::Alignment align) const
{
    const QFont f = uiFont(pixel, QFont::Bold, true);
    const QString cur = clockText(seconds);
    const qint64 t = now();
    const qreal k = progress(t, m_clockFlipStart, kFlipMs);
    if (k >= 1 || m_prevSeconds < 0 || m_prevSeconds == seconds) {
        drawText(p, devRect, cur, f, color, align);
        return;
    }
    // Flip only the characters that changed.
    const QString old = clockText(m_prevSeconds);
    const QFontMetricsF fm(f);
    const qreal total = fm.horizontalAdvance(cur);
    qreal x = devRect.center().x() - total / 2;
    if (align & Qt::AlignLeft) x = devRect.left();
    else if (align & Qt::AlignRight) x = devRect.right() - total;
    const qreal e = outCubic(k);
    p.save();
    p.setClipRect(devRect, Qt::IntersectClip);
    for (int i = 0; i < cur.size(); ++i) {
        const QString ch = cur.mid(i, 1);
        const qreal cw = fm.horizontalAdvance(ch);
        const QRectF cell(x, devRect.top(), cw, devRect.height());
        if (i < old.size() && old.at(i) != cur.at(i)) {
            const qreal h = devRect.height() * 0.55;
            drawText(p, cell.translated(0, h * e), old.mid(i, 1), f, withAlpha(color, 1 - e), Qt::AlignCenter);
            drawText(p, cell.translated(0, -h * (1 - e)), ch, f, withAlpha(color, e), Qt::AlignCenter);
        } else {
            drawText(p, cell, ch, f, color, Qt::AlignCenter);
        }
        x += cw;
    }
    p.restore();
}
