// Native-resolution video: capture-format policy + real FFmpeg round trip.
// Encodes 1920x1080 frames like ClientWindow (bgra pipe -> x264 -> MPEG-TS/UDP),
// announces video_format over the real TCP protocol, and checks MatchServer
// emits full 1080p frames for the broadcast (not the former 640x360).
#include "matchserver.h"
#include "matchprotocol.h"
#include "videostreamsize.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHostAddress>
#include <QImage>
#include <QProcess>
#include <QTcpSocket>
#include <QThread>
#include <QUdpSocket>
#include <cstdio>
#include <functional>
#include <stdexcept>

static void check(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
static void settle(int ms = 20)
{ QElapsedTimer t; t.start(); do { QCoreApplication::processEvents(); QThread::msleep(2); } while (t.elapsed() < ms); }
static void wait(const std::function<bool()> &f, const char *why, int ms = 8000)
{ QElapsedTimer t; t.start(); while (!f() && t.elapsed() < ms) settle(5); check(f(), why); }

static void policy()
{
    using namespace videoformat;
    check(streamBitrateKbps({1280, 720}) == 4000 && streamBitrateKbps({1920, 1080}) == 8000
          && streamBitrateKbps({2560, 1440}) == 12000 && streamBitrateKbps({640, 480}) == 4000, "bitrate tiers");
    check(validStreamSize({1920, 1080}) && !validStreamSize({1921, 1080}) && !validStreamSize({7680, 4320})
          && !validStreamSize({80, 60}), "stream size validation");
    check(previewBound({3840, 2160}) == QSize(1280, 720) && previewBound({640, 480}) == QSize(640, 480)
          && previewBound({1920, 1200}) == QSize(1152, 720), "preview <=720p");
}

static QString ffmpeg()
{
    const QString dir = QCoreApplication::applicationDirPath();
    for (const QString &c : {dir + "/tools/ffmpeg/ffmpeg.exe", dir + "/ffmpeg.exe"})
        if (QFileInfo::exists(c)) return c;
    return {};
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    try {
        policy();
        const QString exe = ffmpeg();
        check(!exe.isEmpty(), "bundled ffmpeg missing");

        MatchServer server;
        check(server.start(0), "match server");
        QImage received; int frames = 0;
        QObject::connect(&server, &MatchServer::videoFrameReceived, &app,
                         [&](const QString &id, const QImage &f) { if (id == "cam-red") { received = f; ++frames; } });

        QTcpSocket client; client.connectToHost(QHostAddress::LocalHost, server.port());
        wait([&] { return client.state() == QAbstractSocket::ConnectedState; }, "tcp connect");
        client.write(matchproto::encode(matchproto::registrationRequest(1, 1, "red", "cam-red", "Red camera")));
        QByteArray reply;
        wait([&] { reply += client.readAll(); return reply.contains("registration_result"); }, "registration");
        check(reply.contains("\"ok\":true"), "registration rejected");
        const quint16 videoPort = server.videoPort(1);
        check(videoPort > 0, "video port");

        // Same encoder arguments as ClientWindow::startStreaming at 1080p.
        const QSize size(1920, 1080);
        const int kbps = videoformat::streamBitrateKbps(size);
        client.write(matchproto::encode({{"type", "video_format"}, {"width", size.width()},
                                         {"height", size.height()}, {"fps", 30}}));
        settle(150);
        QProcess enc;
        enc.start(exe, {"-hide_banner", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "bgra",
                        "-video_size", QString("%1x%2").arg(size.width()).arg(size.height()),
                        "-framerate", "30", "-i", "pipe:0", "-an", "-c:v", "libx264",
                        "-preset", "ultrafast", "-tune", "zerolatency", "-pix_fmt", "yuv420p",
                        "-b:v", QString("%1k").arg(kbps), "-maxrate", QString("%1k").arg(kbps),
                        "-bufsize", QString("%1k").arg(kbps * 2), "-g", "30", "-f", "mpegts",
                        QString("udp://127.0.0.1:%1?pkt_size=1316&buffer_size=1048576&connect=1").arg(videoPort)});
        check(enc.waitForStarted(3000), "encoder start");
        QImage frame(size, QImage::Format_ARGB32);
        QElapsedTimer t; t.start();
        int sent = 0;
        while (frames < 5 && t.elapsed() < 15000) {
            frame.fill(QColor::fromHsv((sent * 7) % 360, 200, 220));
            // A fine vertical stripe that would blur away at 640x360.
            for (int y = 0; y < size.height(); ++y) frame.setPixel(1001, y, qRgb(0, 0, 0));
            enc.write(reinterpret_cast<const char *>(frame.constBits()), frame.sizeInBytes());
            ++sent;
            settle(33);
        }
        enc.closeWriteChannel(); enc.waitForFinished(3000);
        check(frames >= 1, "no decoded frame");
        check(received.size() == size, "broadcast frame not at native 1080p");
        // The 1-pixel stripe survives only at native resolution.
        const QColor stripe = received.pixelColor(1001, 540), beside = received.pixelColor(1010, 540);
        check(stripe.value() + 60 < beside.value(), "native detail lost");

        // A 720p announcement restarts the decoder at the new size.
        client.write(matchproto::encode({{"type", "video_format"}, {"width", 1280}, {"height", 720}, {"fps", 30}}));
        settle(150);
        client.write(matchproto::encode({{"type", "video_format"}, {"width", 1281}, {"height", 720}}));
        reply.clear();
        wait([&] { reply += client.readAll(); return reply.contains("\"type\":\"error\""); }, "odd size accepted");
        server.stop();
        std::printf("PASS: native video policy, 1080p FFmpeg round trip (%d frames, %dx%d), size announce/validation\n",
                    frames, received.width(), received.height());
        std::fflush(stdout);
        return 0;
    } catch (const std::exception &e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
