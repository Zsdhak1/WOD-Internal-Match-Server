#include "fake_field_camera.h"
#include <QCoreApplication>
#include <QDebug>
#include <QTimer>
#include <stdexcept>

static void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
static void fireRetry(FieldCamera &camera) {
    auto *timer = camera.findChild<QTimer *>(QStringLiteral("fieldCameraRetry"));
    check(timer && timer->isActive(), "missing scheduled retry");
    timer->stop();
    check(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection), "retry invoke failed");
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    try {
        auto *backend = new FakeFieldCameraBackend;
        FieldCamera camera(backend);
        int frames = 0, clears = 0;
        QObject::connect(&camera, &FieldCamera::frameReady, [&frames, &clears](const QImage &image) {
            if (image.isNull()) ++clears; else ++frames;
        });
        check(!camera.requested() && backend->starts == 0, "auto-start violates privacy");
        camera.start(); check(!camera.requested(), "empty device was opened");
        camera.selectDevice("usb-A"); camera.start();
        check(backend->opened == "usb-A" && !camera.live(), "start/first-frame state wrong");
        QImage frame(640, 480, QImage::Format_RGB32); frame.fill(Qt::green);
        backend->push(frame); check(camera.live() && frames == 1, "fallback resolution rejected");
        camera.selectDevice("usb-B");
        check(backend->opened == "usb-B" && !camera.live(), "same-name device switch wrong");
        backend->changeDevices({{"usb-A", QStringLiteral("USB Camera")}});
        check(camera.requested() && !camera.live() && backend->opened.isEmpty(), "unplug did not release");
        backend->push(frame); check(frames == 1, "late disconnected frame accepted");
        backend->changeDevices({{"usb-A", QStringLiteral("USB Camera")}, {"usb-B", QStringLiteral("USB Camera")}});
        fireRetry(camera); check(backend->opened == "usb-B", "reconnect opened wrong camera");
        backend->fail("occupied"); check(!camera.live() && camera.requested(), "error did not retry");
        for (int i = 0; i < 3; ++i) { fireRetry(camera); backend->fail("occupied"); }
        check(!camera.requested() && !camera.live(), "unbounded error retry");
        camera.start(); backend->push(frame); check(camera.live(), "manual restart failed");
        camera.stop(); const int starts = backend->starts;
        backend->changeDevices(backend->available); backend->push(frame);
        check(!camera.requested() && !camera.live() && backend->starts == starts, "stop restarted capture");
        check(clears >= 5, "stale frames not cleared");
        qInfo() << "PASS: camera privacy, stable IDs, first frame, fallback format, unplug/replug, errors, bounded retry and stop";
        return 0;
    } catch (const std::exception &error) { qCritical() << "FAIL:" << error.what(); return 1; }
}
