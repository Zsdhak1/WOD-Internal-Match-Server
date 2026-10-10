#include "matchreferee.h"

#include "protocol.h"

#include <QJsonArray>
#include <QSet>
#include <QStringList>

#include <algorithm>

namespace {
constexpr int kMaxTimelineSamples = 600;
constexpr int kMaxSummaryEvents = 40;

QJsonObject teamJson(const MatchReferee::TeamStats &team, int dealt, int permille)
{
    return {
        {QStringLiteral("hp"), team.hp},
        {QStringLiteral("maxHp"), team.maxHp},
        {QStringLiteral("hpPermille"), permille},
        {QStringLiteral("damageDealt"), dealt},
        {QStringLiteral("damageTaken"), team.damageTaken},
        {QStringLiteral("penaltyHp"), team.penaltyHpLost},
        {QStringLiteral("hits"), team.hitsTaken},
        {QStringLiteral("attacks"), team.attacks},
        {QStringLiteral("deaths"), team.deaths},
        {QStringLiteral("kills"), team.kills},
        {QStringLiteral("score"), team.kills},
        {QStringLiteral("yellowCards"), team.yellowCards},
        {QStringLiteral("redCards"), team.redCards},
        {QStringLiteral("offlineMs"), team.offlineTotalMs}
    };
}
} // namespace

void MatchReferee::reset()
{
    m_phase = Phase::Idle;
    m_verdict = Verdict();
    m_verdictPending = false;
    m_suddenDeath = false;
    m_suddenRedKills = m_suddenBlueKills = 0;
    m_suddenFirstKillMs = -1;
    m_regulationEndedMs = -1;
    m_lastScoringKillMs = -1;
    m_red = TeamStats();
    m_blue = TeamStats();
    m_robots.clear();
    m_events.clear();
    m_timeline.clear();
    m_elapsedBeforeResumeMs = 0;
    m_resumedAtMs = -1;
    m_lastClockMs = -1;
    m_nextSampleMs = 0;
}

void MatchReferee::beginRound(const QVector<RobotManager::RobotInfo> &robots, qint64 nowMs)
{
    reset();
    m_phase = Phase::Running;
    m_resumedAtMs = nowMs;
    m_lastClockMs = nowMs;
    applyRobots(robots, nowMs, false);
    recomputeTeams(nowMs);
    logEvent(nowMs, 0, QStringLiteral("start"), QStringLiteral("比赛开始"));
    sampleTimeline(0, true);
}

void MatchReferee::pause(qint64 nowMs)
{
    if (m_phase != Phase::Running)
        return;
    advanceClock(nowMs);
    m_elapsedBeforeResumeMs += qMax<qint64>(0, nowMs - m_resumedAtMs);
    m_resumedAtMs = -1;
    m_phase = Phase::Paused;
    logEvent(nowMs, 0, QStringLiteral("pause"), QStringLiteral("比赛暂停"));
}

void MatchReferee::resume(qint64 nowMs)
{
    if (m_phase != Phase::Paused)
        return;
    m_phase = Phase::Running;
    m_resumedAtMs = nowMs;
    m_lastClockMs = nowMs;
    logEvent(nowMs, 0, QStringLiteral("resume"), QStringLiteral("比赛继续"));
}

void MatchReferee::updateRobots(const QVector<RobotManager::RobotInfo> &robots, qint64 nowMs)
{
    if (m_phase == Phase::Idle || m_phase == Phase::Finished)
        return;
    advanceClock(nowMs);
    applyRobots(robots, nowMs, m_phase == Phase::Running);
    recomputeTeams(nowMs);
}

