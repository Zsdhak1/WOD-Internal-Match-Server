#include "clientwindow.h"
#include "matchprotocol.h"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QGridLayout>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <cstdio>

static void settle(int ms = 150)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
}
#define VERIFY(condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); return 1; } } while (false)

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    // Qt's offscreen Windows platform does not enumerate installed fonts.
    // Load system fonts explicitly so preview text is real, not missing-glyph boxes.
    const QString fonts = qEnvironmentVariable("SystemRoot") + QStringLiteral("/Fonts/");
    QFontDatabase::addApplicationFont(fonts + QStringLiteral("msyh.ttc"));
    QFontDatabase::addApplicationFont(fonts + QStringLiteral("msyhbd.ttc"));
    QFontDatabase::addApplicationFont(fonts + QStringLiteral("segoeui.ttf"));
    ClientWindow window;
    auto *overlay = window.findChild<QWidget *>("registrationOverlay");
    auto *red = window.findChild<QPushButton *>("redTeamTile");
    auto *blue = window.findChild<QPushButton *>("blueTeamTile");
    auto *team = window.findChild<QComboBox *>("clientTeam");
    auto *host = window.findChild<QLineEdit *>("clientServerAddress");
    auto *port = window.findChild<QLineEdit *>("clientServerPort");
    auto *login = window.findChild<QPushButton *>("registerButton");
    auto *status = window.findChild<QLabel *>("registrationStatus");
    auto *video = window.findChild<QWidget *>("videoTile");
    auto *network = window.findChild<QWidget *>("networkTile");
    auto *identity = window.findChild<QWidget *>("identityTile");
    VERIFY(overlay && red && blue && team && host && port && login && status && video && network && identity);
    VERIFY(red->isChecked() && team->currentData().toInt() == 1);
    blue->click();
    VERIFY(blue->isChecked() && !red->isChecked() && team->currentData().toInt() == 2);
    team->setCurrentIndex(0);
    VERIFY(red->isChecked() && !blue->isChecked());
    blue->click();
    const QString output = qEnvironmentVariable("SCOMP_LOGIN_QA_DIR");
    if (!output.isEmpty()) VERIFY(QDir().mkpath(output));
    for (const QSize size : {QSize(1920,1080), QSize(1280,720), QSize(800,900), QSize(2560,1440)}) {
        window.resize(size);
        window.show();
        settle(300);
        VERIFY(overlay->isVisible() && host->isVisible() && login->isVisible());
        std::fprintf(stdout, "SIZE %dx%d host=%d team=%d network=%dx%d video=%dx%d\n", size.width(),size.height(),host->width(),blue->height(),network->width(),network->height(),video->width(),video->height());
        const QImage rendered = window.grab().toImage();
        // Verify the outer page really has a painted backdrop, not a stylesheet
        // flat-fill or the inherited HUD showing through behind transparent areas.
        VERIFY(rendered.pixelColor(5,5) != rendered.pixelColor(rendered.width()-6,rendered.height()-6));
        if (!output.isEmpty()) VERIFY(rendered.save(output + QStringLiteral("/metro-login-%1x%2.png").arg(size.width()).arg(size.height())));
        VERIFY(host->width() > 180 && blue->height() >= 70);
        VERIFY(network->geometry().right() < overlay->width());
        VERIFY(video->geometry().right() < overlay->width());
        if (size.width() < 900) VERIFY(video->geometry().top() > identity->geometry().bottom());
        else {
            VERIFY(video->geometry().left() > network->geometry().right());
            auto *card = window.findChild<QWidget *>("registrationCard");
            const qreal occupancy = qreal(card->width()) / overlay->width();
            std::fprintf(stdout,"CARD %dx%d overlay=%dx%d occupancy=%.3f\n",card->width(),card->height(),overlay->width(),overlay->height(),occupancy);
            VERIFY(occupancy >= 0.79 && occupancy <= 0.81);
            VERIFY(card->height() >= overlay->height() * 0.79);
        }
    }
    host->clear();
    login->click();
    VERIFY(login->isEnabled() && !status->text().isEmpty());
    QTcpServer server;
    VERIFY(server.listen(QHostAddress::LocalHost, 0));
    host->setText("127.0.0.1");
    port->setText(QString::number(server.serverPort()));
    login->click();
    VERIFY(!login->isEnabled());
    settle();
    VERIFY(server.hasPendingConnections());
    QTcpSocket *peer = server.nextPendingConnection();
    settle();
    const QJsonObject request = QJsonDocument::fromJson(peer->readAll().trimmed()).object();
    VERIFY(request.value("type").toString() == "register");
    VERIFY(request.value("team").toInt() == 2 && request.value("robotId").toInt() == 1);
    peer->write(matchproto::encode({{"type","registration_result"},{"ok",true},{"team",2},{"robotId",1},{"sourceId",""}}));
    settle();
    VERIFY(!overlay->isVisible());
    peer->disconnectFromHost();
    settle();
    VERIFY(overlay->isVisible() && login->isEnabled());
    window.close();
    std::puts("PASS: Metro layout four sizes, team synchronization, input validation, TCP registration, session transition and disconnect recovery.");
    return 0;
}
