#include "clientwindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    ClientWindow window;
    window.showFullScreen();
    return app.exec();
}
