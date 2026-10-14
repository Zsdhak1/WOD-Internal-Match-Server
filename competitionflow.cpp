#include "competitionflow.h"
#include <QJsonArray>
#include <QUuid>

CompetitionFlow::CompetitionFlow() { resetSeries(); }
QString CompetitionFlow::approvalKey(const QString &device, quint32 txid)
{ return device.toLower() + QStringLiteral("/0d/") + QString::number(txid); }

void CompetitionFlow::resetSeries()
{
    m_matchId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_roundId.clear();
    m_roundIndex = 1;
    m_advanceRound = false;
    m_phase = Phase::Ready;
    m_deadline = -1;
    for (int i = 0; i < 2; ++i) {
        m_wins[i] = 0; m_remaining[i] = kReviveLimit; m_deadSince[i] = -1; m_deathCycle[i] = 0;
    }
}

bool CompetitionFlow::beginCountdown(qint64 now)
{
    if (m_phase != Phase::Ready && m_phase != Phase::Cancelled) return false;
    if (m_advanceRound) { ++m_roundIndex; m_advanceRound = false; }
    m_roundId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_phase = Phase::Countdown;
    m_deadline = now + kCountdownMs;
    return true;
}

bool CompetitionFlow::cancelCountdown()
{
    if (m_phase != Phase::Countdown) return false;
    m_phase = Phase::Ready; m_deadline = -1;
    return true;
}

bool CompetitionFlow::confirmStart(qint64 now)
{
    if (m_phase != Phase::Starting) return false;
    for (int i = 0; i < 2; ++i) { m_remaining[i] = kReviveLimit; m_deadSince[i] = -1; m_deathCycle[i] = 0; }
    m_phase = Phase::Running;
    m_deadline = now + kRoundMs;
    return true;
}
void CompetitionFlow::cancelStart()
{ m_phase = Phase::Cancelled; m_deadline = -1; }

CompetitionFlow::Event CompetitionFlow::tick(qint64 now)
{
    if (m_deadline < 0 || now < m_deadline) return Event::None;
    if (m_phase == Phase::Countdown) {
        m_phase = Phase::Starting; m_deadline = -1; return Event::StartDevices;
    }
    if (m_phase == Phase::Running) {
        m_frozenRemaining = 0; m_phase = Phase::AwaitingDecision; m_deadline = -1; return Event::TimeExpired;
    }
    if (m_phase == Phase::Intermission) {
        m_phase = Phase::Ready; m_deadline = -1; return Event::PreparationComplete;
    }
    return Event::None;
}

void CompetitionFlow::finishRound(quint8 winner, qint64 now)
{
    if (m_phase != Phase::Running && m_phase != Phase::AwaitingDecision) return;
    const int i = index(winner);
    if (i < 0) { m_phase = Phase::Cancelled; m_deadline = -1; return; }
    ++m_wins[i];
    m_advanceRound = true;
    if (m_wins[i] >= 2) { m_phase = Phase::MatchFinished; m_deadline = -1; }
    else { m_phase = Phase::Intermission; m_deadline = now + kIntermissionMs; }
}

void CompetitionFlow::awaitDecision(qint64 now)
{
    if (m_phase != Phase::Running) return;
    m_frozenRemaining = remainingSeconds(now);
    m_phase = Phase::AwaitingDecision; m_deadline = -1;
}

void CompetitionFlow::observeLife(quint8 team, bool alive, qint64 now)
{
    const int i = index(team);
    if (i < 0 || m_phase != Phase::Running) return;
    if (alive) m_deadSince[i] = -1;
    else if (m_deadSince[i] < 0) { m_deadSince[i] = now; ++m_deathCycle[i]; }
}

CompetitionFlow::RequestResult CompetitionFlow::requestApproval(
    const QString &device, quint8 robotId, quint8 team, quint8 sequence, quint32 txid, qint64 now)
{
    const int i = index(team);
    if (i < 0 || device.isEmpty() || !robotId || !txid || m_phase != Phase::Running
        || m_deadline <= now || m_deadSince[i] < 0 || now - m_deadSince[i] < kReviveDelayMs)
        return {};
    const QString key = approvalKey(device, txid);
    const auto old = m_approvals.constFind(key);
    if (old != m_approvals.constEnd()) {
        if (old->robotId != robotId || old->team != team || old->sequence != sequence
            || old->roundId != m_roundId || old->deathCycle != m_deathCycle[i]
            || old->state == ApprovalState::Rejected)
            return {};
        return {RequestAction::Replay, key};
    }
    for (const Approval &a : m_approvals)
        if (a.roundId == m_roundId && a.team == team && a.deathCycle == m_deathCycle[i]
            && a.state == ApprovalState::Confirmed) return {}; // Already granted this death.
    // An unknown/in-flight execution is not a free slot for another request.
    if (revivePending(team)) return {RequestAction::Uncertain, {}};
    if (m_remaining[i] <= 0) return {RequestAction::NoQuota, {}};
    Approval item;
    item.device = device.toLower(); item.robotId = robotId; item.team = team;
    item.sequence = sequence; item.transactionId = txid; item.roundId = m_roundId;
    item.deathCycle = m_deathCycle[i];
    m_approvals.insert(key, item);
    --m_remaining[i]; // Reserve durably before sending. Retries do not decrement again.
    return {RequestAction::Approve, key};
}

