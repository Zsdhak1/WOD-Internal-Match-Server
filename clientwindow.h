#ifndef CLIENTWINDOW_H
#define CLIENTWINDOW_H

#include "broadcastwindow.h"
#include "playercombatoverlay.h"

#include <QColor>
#include <QPair>
#include <QSet>
#include <QVector>

class QCamera;
class QCameraDevice;
class QCameraFormat;
class QCloseEvent;
class QComboBox;
class QGridLayout;
class QJsonObject;
class QLabel;
class QLineEdit;
class QMediaCaptureSession;
class QPlainTextEdit;
class QProcess;
class QPushButton;
class QTcpSocket;
class QTimer;
class QToolButton;
class QVideoFrame;
class QVideoSink;
class QWidget;

// 选手端复用导播台全屏 HUD，只在同一窗口上叠加登记和视频源控制层。
class ClientWindow : public BroadcastWindow
{
    Q_OBJECT

public:
    explicit ClientWindow(QWidget *parent = nullptr);
    ~ClientWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onLoginClicked();
    void onSocketConnected();
    void onSocketReadyRead();
    void onSocketDisconnected();
    void onSocketError();
    void onLoginTimeout();
    void onLogoutClicked();
    void onRefreshVideoDevices();
    void onPreviewVideo();
    void onUseBuiltInCamera();
    void onSelectVideoDevice();
    void onTeamChanged(int index);
    void onRegistrationSourceChanged(int index);
    void onSessionSourceChanged(int index);
    void onToggleControls();

private:
    void buildUi();
    void applyClientUiScale();
    void layoutRegistrationTiles();
    void buildRegistrationOverlay();
    void buildControlLayer();
    void showLoginPage();
    void showSessionPage(const QString &displayName, int team, int robotId);
    void sendMessage(const QJsonObject &message);
    void handleMessage(const QJsonObject &message);
    void handleRobotSnapshot(const QJsonObject &message);
    void handleMatchState(const QJsonObject &message);
    bool acceptCompetitionSnapshot(const QJsonObject &message);
    void resetCompetitionOrdering();
    void resetCombatFeedback(bool resetRoundState = false);
    void updateCombatFeedback(const QVector<RobotManager::RobotInfo> &robots);
    void layoutCombatOverlay();
    void appendLog(const QString &message);
    void refreshVideoDevices();
    void populateVideoCombo(QComboBox *combo, const QString &selectedId = QString());
    int builtInCameraIndex(const QComboBox *combo) const;
    void updateAutoRobotIdentity();
    void updateSourceLabels(QComboBox *combo, QLabel *idLabel);
    void updateActiveRobotSource();
    void setConnectionStatus(const QString &message, const QColor &color = QColor());
    void setVideoStatus(const QString &message, const QColor &color = QColor());
    void startVideoPreview();
    void stopVideoPreview();
    void scheduleVideoRestart();
    void startStreaming(const QSize &frameSize);
    void feedStream(const QImage &image);
    void stopStreaming();
    void onCameraFrame(const QVideoFrame &frame);
    void showFramePreview(const QImage &frame);
    QString ffmpegExecutable() const;
    QString selectedSourceId(const QComboBox *combo) const;
    QString selectedSourceName(const QComboBox *combo) const;
    QString ownViewTitle() const;
    quint8 selectedRobotId() const;
    QVector<RobotManager::RobotInfo> robotInfosFromSnapshot(const QJsonObject &message) const;

    QTcpSocket *m_socket = nullptr;
    QTimer *m_loginTimeoutTimer = nullptr;
    QTimer *m_videoRestartTimer = nullptr;
    QCamera *m_camera = nullptr;
    QMediaCaptureSession *m_captureSession = nullptr;
    QVideoSink *m_videoSink = nullptr;
    QProcess *m_streamProcess = nullptr;
    QByteArray m_readBuffer;
    QImage m_lastVideoFrame;
    qint64 m_lastStreamDropLogMs = 0;
    qint64 m_lastFrameStatsMs = 0;
    quint64 m_streamFramesDropped = 0;
    quint64 m_framesSinceStats = 0;
    int m_streamInputFps = 30;
    QSize m_streamSize;   // native size the encoder was started with (even-aligned)
    qint64 m_streamRetryAtMs = 0; // throttle restarts when ffmpeg is missing/failed
    QVector<QPair<QString, QString>> m_videoDevices;
    bool m_registered = false;
    bool m_loginInProgress = false;
    qreal m_clientUiScale = 1.0;
    int m_selectedTeam = 0;
    int m_selectedRobotId = 1;
    QString m_displayName;
    QString m_activeSourceId;
    QString m_activeSourceName;
    quint16 m_videoPort = 0;

    PlayerCombatOverlay *m_combatOverlay = nullptr;
    playercombat::HpTracker m_ownHpTracker;
    playercombat::HpTracker m_opponentHpTracker;
    bool m_combatRoundRunning = false;
    int m_combatRemainingSeconds = -1;
    bool m_receivedCompetitionState = false;
    QString m_competitionMatchId;
    QString m_competitionRoundId;
    qint64 m_competitionRevision = -1;
    int m_competitionRoundIndex = 0;
    QSet<QString> m_retiredCompetitionMatches;
    QSet<QString> m_retiredCompetitionRounds;

    QWidget *m_registrationOverlay = nullptr;
    QGridLayout *m_registrationTiles = nullptr;
    QWidget *m_networkTile = nullptr;
    QWidget *m_identityTile = nullptr;
    QWidget *m_videoTile = nullptr;
    QPushButton *m_redTeamTile = nullptr;
    QPushButton *m_blueTeamTile = nullptr;
    bool m_registrationCompact = false;
    bool m_handlingClientResize = false;
    QWidget *m_controlLayer = nullptr;
    QWidget *m_controlPanel = nullptr;
    QToolButton *m_controlsButton = nullptr;

    QLineEdit *m_serverEdit = nullptr;
    QLineEdit *m_portEdit = nullptr;
    QComboBox *m_teamEdit = nullptr;
    QComboBox *m_registrationSourceEdit = nullptr;
    QLineEdit *m_displayNameEdit = nullptr;
    QLabel *m_registrationRobotLabel = nullptr;
    QLabel *m_registrationPreview = nullptr;
    QLabel *m_loginStatus = nullptr;
    QPushButton *m_loginButton = nullptr;

    QLabel *m_identityLabel = nullptr;
    QLabel *m_connectionLabel = nullptr;
    QComboBox *m_videoDeviceEdit = nullptr;
    QLabel *m_videoSourceIdLabel = nullptr;
    QLabel *m_videoPreview = nullptr;
    QLabel *m_videoStatus = nullptr;
    QPlainTextEdit *m_log = nullptr;
    bool m_remoteSummaryExpanded = false;
};

#endif // CLIENTWINDOW_H
