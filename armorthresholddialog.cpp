#include "armorthresholddialog.h"

#include "protocol.h"
#include "robotcommander.h"
#include "robotmanager.h"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace {
enum Col { ColNode, ColStill, ColMoving, ColApplied, ColState, ColStorage, ColSet, ColCount };

QString motionText(quint8 m)
{ return m == 1 ? QObject::tr("静止") : m == 2 ? QObject::tr("运动") : QObject::tr("未知"); }

QSpinBox *thresholdSpin(QWidget *parent, quint32 value)
{
    auto *s = new QSpinBox(parent);
    s->setRange(int(proto::kArmorThresholdMin), int(proto::kArmorThresholdMax));
    s->setSingleStep(10000);
    s->setGroupSeparatorShown(true);
    s->setValue(int(value));
    return s;
}
} // namespace

ArmorThresholdDialog::ArmorThresholdDialog(RobotManager *robots, RobotCommander *commander,
                                           quint8 robotId, QWidget *parent)
    : QDialog(parent), m_robots(robots), m_commander(commander), m_robotId(robotId)
{
    setObjectName(QStringLiteral("armorThresholdDialog"));
    setWindowTitle(tr("装甲板阈值 · 机器人 %1").arg(robotId));
    auto *layout = new QVBoxLayout(this);

    auto *hint = new QLabel(tr("车运动时振动可能误触发装甲板，所以静止和运动分别用两档阈值，"
                               "数值越大越不灵敏。要求静止 ≤ 运动，范围 %L1 – %L2；"
                               "动静状态未知时按运动阈值。配置保存在 L431，装甲板确认后显示「已生效」。")
                                .arg(proto::kArmorThresholdMin).arg(proto::kArmorThresholdMax), this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    m_motionLabel = new QLabel(this);
    m_motionLabel->setObjectName(QStringLiteral("armorMotion"));
    QFont bold = m_motionLabel->font(); bold.setBold(true); m_motionLabel->setFont(bold);
    layout->addWidget(m_motionLabel);

    m_table = new QTableWidget(4, ColCount, this);
    m_table->setObjectName(QStringLiteral("armorTable"));
    m_table->setHorizontalHeaderLabels({tr("装甲板"), tr("静止阈值"), tr("运动阈值"),
                                        tr("当前生效值"), tr("状态"), tr("保存"), QString()});
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    for (int i = 0; i < 4; ++i) {
        auto *node = new QTableWidgetItem(tr("板 %1").arg(i + 1));
        node->setTextAlignment(Qt::AlignCenter);
        m_table->setItem(i, ColNode, node);
        m_stillEdit[i] = thresholdSpin(m_table, proto::kArmorDefaultStill);
        m_stillEdit[i]->setObjectName(QStringLiteral("armorStill%1").arg(i + 1));
        m_movingEdit[i] = thresholdSpin(m_table, proto::kArmorDefaultMoving);
        m_movingEdit[i]->setObjectName(QStringLiteral("armorMoving%1").arg(i + 1));
        m_table->setCellWidget(i, ColStill, m_stillEdit[i]);
        m_table->setCellWidget(i, ColMoving, m_movingEdit[i]);
        for (int c : {ColApplied, ColState, ColStorage}) {
            auto *item = new QTableWidgetItem(QStringLiteral("-"));
            item->setTextAlignment(Qt::AlignCenter);
            m_table->setItem(i, c, item);
        }
        m_applyBtn[i] = new QPushButton(tr("应用"), m_table);
        m_applyBtn[i]->setObjectName(QStringLiteral("armorApply%1").arg(i + 1));
        connect(m_applyBtn[i], &QPushButton::clicked, this, [this, i] { onApplyRow(i + 1); });
        m_table->setCellWidget(i, ColSet, m_applyBtn[i]);
    }
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(ColNode, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(ColSet, QHeaderView::ResizeToContents);
    for (int i = 0; i < 4; ++i) m_table->setRowHeight(i, 40);
    const int tableHeight = m_table->horizontalHeader()->sizeHint().height() + 4 * 40 + 2 * m_table->frameWidth();
    m_table->setFixedHeight(tableHeight);
    m_table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(m_table);

    auto *allGroup = new QGroupBox(tr("四块板统一设置"), this);
    auto *allRow = new QHBoxLayout(allGroup);
    m_allStill = thresholdSpin(allGroup, proto::kArmorDefaultStill);
    m_allStill->setObjectName(QStringLiteral("armorAllStill"));
    m_allMoving = thresholdSpin(allGroup, proto::kArmorDefaultMoving);
    m_allMoving->setObjectName(QStringLiteral("armorAllMoving"));
    m_applyAllBtn = new QPushButton(tr("应用到 4 块板"), allGroup);
    m_applyAllBtn->setObjectName(QStringLiteral("armorApplyAll"));
    m_defaultsBtn = new QPushButton(tr("恢复默认（%L1 / %L2）")
                                        .arg(proto::kArmorDefaultStill).arg(proto::kArmorDefaultMoving), allGroup);
    m_defaultsBtn->setObjectName(QStringLiteral("armorRestoreDefaults"));
    allRow->addWidget(new QLabel(tr("静止"), allGroup));
    allRow->addWidget(m_allStill, 1);
    allRow->addWidget(new QLabel(tr("运动"), allGroup));
    allRow->addWidget(m_allMoving, 1);
    allRow->addWidget(m_applyAllBtn);
    allRow->addWidget(m_defaultsBtn);
    layout->addWidget(allGroup);
    connect(m_applyAllBtn, &QPushButton::clicked, this, &ArmorThresholdDialog::onApplyAll);
    connect(m_defaultsBtn, &QPushButton::clicked, this, &ArmorThresholdDialog::onRestoreDefaults);

    m_statusLabel = new QLabel(tr("就绪"), this);
    m_statusLabel->setObjectName(QStringLiteral("armorStatus"));
    m_statusLabel->setWordWrap(true);
    layout->addWidget(m_statusLabel);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setText(tr("关闭"));
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(250);
    connect(m_refreshTimer, &QTimer::timeout, this, &ArmorThresholdDialog::refresh);
    m_refreshTimer->start();
    resize(980, sizeHint().height());
    refresh();
}

QList<ArmorThresholdDialog::Step> ArmorThresholdDialog::planNode(quint8 node, quint32 curStill,
                                                                quint32 curMoving, quint32 still,
                                                                quint32 moving)
{
    // Keep still <= moving after every single write: raise moving first,
    // lower still first. Unchanged values are skipped.
    QList<Step> plan;
    const Step s{node, 0, still}, m{node, 1, moving};
    if (moving >= curMoving) {
        if (moving != curMoving) plan << m;
        if (still != curStill) plan << s;
    } else {
        if (still != curStill) plan << s;
        plan << m;
    }
    return plan;
}

bool ArmorThresholdDialog::currentConfig(quint8 node, quint32 *still, quint32 *moving) const
{
    if (!m_robots || node < 1 || node > 4) return false;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const RobotManager::RobotInfo &r : m_robots->robots()) {
        if (r.protocolVersion != proto::kVersion2 || r.robotId != m_robotId) continue;
        const RobotManager::RobotInfo::ArmorSlot &slot = r.armor[node - 1];
        if (!r.online || !RobotManager::armorSlotFresh(slot, now)) return false;
        *still = slot.still; *moving = slot.moving;
        return true;
    }
    return false;
}

void ArmorThresholdDialog::refresh()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const RobotManager::RobotInfo *robot = nullptr;
    const QVector<RobotManager::RobotInfo> bots = m_robots ? m_robots->robots()
                                                           : QVector<RobotManager::RobotInfo>{};
    for (const auto &r : bots)
        if (r.protocolVersion == proto::kVersion2 && r.robotId == m_robotId) robot = &r;

    if (!robot || !robot->online) {
        m_motionLabel->setText(tr("机器人 %1 离线，无法读取阈值").arg(m_robotId));
    } else {
        const quint8 esp = RobotManager::effectiveMotion(*robot, now);
        quint8 l431 = 0; qint64 newest = 0;
        for (const auto &slot : robot->armor)
            if (RobotManager::armorSlotFresh(slot, now) && slot.lastSeenMs > newest) {
                newest = slot.lastSeenMs; l431 = slot.l431Motion;
            }
        QString text = tr("手柄动静：%1").arg(motionText(esp));
        text += newest ? tr("  ·  L431 当前使用：%1阈值").arg(l431 == 1 ? tr("静止") : tr("运动"))
                       : tr("  ·  L431 阈值快照：未收到（检查 L431 连接）");
        if (newest && esp != 0 && esp != l431)
            text += tr("  ·  两者不一致，可能 ESP32 与 L431 串口断开");
        m_motionLabel->setText(text);
    }

    const QColor green(0, 150, 60), red(210, 40, 40), amber(190, 120, 0), gray(140, 140, 140);
    for (int i = 0; i < 4; ++i) {
        const RobotManager::RobotInfo::ArmorSlot *slot = robot ? &robot->armor[i] : nullptr;
        const bool fresh = slot && robot->online && RobotManager::armorSlotFresh(*slot, now);
        auto set = [&](int col, const QString &text, const QColor &color) {
            QTableWidgetItem *item = m_table->item(i, col);
            if (item->text() != text) item->setText(text);
            item->setForeground(color);
        };
        if (!fresh) {
            set(ColApplied, QStringLiteral("-"), gray);
            set(ColState, tr("未知"), gray);
            set(ColStorage, QStringLiteral("-"), gray);
            continue;
        }
        if (!m_editsSeeded[i] && !busy()) {
            m_stillEdit[i]->setValue(int(slot->still));
            m_movingEdit[i]->setValue(int(slot->moving));
            m_editsSeeded[i] = true;
        }
        set(ColApplied, slot->appliedValue ? QLocale().toString(slot->appliedValue) : tr("无"),
            palette().color(QPalette::Text));
        if (slot->applied) set(ColState, tr("已生效"), green);
        else if (slot->failed) set(ColState, tr("下发失败 · 重试中"), red);
        else set(ColState, tr("未生效 · 等待装甲板"), amber);
        set(ColStorage, slot->storageStatus == 1 ? tr("已保存")
                        : slot->storageStatus == 2 ? tr("保存失败，沿用旧值") : tr("默认值"),
            slot->storageStatus == 2 ? red : palette().color(QPalette::Text));
    }
}

