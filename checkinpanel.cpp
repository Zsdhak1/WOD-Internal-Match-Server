#include "checkinpanel.h"

#include <QAbstractItemView>
#include <QColor>
#include <QDateTime>
#include <QFont>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPalette>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace {
QString timestamp(qint64 value)
{
    return value > 0 ? QDateTime::fromMSecsSinceEpoch(value).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
                     : QStringLiteral("未观察 / 未上报");
}
QString numberOrUnknown(int value)
{
    return value >= 0 ? QString::number(value) : QStringLiteral("未知");
}
QString teamText(quint8 team)
{
    if (team == 1) return QStringLiteral("红方 (1)");
    if (team == 2) return QStringLiteral("蓝方 (2)");
    return team == 0 ? QStringLiteral("未分配") : QStringLiteral("队伍 %1").arg(team);
}
QString bitmap(quint8 value)
{
    return QStringLiteral("0x%1").arg(value, 2, 16, QLatin1Char('0')).toUpper();
}
QString moduleSummary(const checkin::Row &row)
{
    if (!row.healthFresh) return QStringLiteral("6模块未知 / 待确认");
    QStringList faults;
    for (int bit = 0; bit < 6; ++bit)
        if (row.modules[std::size_t(bit)] == checkin::Health::Fault)
            faults << checkin::CheckinModel::moduleName(bit);
    return faults.isEmpty() ? QStringLiteral("6/6在线（非验收通过）")
        : QStringLiteral("%1/6未在线：%2").arg(faults.size()).arg(faults.join(QStringLiteral("、")));
}
constexpr int kModuleColumn = 12;
}

CheckinPanel::CheckinPanel(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("checkinPanel"));
    setWindowTitle(tr("车辆检录（只读）"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumSize(260, 280);
    QFont readable = font();
    if (readable.pointSizeF() < 10.0) readable.setPointSizeF(10.0);
    setFont(readable);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    auto *title = new QLabel(tr("车辆检录 · 只读健康检查"), this);
    QFont titleFont = readable;
    titleFont.setBold(true);
    title->setFont(titleFont);
    title->setWordWrap(true);
    layout->addWidget(title);
    auto *notice = new QLabel(tr("不会发送比赛开始、罚牌或其他控制命令；不暂停比赛或摄像头。模块“在线”仅表示健康位，不等于硬件功能验收通过。"), this);
    notice->setWordWrap(true);
    layout->addWidget(notice);

    // Vertical actions avoid clipping even in a narrow right-side tab.
    m_modeButton = new QPushButton(this);
    m_modeButton->setObjectName(QStringLiteral("checkinModeButton"));
    layout->addWidget(m_modeButton);
    m_sessionButton = new QPushButton(tr("新检录会话（清空本会话首连）"), this);
    m_sessionButton->setObjectName(QStringLiteral("checkinNewSessionButton"));
    layout->addWidget(m_sessionButton);
    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    layout->addWidget(m_statusLabel);
    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setWordWrap(true);
    layout->addWidget(m_summaryLabel);

    auto *splitter = new QSplitter(Qt::Vertical, this);
    m_table = new QTableWidget(splitter);
    m_table->setObjectName(QStringLiteral("checkinTable"));
    QStringList headers {tr("Robot ID"), tr("队伍"), tr("协议"), tr("连接 / 注册"),
        tr("模块摘要（非验收结论）"), tr("服务端首个上行"), tr("本会话首连"),
        tr("最近上报"), tr("观察到离线"), tr("设备 MAC"), tr("地址 / 源端口"), tr("组件位图")};
    for (int bit = 0; bit < 6; ++bit) headers << checkin::CheckinModel::moduleName(bit);
    headers << tr("异常 / 待确认");
    m_table->setColumnCount(headers.size());
    m_table->setHorizontalHeaderLabels(headers);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);
    m_table->setWordWrap(false);
    m_table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_table->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_table->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->horizontalHeader()->setMinimumSectionSize(75);
    m_table->horizontalHeader()->setStretchLastSection(true);
    for (int column = 0; column < headers.size(); ++column)
        m_table->setColumnWidth(column, column >= 5 && column <= 8 ? 205 : 120);
    m_table->setColumnWidth(0, 80);
    m_table->setColumnWidth(1, 90);
    m_table->setColumnWidth(2, 75);
    m_table->setColumnWidth(3, 180);
    m_table->setColumnWidth(4, 270); // Module status stays visible before the wide history columns.
    m_table->setColumnWidth(9, 180);
    m_table->setColumnWidth(10, 170);
    m_table->setColumnWidth(11, 160);
    m_table->setColumnWidth(kModuleColumn + 6, 380);
    m_table->verticalHeader()->setDefaultSectionSize(36);
    m_table->verticalHeader()->hide();
    m_details = new QPlainTextEdit(splitter);
    m_details->setObjectName(QStringLiteral("checkinDetails"));
    m_details->setReadOnly(true);
    m_details->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);

    connect(m_modeButton, &QPushButton::clicked, this, [this] {
        setInspectionEnabled(!inspectionEnabled());
    });
    connect(m_sessionButton, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, tr("新检录会话"),
            tr("清空本面板的会话首连记录和健康证据，并开始新的只读检录会话？服务端首个上行记录、比赛、设备分配和摄像头不受影响。")) == QMessageBox::Yes)
            startNewSession();
    });
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this] { renderDetails(); });
    auto *timer = new QTimer(this);
    timer->setInterval(500);
    connect(timer, &QTimer::timeout, this, [this] {
        if (inspectionEnabled()) refreshAt(QDateTime::currentMSecsSinceEpoch());
    });
    timer->start(); // Only expires local evidence; never touches network or match timers.
    renderRows();
}