void MatchReferee::recordCombatEvent(quint8 team, quint8 robotId, quint8 type, qint64 nowMs)
{
    if (m_phase != Phase::Running)
        return;
    // Events omit protocol version. Resolve by supplied team/id; never guess
    // when opposite V1 teams or mixed versions share an ID.
    auto eventRobot = m_robots.end();
    for (auto it = m_robots.begin(); it != m_robots.end(); ++it) {
        if (quint8(it.key() & 0xffu) != robotId
            || ((team == kRed || team == kBlue) && it->team != team))
            continue;
        if (eventRobot != m_robots.end())
            return; // Ambiguous identity; a STATUS snapshot remains authoritative.
        eventRobot = it;
    }
    if (eventRobot == m_robots.end())
        return;
    team = eventRobot->team;
    if (team != kRed && team != kBlue)
        return;

    advanceClock(nowMs);
    switch (type) {
    case proto::TypeHit:
        ++stats(team).hitsTaken;
        break;
    case proto::TypeAttack:
        ++stats(team).attacks;
        break;
    case proto::TypeDeath: {
        // The reliable DEATH event can arrive before the next STATUS frame.
        if (eventRobot->online) {
            eventRobot->alive = false;
            recomputeTeams(nowMs);
        }
        break;
    }
    default:
        break;
    }
}

void MatchReferee::recordPenalty(quint8 team, bool redCard, qint64 nowMs)
{
    if ((team != kRed && team != kBlue)
        || (m_phase != Phase::Running && m_phase != Phase::Paused)) {
        return;
    }
    advanceClock(nowMs);
    TeamStats &s = stats(team);
    if (redCard) {
        ++s.redCards;
        logEvent(nowMs, team, QStringLiteral("red_card"),
                 QStringLiteral("%1 红牌").arg(sideName(team)));
    } else {
        ++s.yellowCards;
        // The robot deducts this HP itself; do not credit it to the opponent.
        s.pendingPenaltyHp += kYellowCardHpPenalty;
        if (s.hp > 0 && s.hp <= kYellowCardHpPenalty) s.penaltyDeath = true;
        logEvent(nowMs, team, QStringLiteral("yellow_card"),
                 QStringLiteral("%1 第 %2 张黄牌").arg(sideName(team)).arg(s.yellowCards));
    }
    evaluate(nowMs, false);
}

void MatchReferee::terminate(qint64 nowMs)
{
    if (m_phase == Phase::Finished)
        return;
    if (m_phase == Phase::Idle) {
        m_phase = Phase::Paused;
        m_elapsedBeforeResumeMs = 0;
    }
    advanceClock(nowMs);
    decide(0, Reason::Terminated, nowMs);
}

void MatchReferee::declareWinner(quint8 team, qint64 nowMs)
{
    if (m_phase == Phase::Finished || (team != kRed && team != kBlue && team != 0))
        return;
    if (m_phase == Phase::Idle)
        m_phase = Phase::Paused;
    advanceClock(nowMs);
    decide(team, Reason::Manual, nowMs);
}

void MatchReferee::timeExpired(qint64 nowMs)
{
    if ((m_phase != Phase::Running && m_phase != Phase::Paused)
        || m_suddenDeath || m_regulationEndedMs >= 0)
        return;
    advanceClock(nowMs);
    // Keep the simultaneous-kill confirmation window at the buzzer. Scores
    // arriving in that short window settle regulation, not a premature SD win.
    m_regulationEndedMs = elapsedMs(nowMs);
    evaluate(nowMs, false);
}

bool MatchReferee::tick(qint64 nowMs)
{
    if (m_phase == Phase::Running) {
        advanceClock(nowMs);
        sampleTimeline(elapsedMs(nowMs), false);
        evaluate(nowMs, false);
    }
    if (!m_verdictPending)
        return false;
    m_verdictPending = false;
    return true;
}

void MatchReferee::setScores(int red, int blue)
{
    m_red.kills = qMax(0, red);
    m_blue.kills = qMax(0, blue);
}

const MatchReferee::TeamStats &MatchReferee::team(quint8 team) const
{
    return team == kBlue ? m_blue : m_red;
}

