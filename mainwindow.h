#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "matchreferee.h"
#include "matchserver.h"

#include <QElapsedTimer>
#include <QMainWindow>

class CheckinPanel;
class FieldCamera;
class FieldCameraBackend;
class QCheckBox;
class QGroupBox;
class QGridLayout;
class QHBoxLayout;
class QUdpSocket;
class RobotCommander;
class RobotManager;
class BroadcastWindow;
class MatchServer;
class QLabel;
class QPushButton;
class QPlainTextEdit;
class QComboBox;
class QLineEdit;
class QScreen;
class QResizeEvent;
class QSplitter;
class QSpinBox;
class QTableWidget;
class QTabWidget;
class QTimer;
class ResponsiveUiScaler;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr, FieldCameraBackend *cameraBackend = nullptr);
    ~MainWindow() override;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onReadyRead();
    void refreshTable();
    void onLogMessage(const QString &message);
    void onToggleListen();
    void onShowBroadcast();
    void onBroadcastScreenChanged(int index);
    void onToggleMatch();
    void onTerminateMatch();
    void onResetMatch();
    void onTestVictoryAnimation();
    // Automatic verdict
    void onTimeExpired();
    void onRefereeTick();
    void onDeclareRedWin();
    void onDeclareBlueWin();
    void onToggleSummary();
    void onRefereeRulesChanged();
    void onToggleClientServer();
    void refreshFieldCameraDevices();
    void onToggleFieldCamera();
    void onFieldCameraSelected();
    void onFieldCameraStateChanged();
    void onProgramModeChanged(int index);
    void onProgramSourceChanged(int index);
    void onAutoSwitchTimeout();
    void onTickerPresetChanged(int index);
    void onApplyTicker();
    void onToggleTicker();
    void onTeamNamesChanged();
    void onImportTeamTable();
    void onNextTeamPair();
    void onScoresChanged();
    void onAwardRedCard();
    void onAwardYellowCard();
    void onClearCards();
    void onVideoSourcesChanged(const QVector<MatchServer::VideoSourceInfo> &sources);
    void onSourceFrameUpdated(const QString &sourceId);
    // V2: 回 ACK / 处理 ESP32 ACK / 学习端点
    void onReliableEventNeedsAck(quint8 robotId, quint8 type, quint32 txid,
                                 const QHostAddress &addr, quint16 port);
    void onRobotAck(quint8 ackedType, quint32 txid, quint8 result, quint8 robotId);
    void onEndpointLearned(quint8 robotId, quint32 ipv4, quint16 port);
    // V2 设备管理
    void onAssignTeam();
    void onForcePowerOn();
    void onForcePowerOff();
    void onSetHp();
    void onRequestStatus();
    void onLayoutElementChanged(int index);
    void onLayoutPositionChanged();
    void onLayoutSizeChanged();
    void onResetLayout();
    void onSaveLayout();
    void onLoadLayout();