void CheckinPanel::updateSnapshot(const QVector<RobotManager::RobotInfo> &robots,
                                  const QVector<RobotManager::DeviceInfo> &devices,
                                  qint64 observedAtMs)
{
    m_robots.clear();
    m_robots.reserve(robots.size());
    for (const auto &robot : robots) {
        checkin::RobotSnapshot sample;
        sample.robotId = robot.robotId;
        sample.team = robot.team;
        sample.protocolVersion = robot.protocolVersion;
        sample.online = robot.online;
        sample.lastSeenMs = robot.lastSeenMs;
        sample.firstSeenMs = robot.firstSeenMs;
        sample.addressText = robot.addressText;
        sample.sourcePort = robot.sourcePort;
        sample.hp = robot.hp;
        sample.heat = robot.heat;
        sample.power = robot.power;
        sample.componentOnline = robot.componentOnline;
        m_robots.append(sample);
    }
    m_devices.clear();
    m_devices.reserve(devices.size());
    for (const auto &device : devices) {
        checkin::DeviceSnapshot sample;
        sample.deviceMac = device.deviceMac;
        sample.controllerMac = device.controllerMac;
        sample.robotId = device.robotId;
        sample.addressText = device.addressText;
        sample.sourcePort = device.sourcePort;
        sample.lastSeenMs = device.lastSeenMs;
        sample.firstSeenMs = device.firstSeenMs;
        sample.eventQueueDrops = device.eventQueueDrops;
        sample.udpSendFailures = device.udpSendFailures;
        m_devices.append(sample);
    }
    if (inspectionEnabled())
        refreshAt(observedAtMs < 0 ? QDateTime::currentMSecsSinceEpoch() : observedAtMs);
}

