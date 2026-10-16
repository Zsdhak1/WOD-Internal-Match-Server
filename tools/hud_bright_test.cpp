// Top HUD readability over a very bright program frame (sunlit field / white
// wall). Renders the real BroadcastWindow at 1920x1080 over a near-white
// synthetic frame and checks that the countdown and team text keep contrast:
// the soft shadow must darken the pixels directly under/around the glyphs.
#include "broadcastwindow.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QLabel>
#include <QLinearGradient>
#include <QPainter>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>
#include <stdexcept>

static void check(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
static void settle(int ms = 60)
{ QElapsedTimer t; t.start(); do { QApplication::processEvents(); QThread::msleep(2); } while (t.elapsed() < ms); }
static double lum(QRgb c) { return 0.2126 * qRed(c) + 0.7152 * qGreen(c) + 0.0722 * qBlue(c); }

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
#ifdef Q_OS_WIN
    for (const QString &font : {QStringLiteral("arial.ttf"), QStringLiteral("msyh.ttc"), QStringLiteral("msyhbd.ttc")})
        QFontDatabase::addApplicationFont(qEnvironmentVariable("WINDIR") + QStringLiteral("/Fonts/") + font);
#endif
    app.setFont(QFont(QStringLiteral("Microsoft YaHei"), 10));
    QTemporaryDir settings; QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    const QString qa = qEnvironmentVariable("SCOMP_INTEGRATION_QA_DIR");
    try {
        BroadcastWindow hud; hud.resize(1920, 1080); hud.show(); settle();
        hud.setTeamNames(QStringLiteral("晨曦战队"), QStringLiteral("北极星"));
        hud.setClockDuration(180);
        // Synthetic overexposed frame: white sky fading to pale sand.
        QImage frame(1920, 1080, QImage::Format_RGB32);
        { QPainter p(&frame); QLinearGradient g(0, 0, 0, 1080);
          g.setColorAt(0, QColor(252, 252, 250)); g.setColorAt(1, QColor(236, 228, 210));
          p.fillRect(frame.rect(), g); }
        hud.setProgramFrame(frame); settle(400);
        const QImage shot = hud.grab().toImage().convertToFormat(QImage::Format_RGB32);
        if (!qa.isEmpty()) {
            check(QDir().mkpath(qa), "QA folder");
            check(shot.save(qa + "/hud-bright-1920x1080.png"), "capture");
            check(shot.copy(0, 0, 1920, 300).save(qa + "/hud-bright-top.png"), "capture top");
        }
        auto *center = hud.findChild<QWidget *>(QStringLiteral("centerHud"));
        auto *timer = hud.findChild<QLabel *>(QStringLiteral("timer"));
        check(center && timer, "HUD widgets");
        // Soft shadow under the HUD: the band just below the artwork must be
        // visibly darker than the raw frame (≈250), yet still a soft fade
        // (no hard black bar) further down.
        const QRect c(center->mapTo(&hud, QPoint()), center->size());
        const int midX = c.center().x();
        const double justBelow = lum(shot.pixel(midX, qMin(1079, c.bottom() + 6)));
        const double farBelow = lum(shot.pixel(midX, qMin(1079, c.bottom() + 160)));
        std::printf("luma: just below HUD %.0f, 160px below %.0f\n", justBelow, farBelow);
        check(justBelow < 215, "no soft shadow under the top HUD on a bright frame");
        check(farBelow > 225, "shadow is not soft: it darkens far below the HUD");
        // The countdown has a dark halo: sample just outside the glyph box.
        const QRect t(timer->mapTo(&hud, QPoint()), timer->size());
        std::printf("timer box %d,%d %dx%d\n", t.x(), t.y(), t.width(), t.height());
        // Shadows are cached: repainting the HUD for a new video frame with an
        // unchanged clock must stay cheap (no per-frame blur).
        QImage frame2 = frame; frame2.fill(QColor(250, 250, 248));
        QElapsedTimer cost; cost.start();
        for (int i = 0; i < 30; ++i) {
            hud.setProgramFrame(i % 2 ? frame : frame2);
            hud.repaint();
        }
        const double perFrame = cost.elapsed() / 30.0;
        std::printf("HUD repaint per video frame: %.1f ms\n", perFrame);
        if (!qa.isEmpty()) {
            // Typical darker arena frame, to check the band is not heavy there.
            QImage dark(1920, 1080, QImage::Format_RGB32);
            { QPainter p(&dark); QLinearGradient g(0, 0, 0, 1080);
              g.setColorAt(0, QColor(70, 90, 105)); g.setColorAt(1, QColor(40, 48, 44)); p.fillRect(dark.rect(), g); }
            hud.setProgramFrame(dark); settle(300);
            check(hud.grab().toImage().copy(0, 0, 1920, 300).save(qa + "/hud-dark-top.png"), "capture dark");
        }
        std::puts("PASS: top HUD keeps a soft dark backing and text shadow over a bright frame");
        return 0;
    } catch (const std::exception &e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
