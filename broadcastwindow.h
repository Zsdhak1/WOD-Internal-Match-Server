#ifndef BROADCASTWINDOW_H
#define BROADCASTWINDOW_H

#include "robotmanager.h"

#include <QByteArray>
#include <QImage>
#include <QJsonObject>
#include <QHash>
#include <QMainWindow>
#include <QPoint>
#include <QQueue>
#include <QSize>
#include <QStringList>
#include <QVector>

class QCloseEvent;
class QHideEvent;
class QLabel;
class VideoCanvas;
class QGraphicsOpacityEffect;
class QLayout;
class QPropertyAnimation;
class QProgressBar;
class QProcess;
class QResizeEvent;
class QScreen;
class QShowEvent;
class QTimer;
class QVariantAnimation;
class QVBoxLayout;
class QWidget;
class TickerMarqueeWidget;
class MatchSummaryPanel;

// 独立的赛事输出窗口。视频帧作为底层画面，赛事状态作为透明叠加层显示。
class BroadcastWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit BroadcastWindow(QWidget *parent = nullptr);

    // Hosts such as the player client can place controls over the same
    // full-screen HUD without creating a second window.
    QWidget *interactionLayer() const { return m_interactionLayer; }

    void showOnScreen(QScreen *screen);
    void updateRobots(const QVector<RobotManager::RobotInfo> &robots);

    // 视频接收模块只需要把最新帧交给这里，UI 会按窗口尺寸裁切铺满背景。
    void setProgramFrame(const QImage &frame);
    void setSourceFrame(const QString &sourceId, const QImage &frame);
    void setActiveSource(const QString &sourceId, const QString &sourceTitle = QString());
    // Read-only access for the control panel's per-source preview strip.
    QImage sourceFrame(const QString &sourceId) const;
    QStringList knownSourceIds() const;
    void setTeamNames(const QString &redName, const QString &blueName);
    void setScores(int redScore, int blueScore);
    void setTickerText(const QString &text);
    void setTickerVisible(bool visible, bool animated = true);
    QString tickerText() const { return m_tickerText; }
    bool tickerVisible() const { return m_tickerVisible; }
    QStringList layoutElementIds() const;
    QPoint layoutElementPosition(const QString &elementId) const;
    void setLayoutElementPosition(const QString &elementId, const QPoint &position);
    QSize layoutElementSize(const QString &elementId) const;
    void setLayoutElementSize(const QString &elementId, const QSize &size);
    void resetLayoutPositions();
    bool saveLayout(const QString &filePath) const;
    bool loadLayout(const QString &filePath);
    void awardCard(quint8 team, bool redCard);
    void clearCards();
    void clearProgramFrame();

    void startMatch();
    void pauseMatch();
    void resetMatch();
    void setMatchState(int remainingSeconds, bool running, bool suddenDeath = false);
    bool suddenDeath() const { return m_suddenDeath; }
    void terminateMatch();
    // Ends the round with an externally decided result (e.g. MatchReferee).
    void concludeRound(const QString &settlementType);
    // When disabled, reaching 00:00 only stops the clock and emits
    // timeExpired(); the owner then decides the result. The player client
    // uses this so its local clock can never race the server's verdict.
    void setAutoFinishOnTimeout(bool enabled) { m_autoFinishOnTimeout = enabled; }
    void setRefereeNotice(const QString &notice);
    QString refereeNotice() const { return m_refereeNotice; }
    // Post-match data panel. The panel is revealed after the settlement clip.
    void setMatchSummary(const QJsonObject &summary);
    void clearMatchSummary();
    QJsonObject matchSummary() const { return m_matchSummary; }
    bool summaryVisible() const;
    bool summaryExpanded() const;
    void setSummaryExpanded(bool expanded, bool animated = true);
    void playSettlement(const QString &type);
    void playSettlementPreview(const QString &type);
    void stopSettlement();
    void clearSettlement();
    bool isMatchRunning() const { return m_matchRunning; }
    int remainingSeconds() const { return m_remainingSeconds; }
    int redScore() const { return m_redScore; }
    int blueScore() const { return m_blueScore; }
    bool roundEnded() const { return m_roundEnded; }
    QString settlementType() const { return m_settlementType; }
    bool settlementPlaying() const { return m_settlementProcess != nullptr; }
    QString redTeamName() const { return m_teamNames.value(1, QStringLiteral("红方")); }
    QString blueTeamName() const { return m_teamNames.value(2, QStringLiteral("蓝方")); }

signals:
    void visibilityChanged(bool visible);
    void matchStateChanged(bool running);
    void presentationStateChanged();
    void roundFinished(const QString &settlementType);
    void timeExpired();
    void summaryExpandedChanged(bool expanded);
    // Emitted at most ~10 Hz per source so the control-panel preview strip
    // does not have to scale every incoming frame.
    void sourceFrameUpdated(const QString &sourceId);

protected:
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

    // Current HUD scale relative to the 1920x1080 reference layout. Hosts
    // (e.g. the player client) use it to scale their own overlay chrome.
    qreal uiScaleFactor() const { return m_uiScale; }