private:
    struct PendingAssignment {
        quint8 robotId = 0;
        quint8 team = 0;
        QString deviceMac;
    };
    QHash<quint32, PendingAssignment> m_pendingAssignments;
    void buildUi();
    void showActionResult(const QString &message, const QString &state = QStringLiteral("success"));
    void trackCommand(quint32 txid, const QString &action);
    void reportAction(const QString &message, const QString &state = QStringLiteral("success"));
    QHash<quint32, QString> m_actionCommands;
    int m_commandTotal = 0;
    int m_commandConfirmed = 0;
    int m_commandFailed = 0;
    bool m_collectingCommandBatch = false;
    void finishCommand(quint32 txid, const QString &detail, bool success);
    QLabel *m_cardTargetLabel = nullptr;
    void loadRefereeRules();
    void applyVerdict();
    void sendGameCommand(bool start);
    void refreshRefereeStatus();
    void syncRefereeState();
    qint64 refereeNow() const { return m_refereeClock.elapsed(); }
    void applyResponsiveUi();
    void startListen();
    void stopListen();
    void updateListenInfo();
    void populateScreens();
    void startClientServer();
    void stopClientServer();
    void publishMatchState();
    void updateProgramSourceList();
    void rebuildSourcePreviewStrip();
    void refreshSourcePreviewBadges();
    void refreshLayoutPositionEditors();
    void refreshDeviceList();
    bool importTeamTable(const QString &filePath, QString *errorMessage);
    void updateTeamPairControls();
    void applyTeamPair(int index, bool resetMatch);
    QString layoutFilePath() const;
    QScreen *selectedScreen() const;
    QString localIpv4List() const;

    QUdpSocket     *m_socket    = nullptr;
    RobotManager   *m_robots    = nullptr;
    RobotCommander *m_commander = nullptr;
    BroadcastWindow *m_broadcast = nullptr;
    MatchServer    *m_matchServer = nullptr;

    QLabel         *m_connLabel = nullptr;
    QLabel         *m_actionStatusLabel = nullptr;
    QTabWidget     *m_controlTabs = nullptr;
    QTabWidget     *m_leftTabs = nullptr;
    QTabWidget     *m_rightTabs = nullptr;
    QTabWidget     *m_compactTabs = nullptr;
    QLabel         *m_programMonitor = nullptr;
    QLabel         *m_matchOverview = nullptr;
    bool            m_dashboardCompact = false;
    QTableWidget   *m_table     = nullptr;
    QPlainTextEdit *m_log       = nullptr;
    QComboBox      *m_screenEdit = nullptr;
    QSpinBox       *m_portEdit  = nullptr;
    QPushButton    *m_listenBtn = nullptr;
    QPushButton    *m_broadcastBtn = nullptr;
    QPushButton    *m_matchBtn = nullptr;
    QPushButton    *m_terminateMatchBtn = nullptr;
    QPushButton    *m_resetMatchBtn = nullptr;
    QPushButton    *m_testVictoryAnimationBtn = nullptr;
    QComboBox      *m_settlementPreviewTypeEdit = nullptr;
    QSpinBox       *m_clientPortEdit = nullptr;
    QPushButton    *m_clientListenBtn = nullptr;
    QLabel         *m_clientStateLabel = nullptr;
    QLineEdit      *m_redTeamNameEdit = nullptr;
    QLineEdit      *m_blueTeamNameEdit = nullptr;
    QPushButton    *m_importTeamsBtn = nullptr;
    QPushButton    *m_nextTeamBtn = nullptr;
    QLabel         *m_teamPairLabel = nullptr;
    QSpinBox       *m_redScoreEdit = nullptr;
    QSpinBox       *m_blueScoreEdit = nullptr;
    QPushButton    *m_redCardBtn = nullptr;
    QPushButton    *m_yellowCardBtn = nullptr;
    QPushButton    *m_clearCardsBtn = nullptr;
    CheckinPanel   *m_checkinPanel = nullptr;
    FieldCamera    *m_fieldCamera = nullptr;
    QComboBox      *m_fieldCameraEdit = nullptr;
    QPushButton   *m_fieldCameraToggle = nullptr;
    QLabel        *m_fieldCameraStatus = nullptr;
    QComboBox      *m_programModeEdit = nullptr;
    QComboBox      *m_programSourceEdit = nullptr;
    QSpinBox       *m_autoSwitchIntervalEdit = nullptr;
    QComboBox      *m_tickerPresetEdit = nullptr;
    QLineEdit      *m_tickerTextEdit = nullptr;
    QPushButton    *m_tickerApplyBtn = nullptr;
    QPushButton    *m_tickerToggleBtn = nullptr;
    QComboBox      *m_layoutElementEdit = nullptr;
    QSpinBox       *m_layoutXEdit = nullptr;
    QSpinBox       *m_layoutYEdit = nullptr;
    QSpinBox       *m_layoutWidthEdit = nullptr;
    QSpinBox       *m_layoutHeightEdit = nullptr;
    QPushButton    *m_resetLayoutBtn = nullptr;
    QPushButton    *m_saveLayoutBtn = nullptr;
    QPushButton    *m_loadLayoutBtn = nullptr;
    QTimer         *m_autoSwitchTimer = nullptr;
    QSplitter      *m_workspace = nullptr;
    ResponsiveUiScaler *m_uiScaler = nullptr;
    qreal           m_controlUiScale = 1.0;
    QGroupBox      *m_sourcePreviewGroup = nullptr;
    QGridLayout    *m_sourcePreviewLayout = nullptr;
    // sourceId -> preview QLabel. Rebuilt whenever the source list changes.
    QHash<QString, QLabel *> m_sourcePreviewViews;
    // V2 设备管理
    QSpinBox       *m_deviceRobotEdit = nullptr;
    QComboBox      *m_deviceTeamEdit = nullptr;
    QSpinBox       *m_deviceHpEdit = nullptr;
    QComboBox      *m_deviceSelectEdit = nullptr;
    QLineEdit      *m_deviceControllerMacEdit = nullptr;
    QVector<MatchServer::VideoSourceInfo> m_videoSources;
    struct TeamPair {
        QString redName;
        QString blueName;
    };
    QVector<TeamPair> m_teamPairs;
    int             m_currentTeamPairIndex = -1;
    bool            m_listening = false;

    // Automatic verdict
    MatchReferee    m_referee;
    QElapsedTimer   m_refereeClock;
    QTimer         *m_refereeTimer = nullptr;
    QCheckBox      *m_redCardDisqualifiesEdit = nullptr;
    QSpinBox       *m_offlineGraceEdit = nullptr;
    QLabel         *m_refereeStatusLabel = nullptr;
    QPushButton    *m_declareRedBtn = nullptr;
    QPushButton    *m_declareBlueBtn = nullptr;
    QPushButton    *m_summaryToggleBtn = nullptr;
};

#endif // MAINWINDOW_H