void ArmorThresholdDialog::onApplyRow(int node)
{
    quint32 curStill = 0, curMoving = 0;
    if (!currentConfig(quint8(node), &curStill, &curMoving)) {
        finishPlan(false, tr("板 %1 没有新鲜的 L431 快照，不能确定修改顺序").arg(node));
        return;
    }
    const quint32 still = quint32(m_stillEdit[node - 1]->value());
    const quint32 moving = quint32(m_movingEdit[node - 1]->value());
    if (still > moving) { finishPlan(false, tr("板 %1：静止阈值必须 ≤ 运动阈值").arg(node)); return; }
    const QList<Step> plan = planNode(quint8(node), curStill, curMoving, still, moving);
    if (plan.isEmpty()) { finishPlan(true, tr("板 %1 已是目标值").arg(node)); return; }
    startPlan(plan, tr("板 %1").arg(node));
}

void ArmorThresholdDialog::onApplyAll()
{
    const quint32 still = quint32(m_allStill->value());
    const quint32 moving = quint32(m_allMoving->value());
    if (still > moving) { finishPlan(false, tr("静止阈值必须 ≤ 运动阈值")); return; }
    QList<Step> plan;
    for (quint8 node = 1; node <= 4; ++node) {
        quint32 curStill = 0, curMoving = 0;
        if (!currentConfig(node, &curStill, &curMoving)) {
            finishPlan(false, tr("板 %1 没有新鲜的 L431 快照，未下发任何修改").arg(node));
            return;
        }
        plan << planNode(node, curStill, curMoving, still, moving);
        m_stillEdit[node - 1]->setValue(int(still));
        m_movingEdit[node - 1]->setValue(int(moving));
    }
    if (plan.isEmpty()) { finishPlan(true, tr("4 块板已是目标值")); return; }
    startPlan(plan, tr("4 块板"));
}

