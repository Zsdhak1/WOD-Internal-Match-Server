#include "videocanvas.h"

#include <QApplication>
#include <QGridLayout>
#include <QPainter>
#include <QWidget>
#include <QTimer>

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    QWidget host;
    host.resize(320, 180);
    auto *layout = new QGridLayout(&host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *placeholder = new QWidget(&host);
    layout->addWidget(placeholder, 0, 0);
    auto *canvas = new VideoCanvas(&host);
    layout->addWidget(canvas, 0, 0);
    auto *overlay = new QWidget(&host);
    overlay->setStyleSheet(QStringLiteral("background: #ff0000;"));
    overlay->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->addWidget(overlay, 0, 0);

    QImage frame(1280, 720, QImage::Format_ARGB32);
    frame.fill(Qt::darkBlue);
    QPainter painter(&frame);
    painter.setPen(Qt::white);
    painter.drawText(frame.rect(), Qt::AlignCenter, QStringLiteral("VideoCanvas"));
    painter.end();

    canvas->setFrame(frame);
    host.show();
    QTimer::singleShot(300, &application, [&application, &host] {
        const QImage image = host.grab().toImage();
        const QColor center = image.pixelColor(image.width() / 2, image.height() / 2);
        application.exit(center.red() > 200 && center.green() < 50 ? 0 : 4);
    });
    return application.exec();
}
