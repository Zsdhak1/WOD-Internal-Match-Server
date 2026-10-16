#include "matchtimingpresets.h"

#include <QCoreApplication>
#include <QSet>
#include <QSettings>

namespace matchtiming {

namespace {
// Same store as the other referee rules (QSettings is not copyable).
#define TIMING_SETTINGS(var) QSettings var(QSettings::defaultFormat(), QSettings::UserScope, \
    QStringLiteral("Scompetition"), QStringLiteral("MatchReferee"))
QString tr(const char *text) { return QCoreApplication::translate("MatchTimingPresets", text); }
}

QVector<MatchTimingPreset> defaultPresets()
{
    return {{tr("小组赛"), 120, 60}, {tr("淘汰赛"), 180, 120}};
}

bool isValid(const MatchTimingPreset &p, QString *error)
{
    const auto fail = [error](const QString &why) { if (error) *error = why; return false; };
    const QString name = p.name.trimmed();
    if (name.isEmpty()) return fail(tr("预设名称不能为空"));
    if (name.size() > kMaxNameLength) return fail(tr("预设名称最多 %1 个字").arg(kMaxNameLength));
    if (p.roundSeconds < kMinRoundSeconds || p.roundSeconds > kMaxRoundSeconds)
        return fail(tr("单局时长需在 %1 秒到 %2 分钟之间").arg(kMinRoundSeconds).arg(kMaxRoundSeconds / 60));
    if (p.intermissionSeconds < 0 || p.intermissionSeconds > kMaxIntermissionSeconds)
        return fail(tr("局间时长需在 0 到 %1 分钟之间").arg(kMaxIntermissionSeconds / 60));
    return true;
}

bool isValidList(const QVector<MatchTimingPreset> &presets, QString *error)
{
    if (presets.isEmpty()) { if (error) *error = tr("至少保留一个预设"); return false; }
    if (presets.size() > kMaxPresets) { if (error) *error = tr("最多 %1 个预设").arg(kMaxPresets); return false; }
    QSet<QString> names;
    for (const MatchTimingPreset &p : presets) {
        if (!isValid(p, error)) return false;
        const QString key = p.name.trimmed().toCaseFolded();
        if (names.contains(key)) { if (error) *error = tr("预设名称重复：%1").arg(p.name.trimmed()); return false; }
        names.insert(key);
    }
    return true;
}

QVector<MatchTimingPreset> load(QString *selectedName)
{
    TIMING_SETTINGS(settings);
    QVector<MatchTimingPreset> presets;
    const int count = settings.beginReadArray(QStringLiteral("timingPresets"));
    for (int i = 0; i < qMin(count, kMaxPresets); ++i) {
        settings.setArrayIndex(i);
        presets.append({settings.value(QStringLiteral("name")).toString().trimmed(),
                        settings.value(QStringLiteral("roundSeconds")).toInt(),
                        settings.value(QStringLiteral("intermissionSeconds"), -1).toInt()});
    }
    settings.endArray();
    if (!isValidList(presets)) presets = defaultPresets(); // missing or corrupted
    if (selectedName) {
        const QString wanted = settings.value(QStringLiteral("timingPresetSelected")).toString();
        *selectedName = presets.last().name; // knockout = previous fixed 3+2 behaviour
        for (const MatchTimingPreset &p : presets)
            if (p.name == wanted) { *selectedName = p.name; break; }
    }
    return presets;
}

bool save(const QVector<MatchTimingPreset> &presets, const QString &selectedName)
{
    if (!isValidList(presets)) return false;
    TIMING_SETTINGS(settings);
    settings.remove(QStringLiteral("timingPresets"));
    settings.beginWriteArray(QStringLiteral("timingPresets"), presets.size());
    for (int i = 0; i < presets.size(); ++i) {
        settings.setArrayIndex(i);
        settings.setValue(QStringLiteral("name"), presets.at(i).name.trimmed());
        settings.setValue(QStringLiteral("roundSeconds"), presets.at(i).roundSeconds);
        settings.setValue(QStringLiteral("intermissionSeconds"), presets.at(i).intermissionSeconds);
    }
    settings.endArray();
    settings.setValue(QStringLiteral("timingPresetSelected"), selectedName);
    settings.sync();
    return settings.status() == QSettings::NoError;
}

QString durationText(int seconds)
{
    if (seconds == 0) return tr("无");
    if (seconds % 60 == 0) return tr("%1 分钟").arg(seconds / 60);
    if (seconds < 60) return tr("%1 秒").arg(seconds);
    return tr("%1 分 %2 秒").arg(seconds / 60).arg(seconds % 60);
}

QString summary(const MatchTimingPreset &p)
{
    return tr("单局 %1 · 局间 %2").arg(durationText(p.roundSeconds), durationText(p.intermissionSeconds));
}

} // namespace matchtiming
