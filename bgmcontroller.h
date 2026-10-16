#ifndef BGMCONTROLLER_H
#define BGMCONTROLLER_H

#include <QObject>
#include <QString>

class QAudioOutput;
class QMediaPlayer;
class QSoundEffect;

// Server-side background music driven by the BO3 phase.
//
//   Prepare   : after new/reset BO3 until "start round", and every intermission (loops)
//   Match     : from the 5 s countdown start, once; its voice countdown is in the
//               first 5 s of the file, so playback must begin with the countdown
//   Interlude : after the BO3 is decided until new/reset (loops)
//
// The match track (3:00) is shorter than countdown + round, so during the last
// seconds of the round a beep plays once per second as a time warning.
class BgmController : public QObject
{
    Q_OBJECT
public:
    enum class Track { None, Prepare, Match, Interlude };
    Q_ENUM(Track)

    explicit BgmController(QObject *parent = nullptr);
    ~BgmController() override;

    // Directory holding bgm_*.m4a and countdown_beep.wav.
    static QString defaultAssetDir();
    void setAssetDir(const QString &dir);

    void setEnabled(bool enabled);
    bool enabled() const { return m_enabled; }
    void setVolume(int percent);           // 0..100
    int volume() const { return m_volume; }

    // Desired track for the current phase. Changing track restarts it from
    // the beginning; requesting the current track again is a no-op, so this
    // can be called on every UI refresh.
    void setTrack(Track track);
    Track track() const { return m_track; }
    // Restart the match track from 0 (a new countdown) even if it is current.
    void restartMatch();

    // Round time remaining, called on each referee tick while running. Beeps
    // once per whole second for the final kBeepSeconds seconds (5,4,3,2,1).
    void updateRoundRemainingMs(qint64 remainingMs);
    static constexpr int kBeepSeconds = 5;

    // Test hooks.
    QString currentSource() const;
    bool isPlaying() const;
    int beepCount() const { return m_beepCount; }
    static QString trackName(Track track);

signals:
    void trackChanged(BgmController::Track track);
    void beeped(int secondsLeft);

private:
    void applyTrack(bool allowMatchStart);
    void applyVolume();
    QString fileFor(Track track) const;

    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_output = nullptr;
    QSoundEffect *m_beep = nullptr;
    QString m_assetDir;
    Track m_track = Track::None;
    bool m_enabled = true;
    int m_volume = 70;
    int m_lastBeepSecond = -1;
    int m_beepCount = 0;
};

#endif // BGMCONTROLLER_H
