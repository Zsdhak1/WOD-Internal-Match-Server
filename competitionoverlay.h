#ifndef COMPETITIONOVERLAY_H
#define COMPETITIONOVERLAY_H

#include <QElapsedTimer>
#include <QJsonObject>
#include <QLabel>
#include <QPixmap>
#include <QRect>
#include <QString>
#include <QWidget>

class QPainter;
class QTimer;

// Three self-drawn "mech core" revive icons under each HP ribbon. It stays a
// QLabel so text()/accessibleName()/toolTip() keep the plain description
// ("剩余复活  2 / 3", "未知", "确认中") for tests and screen readers; the
// visible output is fully custom painted. Idle = zero timers.
class ReviveIndicator final : public QLabel
{
public:
    static constexpr int kLimit = 3;
    explicit ReviveIndicator(bool red, QWidget *parent = nullptr);

    // remaining: 0..3 known, anything else = unknown (never shown as 0).
    // animate=false snaps (e.g. first snapshot after connecting).
    void setResourceState(int remaining, bool pending, bool animate = true);
    int remainingRevives() const { return m_remaining; } // -1 = unknown
    bool pending() const { return m_pending; }
    bool known() const { return m_remaining >= 0; }
    bool animating() const;

    void setManualClockForTest(bool manual);
    void advanceForTest(int milliseconds);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    qint64 now() const;
    void ensureTicking();

    bool m_red = true;
    bool m_initialized = false;
    int m_remaining = -1;
    bool m_pending = false;
    qint64 m_breakStart[kLimit] = {-100000, -100000, -100000};
    qint64 m_lightStart[kLimit] = {-100000, -100000, -100000};
    QElapsedTimer m_clock;
    bool m_manualClock = false;
    qint64 m_manualNow = 0;
    QTimer *m_timer = nullptr;
};

// Formal (remaining_resources) presentation layer for the broadcast and the
// player client: start countdown, launch flash, device confirmation, verdict
// card, intermission bar, BO3 final and neutral states.
//
// It only *displays* the authoritative server snapshot: it never advances a
// phase, expires a clock or decides a winner. All drawing is QPainter-based,
// mouse-transparent and uses painter opacity/gradients only (no graphics
// effects over the video). A frame timer runs only while something animates.
class CompetitionOverlay final : public QWidget
{
public:
    enum class Mode {
        Hidden,           // ready / running / unknown / blocked by the settlement clip
        Countdown,        // 5..1
        Launch,           // countdown reached 0: only the launch flash is shown
        Starting,         // waiting for both devices to confirm the start
        AwaitingDecision, // neutral, referee decides
        Result,           // large verdict card after the settlement clip
        Intermission,     // compact preparation bar (after the card hold)
        MatchFinished,    // BO3 final
        Cancelled
    };

    explicit CompetitionOverlay(QWidget *parent = nullptr);

    // Snapshot is the same object BroadcastWindow::setCompetitionState gets.
    // Repeated identical snapshots never restart an animation.
    void setCompetitionState(const QJsonObject &state);
    // True while the settlement clip plays: results stay hidden (and the
    // widget hidden) so the clip is never covered. Revealed when it clears.
    void setSettlementBlocking(bool blocking);
    void setTeamNameFallback(const QString &redName, const QString &blueName);
    // viewport: scaled 1920x1080 HUD rectangle in this widget's coordinates.
    // safeBottom: y (widget coordinates) the compact bar must stay above.
    void setLayoutMetrics(const QRect &viewport, qreal scale, int safeBottom);
    void clear(); // Leaves formal presentation (legacy mode).

