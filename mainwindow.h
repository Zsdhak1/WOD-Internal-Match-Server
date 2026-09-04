#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>

class QUdpSocket;
class RobotManager;
class BroadcastWindow;
class QLabel;
class QPushButton;
class QPlainTextEdit;
class QComboBox;
class QScreen;
class QSpinBox;
class QTableWidget;

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

private:
    void buildUi();
    void startListen();
    void stopListen();
    void updateListenInfo();
    void populateScreens();
    QScreen *selectedScreen() const;
    QString localIpv4List() const;

    QUdpSocket     *m_socket    = nullptr;
    RobotManager   *m_robots    = nullptr;
    BroadcastWindow *m_broadcast = nullptr;

    QLabel         *m_connLabel = nullptr;
    QTableWidget   *m_table     = nullptr;
    QPlainTextEdit *m_log       = nullptr;
    QComboBox      *m_screenEdit = nullptr;
    QSpinBox       *m_portEdit  = nullptr;
    QPushButton    *m_listenBtn = nullptr;
    QPushButton    *m_broadcastBtn = nullptr;
    bool            m_listening = false;
};

#endif // MAINWINDOW_H
