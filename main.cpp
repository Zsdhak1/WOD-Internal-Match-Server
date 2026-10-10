#include "mainwindow.h"
#include "applogging.h"
#include "responsiveui.h"

#include <QApplication>
#include <QWindow>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    // Dependency-only release smoke check: load QApplication/platform plugin,
    // but never open windows, bind match ports or modify operator settings.
    if (a.arguments().contains(QStringLiteral("--package-smoke")))
        return 0;
    applog::initialize(&a);
    MainWindow w;
    // Create the native handle before selecting a test screen. This makes the
    // screen assignment deterministic even before the first show event.
    w.winId();
    if (qEnvironmentVariableIsSet("SCOMP_MAIN_SCREEN")) {
        w.setAttribute(Qt::WA_ShowWithoutActivating);
        if (QScreen *screen = responsiveScreenFromEnvironment("SCOMP_MAIN_SCREEN")) {
            if (w.windowHandle())
                w.windowHandle()->setScreen(screen);
            const QRect available = screen->availableGeometry();
            const QSize size = w.size();
            w.move(available.x() + qMax(0, (available.width() - size.width()) / 2),
                   available.y() + qMax(0, (available.height() - size.height()) / 2));
        }
    }
    w.show();
    return a.exec();
}
