#include "competitionflow.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <cstdio>
#include <stdexcept>

static void check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
static void start(CompetitionFlow &f, qint64 &now)
{
    check(f.beginCountdown(now), "countdown not accepted");
    check(f.remainingSeconds(now) == 5, "countdown not five seconds");
    check(f.tick(now+4999) == CompetitionFlow::Event::None, "early start");
    now += 5000;
    check(f.tick(now) == CompetitionFlow::Event::StartDevices, "missing start event");
    check(f.tick(now) == CompetitionFlow::Event::None, "duplicate start event");
    check(f.confirmStart(now), "confirmation rejected");
    check(f.remainingSeconds(now) == 180, "round not 180 seconds");
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    try {
        CompetitionFlow f; qint64 now=1000;
        check(f.cancelCountdown()==false, "cancelled idle");
        check(f.beginCountdown(now) && f.cancelCountdown(), "countdown cancellation failed");
        start(f, now);
        check(!f.beginCountdown(now), "started twice while running");
        check(f.remainingRevives(1)==3 && f.remainingRevives(2)==3, "initial quotas");
        f.observeLife(1,false,now);
        check(f.requestApproval("mac-red",71,1,1,500,now+4999).action==CompetitionFlow::RequestAction::Ignore, "early revival");
        now += 5000;
        auto request=f.requestApproval("mac-red",71,1,1,500,now);
        check(request.action==CompetitionFlow::RequestAction::Approve && f.remainingRevives(1)==2 && f.revivePending(1), "quota not reserved");
        check(f.requestApproval("mac-red",71,1,1,500,now).action==CompetitionFlow::RequestAction::Replay && f.remainingRevives(1)==2, "repeat charged twice");
        check(f.requestApproval("mac-red",71,1,2,501,now).action==CompetitionFlow::RequestAction::Uncertain, "concurrent request allowed");
        f.approvalTimeout(71,500);
        check(f.acknowledgeApproval("mac-red",71,500,2) && f.remainingRevives(1)==2 && f.revivePending(1), "unknown returned quota");
        check(f.acknowledgeApproval("mac-red",71,500,0) && !f.revivePending(1), "late success not reconciled");
        check(!f.acknowledgeApproval("mac-red",71,500,0) && f.remainingRevives(1)==2, "ACK charged twice");
        check(f.requestApproval("mac-red",71,1,1,502,now).action==CompetitionFlow::RequestAction::Ignore, "same death granted under new tx");
        f.observeLife(1,true,now); f.observeLife(1,false,now);
        now += 5000;
        check(f.requestApproval("mac-red",71,1,2,501,now).action==CompetitionFlow::RequestAction::Approve, "second death not granted");
        check(f.acknowledgeApproval("mac-red",71,501,0) && f.remainingRevives(1)==1, "second quota wrong");
        f.observeLife(1,true,now); f.observeLife(1,false,now); now+=5000;
        check(f.requestApproval("mac-red",71,1,3,503,now).action==CompetitionFlow::RequestAction::Approve, "third revival denied");
        check(f.acknowledgeApproval("mac-red",71,503,0) && f.remainingRevives(1)==0, "third quota not zero");
        f.observeLife(1,true,now); f.observeLife(1,false,now); now+=5000;
        check(f.requestApproval("mac-red",71,1,4,504,now).action==CompetitionFlow::RequestAction::NoQuota, "fourth revival granted");
        // Same numeric transaction on a distinct physical device is independent.
        f.observeLife(2,false,now); now+=5000;
        check(f.requestApproval("mac-blue",72,2,1,500,now).action==CompetitionFlow::RequestAction::Approve && f.remainingRevives(2)==2, "cross-device tx collision");
        check(f.acknowledgeApproval("mac-blue",72,500,1) && f.remainingRevives(2)==3, "explicit denial not released once");
        const QString oldRound=f.roundId();
        f.finishRound(1,now);
        check(f.wins(1)==1 && f.phase()==CompetitionFlow::Phase::Intermission && f.remainingSeconds(now)==120, "intermission / score wrong");
        f.finishRound(1,now); check(f.wins(1)==1, "duplicate settlement counted");
        check(f.requestApproval("mac-red",71,1,4,504,now).action==CompetitionFlow::RequestAction::Ignore, "approval during preparation");
        check(!f.beginCountdown(now+119999), "preparation skipped");
        check(f.tick(now+119999)==CompetitionFlow::Event::None, "preparation ended early");
        now+=120000;
        check(f.tick(now)==CompetitionFlow::Event::PreparationComplete && f.phase()==CompetitionFlow::Phase::Ready, "no preparation completion");
        start(f,now);
        check(f.roundIndex()==2 && f.roundId()!=oldRound && f.remainingRevives(1)==3 && f.remainingRevives(2)==3 && f.wins(1)==1, "next round reset wrong");
        f.observeLife(1,false,now); now+=5000;
        check(f.requestApproval("mac-red",71,1,1,500,now).action==CompetitionFlow::RequestAction::Ignore, "old request reused new round");
        f.finishRound(1,now);
        check(f.phase()==CompetitionFlow::Phase::MatchFinished && f.wins(1)==2 && !f.beginCountdown(now), "2:0 created third round");
        CompetitionFlow restored;
        check(restored.restoreLedger(f.ledgerJson()), "ledger restore failed");
        check(restored.phase()==CompetitionFlow::Phase::Ready && restored.matchId()!=f.matchId(), "auto-resumed old match");
        auto record=restored.ledgerJson().value("decisions").toArray();
        check(!record.isEmpty(), "restore dropped dedup ledger");
        CompetitionFlow full; now=0;
        start(full,now); full.finishRound(1,now); now+=120000; full.tick(now); start(full,now);
        full.finishRound(2,now); now+=120000; full.tick(now); start(full,now);
        check(full.roundIndex()==3 && full.wins(1)==1 && full.wins(2)==1, "1:1 no deciding round");
        const qint64 expiry=now+180000;
        check(full.tick(expiry-1)==CompetitionFlow::Event::None && full.tick(expiry)==CompetitionFlow::Event::TimeExpired, "deadline inaccurate");
        check(full.tick(expiry)==CompetitionFlow::Event::None && full.remainingSeconds(expiry)==0, "expiry repeated");
        full.finishRound(2,expiry); check(full.wins(2)==2 && full.phase()==CompetitionFlow::Phase::MatchFinished,"2:1 not ended");
        CompetitionFlow uncertain; now=0; start(uncertain,now);
        uncertain.awaitDecision(now+10000);
        check(uncertain.remainingSeconds(now+20000)==170 && !uncertain.beginCountdown(now), "manual decision clock not frozen");
        uncertain.finishRound(0,now); check(uncertain.wins(1)==0 && uncertain.wins(2)==0 && uncertain.phase()==CompetitionFlow::Phase::Cancelled,"termination credited winner");
        std::puts("PASS: authoritative BO3 clock, three-revival quotas, same-death/transaction dedup, unknown outcomes, cross-device IDs and ledger restart");
        return 0;
    } catch (const std::exception &error) { std::fprintf(stderr,"FAIL: %s\n",error.what()); return 1; }
}