MatchReferee::TeamStats &MatchReferee::stats(quint8 team)
{
    return team == kBlue ? m_blue : m_red;
}

int MatchReferee::damageDealt(quint8 team) const
{
    // 1v1: every non-penalty HP loss of one side was dealt by the other side.
    return this->team(opponent(team)).damageTaken;
}

qint64 MatchReferee::elapsedMs(qint64 nowMs) const
{
    if (m_phase == Phase::Finished)
        return m_verdict.elapsedMs;
    qint64 elapsed = m_elapsedBeforeResumeMs;
    if (m_phase == Phase::Running && m_resumedAtMs >= 0)
        elapsed += qMax<qint64>(0, nowMs - m_resumedAtMs);
    return elapsed;
}

QString MatchReferee::liveNotice() const
{
    if (m_phase == Phase::Finished)
        return m_verdict.reasonText;
    if (m_phase == Phase::Paused)
        return QStringLiteral("比赛暂停");
    if (m_phase != Phase::Running)
        return QString();

    QStringList parts;
    if (m_suddenDeath)
        parts << QStringLiteral("SUDDEN DEATH · 一命定胜负");
    else if (m_regulationEndedMs >= 0)
        parts << QStringLiteral("时间到 · 击杀积分确认中");
    for (const quint8 side : {kRed, kBlue}) {
        const TeamStats &s = team(side);
        if (!s.online) {
            if (m_rules.offlineForfeitMs > 0) {
                // Whole seconds: the notice is mirrored to clients and should
                // change at most once per second.
                const qint64 left = qMax<qint64>(0, m_rules.offlineForfeitMs - s.offlineRunMs);
                parts << QStringLiteral("%1离线，%2 秒后判负")
                             .arg(sideName(side))
                             .arg((left + 999) / 1000);
            } else {
                parts << QStringLiteral("%1离线").arg(sideName(side));
            }
        }
        if (s.deadSinceMs >= 0)
            parts << QStringLiteral("%1阵亡").arg(sideName(side));
    }
    return parts.join(QStringLiteral(" · "));
}

QJsonObject MatchReferee::summaryJson() const
{
    QJsonArray timeline;
    for (const Sample &sample : m_timeline)
        timeline.append(QJsonArray{double(sample.tMs), sample.redPermille, sample.bluePermille});

    QJsonArray events;
    const int first = qMax(0, int(m_events.size()) - kMaxSummaryEvents);
    for (int i = first; i < m_events.size(); ++i) {
        const Event &event = m_events.at(i);
        events.append(QJsonObject{
            {QStringLiteral("t"), double(event.tMs)},
            {QStringLiteral("team"), event.team},
            {QStringLiteral("kind"), event.kind},
            {QStringLiteral("text"), event.text}
        });
    }

    return {
        {QStringLiteral("valid"), m_verdict.valid},
        {QStringLiteral("suddenDeath"), m_suddenDeath},
        {QStringLiteral("winner"), m_verdict.winner},
        {QStringLiteral("reason"), reasonKey(m_verdict.reason)},
        {QStringLiteral("reasonText"), m_verdict.reasonText},
        {QStringLiteral("settlement"), m_verdict.settlementType},
        {QStringLiteral("durationMs"), double(m_verdict.valid ? m_verdict.elapsedMs : 0)},
        {QStringLiteral("red"), teamJson(m_red, damageDealt(kRed), hpPermille(kRed))},
        {QStringLiteral("blue"), teamJson(m_blue, damageDealt(kBlue), hpPermille(kBlue))},
        {QStringLiteral("timeline"), timeline},
        {QStringLiteral("events"), events}
    };
}

