#ifndef CLIENTWINDOW_H
#define CLIENTWINDOW_H

#include "broadcastwindow.h"

#include <QColor>
#include <QPair>
#include <QVector>

class QCloseEvent;
class QComboBox;
class QJsonObject;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProcess;
class QPushButton;
class QTcpSocket;
class QTimer;
class QToolButton;
class QWidget;

// 选手端复用导播台全屏 HUD，只在同一窗口上叠加登记和视频源控制层。
class ClientWindow : public BroadcastWindow
{
    Q_OBJECT

public:
    explicit ClientWindow(QWidget *parent = nullptr);

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
    void buildRegistrationOverlay();
    void buildControlLayer();
    void showLoginPage();
    void showSessionPage(const QString &displayName, int team, int robotId);
    void sendMessage(const QJsonObject &message);
    void handleMessage(const QJsonObject &message);
    void handleRobotSnapshot(const QJsonObject &message);
    void handleMatchState(const QJsonObject &message);
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
    void consumeVideoOutput();
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
    QProcess *m_videoProcess = nullptr;
    QByteArray m_readBuffer;
    QByteArray m_videoBuffer;
    QImage m_lastVideoFrame;
    QVector<QPair<QString, QString>> m_videoDevices;
    bool m_registered = false;
    bool m_loginInProgress = false;
    int m_selectedTeam = 0;
    int m_selectedRobotId = 1;
    QString m_displayName;
    QString m_activeSourceId;
    QString m_activeSourceName;
    quint16 m_videoPort = 0;

    QWidget *m_registrationOverlay = nullptr;
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
};

#endif // CLIENTWINDOW_H
