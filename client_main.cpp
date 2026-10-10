#include "clientwindow.h"
#include "applogging.h"

#include <QApplication>
#include <QGuiApplication>
#include <QScreen>

namespace {
QScreen *screenFromEnvironment()
{
    const QByteArray requested = qgetenv("SCOMP_CLIENT_SCREEN").trimmed().toLower();
    const auto screens = QGuiApplication::screens();
    if (screens.isEmpty())
        return QGuiApplication::primaryScreen();

    if (requested == QByteArrayLiteral("secondary")
        || requested == QByteArrayLiteral("non-primary")) {
        const QScreen *primary = QGuiApplication::primaryScreen();
        for (QScreen *screen : screens) {
            if (screen != primary)
                return screen;
        }
    }

    bool ok = false;
    const int index = requested.toInt(&ok);
    if (ok && index >= 0 && index < screens.size())
        return screens.at(index);
    return QGuiApplication::primaryScreen();
}
} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--package-smoke")))
        return 0;
    applog::initialize(&app);
    ClientWindow window;
    // Production clients remain full-screen on the primary display. Automated
    // layout verification can opt into a non-primary display and a regular
    // requested-size window without disturbing the operator's main screen.
    const QByteArray testSize = qgetenv("SCOMP_CLIENT_SIZE").trimmed();
    const QByteArray testScreen = qgetenv("SCOMP_CLIENT_SCREEN").trimmed();
    if (!testSize.isEmpty()) {
        qputenv("SCOMP_BROADCAST_SIZE", testSize);
        window.showOnScreen(screenFromEnvironment());
        qunsetenv("SCOMP_BROADCAST_SIZE");
    } else if (!testScreen.isEmpty()) {
        window.showOnScreen(screenFromEnvironment());
    } else {
        window.showFullScreen();
    }
    return app.exec();
}
