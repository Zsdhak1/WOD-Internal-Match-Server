#ifndef CHECKINPANEL_H
#define CHECKINPANEL_H

#include "checkinmodel.h"
#include "robotmanager.h"

#include <QWidget>

class QLabel;
class QPushButton;
class QPlainTextEdit;
class QTableWidget;

// Embeddable, strictly read-only inspector. Does not own or mutate RobotManager.
// No Q_OBJECT and no transport/match/camera dependency.
class CheckinPanel : public QWidget
{
public:
    explicit CheckinPanel(QWidget *parent = nullptr);
    void updateSnapshot(const QVector<RobotManager::RobotInfo> &robots,
                        const QVector<RobotManager::DeviceInfo> &devices,
                        qint64 observedAtMs = -1);
    void observeDeviceHealth(quint8 robotId, quint8 bitmap, const QString &addressText,
                             qint64 receivedAtMs = -1);
    void setInspectionEnabled(bool enabled);
    bool inspectionEnabled() const { return m_model.isEnabled(); }
    void startNewSession();
    const checkin::CheckinModel &stateModel() const { return m_model; }

private:
    void refreshAt(qint64 observedAtMs);
    void renderRows();
    void renderDetails();
    checkin::CheckinModel m_model;
    QVector<checkin::RobotSnapshot> m_robots;
    QVector<checkin::DeviceSnapshot> m_devices;
    QVector<checkin::Row> m_rows;
    QPushButton *m_modeButton = nullptr;
    QPushButton *m_sessionButton = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_summaryLabel = nullptr;
    QTableWidget *m_table = nullptr;
    QPlainTextEdit *m_details = nullptr;
};

#endif // CHECKINPANEL_H