void CheckinPanel::observeDeviceHealth(quint8 robotId, quint8 value,
                                       const QString &addressText, qint64 receivedAtMs)
{
    m_model.observeDeviceHealth(robotId, value, addressText,
        receivedAtMs < 0 ? QDateTime::currentMSecsSinceEpoch() : receivedAtMs);
    // Render with the next consistent RobotManager snapshot, not an old bitmap.
}
void CheckinPanel::setInspectionEnabled(bool enabled)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    m_model.setEnabled(enabled, now);
    if (enabled) refreshAt(now);
    else renderRows();
}
void CheckinPanel::startNewSession()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    m_model.startNewSession(now);
    refreshAt(now);
}
void CheckinPanel::refreshAt(qint64 observedAtMs)
{
    m_model.updateSnapshot(m_robots, m_devices, observedAtMs);
    renderRows();
}

void CheckinPanel::renderRows()
{
    QString selectedKey;
    if (m_table->currentRow() >= 0 && m_table->item(m_table->currentRow(), 0))
        selectedKey = m_table->item(m_table->currentRow(), 0)->data(Qt::UserRole).toString();
    const int verticalScroll = m_table->verticalScrollBar()->value();
    const int horizontalScroll = m_table->horizontalScrollBar()->value();
    const QSignalBlocker blocker(m_table);
    m_rows = m_model.rows();
    m_table->setUpdatesEnabled(false);
    m_table->setRowCount(m_rows.size());
    int online = 0, abnormal = 0, unknown = 0, selectedIndex = -1;
    for (int index = 0; index < m_rows.size(); ++index) {
        const checkin::Row &row = m_rows[index];
        if (row.key == selectedKey) selectedIndex = index;
        if (row.connection == checkin::Connection::Online) ++online;
        if (!row.issues.isEmpty()) ++abnormal;
        if (!row.healthFresh) ++unknown;
        QStringList cells {row.robotId == 0 ? tr("未分配") : QString::number(row.robotId),
            teamText(row.team), QStringLiteral("V%1").arg(row.protocolVersion),
            checkin::CheckinModel::connectionText(row.connection), moduleSummary(row),
            timestamp(row.serverFirstSeenMs), timestamp(row.firstConnectedMs),
            timestamp(row.lastSeenMs), timestamp(row.offlineObservedMs),
            row.deviceMac.isEmpty() ? tr("未获注册 MAC") : row.deviceMac,
            row.addressText.isEmpty() ? tr("未知") : QStringLiteral("%1:%2").arg(row.addressText).arg(row.sourcePort),
            row.healthFresh ? bitmap(row.confirmedBitmap) : bitmap(row.rawBitmap) + tr("（未确认）")};
        for (const auto health : row.modules) cells << checkin::CheckinModel::healthText(health);
        cells << (row.issues + row.notes).join(QStringLiteral("；"));
        for (int column = 0; column < cells.size(); ++column) {
            auto *item = m_table->item(index, column);
            if (!item) {
                item = new QTableWidgetItem;
                m_table->setItem(index, column, item);
            }
            item->setText(cells[column]);
            item->setToolTip(cells[column]);
            item->setData(Qt::UserRole, row.key);
            item->setForeground(palette().brush(QPalette::Text));
            if (column == 4 && row.healthFresh) {
                item->setForeground(QColor((row.confirmedBitmap & 0x3fU) == 0x3fU
                    ? QStringLiteral("#137547") : QStringLiteral("#b42318")));
            }
            if (column >= kModuleColumn && column < kModuleColumn + 6) {
                const auto health = row.modules[std::size_t(column - kModuleColumn)];
                if (health == checkin::Health::Fault) item->setForeground(QColor(QStringLiteral("#b42318")));
                else if (health == checkin::Health::Healthy) item->setForeground(QColor(QStringLiteral("#137547")));
            }
        }
    }
    if (selectedIndex < 0 && !m_rows.isEmpty()) selectedIndex = 0;
    if (selectedIndex >= 0) m_table->selectRow(selectedIndex);
    m_table->verticalScrollBar()->setValue(verticalScroll);
    m_table->horizontalScrollBar()->setValue(horizontalScroll);
    m_table->setUpdatesEnabled(true);
    m_modeButton->setText(inspectionEnabled() ? tr("退出检录（保留本会话记录）") : tr("开启只读检录"));
    m_statusLabel->setText(m_model.sessionNumber() == 0 ? tr("未开启检录；首连记录尚未开始。")
        : tr("会话 #%1 · %2 · 开始 %3\n本会话首连 = 首次观察连接；退出后重开保留，新会话清空。服务端首个上行为独立记录。")
            .arg(m_model.sessionNumber()).arg(inspectionEnabled() ? tr("检录中") : tr("已退出 / 数据冻结"))
            .arg(timestamp(m_model.sessionStartedMs())));
    m_summaryLabel->setText(tr("记录 %1 · 遥测在线 %2 · 异常/诊断提示 %3 · 模块待确认 %4\n最近刷新 %5")
        .arg(m_rows.size()).arg(online).arg(abnormal).arg(unknown).arg(timestamp(m_model.lastUpdatedMs())));
    renderDetails();
}