QString MatchReferee::reasonKey(Reason reason)
{
    switch (reason) {
    case Reason::Terminated: return QStringLiteral("terminated");
    case Reason::Manual: return QStringLiteral("manual");
    case Reason::RedCard: return QStringLiteral("red_card");
    case Reason::YellowCardLimit: return QStringLiteral("yellow_card_limit");
    case Reason::DoubleDisqualification: return QStringLiteral("double_disqualification");
    case Reason::Knockout: return QStringLiteral("knockout");
    case Reason::DoubleKnockout: return QStringLiteral("double_knockout");
    case Reason::Forfeit: return QStringLiteral("forfeit");
    case Reason::FewerDeaths: return QStringLiteral("fewer_deaths");
    case Reason::HpRatio: return QStringLiteral("hp_ratio");
    case Reason::DamageDealt: return QStringLiteral("damage_dealt");
    case Reason::FewerPenalties: return QStringLiteral("fewer_penalties");
    case Reason::TimeDraw: return QStringLiteral("time_draw");
    case Reason::KillScore: return QStringLiteral("kill_score");
    case Reason::SuddenDeathKill: return QStringLiteral("sudden_death_kill");
    case Reason::None: break;
    }
    return QString();
}

QString MatchReferee::sideName(quint8 team)
{
    return team == kRed ? QStringLiteral("红方")
         : team == kBlue ? QStringLiteral("蓝方")
                         : QString();
}

int MatchReferee::maxHpFor(quint8 version)
{
    return version == proto::kVersion1 ? 5000 : 300;
}

quint32 MatchReferee::robotKey(quint8 version, quint8 team, quint8 robotId)
{
    return (quint32(version) << 16)
           | (version == proto::kVersion1 ? quint32(team) << 8 : 0u)
           | robotId;
}

bool MatchReferee::isDead(const RobotTrack &robot)
{
    return !robot.alive || robot.hp == 0;
}

void MatchReferee::applyRobots(const QVector<RobotManager::RobotInfo> &robots,
                               qint64 nowMs, bool countDamage)
{
    Q_UNUSED(nowMs);
    QSet<quint32> seen;
    for (const RobotManager::RobotInfo &info : robots) {
        if (info.robotId == 0)
            continue;
        const quint32 key = robotKey(info.protocolVersion, info.team, info.robotId);
        seen.insert(key);
        auto it = m_robots.find(key);
        if (it == m_robots.end()) {
            RobotTrack track;
            track.team = info.team;
            track.version = info.protocolVersion;
            track.hp = info.hp;
            track.alive = info.alive;
            track.online = info.online;
            m_robots.insert(key, track);
            continue;
        }

        RobotTrack &track = it.value();
        const bool sameContext = track.team == info.team
                                 && track.version == info.protocolVersion;
        if (countDamage && sameContext && info.online
            && (info.team == kRed || info.team == kBlue)
            && track.hp >= 0 && info.hp >= 0 && info.hp < track.hp) {
            TeamStats &s = stats(info.team);
            const int delta = track.hp - info.hp;
            const int absorbed = qMin(delta, s.pendingPenaltyHp);
            s.pendingPenaltyHp -= absorbed;
            s.penaltyHpLost += absorbed;
            s.damageTaken += delta - absorbed;
        }
        track.team = info.team;
        track.version = info.protocolVersion;
        track.online = info.online;
        // An offline robot keeps its last reported state; stale snapshots
        // must not create deaths or revivals.
        if (info.online || !sameContext) {
            track.hp = info.hp;
            track.alive = info.alive;
        }
    }
    for (auto it = m_robots.begin(); it != m_robots.end();) {
        if (!seen.contains(it.key()))
            it = m_robots.erase(it);
        else
            ++it;
    }
}

