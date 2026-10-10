#include "applogging.h"

#include <QCoreApplication>
#include <QDebug>

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("Scompetition"));
    application.setOrganizationName(QStringLiteral("Scompetition"));
    applog::initialize(&application);
    qInfo().noquote() << QStringLiteral("applog smoke test");
    return 0;
}
