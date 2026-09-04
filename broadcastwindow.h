#ifndef BROADCASTWINDOW_H
#define BROADCASTWINDOW_H

#include "robotmanager.h"

#include <QMainWindow>
#include <QVector>

class QCloseEvent;
class QHideEvent;
class QLabel;
class QScreen;
class QShowEvent;

// 独立的赛事输出窗口。控制台负责控制它的显示器和可见性，状态数据由服务端模型同步进来。
class BroadcastWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit BroadcastWindow(QWidget *parent = nullptr);

    void showOnScreen(QScreen *screen);
    void updateRobots(const QVector<RobotManager::RobotInfo> &robots);

signals:
    void visibilityChanged(bool visible);

protected:
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    struct RobotCard {
        QLabel *name = nullptr;
        QLabel *hp = nullptr;
        QLabel *state = nullptr;
        QLabel *shoot = nullptr;
    };

    void buildUi();
    QWidget *createRobotCard(const QString &title, RobotCard *card);
    void updateCard(const RobotManager::RobotInfo *robot, RobotCard &card);
    static QString teamName(quint8 team);

    QHash<quint64, RobotCard> m_cards;
    QLabel *m_programView = nullptr;
    QLabel *m_leftPreview = nullptr;
    QLabel *m_rightPreview = nullptr;
    QLabel *m_matchState = nullptr;
};

#endif // BROADCASTWINDOW_H
