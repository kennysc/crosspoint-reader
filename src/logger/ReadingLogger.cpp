#include "ReadingLogger.h"

#include <HalStorage.h>
#include <Rtc.h>
#include "HalClock.h"
#include "HalPowerManager.h"

static constexpr const char* LOG_PATH = "/.crosspoint/reading_log.csv";

void ReadingLogger::logPageTurn() {
    const bool isNew = !Storage.exists(LOG_PATH);

    HalFile f = Storage.open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND);
    if (!f) return;

    if (isNew) {
        f.println("timestamp,battery_pct,voltage_mv,charging");
    }

    char ts[20] = "0000-00-00T00:00:00";
    Rtc::DateTime dt;
    if (halClock.getDateTime(dt)) {
        snprintf(ts, sizeof(ts), "%04u-%02u-%02uT%02u:%02u:%02u",
                 dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
    }

    const auto status = powerManager.getBatteryStatus();
    const uint16_t pct = status.percentageKnown ? status.percentage : powerManager.getBatteryPercentage();
    const uint16_t mv  = status.millivoltsKnown  ? status.millivolts : 0;
    const int charging = status.chargingKnown    ? (int)status.charging : -1;

    char line[48];
    snprintf(line, sizeof(line), "%s,%u,%u,%d\n", ts, pct, mv, charging);
    f.write(reinterpret_cast<const uint8_t*>(line), strlen(line));
    f.close();
}
