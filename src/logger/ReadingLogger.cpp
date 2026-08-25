#include "ReadingLogger.h"

#include <HalStorage.h>
#include <Rtc.h>

#include "BatterySessionTracker.h"
#include "CrossPointState.h"
#include "HalClock.h"
#include "HalPowerManager.h"

static constexpr const char* LOG_PATH = "/.crosspoint/reading_log.csv";

const char* ReadingLogger::logPath() { return LOG_PATH; }

void ReadingLogger::logPageTurn() {
    Rtc::DateTime dt;
    const bool haveClock = halClock.getDateTime(dt);
    const uint32_t epoch =
        haveClock ? batteryEpochFromParts(dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second) : 0;

    const auto status = powerManager.getBatteryStatus();
    const uint8_t pct =
        static_cast<uint8_t>(status.percentageKnown ? status.percentage : powerManager.getBatteryPercentage());
    const uint16_t mv = status.millivoltsKnown ? status.millivolts : 0;
    const int chargingRaw = status.chargingKnown ? (int)status.charging : -1;

    BatterySessionTracker tracker;
    tracker.sessionStartEpoch = APP_STATE.battSessionStartEpoch;
    tracker.sessionStartPct = APP_STATE.battSessionStartPct;
    tracker.lastSampleEpoch = APP_STATE.battLastSampleEpoch;
    tracker.lastSamplePct = APP_STATE.battLastSamplePct;
    tracker.lastCharging = APP_STATE.battLastCharging;
    tracker.hasSample = APP_STATE.battHasSample;
    tracker.activeReadSeconds = APP_STATE.battActiveReadSeconds;
    tracker.lastPageTurnEpoch = APP_STATE.battLastPageTurnEpoch;

    // Accumulate active-reading time on every page turn, not just when the battery
    // sample changes -- this is a pure in-RAM update (APP_STATE is memory-resident),
    // flushed to SD only at the existing save points below and in enterDeepSleep().
    tracker.observePageTurn(epoch);
    APP_STATE.battActiveReadSeconds = tracker.activeReadSeconds;
    APP_STATE.battLastPageTurnEpoch = tracker.lastPageTurnEpoch;

    // Unknown charging reads carry forward the last known state rather than
    // forcing a (possibly spurious) charge/discharge transition.
    const bool chargingKnown = chargingRaw != -1;
    const bool charging = chargingKnown ? (chargingRaw == 1) : tracker.lastCharging;
    const bool changed =
        !tracker.hasSample || tracker.lastSamplePct != pct || (chargingKnown && tracker.lastCharging != charging);
    if (!changed) return;  // Nothing to log or persist -- avoids an SD write on most page turns.

    const bool isNew = !Storage.exists(LOG_PATH);

    HalFile f = Storage.open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND);
    if (!f) return;

    if (isNew) {
        f.println("timestamp,battery_pct,voltage_mv,charging,active_read_seconds");
    }

    char ts[20] = "0000-00-00T00:00:00";
    if (haveClock) {
        snprintf(ts, sizeof(ts), "%04u-%02u-%02uT%02u:%02u:%02u",
                 dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
    }

    // active_read_seconds is the tally for the session/interval that was open going
    // into this row (pre-observe()) -- readers replaying the log get a trusted
    // per-row checkpoint instead of having to reconstruct active time from timestamps.
    char line[64];
    snprintf(line, sizeof(line), "%s,%u,%u,%d,%u\n", ts, pct, mv, chargingRaw, tracker.activeReadSeconds);
    f.write(reinterpret_cast<const uint8_t*>(line), strlen(line));
    f.close();

    tracker.observe(epoch, pct, charging);
    APP_STATE.battSessionStartEpoch = tracker.sessionStartEpoch;
    APP_STATE.battSessionStartPct = tracker.sessionStartPct;
    APP_STATE.battLastSampleEpoch = tracker.lastSampleEpoch;
    APP_STATE.battLastSamplePct = tracker.lastSamplePct;
    APP_STATE.battLastCharging = tracker.lastCharging;
    APP_STATE.battHasSample = tracker.hasSample;
    APP_STATE.battActiveReadSeconds = tracker.activeReadSeconds;
    APP_STATE.battLastPageTurnEpoch = tracker.lastPageTurnEpoch;
    APP_STATE.saveToFile();
}