void MatchReferee::recomputeTeams(qint64 nowMs)
{
    for (const quint8 side : {kRed, kBlue}) {
        bool any = false;
        bool online = false;
        bool alive = false;
        bool hpKnown = false;
        int hp = 0;
        int maxHp = 0;
        for (const RobotTrack &track : std::as_const(m_robots)) {
            if (track.team != side)
                continue;
            any = true;
            online = online || track.online;
            alive = alive || !isDead(track);
            const int robotMax = maxHpFor(track.version);
            maxHp += robotMax;
            if (track.hp >= 0) {
                hp += qMin(track.hp, robotMax);
                hpKnown = true;
            }
        }

        TeamStats &s = stats(side);
        const bool wasAlive = s.alive;
        const bool wasOnline = s.online;
        s.online = online;
        s.alive = any && alive;
        s.hp = hpKnown ? hp : -1;
        s.maxHp = maxHp;
        if (online)
            s.offlineRunMs = 0;

        if (m_phase != Phase::Running)
            continue;
        if (wasOnline && !online)
            logEvent(nowMs, side, QStringLiteral("offline"),
                     QStringLiteral("%1离线").arg(sideName(side)));
        else if (!wasOnline && online && s.seenAlive)
            logEvent(nowMs, side, QStringLiteral("online"),
                     QStringLiteral("%1恢复连接").arg(sideName(side)));

        // Removal/offline must not manufacture kills. Online transitions only.
        if (wasAlive && !s.alive && s.seenAlive && online) {
            markDeath(side, nowMs);
        } else if (!wasAlive && s.alive && s.deadSinceMs >= 0) {
            s.deadSinceMs = -1;
            s.penaltyDeath = false;
            logEvent(nowMs, side, QStringLiteral("revive"),
                     QStringLiteral("%1复活").arg(sideName(side)));
        }
        if (s.alive && online)
            s.seenAlive = true;
    }
}

void MatchReferee::markDeath(quint8 team, qint64 nowMs)
{
    TeamStats &s = stats(team);
    ++s.deaths;
    s.deadSinceMs = elapsedMs(nowMs);
    if (s.penaltyDeath || (m_rules.redCardDisqualifies && s.redCards > 0)
        || (m_rules.yellowCardLimit > 0 && s.yellowCards >= m_rules.yellowCardLimit)) {
        s.penaltyDeath = false;
        logEvent(nowMs, team, QStringLiteral("penalty_death"),
                 QStringLiteral("%1判罚阵亡，不计对方击杀分").arg(sideName(team)));
        return;
    }
    TeamStats &killer = stats(opponent(team));
    ++killer.kills;
    m_lastScoringKillMs = elapsedMs(nowMs);
    if (m_suddenDeath) {
        if (opponent(team) == kRed) ++m_suddenRedKills;
        else ++m_suddenBlueKills;
        if (m_suddenFirstKillMs < 0)
            m_suddenFirstKillMs = elapsedMs(nowMs);
    }
    logEvent(nowMs, team, QStringLiteral("death"),
             QStringLiteral("%1被击毁，%2 +1 分（%3 : %4）")
                 .arg(sideName(team), sideName(opponent(team))).arg(m_red.kills).arg(m_blue.kills));
}

void MatchReferee::advanceClock(qint64 nowMs)
{
    if (m_phase != Phase::Running) {
        m_lastClockMs = nowMs;
        return;
    }
    const qint64 dt = m_lastClockMs >= 0 ? qMax<qint64>(0, nowMs - m_lastClockMs) : 0;
    m_lastClockMs = nowMs;
    for (TeamStats *s : {&m_red, &m_blue}) {
        if (s->online) {
            s->offlineRunMs = 0;
        } else {
            s->offlineRunMs += dt;
            s->offlineTotalMs += dt;
        }
    }
}

void MatchReferee::sampleTimeline(qint64 elapsed, bool force)
{
    if (!force && elapsed < m_nextSampleMs)
        return;
    if (m_timeline.size() >= kMaxTimelineSamples)
        return;
    if (!m_timeline.isEmpty() && m_timeline.constLast().tMs == elapsed)
        m_timeline.removeLast();
    m_timeline.append({elapsed, hpPermille(kRed), hpPermille(kBlue)});
    m_nextSampleMs = (elapsed / 1000 + 1) * 1000;
}

