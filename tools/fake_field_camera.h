#ifndef FAKE_FIELD_CAMERA_H
#define FAKE_FIELD_CAMERA_H
#include "fieldcamera.h"

class FakeFieldCameraBackend final : public FieldCameraBackend
{
public:
    QVector<FieldCameraDevice> available{{QByteArray("usb-A"), QStringLiteral("USB Camera")},
                                       {QByteArray("usb-B"), QStringLiteral("USB Camera")}};
    QByteArray opened;
    int starts = 0;
    int stops = 0;
    QVector<FieldCameraDevice> devices() const override { return available; }
    void start(const QByteArray &id) override { opened = id; ++starts; }
    void stop() override { opened.clear(); ++stops; }
    void push(const QImage &frame) { emit frameReady(frame); }
    void fail(const QString &message) { emit captureError(message); }
    void changeDevices(const QVector<FieldCameraDevice> &value) {
        available = value; emit devicesChanged();
    }
};
#endif
