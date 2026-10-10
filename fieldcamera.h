#ifndef FIELDCAMERA_H
#define FIELDCAMERA_H

#include <QByteArray>
#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QString>
#include <QVector>

class QTimer;

struct FieldCameraDevice {
    QByteArray id;
    QString name;
};

// Injectable capture boundary: production uses Qt Multimedia, tests never open
// a user's camera. Device IDs, not friendly names, are the identity.
class FieldCameraBackend : public QObject
{
    Q_OBJECT
public:
    explicit FieldCameraBackend(QObject *parent = nullptr) : QObject(parent) {}
    virtual QVector<FieldCameraDevice> devices() const = 0;
    virtual void start(const QByteArray &id) = 0;
    virtual void stop() = 0;
signals:
    void devicesChanged();
    void frameReady(const QImage &frame);
    void captureError(const QString &message);
};

class FieldCamera : public QObject
{
    Q_OBJECT
public:
    explicit FieldCamera(QObject *parent = nullptr);
    explicit FieldCamera(FieldCameraBackend *backend, QObject *parent = nullptr);
    ~FieldCamera() override;
    QVector<FieldCameraDevice> devices() const;
    QByteArray selectedDevice() const { return m_selected; }
    bool requested() const { return m_requested; }
    bool live() const { return m_live; }
    QString status() const { return m_status; }
    void selectDevice(const QByteArray &id);
    void start();
    void stop();
signals:
    void devicesChanged();
    void stateChanged();
    void frameReady(const QImage &frame); // null = clear stale signal
    void logMessage(const QString &message);
private slots:
    void refreshDevices();
    void attemptStart();
    void onFrame(const QImage &frame);
    void onError(const QString &message);
    void watchdog();
private:
    void closeCapture();
    void setStatus(const QString &message);
    void retry(const QString &reason);
    bool devicePresent() const;
    FieldCameraBackend *m_backend = nullptr;
    QTimer *m_retryTimer = nullptr;
    QTimer *m_watchdog = nullptr;
    QElapsedTimer m_frameClock;
    QElapsedTimer m_stableClock;
    QByteArray m_selected;
    QString m_status;
    bool m_requested = false;
    bool m_acceptFrames = false;
    bool m_live = false;
    int m_failures = 0;
};

#endif
