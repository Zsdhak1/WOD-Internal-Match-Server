#include "bgmcontroller.h"

#include <QAudioOutput>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMediaPlayer>
#include <QSoundEffect>
#include <QUrl>
#include <QtMath>

BgmController::BgmController(QObject *parent)
    : QObject(parent)
{
    m_player = new QMediaPlayer(this);
    m_output = new QAudioOutput(this);
    m_player->setAudioOutput(m_output);
    m_beep = new QSoundEffect(this);
    setAssetDir(defaultAssetDir());
    applyVolume();
}

BgmController::~BgmController()
{
    // Stop before Qt tears down the audio sink; avoids a late buffer callback.
    if (m_player) m_player->stop();
}

QString BgmController::defaultAssetDir()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("Assets/bgm"));
}

void BgmController::setAssetDir(const QString &dir)
{
    m_assetDir = dir;
    m_beep->setSource(QUrl::fromLocalFile(QDir(dir).filePath(QStringLiteral("countdown_beep.wav"))));
}

QString BgmController::trackName(Track track)
{
    switch (track) {
    case Track::Prepare: return QStringLiteral("prepare");
    case Track::Match: return QStringLiteral("match");
    case Track::Interlude: return QStringLiteral("interlude");
    case Track::None: break;
    }
    return QStringLiteral("none");
}

QString BgmController::fileFor(Track track) const
{
    if (track == Track::None) return {};
    return QDir(m_assetDir).filePath(QStringLiteral("bgm_%1.m4a").arg(trackName(track)));
}

void BgmController::setEnabled(bool enabled)
{
    if (enabled == m_enabled) return;
    m_enabled = enabled;
    // Re-enabling restarts the current phase's track from the beginning; the
    // match track would be out of sync with the countdown, so it stays silent
    // until the next countdown.
    applyTrack(/*allowMatchStart=*/false);
}

void BgmController::setVolume(int percent)
{
    m_volume = qBound(0, percent, 100);
    applyVolume();
}

void BgmController::applyVolume()
{
    // Perceptual (log-ish) curve: linear 50% sounds far louder than half.
    const float v = float(m_volume) / 100.0f;
    m_output->setVolume(v * v);
    m_beep->setVolume(qMin(1.0f, v * v * 1.4f));
}

void BgmController::setTrack(Track track)
{
    if (track == m_track) return;
    m_track = track;
    m_lastBeepSecond = -1;
    applyTrack(true);
    emit trackChanged(track);
}

void BgmController::restartMatch()
{
    m_track = Track::Match;
    m_lastBeepSecond = -1;
    applyTrack(true);
    emit trackChanged(m_track);
}

void BgmController::applyTrack(bool allowMatchStart)
{
    m_player->stop();
    const QString file = fileFor(m_track);
    if (file.isEmpty() || !QFileInfo::exists(file)) {
        m_player->setSource(QUrl());
        return;
    }
    m_player->setSource(QUrl::fromLocalFile(file));
    // Match plays once (looping would replay the voice countdown); the others loop.
    m_player->setLoops(m_track == Track::Match ? 1 : QMediaPlayer::Infinite);
    if (m_enabled && (m_track != Track::Match || allowMatchStart))
        m_player->play();
}

void BgmController::updateRoundRemainingMs(qint64 remainingMs)
{
    if (remainingMs <= 0 || remainingMs > qint64(kBeepSeconds) * 1000) return;
    // ceil: 4.2 s left shows "5" on the HUD clock, beep for 5.
    const int second = int((remainingMs + 999) / 1000);
    if (second == m_lastBeepSecond) return;
    m_lastBeepSecond = second;
    ++m_beepCount;
    if (m_enabled) m_beep->play();
    emit beeped(second);
}

QString BgmController::currentSource() const
{
    return m_player->source().toLocalFile();
}

bool BgmController::isPlaying() const
{
    return m_player->playbackState() == QMediaPlayer::PlayingState;
}
