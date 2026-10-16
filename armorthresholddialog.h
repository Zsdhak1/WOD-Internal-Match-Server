#ifndef ARMORTHRESHOLDDIALOG_H
#define ARMORTHRESHOLDDIALOG_H

#include <QDialog>
#include <QList>

class QLabel;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTimer;
class RobotCommander;
class RobotManager;

// 装甲板动静双阈值调参（ESP32 f1c17e0 协议）。
// 读：0x0E 动静快照 + 0x0F 每个 NodeID 的 L431 阈值快照。
// 写：0x8A 单板单档，逐条等待 ACK；ACK=L431 已保存，APPLIED 以快照 applied_mask 为准。
class ArmorThresholdDialog : public QDialog
{
    Q_OBJECT

public:
    ArmorThresholdDialog(RobotManager *robots, RobotCommander *commander,
                         quint8 robotId, QWidget *parent = nullptr);

    // 由 MainWindow 转发 commandAcked / commandTimeout；返回是否属于本窗口。
    bool handleAck(quint32 txid, quint8 type, quint8 result);
    bool handleTimeout(quint32 txid, quint8 type);
    quint8 robotId() const { return m_robotId; }
    bool busy() const { return m_inFlight != 0 || !m_queue.isEmpty(); }

    struct Step { quint8 node = 0; quint8 profile = 0; quint32 value = 0; };
    // 计算把 node 从 (curStill,curMoving) 改到 (still,moving) 的安全顺序。
    static QList<Step> planNode(quint8 node, quint32 curStill, quint32 curMoving,
                                quint32 still, quint32 moving);

public slots:
    void refresh();

private slots:
    void onApplyRow(int node);
    void onApplyAll();
    void onRestoreDefaults();

private:
    void startPlan(const QList<Step> &plan, const QString &what);
    void sendNext();
    void finishPlan(bool ok, const QString &message);
    bool currentConfig(quint8 node, quint32 *still, quint32 *moving) const;
    void setBusyUi(bool busy);

    RobotManager *m_robots = nullptr;
    RobotCommander *m_commander = nullptr;
    quint8 m_robotId = 0;
    QLabel *m_motionLabel = nullptr;
    QLabel *m_statusLabel = nullptr;
    QTableWidget *m_table = nullptr;
    QSpinBox *m_stillEdit[4] = {};
    QSpinBox *m_movingEdit[4] = {};
    QPushButton *m_applyBtn[4] = {};
    QSpinBox *m_allStill = nullptr;
    QSpinBox *m_allMoving = nullptr;
    QPushButton *m_applyAllBtn = nullptr;
    QPushButton *m_defaultsBtn = nullptr;
    QTimer *m_refreshTimer = nullptr;
    bool m_editsSeeded[4] = {};

    QList<Step> m_queue;
    Step m_current;
    quint32 m_inFlight = 0;
    int m_planTotal = 0;
    QString m_planName;
};

#endif // ARMORTHRESHOLDDIALOG_H
