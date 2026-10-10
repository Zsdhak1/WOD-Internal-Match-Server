#ifndef PLAYERCOMBATOVERLAY_H
#define PLAYERCOMBATOVERLAY_H

#include <QElapsedTimer>
#include <QWidget>

#include <string>

class QPaintEvent;
class QTimer;

namespace playercombat {

// Value-only tracking: no socket, animation, shooting event or referee inference.
// An absent/unknown/offline sample invalidates the baseline; a replacement or
// reconnect establishes a fresh baseline, even when its HP is lower.
struct HpSample {
    bool present = false;
    bool online = false;
    bool linkUp = false;
    int team = 0;
    int robotId = 0;
    int protocol = 0;
    int hp = -1;
    std::string address;
};

class HpTracker
{
public:
    void reset() { m_previous = HpSample{}; }

    bool hasBaselineFor(const HpSample &sample) const
    {
        return sample.present && sample.online && sample.linkUp && sample.hp >= 0
               && m_previous.present && m_previous.team == sample.team
               && m_previous.robotId == sample.robotId
               && m_previous.protocol == sample.protocol
               && m_previous.address == sample.address;
    }

    int observe(const HpSample &sample)
    {
        if (!sample.present || !sample.online || !sample.linkUp || sample.hp < 0
            || sample.robotId < 1 || (sample.team != 1 && sample.team != 2)
            || (sample.protocol != 1 && sample.protocol != 2)) {
            reset();
            return 0;
        }
        const bool sameRobot = m_previous.present
                               && m_previous.team == sample.team
                               && m_previous.robotId == sample.robotId
                               && m_previous.protocol == sample.protocol
                               && m_previous.address == sample.address;
        const int damage = sameRobot && sample.hp < m_previous.hp
                               ? m_previous.hp - sample.hp : 0;
        m_previous = sample; // Healing is a new baseline, never a hit.
        return damage;
    }

private:
    HpSample m_previous;
};

// SHOOT main_xjx @ 6222c4c: thermal.h / thermal.c. This is heat budget,
// not magazine ammunition and never grants hardware shooting permission.
constexpr int kHeatLimit = 200;
constexpr int kHeatPerShot = 10;
constexpr int kCoolingPeriodMs = 1000;
constexpr int kHeatSampleTimeoutMs = 1500;

class HeatTracker
{
public:
    void reset() { m_heat = -1; m_received = m_cycleStarted = -1; m_canShoot = false; }
    void observe(int heat, bool canShoot, qint64 now)
    {
        if (heat < 0 || heat > kHeatLimit) { reset(); return; }
        // Shooting does not reset the firmware cooling tick. A fall calibrates
        // the estimated phase; irregular/manual values establish a new phase.
        if (!valid(now) || m_heat == 0 || heat < m_heat
            || (heat != m_heat && (heat - m_heat) % kHeatPerShot != 0))
            m_cycleStarted = now;
        m_heat = heat;
        m_canShoot = canShoot;
        m_received = now;
    }
    bool valid(qint64 now) const
    {
        return m_heat >= 0 && m_received >= 0 && now >= m_received
               && now - m_received <= kHeatSampleTimeoutMs;
    }
    int availableShots(qint64 now) const
    {
        return !valid(now) ? -1 : (!m_canShoot ? 0 : (kHeatLimit - m_heat) / kHeatPerShot);
    }
    int heat() const { return m_heat; }
    bool canShoot() const { return m_canShoot; }
    qreal coolingProgress(qint64 now) const
    {
        if (!valid(now)) return 0;
        if (m_heat == 0) return 1;
        // No invented repeated cooling cycles: wait at full until telemetry
        // confirms a reduction. The protocol carries no device cooling clock.
        return qBound(qreal(0), qreal(now - m_cycleStarted) / kCoolingPeriodMs, qreal(1));
    }
private:
    int m_heat = -1;
    bool m_canShoot = false;
    qint64 m_received = -1;
    qint64 m_cycleStarted = -1;
};

} // namespace playercombat

// Logical-coordinate, mouse-transparent player-only overlay. It is never part
// of the shared broadcast HUD and cannot create or submit combat events.
class PlayerCombatOverlay : public QWidget
{
public:
    explicit PlayerCombatOverlay(QWidget *parent = nullptr);
    void setHudScale(qreal scale);
    void clearFeedback();
    void pulseOwnDamage();
    void pulseOpponentDamage();
    void setHeatTelemetry(int heat, bool canShoot);
    int availableShots() const { return m_heat.availableShots(m_clock.elapsed()); }
    bool heatTelemetryValid() const { return m_heat.valid(m_clock.elapsed()); }
    qreal coolingProgress() const { return m_heat.coolingProgress(m_clock.elapsed()); }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void advanceFeedback();
    QElapsedTimer m_clock;
    QTimer *m_animationTimer = nullptr;
    qreal m_scale = 1.0;
    playercombat::HeatTracker m_heat;
    qint64 m_damageStarted = -1;
    qint64 m_hitStarted = -1;
    qint64 m_lastDamagePulse = -1000;
    qint64 m_lastHitPulse = -1000;
};

#endif // PLAYERCOMBATOVERLAY_H