bool CompetitionFlow::acknowledgeApproval(const QString &device, quint8 id, quint32 txid, quint8 result)
{
    auto it = m_approvals.find(approvalKey(device, txid));
    if (it == m_approvals.end() || it->robotId != id) return false;
    if (it->state == ApprovalState::Confirmed || it->state == ApprovalState::Rejected) return false;
    const bool wasUnknown = it->state == ApprovalState::Unknown;
    if (result == 0) it->state = ApprovalState::Confirmed;
    else if (result == 1 && !wasUnknown) {
        it->state = ApprovalState::Rejected;
        const int i = index(it->team);
        if (i >= 0 && it->roundId == m_roundId) m_remaining[i] = qMin(kReviveLimit, m_remaining[i] + 1);
    } else it->state = ApprovalState::Unknown;
    return true;
}
void CompetitionFlow::approvalTimeout(quint8 id, quint32 txid)
{
    for (auto it = m_approvals.begin(); it != m_approvals.end(); ++it)
        if (it->robotId == id && it->transactionId == txid && it->roundId == m_roundId
            && it->state == ApprovalState::Reserved) it->state = ApprovalState::Unknown;
}
int CompetitionFlow::remainingRevives(quint8 team) const
{ const int i = index(team); return i < 0 ? -1 : m_remaining[i]; }
bool CompetitionFlow::revivePending(quint8 team) const
{
    for (const Approval &a : m_approvals)
        if (a.roundId == m_roundId && a.team == team
            && (a.state == ApprovalState::Reserved || a.state == ApprovalState::Unknown)) return true;
    return false;
}
bool CompetitionFlow::locked() const
{
    return m_phase == Phase::Countdown || m_phase == Phase::Starting || m_phase == Phase::Running
           || m_phase == Phase::AwaitingDecision || m_phase == Phase::Intermission;
}
int CompetitionFlow::remainingSeconds(qint64 now) const
{
    if (m_phase == Phase::Ready || m_phase == Phase::Cancelled) return kRoundMs / 1000;
    if (m_phase == Phase::AwaitingDecision) return m_frozenRemaining;
    if (m_deadline < 0) return 0;
    return int(qMax<qint64>(0, m_deadline - now + 999) / 1000);
}
int CompetitionFlow::roundRemainingSeconds(qint64 now) const
{ return m_phase == Phase::Running ? remainingSeconds(now) : m_phase == Phase::AwaitingDecision ? m_frozenRemaining : 180; }
int CompetitionFlow::wins(quint8 team) const
{ const int i = index(team); return i < 0 ? 0 : m_wins[i]; }
QString CompetitionFlow::phaseKey() const
{
    switch (m_phase) {
    case Phase::Ready: return QStringLiteral("ready");
    case Phase::Countdown: return QStringLiteral("countdown");
    case Phase::Starting: return QStringLiteral("starting");
    case Phase::Running: return QStringLiteral("running");
    case Phase::AwaitingDecision: return QStringLiteral("awaiting_decision");
    case Phase::Intermission: return QStringLiteral("intermission");
    case Phase::MatchFinished: return QStringLiteral("match_finished");
    case Phase::Cancelled: return QStringLiteral("cancelled");
    }
    return {};
}
QString CompetitionFlow::phaseText() const
{
    switch (m_phase) {
    case Phase::Ready: return QStringLiteral("等待裁判开赛");
    case Phase::Countdown: return QStringLiteral("开赛倒计时");
    case Phase::Starting: return QStringLiteral("等待双方设备开赛确认");
    case Phase::Running: return QStringLiteral("比赛进行中");
    case Phase::AwaitingDecision: return QStringLiteral("等待裁判判定");
    case Phase::Intermission: return QStringLiteral("小局间准备");
    case Phase::MatchFinished: return QStringLiteral("BO3 已结束");
    case Phase::Cancelled: return QStringLiteral("本局已终止 · 可重赛");
    }
    return {};
}
QJsonObject CompetitionFlow::ledgerJson() const
{
    QJsonArray decisions;
    for (const Approval &a : m_approvals) decisions.append(QJsonObject{
        {"device", a.device}, {"robotId", a.robotId}, {"team", a.team}, {"sequence", a.sequence},
        {"txid", double(a.transactionId)}, {"roundId", a.roundId}, {"state", int(a.state)}, {"deathCycle", a.deathCycle}});
    return {{"schema", 1}, {"matchId", m_matchId}, {"roundId", m_roundId},
            {"roundIndex", m_roundIndex}, {"phase", phaseKey()},
            {"remainingRed", m_remaining[0]}, {"remainingBlue", m_remaining[1]}, {"decisions", decisions}};
}
bool CompetitionFlow::restoreLedger(const QJsonObject &object)
{
    if (object.value("schema").toInt() != 1 || !object.value("decisions").isArray()) return false;
    for (const QJsonValue &value : object.value("decisions").toArray()) {
        const QJsonObject o = value.toObject();
        Approval a;
        a.device = o.value("device").toString().toLower(); a.roundId = o.value("roundId").toString();
        a.robotId = quint8(o.value("robotId").toInt()); a.team = quint8(o.value("team").toInt());
        a.sequence = quint8(o.value("sequence").toInt()); a.transactionId = quint32(o.value("txid").toDouble());
        const int state = o.value("state").toInt(-1);
        if (a.device.isEmpty() || a.roundId.isEmpty() || !a.robotId || index(a.team) < 0
            || !a.transactionId || state < 0 || state > int(ApprovalState::Unknown)) return false;
        a.state = ApprovalState(state); a.deathCycle = o.value("deathCycle").toInt();
        if (a.state == ApprovalState::Reserved) a.state = ApprovalState::Unknown;
        m_approvals.insert(approvalKey(a.device, a.transactionId), a);
    }
    // New server lifetime starts a new identity, not a guessed continuation.
    return true;
}
