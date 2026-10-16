// Manual hardware probe (not a CTest): measures the largest GUI event-loop
// stall while the production FieldCamera enumerates, opens, streams and
// closes a real camera. Run with a camera attached:
//   CameraLatencyProbe.exe [seconds]
#include "fieldcamera.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QTimer>
#include <cstdio>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    const int seconds = argc > 1 ? atoi(argv[1]) : 6;
    QElapsedTimer loop; loop.start();
    qint64 maxGap = 0, last = 0; const char *phase = "construct";
    QTimer meter; meter.setInterval(5);
    QObject::connect(&meter, &QTimer::timeout, [&] {
        const qint64 now = loop.elapsed();
        if (now - last > maxGap) { maxGap = now - last; std::printf("  stall %lld ms during %s\n", maxGap, phase); }
        last = now;
    });
    meter.start();
    QElapsedTimer t; t.start();
    FieldCamera camera;
    std::printf("FieldCamera ctor: %lld ms\n", t.elapsed());
    int frames = 0; qint64 first = -1; QElapsedTimer open;
    QObject::connect(&camera, &FieldCamera::frameReady, [&](const QImage &f) {
        if (f.isNull()) return; if (first < 0) first = open.elapsed(); ++frames; });
    QObject::connect(&camera, &FieldCamera::devicesChanged, [&] {
        std::printf("devices ready after %lld ms: %lld\n", loop.elapsed(), qlonglong(camera.devices().size()));
        if (camera.devices().isEmpty() || camera.requested()) return;
        phase = "open"; last = loop.elapsed(); maxGap = 0;
        camera.selectDevice(camera.devices().first().id);
        t.start(); open.start(); camera.start();
        std::printf("FieldCamera::start(): %lld ms\n", t.elapsed());
        QTimer::singleShot(seconds * 1000, [&] {
            std::printf("first frame after %lld ms, %d frames delivered\n", first, frames);
            std::printf("largest GUI stall while opening/streaming: %lld ms\n", maxGap);
            phase = "stop"; maxGap = 0; last = loop.elapsed();
            t.start(); camera.stop(); std::printf("FieldCamera::stop(): %lld ms\n", t.elapsed());
            QTimer::singleShot(1500, [&] {
                std::printf("largest GUI stall during/after stop: %lld ms\n", maxGap); app.quit(); });
        });
    });
    QTimer::singleShot(15000, [&] { std::puts("timeout: no camera enumerated"); app.exit(1); });
    return app.exec();
}