int MatchReferee::hpPermille(quint8 side) const
{
    const TeamStats &s = team(side);
    if (s.hp < 0 || s.maxHp <= 0)
        return -1;
    return qBound(0, int(qint64(s.hp) * 1000 / s.maxHp), 1000);
}

bool MatchReferee::evaluate(qint64 nowMs, bool forceKnockout)
{
    if (m_phase == Phase::Idle || m_phase == Phase::Finished)
        return m_phase == Phase::Finished;

    const auto disqualified = [this](const TeamStats &s) {
        return (m_rules.redCardDisqualifies && s.redCards > 0)
               || (m_rules.yellowCardLimit > 0 && s.yellowCards >= m_rules.yellowCardLimit);
    };
    const bool redOut = disqualified(m_red);
    const bool blueOut = disqualified(m_blue);
    if (redOut && blueOut) {
        decide(0, Reason::DoubleDisqualification, nowMs);
        return true;
    }
    if (redOut || blueOut) {
        const quint8 loser = redOut ? kRed : kBlue;
        const TeamStats &s = team(loser);
        decide(opponent(loser),
               m_rules.redCardDisqualifies && s.redCards > 0 ? Reason::RedCard
                                                             : Reason::YellowCardLimit,
               nowMs);
        return true;
    }

    if (m_phase != Phase::Running && !forceKnockout)
        return false;

    if (m_rules.offlineForfeitMs > 0) {
        const bool redGone = !m_red.online && m_red.offlineRunMs >= m_rules.offlineForfeitMs;
        const bool blueGone = !m_blue.online && m_blue.offlineRunMs >= m_rules.offlineForfeitMs;
        if (redGone != blueGone) {
            const quint8 loser = redGone ? kRed : kBlue;
            if (team(opponent(loser)).online) {
                decide(opponent(loser), Reason::Forfeit, nowMs);
                return true;
            }
        }
    }

    const qint64 elapsed = elapsedMs(nowMs);
    if (m_suddenDeath && m_suddenFirstKillMs >= 0
        && elapsed - m_suddenFirstKillMs >= m_rules.knockoutConfirmMs) {
        if (m_suddenRedKills != m_suddenBlueKills) {
            decide(m_suddenRedKills > m_suddenBlueKills ? kRed : kBlue,
                   Reason::SuddenDeathKill, nowMs);
            return true;
        }
        // Mutual elimination is not a winner: retain both points and wait for
        // the next single-sided kill after hardware revival.
        m_suddenRedKills = m_suddenBlueKills = 0;
        m_suddenFirstKillMs = -1;
        logEvent(nowMs, 0, QStringLiteral("sudden_death_tie"),
                 QStringLiteral("双方同时击杀，继续 SUDDEN DEATH"));
    }

    if (!m_suddenDeath && m_regulationEndedMs >= 0) {
        const bool pendingKill = m_lastScoringKillMs >= 0
                                 && elapsed - m_lastScoringKillMs < m_rules.knockoutConfirmMs;
        if (!pendingKill) {
            if (m_red.kills != m_blue.kills) {
                decide(m_red.kills > m_blue.kills ? kRed : kBlue, Reason::KillScore, nowMs);
                return true;
            }
            m_suddenDeath = true;
            m_suddenRedKills = m_suddenBlueKills = 0;
            m_suddenFirstKillMs = -1;
            logEvent(nowMs, 0, QStringLiteral("sudden_death"),
                     QStringLiteral("常规时间积分相同，进入 SUDDEN DEATH · 一命定胜负"));
        }
    }

    return false;
}

