#include "matchtimingdialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTime>
#include <QTimeEdit>
#include <QVBoxLayout>

namespace {
int toSeconds(const QTime &t) { return t.minute() * 60 + t.second(); }
QTime fromSeconds(int s) { return QTime(0, 0).addSecs(qBound(0, s, 59 * 60 + 59)); }
}

MatchTimingDialog::MatchTimingDialog(const QVector<MatchTimingPreset> &presets,
                                     const QString &current, QWidget *parent)
    : QDialog(parent), m_presets(presets)
{
    setObjectName(QStringLiteral("matchTimingDialog"));
    setWindowTitle(tr("比赛时间预设"));
    setModal(true);

    auto *layout = new QVBoxLayout(this);
    auto *hint = new QLabel(tr("选择左侧预设进行编辑。修改在点“保存”后生效，正在进行的小局不受影响。"), this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto *body = new QHBoxLayout;
    auto *left = new QVBoxLayout;
    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("timingPresetList"));
    m_list->setMinimumWidth(200);
    left->addWidget(m_list, 1);
    auto *listButtons = new QHBoxLayout;
    auto *add = new QPushButton(tr("新增"), this);
    add->setObjectName(QStringLiteral("timingPresetAdd"));
    m_remove = new QPushButton(tr("删除"), this);
    m_remove->setObjectName(QStringLiteral("timingPresetRemove"));
    listButtons->addWidget(add);
    listButtons->addWidget(m_remove);
    left->addLayout(listButtons);
    auto *restore = new QPushButton(tr("恢复默认预设"), this);
    restore->setObjectName(QStringLiteral("timingPresetRestore"));
    left->addWidget(restore);
    body->addLayout(left);

    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_name = new QLineEdit(this);
    m_name->setObjectName(QStringLiteral("timingPresetName"));
    m_name->setMaxLength(matchtiming::kMaxNameLength);
    m_round = new QTimeEdit(this);
    m_round->setObjectName(QStringLiteral("timingPresetRound"));
    m_intermission = new QTimeEdit(this);
    m_intermission->setObjectName(QStringLiteral("timingPresetIntermission"));
    for (QTimeEdit *edit : {m_round, m_intermission}) {
        edit->setDisplayFormat(QStringLiteral("mm:ss"));
        edit->setCurrentSection(QDateTimeEdit::MinuteSection);
    }
    m_round->setMinimumTime(fromSeconds(matchtiming::kMinRoundSeconds));
    m_round->setMaximumTime(fromSeconds(matchtiming::kMaxRoundSeconds));
    m_intermission->setMaximumTime(fromSeconds(matchtiming::kMaxIntermissionSeconds));
    m_round->setToolTip(tr("每个小局的比赛时长（分:秒）"));
    m_intermission->setToolTip(tr("两个小局之间的准备/调试时间（分:秒），00:00 表示不等待"));
    form->addRow(tr("名称"), m_name);
    form->addRow(tr("单局时长"), m_round);
    form->addRow(tr("局间准备"), m_intermission);
    body->addLayout(form, 1);
    layout->addLayout(body, 1);

    m_error = new QLabel(this);
    m_error->setObjectName(QStringLiteral("timingPresetError"));
    m_error->setStyleSheet(QStringLiteral("color:#d9534f;"));
    m_error->setWordWrap(true);
    m_error->hide();
    layout->addWidget(m_error);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Save)->setText(tr("保存"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    layout->addWidget(buttons);

    connect(m_list, &QListWidget::currentRowChanged, this, &MatchTimingDialog::onRowChanged);
    connect(m_name, &QLineEdit::textEdited, this, &MatchTimingDialog::onFieldsEdited);
    connect(m_round, &QTimeEdit::timeChanged, this, &MatchTimingDialog::onFieldsEdited);
    connect(m_intermission, &QTimeEdit::timeChanged, this, &MatchTimingDialog::onFieldsEdited);
    connect(add, &QPushButton::clicked, this, &MatchTimingDialog::onAdd);
    connect(m_remove, &QPushButton::clicked, this, &MatchTimingDialog::onRemove);
    connect(restore, &QPushButton::clicked, this, &MatchTimingDialog::onRestoreDefaults);
    connect(buttons, &QDialogButtonBox::accepted, this, &MatchTimingDialog::onAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    int row = 0;
    for (int i = 0; i < m_presets.size(); ++i)
        if (m_presets.at(i).name == current) row = i;
    rebuildList(row);
    resize(560, 340);
}

QString MatchTimingDialog::currentName() const
{
    const int row = m_list->currentRow();
    return row >= 0 && row < m_presets.size() ? m_presets.at(row).name.trimmed() : QString();
}

void MatchTimingDialog::rebuildList(int selectRow)
{
    m_loading = true;
    m_list->clear();
    for (const MatchTimingPreset &p : m_presets)
        m_list->addItem(QStringLiteral("%1\n%2").arg(p.name.trimmed(), matchtiming::summary(p)));
    m_loading = false;
    m_remove->setEnabled(m_presets.size() > 1);
    m_list->setCurrentRow(qBound(0, selectRow, m_presets.size() - 1));
    loadFields(m_list->currentRow());
}

void MatchTimingDialog::loadFields(int row)
{
    if (row < 0 || row >= m_presets.size()) return;
    m_loading = true;
    m_name->setText(m_presets.at(row).name);
    m_round->setTime(fromSeconds(m_presets.at(row).roundSeconds));
    m_intermission->setTime(fromSeconds(m_presets.at(row).intermissionSeconds));
    m_loading = false;
}

MatchTimingPreset MatchTimingDialog::fieldsPreset() const
{
    return {m_name->text(), toSeconds(m_round->time()), toSeconds(m_intermission->time())};
}

void MatchTimingDialog::onRowChanged(int row)
{
    if (!m_loading) loadFields(row);
}

void MatchTimingDialog::onFieldsEdited()
{
    const int row = m_list->currentRow();
    if (m_loading || row < 0 || row >= m_presets.size()) return;
    m_presets[row] = fieldsPreset();
    if (QListWidgetItem *item = m_list->item(row))
        item->setText(QStringLiteral("%1\n%2").arg(m_presets.at(row).name.trimmed(),
                                                   matchtiming::summary(m_presets.at(row))));
    m_error->hide();
}

void MatchTimingDialog::onAdd()
{
    if (m_presets.size() >= matchtiming::kMaxPresets) {
        m_error->setText(tr("最多 %1 个预设").arg(matchtiming::kMaxPresets)); m_error->show(); return;
    }
    QString name = tr("新预设");
    for (int n = 2; ; ++n) {
        bool taken = false;
        for (const MatchTimingPreset &p : m_presets) taken = taken || p.name.trimmed() == name;
        if (!taken) break;
        name = tr("新预设 %1").arg(n);
    }
    const int row = m_list->currentRow();
    MatchTimingPreset copy = row >= 0 && row < m_presets.size() ? m_presets.at(row) : MatchTimingPreset{};
    copy.name = name;
    m_presets.append(copy);
    rebuildList(m_presets.size() - 1);
    m_name->setFocus();
    m_name->selectAll();
}

void MatchTimingDialog::onRemove()
{
    const int row = m_list->currentRow();
    if (m_presets.size() <= 1 || row < 0 || row >= m_presets.size()) return;
    m_presets.removeAt(row);
    rebuildList(qMin(row, m_presets.size() - 1));
}

void MatchTimingDialog::onRestoreDefaults()
{
    if (QMessageBox::question(this, tr("恢复默认预设"),
                              tr("将列表恢复为“小组赛”和“淘汰赛”两个默认预设，自定义预设会被删除。继续吗？"))
        != QMessageBox::Yes)
        return;
    m_presets = matchtiming::defaultPresets();
    rebuildList(m_presets.size() - 1);
    m_error->hide();
}

void MatchTimingDialog::onAccept()
{
    QString error;
    if (!matchtiming::isValidList(m_presets, &error)) {
        m_error->setText(error); m_error->show(); return;
    }
    for (MatchTimingPreset &p : m_presets) p.name = p.name.trimmed();
    accept();
}
