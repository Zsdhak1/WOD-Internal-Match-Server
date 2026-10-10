#ifndef MATCHREFEREE_H
#define MATCHREFEREE_H

#include "robotmanager.h"

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QVector>

// 1v1 胜负自动判定。
//
// 这是一个不依赖 UI、网络和系统时钟的纯逻辑类：所有入口都显式传入单调毫秒
// 时间，便于在单元测试中精确复现“同时击毁”“离线宽限”等时序。调用方负责
// 周期性调用 tick()，并在其返回 true 时读取 verdict() 结算本局。
//
// 常规时间内击杀 +1，允许复活；时间到按本局击杀分判胜。
// 同分进入 sudden death，下一次单方击杀（经过同时击杀确认窗口）定胜负。
// 同时击杀双方各 +1，继续 sudden death。判罚/裁判/离线判负仍优先。
class MatchReferee
{
public:
    enum class Phase { Idle, Running, Paused, Finished };

    enum class Reason {
        None,
        Terminated,
        Manual,
        RedCard,
        YellowCardLimit,
        DoubleDisqualification,
        Knockout,
        DoubleKnockout,
        Forfeit,
        FewerDeaths,
        HpRatio,
        DamageDealt,
        FewerPenalties,
        TimeDraw,
        KillScore,
        SuddenDeathKill
    };

    struct Rules {
        bool redCardDisqualifies = true;
        int yellowCardLimit = 3;        // 0 = 黄牌不判负
        int offlineForfeitMs = 10000;   // 0 = 离线不判负
        int knockoutConfirmMs = 300;    // 同时击毁判定窗口
    };

    struct Verdict {
        bool valid = false;
        quint8 winner = 0;              // 0 = 平局 / 终止
        Reason reason = Reason::None;
        QString settlementType;         // redwin / bluewin / draw / termination
        QString reasonText;
        qint64 elapsedMs = 0;
    };

    struct TeamStats {
        int hp = -1;
        int maxHp = 0;
        bool online = false;
        bool alive = false;
        bool seenAlive = false;          // 本局内观察到过存活，才可能被“击毁”
        int damageTaken = 0;             // 不含黄牌扣血
        int penaltyHpLost = 0;
        int pendingPenaltyHp = 0;
        bool penaltyDeath = false;
        int hitsTaken = 0;
        int attacks = 0;
        int deaths = 0;
        int kills = 0;                  // 本局击杀积分（新局清零）
        int yellowCards = 0;
        int redCards = 0;
        qint64 deadSinceMs = -1;
        qint64 offlineRunMs = 0;         // 当前连续离线（仅计比赛进行时间）
        qint64 offlineTotalMs = 0;
    };

    static constexpr quint8 kRed = 1;
    static constexpr quint8 kBlue = 2;
    static constexpr int kYellowCardHpPenalty = 50;

    void setRules(const Rules &rules) { m_rules = rules; }
    const Rules &rules() const { return m_rules; }

    void reset();
    void beginRound(const QVector<RobotManager::RobotInfo> &robots, qint64 nowMs);
    void pause(qint64 nowMs);
    void resume(qint64 nowMs);

    void updateRobots(const QVector<RobotManager::RobotInfo> &robots, qint64 nowMs);
    void recordCombatEvent(quint8 team, quint8 robotId, quint8 type, qint64 nowMs);
    void recordPenalty(quint8 team, bool redCard, qint64 nowMs);

    // 立即产生裁决（优先于所有自动规则）。
    void terminate(qint64 nowMs);
    void declareWinner(quint8 team, qint64 nowMs);
    // 倒计时归零：确认边界击杀后比较击杀积分；同分进入 sudden death。
    void timeExpired(qint64 nowMs);

    // 推进计时并检查自动规则。本次调用产生新裁决时返回 true（只返回一次）。
    bool tick(qint64 nowMs);

    Phase phase() const { return m_phase; }
    bool isRunning() const { return m_phase == Phase::Running; }
    bool suddenDeath() const { return m_suddenDeath; }
    void setScores(int red, int blue); // 裁判纠正击杀积分
    const Verdict &verdict() const { return m_verdict; }
    const TeamStats &team(quint8 team) const;
    int damageDealt(quint8 team) const;
    qint64 elapsedMs(qint64 nowMs) const;
    // 当前需要提示给观众/导播的状态，例如“红方离线 3.4 / 10 秒”。
    QString liveNotice() const;
    // 赛后数据总结，供导播台和选手端面板渲染。
    QJsonObject summaryJson() const;

    static QString reasonKey(Reason reason);
    static QString sideName(quint8 team);

private:
    struct RobotTrack {
        quint8 team = 0;
        quint8 version = 0;
        int hp = -1;
        bool alive = false;
        bool online = false;
    };
    struct Event {
        qint64 tMs = 0;
        quint8 team = 0;
        QString kind;
        QString text;
    };
    struct Sample {
        qint64 tMs = 0;
        int redPermille = 0;
        int bluePermille = 0;
    };

    TeamStats &stats(quint8 team);
    static quint8 opponent(quint8 team) { return team == kRed ? kBlue : kRed; }
    static int maxHpFor(quint8 version);
    static quint32 robotKey(quint8 version, quint8 team, quint8 robotId);
    static bool isDead(const RobotTrack &robot);
    void applyRobots(const QVector<RobotManager::RobotInfo> &robots, qint64 nowMs,
                     bool countDamage);
    void recomputeTeams(qint64 nowMs);
    void markDeath(quint8 team, qint64 nowMs);
    void advanceClock(qint64 nowMs);
    void sampleTimeline(qint64 elapsed, bool force);
    int hpPermille(quint8 team) const;
    bool evaluate(qint64 nowMs, bool forceKnockout);
    void decide(quint8 winner, Reason reason, qint64 nowMs, const QString &detail = QString());
    void logEvent(qint64 nowMs, quint8 team, const QString &kind, const QString &text);

    Rules m_rules;
    Phase m_phase = Phase::Idle;
    Verdict m_verdict;
    bool m_verdictPending = false;
    bool m_suddenDeath = false;
    int m_suddenRedKills = 0;
    int m_suddenBlueKills = 0;
    qint64 m_suddenFirstKillMs = -1;
    qint64 m_regulationEndedMs = -1;
    qint64 m_lastScoringKillMs = -1;
    TeamStats m_red;
    TeamStats m_blue;
    QHash<quint32, RobotTrack> m_robots; // V1: version/team/id; V2: version/id.
    QVector<Event> m_events;
    QVector<Sample> m_timeline;
    qint64 m_elapsedBeforeResumeMs = 0;
    qint64 m_resumedAtMs = -1;
    qint64 m_lastClockMs = -1;
    qint64 m_nextSampleMs = 0;
};

#endif // MATCHREFEREE_H