void CheckinPanel::renderDetails()
{
    const int index = m_table->currentRow();
    if (index < 0 || index >= m_rows.size()) {
        m_details->setPlainText(tr("开启检录后选择车辆或设备查看详情。无数据不代表正常；V1 没有模块健康位图。"));
        return;
    }
    const auto &row = m_rows[index];
    QStringList lines {
        tr("%1 · %2 · V%3").arg(row.robotId == 0 ? tr("未分配设备") : tr("机器人 %1").arg(row.robotId))
            .arg(teamText(row.team)).arg(row.protocolVersion),
        tr("连接：%1%2").arg(checkin::CheckinModel::connectionText(row.connection))
            .arg(inspectionEnabled() ? QString() : tr("（已退出，非实时）")),
        tr("模块摘要：%1").arg(moduleSummary(row)),
        tr("服务端首次有效上行 / 注册：%1（本进程记录，非设备开机时间）").arg(timestamp(row.serverFirstSeenMs)),
        tr("本会话首次观察连接：%1").arg(timestamp(row.firstConnectedMs)),
        tr("最近上报：%1；最近设备注册：%2").arg(timestamp(row.lastSeenMs), timestamp(row.lastAnnounceMs)),
        tr("观察到离线：%1").arg(timestamp(row.offlineObservedMs)),
        tr("设备 MAC：%1；手柄 MAC：%2").arg(row.deviceMac.isEmpty() ? tr("未知") : row.deviceMac,
            row.controllerMac.isEmpty() ? tr("未知 / 未分配") : row.controllerMac),
        tr("遥测 HP=%1 / 热量=%2 / 功率=%3 W（未知和历史值不用于模块验收）")
            .arg(numberOrUnknown(row.hp), numberOrUnknown(row.heat), numberOrUnknown(row.power)),
        tr("健康帧最近接收：%1；有效性：%2").arg(timestamp(row.healthReceivedMs),
            row.healthFresh ? tr("当前有效") : tr("未确认 / 已过期 / 离线")),
        tr("原始快照位图：%1；bit6/7 保留，禁止推断为新模块。").arg(bitmap(row.rawBitmap))};
    for (int bit = 0; bit < 6; ++bit)
        lines << QStringLiteral("bit%1 %2：%3").arg(bit).arg(checkin::CheckinModel::moduleName(bit),
            checkin::CheckinModel::healthText(row.modules[std::size_t(bit)]));
    lines << tr("异常 / 诊断提示：") << (row.issues.isEmpty() ? tr("未检测到明确异常；不等于检录通过") : row.issues.join(QLatin1Char('\n')));
    lines << tr("待确认 / 说明：") << row.notes.join(QLatin1Char('\n'));
    const QString text = lines.join(QLatin1Char('\n'));
    if (m_details->toPlainText() != text) {
        const int scroll = m_details->verticalScrollBar()->value();
        m_details->setPlainText(text);
        m_details->verticalScrollBar()->setValue(scroll);
    }
}
