#ifndef MATCHTIMINGPRESETS_H
#define MATCHTIMINGPRESETS_H

#include <QString>
#include <QVector>

// Named formal-match timing presets (round length + intermission between
// rounds). Persisted in QSettings; the two built-in presets are always
// available and restored if the stored list is empty or invalid.
struct MatchTimingPreset {
    QString name;
    int roundSeconds = 180;
    int intermissionSeconds = 120;
    bool operator==(const MatchTimingPreset &o) const
    { return name == o.name && roundSeconds == o.roundSeconds && intermissionSeconds == o.intermissionSeconds; }
};

namespace matchtiming {
constexpr int kMinRoundSeconds = 10;
constexpr int kMaxRoundSeconds = 30 * 60;
constexpr int kMaxIntermissionSeconds = 30 * 60;
constexpr int kMaxPresets = 20;
constexpr int kMaxNameLength = 24;

QVector<MatchTimingPreset> defaultPresets(); // 小组赛 2+1 min, 淘汰赛 3+2 min
// Trimmed, length-limited, within range and unique (case-insensitive) names.
bool isValid(const MatchTimingPreset &preset, QString *error = nullptr);
bool isValidList(const QVector<MatchTimingPreset> &presets, QString *error = nullptr);
QVector<MatchTimingPreset> load(QString *selectedName = nullptr);
bool save(const QVector<MatchTimingPreset> &presets, const QString &selectedName);
QString summary(const MatchTimingPreset &preset); // "单局 3 分钟 · 局间 2 分钟"
QString durationText(int seconds);
}

#endif