void ArmorThresholdDialog::onRestoreDefaults()
{
    m_allStill->setValue(int(proto::kArmorDefaultStill));
    m_allMoving->setValue(int(proto::kArmorDefaultMoving));
    onApplyAll();
}

void ArmorThresholdDialog::startPlan(const QList<Step> &plan, const QString &what)
{
    if (busy()) return;
    m_queue = plan;
    m_planTotal = plan.size();
    m_planName = what;
    setBusyUi(true);
    sendNext();
}

void ArmorThresholdDialog::sendNext()
{
    if (m_queue.isEmpty()) {
        finishPlan(true, tr("%1 已保存到 L431（%2 项）；等待装甲板确认后显示「已生效」")
                             .arg(m_planName).arg(m_planTotal));
        return;
    }
    m_current = m_queue.takeFirst();
    m_inFlight = m_commander ? m_commander->sendArmorThreshold(m_robotId, m_current.node,
                                                               m_current.profile, m_current.value)
                             : 0;
    if (!m_inFlight) {
        finishPlan(false, tr("下发失败：机器人 %1 端点未知或网络未监听").arg(m_robotId));
        return;
    }
    m_statusLabel->setText(tr("下发中 %1/%2 · 板 %3 %4 = %L5 · 等待 L431 保存…")
                               .arg(m_planTotal - m_queue.size()).arg(m_planTotal)
                               .arg(m_current.node)
                               .arg(m_current.profile == 0 ? tr("静止") : tr("运动"))
                               .arg(m_current.value));
}

