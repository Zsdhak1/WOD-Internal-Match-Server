#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "matchserver.h"

#include <QMainWindow>

class QUdpSocket;
class RobotManager;
class BroadcastWindow;
class MatchServer;
class QLabel;
class QPushButton;
class QPlainTextEdit;
class QComboBox;
class QLineEdit;
class QScreen;
class QSpinBox;
class QTableWidget;
class QTimer;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void onReadyRead();
    void refreshTable();
    void onLogMessage(const QString &message);
    void onToggleListen();
    void onShowBroadcast();
    void onBroadcastScreenChanged(int index);
    void onToggleMatch();
    void onResetMatch();
    void onToggleClientServer();
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
    void onLayoutElementChanged(int index);
    void onLayoutPositionChanged();
    void onLayoutSizeChanged();
    void onResetLayout();
    void onSaveLayout();
    void onLoadLayout();

private:
    void buildUi();
    void startListen();
    void stopListen();
    void updateListenInfo();
    void populateScreens();
    void startClientServer();
    void stopClientServer();
    void publishMatchState();
    void updateProgramSourceList();
    void refreshLayoutPositionEditors();
    bool importTeamTable(const QString &filePath, QString *errorMessage);
    void updateTeamPairControls();
    void applyTeamPair(int index, bool resetMatch);
    QString layoutFilePath() const;
    QScreen *selectedScreen() const;
    QString localIpv4List() const;

    QUdpSocket     *m_socket    = nullptr;
    RobotManager   *m_robots    = nullptr;
    BroadcastWindow *m_broadcast = nullptr;
    MatchServer    *m_matchServer = nullptr;

    QLabel         *m_connLabel = nullptr;
    QTableWidget   *m_table     = nullptr;
    QPlainTextEdit *m_log       = nullptr;
    QComboBox      *m_screenEdit = nullptr;
    QSpinBox       *m_portEdit  = nullptr;
    QPushButton    *m_listenBtn = nullptr;
    QPushButton    *m_broadcastBtn = nullptr;
    QPushButton    *m_matchBtn = nullptr;
    QPushButton    *m_resetMatchBtn = nullptr;
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
    QVector<MatchServer::VideoSourceInfo> m_videoSources;
    struct TeamPair {
        QString redName;
        QString blueName;
    };
    QVector<TeamPair> m_teamPairs;
    int             m_currentTeamPairIndex = -1;
    bool            m_listening = false;
};

#endif // MAINWINDOW_H
