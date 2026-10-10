#include "matchreferee.h"
#include "protocol.h"

#include <QCoreApplication>
#include <QJsonArray>

#include <cstdio>
#include <functional>
#include <stdexcept>

namespace {
using Robot = RobotManager::RobotInfo;
using Reason = MatchReferee::Reason;

int g_passed = 0;

void check(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

Robot robot(quint8 id, quint8 team, int hp = 300)
{
    Robot r;
    r.robotId = id;
    r.team = team;
    r.protocolVersion = proto::kVersion2;
    r.hp = hp;
    r.alive = hp > 0;
    r.shootEnabled = true;
    r.online = true;
    return r;
}

struct Match {
    MatchReferee referee;
    Robot red = robot(71, 1);
    Robot blue = robot(72, 2);
    qint64 now = 1000;

    explicit Match(const MatchReferee::Rules &rules = MatchReferee::Rules())
    {
        referee.setRules(rules);
        referee.beginRound({red, blue}, now);
    }
    // Advance time in 100 ms ticks, as MainWindow does. Returns true as soon as
    // a verdict is produced.
    bool advance(int ms)
    {
        for (int t = 0; t < ms; t += 100) {
            now += 100;
            referee.updateRobots({red, blue}, now);
            if (referee.tick(now))
                return true;
        }
        return false;
    }
    void push() { referee.updateRobots({red, blue}, now); }
    const MatchReferee::Verdict &verdict() const { return referee.verdict(); }
};

void run(const char *name, const std::function<void()> &test)
{
    test();
    ++g_passed;
    std::printf("PASS: %s\n", name);
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    try {
        run("V1 same robot ID on opposite teams stays isolated", [] {
            Robot red = robot(1, 1, 5000), blue = robot(1, 2, 5000);
            red.protocolVersion = blue.protocolVersion = proto::kVersion1;
            MatchReferee referee;
            referee.beginRound({red, blue}, 1000);
            check(referee.team(1).hp == 5000 && referee.team(2).hp == 5000,
                  "V1 opposite teams sharing ID overwrite each other");
            referee.updateRobots({red, blue}, 12000);
            check(!referee.tick(12000), "same-ID V1 robot falsely forfeited offline");
            referee.recordCombatEvent(0, 1, proto::TypeDeath, 12010);
            check(referee.team(1).deaths == 0 && referee.team(2).deaths == 0,
                  "ambiguous event without team changed a V1 robot");
            referee.recordCombatEvent(1, 1, proto::TypeDeath, 12020);
            check(referee.team(1).deaths == 1 && referee.team(2).kills == 1
                      && referee.team(2).alive,
                  "V1 red death altered blue robot sharing ID");
            red.hp = 0; red.alive = false;
            referee.updateRobots({red, blue}, 12100);
            check(referee.team(2).kills == 1 && referee.team(2).hp == 5000,
                  "V1 death snapshot scored twice or overwrote opponent");
        });

        run("regulation kills score once and do not end the round", [] {
            Match m;
            m.red.hp = 120; m.push();
            m.red.hp = 0; m.red.alive = false; m.push();
            check(!m.referee.tick(m.now), "regulation kill ended round");
            check(!m.advance(400), "regulation kill ended after window");
            check(m.referee.team(2).kills == 1, "kill not scored or repeated");
            m.referee.timeExpired(m.now);
            check(m.referee.tick(m.now) && m.verdict().winner == 2
                      && m.verdict().reason == Reason::KillScore, "kill score timeout wrong");
            check(m.verdict().settlementType == QStringLiteral("bluewin"), "wrong settlement");
            check(m.referee.damageDealt(2) == 300 && m.referee.team(1).deaths == 1,
                  "damage/death statistics wrong");
            check(!m.referee.tick(m.now), "verdict must only be reported once");
        });

        run("mutual regulation kills each score then enter SD at timeout", [] {
            Match m;
            m.red.hp = 0; m.red.alive = false; m.push();
            m.now += 200;
            m.blue.hp = 0; m.blue.alive = false; m.push();
            check(!m.advance(300), "regulation double kill ended match");
            check(m.referee.team(1).kills == 1 && m.referee.team(2).kills == 1,
                  "mutual kill points wrong");
            m.referee.timeExpired(m.now);
            check(!m.referee.tick(m.now) && m.referee.suddenDeath(), "tied kills did not enter SD");
        });

        run("revival inside the window cancels the knockout", [] {
            Match m;
            m.red.hp = 0; m.red.alive = false; m.push();
            m.now += 100;
            m.red.hp = 300; m.red.alive = true; m.push();
            check(!m.advance(1000), "revived robot still lost");
            check(m.referee.team(1).deaths == 1, "death not counted");
        });

        run("DEATH event before the STATUS frame starts the knockout", [] {
            Match m;
            m.referee.recordCombatEvent(1, 71, proto::TypeDeath, m.now);
            m.now += 350;
            check(!m.referee.tick(m.now) && m.referee.team(2).kills == 1, "death event not scored");
            m.referee.recordCombatEvent(1, 71, proto::TypeDeath, m.now);
            check(m.referee.team(2).kills == 1, "repeated DEATH scored twice");
        });

        run("a robot already at 0 HP when the round starts is not knocked out", [] {
            MatchReferee referee;
            Robot red = robot(71, 1, 0), blue = robot(72, 2);
            referee.beginRound({red, blue}, 0);
            check(!referee.tick(1000), "stale 0 HP caused a knockout");
            red.hp = 300; red.alive = true;          // GAME_START resets HP
            referee.updateRobots({red, blue}, 1100);
            check(referee.team(1).damageTaken == 0, "HP reset counted as damage");
            red.hp = 0; red.alive = false;
            referee.updateRobots({red, blue}, 1200);
            check(!referee.tick(1600) && referee.team(2).kills == 1,
                  "observed death after reset must score not finish");
        });

        run("knockout counting mode lets the round continue and decides on deaths", [] {
            Match m;
            m.red.hp = 0; m.red.alive = false; m.push();
            check(!m.advance(1000), "counting mode ended the round");
            m.red.hp = 300; m.red.alive = true;
            m.blue.hp = 10; m.push();
            m.referee.timeExpired(m.now);
            check(m.referee.tick(m.now), "timeout not reported");
            check(m.verdict().winner == 2 && m.verdict().reason == Reason::KillScore,
                  "fewer deaths must win before HP");
        });

        run("third yellow card disqualifies and its HP loss is not opponent damage", [] {
            Match m;
            m.referee.recordPenalty(1, false, m.now);
            m.red.hp = 250; m.push();
            m.referee.recordPenalty(1, false, m.now);
            m.red.hp = 200; m.push();
            check(m.referee.team(1).penaltyHpLost == 100
                      && m.referee.damageDealt(2) == 0, "penalty HP credited as damage");
            m.red.hp = 180; m.push();
            check(m.referee.damageDealt(2) == 20, "real damage after penalty lost");
            check(!m.referee.tick(m.now), "two yellow cards must not disqualify");
            m.referee.recordPenalty(1, false, m.now);
            check(m.referee.tick(m.now), "third yellow card not reported");
            check(m.verdict().winner == 2 && m.verdict().reason == Reason::YellowCardLimit,
                  "wrong yellow card verdict");
        });

        run("DQ has priority over a same-instant knockout", [] {
            Match m;
            m.referee.recordPenalty(1, false, m.now);
            m.referee.recordPenalty(1, false, m.now);
            m.referee.recordPenalty(1, false, m.now);
            m.red.hp = 0; m.red.alive = false; m.push();
            check(m.referee.tick(m.now) && m.verdict().reason == Reason::YellowCardLimit,
                  "knockout overrode disqualification");
        });

        run("red card disqualification can be disabled", [] {
            MatchReferee::Rules rules; rules.redCardDisqualifies = false;
            Match m(rules);
            m.referee.recordPenalty(2, true, m.now);
            check(!m.advance(500), "disabled red card still disqualified");
            Match strict;
            strict.referee.recordPenalty(2, true, strict.now);
            check(strict.referee.tick(strict.now) && strict.verdict().winner == 1
                      && strict.verdict().reason == Reason::RedCard, "red card not applied");
        });

        run("offline forfeit uses running time only and resets after reconnecting", [] {
            MatchReferee::Rules rules; rules.offlineForfeitMs = 2000;
            Match m(rules);
            m.blue.online = false;
            check(!m.advance(1500), "forfeit before grace period");
            m.referee.pause(m.now);
            m.now += 10000;                           // paused time must not count
            check(!m.referee.tick(m.now), "paused time counted as offline");
            m.referee.resume(m.now);
            m.blue.online = true;
            check(!m.advance(200), "reconnect did not reset offline timer");
            m.blue.online = false;
            check(!m.advance(1800), "offline run was not reset");
            check(m.advance(400), "forfeit not applied");
            check(m.verdict().winner == 1 && m.verdict().reason == Reason::Forfeit,
                  "wrong forfeit verdict");
            check(m.referee.team(2).offlineTotalMs >= 3500, "offline total not tracked");
        });

        run("both sides offline never forfeits automatically", [] {
            MatchReferee::Rules rules; rules.offlineForfeitMs = 1000;
            Match m(rules);
            m.red.online = false; m.blue.online = false;
            check(!m.advance(3000), "double offline produced a verdict");
        });

        run("offline robot keeps its last state; stale data is not a death", [] {
            Match m;
            m.red.online = false; m.red.alive = false; m.red.hp = 0; m.push();
            check(!m.advance(1000), "offline snapshot treated as death");
        });

        run("equal kill scores ignore HP ratio across protocol versions", [] {
            MatchReferee referee;
            Robot red = robot(71, 1), blue = robot(72, 2);
            red.protocolVersion = proto::kVersion1; red.hp = 5000;
            referee.beginRound({red, blue}, 0);
            red.hp = 2500; blue.hp = 151;             // 50.0% vs 50.3%
            referee.updateRobots({red, blue}, 100);
            referee.timeExpired(60000);
            check(!referee.tick(60000) && referee.suddenDeath(), "HP ratio overrode kill tie");
        });

        run("equal kill scores ignore damage and penalty statistics and enter SD", [] {
            // Equal HP ratio but different damage cannot happen with fixed max
            // HP unless a robot was healed, so heal after taking damage.
            Match dmg;
            dmg.blue.hp = 200; dmg.push();
            dmg.blue.hp = 300; dmg.push();
            dmg.referee.timeExpired(dmg.now);
            check(!dmg.referee.tick(dmg.now) && dmg.referee.suddenDeath(), "damage broke score tie");

            MatchReferee::Rules rules; rules.yellowCardLimit = 0;
            Match pen(rules);
            pen.referee.recordPenalty(1, false, pen.now);
            pen.red.hp = 250; pen.push(); pen.red.hp = 300; pen.push();
            pen.referee.timeExpired(pen.now);
            check(!pen.referee.tick(pen.now) && pen.referee.suddenDeath(), "penalties broke score tie");

            Match draw;
            draw.referee.timeExpired(draw.now);
            check(!draw.referee.tick(draw.now) && draw.referee.suddenDeath()
                      && !draw.verdict().valid, "tie finished instead of SD");
        });

        run("pending knockout is resolved when time expires", [] {
            Match m;
            m.red.hp = 0; m.red.alive = false; m.push();
            m.referee.timeExpired(m.now + 50);
            check(!m.referee.tick(m.now + 50), "buzzer skipped simultaneous confirmation");
            check(m.referee.tick(m.now + 350) && m.verdict().reason == Reason::KillScore,
                  "buzzer kill score unresolved");
        });

        run("terminate and manual verdicts override automatic rules", [] {
            Match m;
            m.referee.terminate(m.now);
            check(m.referee.tick(m.now) && m.verdict().settlementType == QStringLiteral("termination"),
                  "termination");
            m.red.hp = 0; m.red.alive = false; m.push();
            check(!m.advance(500), "finished round accepted new verdict");

            Match manual;
            manual.referee.declareWinner(1, manual.now);
            check(manual.referee.tick(manual.now) && manual.verdict().winner == 1
                      && manual.verdict().reason == Reason::Manual, "manual verdict");
        });

        run("paused rounds ignore combat damage and elapsed time", [] {
            Match m;
            m.advance(2000);
            m.referee.pause(m.now);
            m.red.hp = 100; m.push();
            m.now += 5000;
            check(m.referee.team(1).damageTaken == 0, "damage counted while paused");
            check(m.referee.elapsedMs(m.now) == 2000, "paused time counted as elapsed");
            m.referee.resume(m.now);
            m.red.hp = 80; m.push();
            check(m.referee.team(1).damageTaken == 20, "damage after resume lost");
        });

        run("summary JSON contains statistics, timeline and events", [] {
            Match m;
            m.referee.recordCombatEvent(1, 71, proto::TypeAttack, m.now);
            m.referee.recordCombatEvent(2, 72, proto::TypeHit, m.now);
            m.blue.hp = 280; m.push();
            m.advance(2500);
            m.referee.setScores(1, 0);
            m.referee.timeExpired(m.now);
            check(m.referee.tick(m.now), "no verdict");
            const QJsonObject summary = m.referee.summaryJson();
            const QJsonObject red = summary.value(QStringLiteral("red")).toObject();
            const QJsonObject blue = summary.value(QStringLiteral("blue")).toObject();
            check(summary.value(QStringLiteral("valid")).toBool()
                      && summary.value(QStringLiteral("winner")).toInt() == 1
                      && summary.value(QStringLiteral("reason")).toString() == QStringLiteral("kill_score"),
                  "summary verdict");
            check(red.value(QStringLiteral("attacks")).toInt() == 1
                      && red.value(QStringLiteral("damageDealt")).toInt() == 20
                      && blue.value(QStringLiteral("hits")).toInt() == 1
                      && blue.value(QStringLiteral("hpPermille")).toInt() == 933,
                  "summary statistics");
            const QJsonArray timeline = summary.value(QStringLiteral("timeline")).toArray();
            check(timeline.size() >= 3 && timeline.first().toArray().at(0).toDouble() == 0,
                  "timeline samples");
            check(summary.value(QStringLiteral("events")).toArray().size() >= 2, "events");
            check(summary.value(QStringLiteral("durationMs")).toDouble() == 2500, "duration");
        });

        run("sudden death first single kill wins once and includes its point", [] {
            Match m;
            m.referee.timeExpired(m.now);
            check(m.referee.suddenDeath() && !m.referee.tick(m.now), "SD not entered");
            m.referee.timeExpired(m.now + 50);
            check(!m.verdict().valid, "repeated expiry ended SD");
            m.blue.hp = 0; m.blue.alive = false; m.push();
            check(m.referee.team(1).kills == 1 && !m.referee.tick(m.now), "SD killed without window");
            check(m.advance(400) && m.verdict().winner == 1
                      && m.verdict().reason == Reason::SuddenDeathKill, "SD kill did not win");
            check(!m.referee.tick(m.now), "SD verdict repeated");
        });
        run("sudden death mutual kill continues then next life decides", [] {
            Match m;
            m.referee.timeExpired(m.now);
            m.red.hp = 0; m.red.alive = false; m.push();
            m.now += 200; m.blue.hp = 0; m.blue.alive = false; m.push();
            check(!m.advance(400) && m.referee.suddenDeath(), "mutual SD kill ended match");
            check(m.referee.team(1).kills == 1 && m.referee.team(2).kills == 1, "mutual SD score wrong");
            m.red.hp = m.blue.hp = 300; m.red.alive = m.blue.alive = true; m.push();
            m.red.hp = 0; m.red.alive = false; m.push();
            check(m.advance(400) && m.verdict().winner == 2, "next SD life did not decide");
            check(m.referee.team(2).kills == 2, "deciding SD kill not scored");
        });
        run("revival cannot erase a kill, next death scores again, new round resets", [] {
            Match m;
            for (int i = 0; i < 3; ++i) {
                m.blue.hp = 0; m.blue.alive = false; m.push();
                m.blue.hp = 300; m.blue.alive = true; m.push();
            }
            check(m.referee.team(1).kills == 3 && !m.advance(500), "multi-life scoring wrong");
            m.referee.beginRound({m.red, m.blue}, m.now);
            check(m.referee.team(1).kills == 0 && !m.referee.suddenDeath(), "new round retained scores/SD");
        });
        run("pause freezes SD confirmation and ignores death points", [] {
            Match m; m.referee.timeExpired(m.now);
            m.blue.hp = 0; m.blue.alive = false; m.push();
            m.referee.pause(m.now); m.now += 10000;
            check(!m.referee.tick(m.now), "paused SD finished");
            m.referee.resume(m.now);
            check(!m.referee.tick(m.now), "pause counted towards confirmation");
            check(m.advance(400), "resumed SD not finished");
            Match paused; paused.referee.pause(paused.now);
            paused.blue.hp = 0; paused.blue.alive = false; paused.push();
            paused.referee.resume(paused.now);
            check(paused.referee.team(1).kills == 0, "paused death scored");
        });
        run("removal and penalty death are not kills", [] {
            Match m;
            m.referee.updateRobots({m.red}, m.now);
            check(m.referee.team(1).kills == 0, "removed robot scored as kill");
            Match penalty; penalty.red.hp = 30; penalty.push();
            penalty.referee.recordPenalty(1, false, penalty.now);
            penalty.red.hp = 0; penalty.red.alive = false; penalty.push();
            check(penalty.referee.team(2).kills == 0, "yellow penalty credited as kill");
        });
        run("buzzer mutual kills settle before SD; score correction is authoritative", [] {
            Match m; m.red.hp = 0; m.red.alive = false; m.push();
            m.referee.timeExpired(m.now + 50);
            m.now += 200; m.blue.hp = 0; m.blue.alive = false; m.push();
            check(!m.advance(400) && m.referee.suddenDeath(), "buzzer mutual kill not tied");
            Match corrected; corrected.referee.setScores(5, 4);
            corrected.referee.timeExpired(corrected.now);
            check(corrected.referee.tick(corrected.now) && corrected.verdict().winner == 1,
                  "score correction not used for timeout");
        });

        std::printf("MatchRefereeTest passed (%d cases)\n", g_passed);
        return 0;
    } catch (const std::exception &error) {
        std::printf("FAIL: %s\n", error.what());
        return 1;
    }
}
