#ifndef MATCHREFEREE_H
#define MATCHREFEREE_H

#include "robotmanager.h"

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QVector>

// 1v1 胜负自动判定。纯逻辑类，所有入口显式传入单调毫秒时间。
// 调用方周期性 tick()，返回 true 时读取 verdict() 结算（只报告一次）。
// 正式模式：180 秒小局由调用方倒计时，每局双方 3 次复活，配额由主流程唯一维护。
// 时间到按剩余复活数、实际 HP 判定；资源同值/未知或同时耗尽时冻结等待裁判。
// KillScore 保留旧击杀积分 / sudden death 作为调试兼容模式。处罚始终优先。
class MatchReferee
{
public:
    enum class Phase { Idle, Running, Paused, Finished, AwaitingDecision };

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
        SuddenDeathKill,
        RemainingRevives,
        RemainingHp,
        ReviveExhausted
    };

    struct Rules {
        enum class ScoringMode { KillScore, RemainingResources };
        bool redCardDisqualifies = true;
        int yellowCardLimit = 3;        // 0 = 黄牌不判负
        int offlineForfeitMs = 10000;   // 0 = 离线不判负（正式主流程设置 0）
        int knockoutConfirmMs = 300;    // 同时耗尽 / 击毁判定窗口
        ScoringMode scoringMode = ScoringMode::KillScore; // 保留旧聚合初始化顺序
    };
    using ScoringMode = Rules::ScoringMode;

    struct Verdict {
        bool valid = false;
        quint8 winner = 0;              // 0 = 平局 / 终止
        Reason reason = Reason::None;
        QString settlementType;         // redwin / bluewin / draw / termination
        QString endingMethod;           // time_decision / revive_exhausted / violation / safety_stop / normal
        QString reasonText;
        qint64 elapsedMs = 0;
    };

    struct TeamStats {
        int hp = -1;                   // -1 = 未知，不当作 0
        int maxHp = 0;
        int remainingRevives = 3;      // -1 = 未知，主流程是唯一配额权威
        bool revivePending = false;    // 批准执行在途，不允许提前判负
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
        int kills = 0;                  // 旧击杀统计（正式模式不用于胜负）
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
    // 每帧由配额权威更新；负值统一为未知 -1。等待裁决 / 结束后不再改资源快照。
    void setReviveState(quint8 team, int remaining, bool pending = false);
    void recordCombatEvent(quint8 team, quint8 robotId, quint8 type, qint64 nowMs);
    void recordPenalty(quint8 team, bool redCard, qint64 nowMs);

    // 立即产生裁决（允许 AwaitingDecision；判罚始终优先于自动规则）。
    void terminate(qint64 nowMs);
    void declareWinner(quint8 team, qint64 nowMs);
    // 正式：立即冻结资源并判定 / 等待裁判；调试：同分进入 sudden death。
    void timeExpired(qint64 nowMs);

    // 推进计时并检查自动规则。本次调用产生新裁决时返回 true（只返回一次）。
    bool tick(qint64 nowMs);

    Phase phase() const { return m_phase; }
    bool isRunning() const { return m_phase == Phase::Running; }
    bool awaitingDecision() const { return m_phase == Phase::AwaitingDecision; }
    bool suddenDeath() const { return m_suddenDeath; }
    void setScores(int red, int blue); // 裁判纠正旧击杀积分
    const Verdict &verdict() const { return m_verdict; }
    const TeamStats &team(quint8 team) const;
    int damageDealt(quint8 team) const;
    qint64 elapsedMs(qint64 nowMs) const;
    // 当前需要提示给观众/导播的状态，含明确待裁决原因。
    QString liveNotice() const;
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
    bool evaluateResources(qint64 nowMs);
    QString resourceUncertainty() const;
    void awaitDecision(qint64 nowMs, const QString &notice);
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
    qint64 m_redExhaustedSinceMs = -1;
    qint64 m_blueExhaustedSinceMs = -1;
    QString m_awaitingNotice;
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
