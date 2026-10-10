#include "simulatorwindow.h"
#include "responsiveui.h"

#include <QApplication>
#include <QWindow>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    SimulatorWindow window;
    window.winId();
    if (qEnvironmentVariableIsSet("SCOMP_SIMULATOR_SCREEN")) {
        window.setAttribute(Qt::WA_ShowWithoutActivating);
        if (QScreen *screen = responsiveScreenFromEnvironment("SCOMP_SIMULATOR_SCREEN")) {
            if (window.windowHandle())
                window.windowHandle()->setScreen(screen);
            const QRect available = screen->availableGeometry();
            const QSize size = window.size();
            window.move(available.x() + qMax(0, (available.width() - size.width()) / 2),
                        available.y() + qMax(0, (available.height() - size.height()) / 2));
        }
    }
    window.show();
    return app.exec();
}