void MatchReferee::decide(quint8 winner, Reason reason, qint64 nowMs, const QString &detail)
{
    if (m_phase == Phase::Finished)
        return;

    const qint64 elapsed = elapsedMs(nowMs);
    const quint8 loser = winner == 0 ? 0 : opponent(winner);
    QString text;
    switch (reason) {
    case Reason::Terminated:
        text = QStringLiteral("裁判终止本局");
        break;
    case Reason::Manual:
        text = winner == 0 ? QStringLiteral("裁判判定平局")
                           : QStringLiteral("裁判判定%1获胜").arg(sideName(winner));
        break;
    case Reason::RedCard:
        text = QStringLiteral("%1红牌判负").arg(sideName(loser));
        break;
    case Reason::YellowCardLimit:
        text = QStringLiteral("%1累计 %2 张黄牌判负")
                   .arg(sideName(loser)).arg(team(loser).yellowCards);
        break;
    case Reason::DoubleDisqualification:
        text = QStringLiteral("双方同时判负，本局平局");
        break;
    case Reason::Knockout:
        text = QStringLiteral("%1击毁%2").arg(sideName(winner), sideName(loser));
        break;
    case Reason::DoubleKnockout:
        text = QStringLiteral("双方同时被击毁，本局平局");
        break;
    case Reason::Forfeit:
        text = QStringLiteral("%1离线超过 %2 秒判负")
                   .arg(sideName(loser)).arg(m_rules.offlineForfeitMs / 1000.0, 0, 'g', 3);
        break;
    case Reason::FewerDeaths:
        text = QStringLiteral("时间到，%1阵亡次数更少（%2 : %3）")
                   .arg(sideName(winner)).arg(m_red.deaths).arg(m_blue.deaths);
        break;
    case Reason::HpRatio:
        text = QStringLiteral("时间到，%1剩余血量更高（%2% : %3%）")
                   .arg(sideName(winner))
                   .arg(qMax(0, hpPermille(kRed)) / 10.0, 0, 'f', 1)
                   .arg(qMax(0, hpPermille(kBlue)) / 10.0, 0, 'f', 1);
        break;
    case Reason::DamageDealt:
        text = QStringLiteral("时间到，%1造成伤害更多（%2 : %3）")
                   .arg(sideName(winner)).arg(damageDealt(kRed)).arg(damageDealt(kBlue));
        break;
    case Reason::FewerPenalties:
        text = QStringLiteral("时间到，%1判罚更少").arg(sideName(winner));
        break;
    case Reason::TimeDraw:
        text = QStringLiteral("时间到，双方数据相同，本局平局");
        break;
    case Reason::KillScore:
        text = QStringLiteral("时间到，%1击杀积分更高（%2 : %3）")
                   .arg(sideName(winner)).arg(m_red.kills).arg(m_blue.kills);
        break;
    case Reason::SuddenDeathKill:
        text = QStringLiteral("SUDDEN DEATH，%1击杀%2获胜（%3 : %4）")
                   .arg(sideName(winner), sideName(loser)).arg(m_red.kills).arg(m_blue.kills);
        break;
    case Reason::None:
        break;
    }
    if (!detail.isEmpty())
        text += QStringLiteral("（%1）").arg(detail);

    m_verdict.valid = true;
    m_verdict.winner = winner;
    m_verdict.reason = reason;
    m_verdict.reasonText = text;
    m_verdict.elapsedMs = elapsed;
    m_verdict.settlementType = reason == Reason::Terminated ? QStringLiteral("termination")
                             : winner == kRed ? QStringLiteral("redwin")
                             : winner == kBlue ? QStringLiteral("bluewin")
                                               : QStringLiteral("draw");
    m_phase = Phase::Finished;
    m_verdictPending = true;
    sampleTimeline(elapsed, true);
    logEvent(nowMs, winner, QStringLiteral("verdict"), text);
}

void MatchReferee::logEvent(qint64 nowMs, quint8 team, const QString &kind, const QString &text)
{
    m_events.append({elapsedMs(nowMs), team, kind, text});
    // Keep memory bounded if a robot flaps for a long time.
    if (m_events.size() > kMaxSummaryEvents * 4)
        m_events.remove(0, m_events.size() - kMaxSummaryEvents * 2);
}