private:
    struct RobotOverlay {
        quint8 teamId = 0;
        QLabel *team = nullptr;
        QLabel *connection = nullptr;
        QProgressBar *healthBar = nullptr;
        QProgressBar *heatBar = nullptr;
        QLabel *health = nullptr;
        QLabel *heat = nullptr;
        QLabel *state = nullptr;
        QLabel *shoot = nullptr;
        QLabel *diagnostics = nullptr;
        quint64 robotKey = 0;
        QLabel *redCard = nullptr;
        QLabel *yellowCard = nullptr;
        QLabel *redCardIcon = nullptr;
        QLabel *yellowCardIcon = nullptr;
        QLabel *damageNotice = nullptr;
        QGraphicsOpacityEffect *damageOpacity = nullptr;
        QPropertyAnimation *damageAnimation = nullptr;
        QPropertyAnimation *damageDropAnimation = nullptr;
        QVariantAnimation *damageBufferAnimation = nullptr;
        QWidget *stateLayer = nullptr;   // death dim / hit glow / winner frame
    };

    void buildUi();
    QWidget *createRobotOverlay(const QString &teamTitle,
                                quint8 team,
                                RobotOverlay *overlay);
    void updateOverlay(const RobotManager::RobotInfo *robot,
                       RobotOverlay &overlay,
                       quint64 robotKey);
    void triggerDamageEffect(RobotOverlay &overlay,
                             int damage,
                             int previousHp,
                             int currentHp);
    void finishMatch(const QString &settlementType);
    void resetRound();
    void revealSummaryIfReady();
    void layoutSummaryPanel();
    void updateWinnerHighlight();
    void flashScore(QLabel *label);
    QString settlementTypeForScores() const;
    void updateTimerDisplay();
    void updateMatchState();
    void animateTicker();
    void scheduleProgramRender();
    void renderProgramFrame();
    QString settlementAssetDirectory() const;
    QString settlementAssetPath(const QString &type, const QString &suffix) const;
    QString ffmpegExecutable() const;
    void consumeSettlementOutput();
    void renderSettlementFrame(const QImage &frame);
    void registerLayoutElement(const QString &elementId, QWidget *widget);
    void applyLayoutPositions();
    void applyUiScale();
    void setBaseFont(QWidget *widget, double pointSize, bool bold);
    int tickerBarHeight() const;
    const RobotManager::RobotInfo *robotForTeam(
        quint8 team,
        const QHash<quint64, RobotManager::RobotInfo> &robots) const;

    QHash<quint64, RobotOverlay> m_overlays;
    QHash<QString, QWidget *> m_layoutElements;
    QHash<QString, QLayout *> m_layoutManagers;
    QHash<QString, QPoint> m_layoutOffsets;
    QHash<QString, QSize> m_layoutSizes;
    QHash<QString, QSize> m_layoutMinimumSizes;
    QHash<QString, QSize> m_layoutMaximumSizes;
    QLabel *m_programView = nullptr;
    VideoCanvas *m_videoCanvas = nullptr;
    QLabel *m_settlementView = nullptr;
    QLabel *m_matchTitle = nullptr;
    QLabel *m_timerLabel = nullptr;
    QLabel *m_scoreLabel = nullptr;
    QLabel *m_redRoundScoreLabel = nullptr;
    QLabel *m_blueRoundScoreLabel = nullptr;
    QLabel *m_matchState = nullptr;
    QLabel *m_sourceLabel = nullptr;
    QWidget *m_tickerBar = nullptr;
    TickerMarqueeWidget *m_tickerTextView = nullptr;
    QWidget *m_titleLayer = nullptr;
    QWidget *m_timerLayer = nullptr;
    QLabel *m_tickerTagLabel = nullptr;
    QWidget *m_hudOverlay = nullptr;
    QVBoxLayout *m_overlayLayout = nullptr;
    QHash<QWidget *, double> m_baseFontPoints;
    qreal m_uiScale = 1.0;
    bool m_uiScaleReady = false;
    QGraphicsOpacityEffect *m_tickerOpacity = nullptr;
    QPropertyAnimation *m_tickerHeightAnimation = nullptr;
    QPropertyAnimation *m_tickerOpacityAnimation = nullptr;
    QWidget *m_interactionLayer = nullptr;
    QTimer *m_matchTimer = nullptr;
    QTimer *m_programRenderTimer = nullptr;
    QTimer *m_settlementFrameTimer = nullptr;
    QProcess *m_settlementProcess = nullptr;
    QHash<QString, QImage> m_sourceFrames;
    QHash<QString, qint64> m_sourceFrameSignalTimes;
    QByteArray m_settlementBuffer;
    QQueue<QImage> m_settlementFrames;
    QImage m_lastSettlementFrame;
    QHash<quint8, QString> m_teamNames;
    QHash<quint64, int> m_lastHp;
    QHash<quint8, int> m_redCards;
    QHash<quint8, int> m_yellowCards;
    QString m_activeSource = QStringLiteral("field");
    QString m_activeSourceTitle = QStringLiteral("全场视角");
    int m_remainingSeconds = 60;
    int m_redScore = 0;
    int m_blueScore = 0;
    int m_onlineCount = 0;
    QString m_tickerText;
    bool m_tickerVisible = false;
    bool m_matchRunning = false;
    bool m_suddenDeath = false;
    bool m_roundEnded = false;
    bool m_settlementFinished = false;
    bool m_autoFinishOnTimeout = true;
    QString m_settlementType;
    QString m_refereeNotice;
    QJsonObject m_matchSummary;
    MatchSummaryPanel *m_summaryPanel = nullptr;
    QTimer *m_summaryRevealTimer = nullptr;
    QVariantAnimation *m_timerPulse = nullptr;
    QHash<QLabel *, QVariantAnimation *> m_scoreFlashes;
};

#endif // BROADCASTWINDOW_H