    // ---- Test hooks ----
    Mode currentMode() const { return m_cur.mode; }
    bool resultVisible() const;
    bool launchFlashActive() const;
    bool settlementBlocking() const { return m_blocking; }
    int countdownDigit() const { return m_cur.mode == Mode::Countdown ? m_cur.digit : -1; }
    QString headlineText() const { return m_cur.headline; }
    QString reasonText() const { return m_cur.reason; }
    QString intermissionClockText() const;
    QString statusText() const { return m_cur.title; }
    // Number of started transitions (mode changes, new countdown digits,
    // launch flashes, clock flips). Unchanged by repeated snapshots.
    int transitionCount() const { return m_transitions; }
    bool animating() const;
    // Union of everything currently painted, in widget coordinates.
    QRect occupiedRect() const;
    // Deterministic clock: once enabled, time only moves via advanceForTest.
    void setManualClockForTest(bool manual);
    void advanceForTest(int milliseconds);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    struct Side {
        QString name;
        int revives = -1;   // -1 unknown
        bool pending = false;
        int hp = -1;        // -1 unknown
        int maxHp = -1;
    };
    struct Content {
        Mode mode = Mode::Hidden;
        int digit = -1;
        int round = 1;
        int redWins = 0, blueWins = 0;
        int beforeRed = 0, beforeBlue = 0;
        int winner = 0;     // 0 neutral, 1 red, 2 blue
        bool hasResult = false;
        QString title, headline, kicker, reason, method, footer;
        Side red, blue;
        qint64 durationMs = -1;
        int seconds = 0;    // intermission seconds
        QString staticKey;  // everything cached in the static pixmap
    };

    qint64 now() const;
    void refresh();
    Content buildContent(qint64 now);
    void fillResult(Content &c) const;
    void apply(const Content &next, qint64 now);
    void evaluateTimedMode();
    void ensureTicking();
    void tick();
    void syncVisibility();
    void rebuildStatic(const Content &c, QPixmap *target) const;
    void rebuildGlyphs();
    QRectF modeRef(Mode mode) const;  // reference coordinates
    QRect toDevice(const QRectF &ref) const;
    QRect paintedRect(Mode mode) const;
    QRectF goRef() const;

    void paintMode(QPainter &p, const Content &c, const QPixmap &cache, qint64 t,
                   qreal opacity, bool exiting) const;
    void paintCountdown(QPainter &p, const Content &c, qint64 t) const;
    void paintLaunch(QPainter &p, qint64 t, qreal envelope) const;
    void paintStarting(QPainter &p, const Content &c, qint64 t) const;
    void paintResultLive(QPainter &p, const Content &c, qint64 t, bool exiting) const;
    void paintIntermissionLive(QPainter &p, const Content &c) const;
    void paintFinishedLive(QPainter &p, const Content &c, qint64 t, bool exiting) const;
    void paintClock(QPainter &p, const QRectF &devRect, qreal pixel, int seconds,
                    const QColor &color, Qt::Alignment align) const;

    qreal u(qreal v) const { return v * m_scale; }
    QPointF pt(qreal x, qreal y) const;
    QRectF rr(qreal x, qreal y, qreal w, qreal h) const;

    // Raw authoritative snapshot fields.
    QString m_phase, m_previousPhase, m_matchId, m_roundId, m_notice;
    QString m_redName, m_blueName, m_fallbackRed, m_fallbackBlue;
    int m_remaining = 0, m_round = 1, m_redWins = 0, m_blueWins = 0;
    int m_snapRedRevives = -1, m_snapBlueRevives = -1;
    bool m_snapRedPending = false, m_snapBluePending = false;
    bool m_ended = false;
    QJsonObject m_summary;  // retained per round
    bool m_active = false;
    bool m_blocking = false;
    qint64 m_revealMs = -1;
    QString m_goKey;
    QString m_resultKey;    // round whose result card hold is tracked

    QRect m_viewport;
    qreal m_scale = 1.0;
    int m_safeBottom = -1;

    Content m_cur, m_prev;
    QPixmap m_curCache, m_prevCache;
    qint64 m_modeStart = 0, m_prevStart = -100000, m_digitStart = 0;
    int m_prevDigit = -1;
    qint64 m_goStart = -100000, m_goFadeStart = -1;
    qint64 m_clockFlipStart = -100000;
    int m_prevSeconds = -1;
    int m_transitions = 0;

    QPixmap m_digitPix[6];
    QPixmap m_goPix;
    qreal m_glyphScale = -1;

    QElapsedTimer m_clock;
    bool m_manualClock = false;
    qint64 m_manualNow = 0;
    QTimer *m_frameTimer = nullptr;
    QTimer *m_holdTimer = nullptr;
};

#endif // COMPETITIONOVERLAY_H