bool ArmorThresholdDialog::handleAck(quint32 txid, quint8 type, quint8 result)
{
    if (type != proto::TypeArmorThresholdSet || txid != m_inFlight) return false;
    m_inFlight = 0;
    if (result == 0) { sendNext(); return true; }
    const QString what = tr("板 %1 %2").arg(m_current.node)
                             .arg(m_current.profile == 0 ? tr("静止") : tr("运动"));
    finishPlan(false, result == 1
        ? tr("%1 被 L431 拒绝（参数越界或违反 静止 ≤ 运动）；后续项未下发").arg(what)
        : tr("%1 暂时失败（链路、队列或 Flash）；后续项未下发，可重试").arg(what));
    return true;
}

bool ArmorThresholdDialog::handleTimeout(quint32 txid, quint8 type)
{
    if (type != proto::TypeArmorThresholdSet || txid != m_inFlight) return false;
    m_inFlight = 0;
    finishPlan(false, tr("板 %1 未收到确认（3 次），保存结果未知；后续项未下发，请看快照核实")
                          .arg(m_current.node));
    return true;
}

void ArmorThresholdDialog::finishPlan(bool ok, const QString &message)
{
    m_queue.clear();
    m_inFlight = 0;
    setBusyUi(false);
    m_statusLabel->setText(message);
    m_statusLabel->setStyleSheet(ok ? QStringLiteral("color:#1f9d55;")
                                    : QStringLiteral("color:#d23c3c;"));
}

void ArmorThresholdDialog::setBusyUi(bool busy)
{
    if (busy) m_statusLabel->setStyleSheet(QString());
    for (int i = 0; i < 4; ++i) m_applyBtn[i]->setEnabled(!busy);
    m_applyAllBtn->setEnabled(!busy);
    m_defaultsBtn->setEnabled(!busy);
}
