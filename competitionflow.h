#ifndef COMPETITIONFLOW_H
#define COMPETITIONFLOW_H

#include <QHash>
#include <QJsonObject>
#include <QString>

// Server-owned BO3 clock and revival quota. All times are injected monotonic ms.
// Transport ACKs and retransmissions never count as a second revival.
class CompetitionFlow
{
public:
    enum class Phase { Ready, Countdown, Starting, Running, AwaitingDecision, Intermission, MatchFinished, Cancelled };
    enum class Event { None, StartDevices, TimeExpired, PreparationComplete };
    enum class ApprovalState { Reserved, Confirmed, Rejected, Unknown };
    struct Approval {
        QString device;
        QString roundId;
        quint8 robotId = 0;
        quint8 team = 0;
        quint8 sequence = 0;
        quint32 transactionId = 0;
        ApprovalState state = ApprovalState::Reserved;
        int deathCycle = 0;
    };
    enum class RequestAction { Ignore, Approve, Replay, NoQuota, Uncertain };
    struct RequestResult { RequestAction action = RequestAction::Ignore; QString key; };

    static constexpr int kCountdownMs = 5000;
    // Defaults (knockout preset). The active values come from setTimings().
    static constexpr int kRoundMs = 180000;
    static constexpr int kIntermissionMs = 120000;
    static constexpr int kMinRoundMs = 10000;
    static constexpr int kMaxRoundMs = 30 * 60000;
    static constexpr int kMaxIntermissionMs = 30 * 60000;
    static constexpr int kReviveDelayMs = 5000;
    static constexpr int kReviveLimit = 3;

    CompetitionFlow();
    void resetSeries(); // Keep the global decision ledger; never revive old requests.
    // Round / intermission length for the next round. Rejected (false) while a
    // round or intermission is live, or out of range; durations never change
    // under a running deadline.
    bool setTimings(int roundMs, int intermissionMs);
    // True from the first countdown until the BO3 is decided or reset, so one
    // series never mixes durations (also between rounds once a round is won).
    bool timingLocked() const
    { return locked() || (m_phase != Phase::MatchFinished && (m_wins[0] + m_wins[1] > 0 || m_advanceRound)); }
    int roundMs() const { return m_roundMs; }
    int intermissionMs() const { return m_intermissionMs; }
    bool beginCountdown(qint64 nowMs);
    bool cancelCountdown();
    bool confirmStart(qint64 nowMs);
    void cancelStart();
    Event tick(qint64 nowMs);
    void finishRound(quint8 winner, qint64 nowMs);
    void awaitDecision(qint64 nowMs);
    void observeLife(quint8 team, bool alive, qint64 nowMs);
    RequestResult requestApproval(const QString &device, quint8 robotId, quint8 team,
                                  quint8 sequence, quint32 transactionId, qint64 nowMs);
    bool acknowledgeApproval(const QString &device, quint8 robotId, quint32 txid, quint8 result);
    void approvalTimeout(quint8 robotId, quint32 txid);
    int remainingRevives(quint8 team) const;
    bool revivePending(quint8 team) const;
    bool locked() const;
    int remainingSeconds(qint64 nowMs) const;
    int roundRemainingSeconds(qint64 nowMs) const;
    // Exact ms to the running round's deadline; -1 when no round is running.
    qint64 runningRemainingMs(qint64 nowMs) const
    { return m_phase == Phase::Running && m_deadline >= 0 ? qMax<qint64>(0, m_deadline - nowMs) : -1; }
    int wins(quint8 team) const;
    Phase phase() const { return m_phase; }
    QString phaseKey() const;
    QString phaseText() const;
    QString matchId() const { return m_matchId; }
    QString roundId() const { return m_roundId; }
    int roundIndex() const { return m_roundIndex; }
    const QHash<QString, Approval> &approvals() const { return m_approvals; }
    QJsonObject ledgerJson() const;
    bool restoreLedger(const QJsonObject &object); // No automatic resumption after restart.

private:
    static int index(quint8 team) { return team == 1 ? 0 : team == 2 ? 1 : -1; }
    static QString approvalKey(const QString &device, quint32 txid);
    Phase m_phase = Phase::Ready;
    QString m_matchId;
    QString m_roundId;
    int m_roundIndex = 1;
    bool m_advanceRound = false;
    qint64 m_deadline = -1;
    int m_roundMs = kRoundMs;
    int m_intermissionMs = kIntermissionMs;
    int m_frozenRemaining = 0;
    int m_wins[2] = {0, 0};
    int m_remaining[2] = {3, 3};
    qint64 m_deadSince[2] = {-1, -1};
    int m_deathCycle[2] = {0, 0};
    QHash<QString, Approval> m_approvals;
};
#endif
