#pragma once

#include <cstdint>
#include <ctime>

// Tracks the current battery discharge session: the run of samples observed
// since charging was last active. Used two ways: as a live incremental
// accumulator fed one sample per meaningful battery-state change (persisted
// in CrossPointState by ReadingLogger, for an instant zero-I/O stats display),
// and as an ephemeral accumulator replaying reading_log.csv on demand (for
// the Battery Stats screen's "verify from log" action) -- both paths share
// this logic so their results are directly comparable.
struct BatterySessionTracker {
  uint32_t sessionStartEpoch = 0;
  uint8_t sessionStartPct = 0;
  uint32_t lastSampleEpoch = 0;
  uint8_t lastSamplePct = 0;
  bool lastCharging = false;
  bool hasSample = false;

  void observe(uint32_t epoch, uint8_t pct, bool charging) {
    if (!hasSample || (lastCharging && !charging)) {  // first sample, or charge->discharge transition
      sessionStartEpoch = epoch;
      sessionStartPct = pct;
    }
    lastSampleEpoch = epoch;
    lastSamplePct = pct;
    lastCharging = charging;
    hasSample = true;
  }

  // Seconds spanned by the current discharge session. 0 if charging or no data yet.
  uint32_t totalReadSeconds() const {
    if (!hasSample || lastCharging || lastSampleEpoch <= sessionStartEpoch) return 0;
    return lastSampleEpoch - sessionStartEpoch;
  }

  // Average discharge rate in percent per hour. 0 if not enough data to estimate.
  float avgDischargePctPerHour() const {
    const uint32_t seconds = totalReadSeconds();
    if (seconds == 0 || sessionStartPct <= lastSamplePct) return 0.0f;
    const float hours = static_cast<float>(seconds) / 3600.0f;
    return static_cast<float>(sessionStartPct - lastSamplePct) / hours;
  }

  // Estimated seconds left until thresholdPct, or -1 if not estimable (charging,
  // no discharge observed yet). 0 if already at or under the threshold.
  int32_t estimatedSecondsLeft(uint8_t thresholdPct) const {
    if (!hasSample || lastCharging) return -1;
    if (lastSamplePct <= thresholdPct) return 0;
    const float ratePerHour = avgDischargePctPerHour();
    if (ratePerHour <= 0.0f) return -1;
    const float hoursLeft = static_cast<float>(lastSamplePct - thresholdPct) / ratePerHour;
    return static_cast<int32_t>(hoursLeft * 3600.0f);
  }
};

// Converts a wall-clock timestamp to seconds-since-epoch via mktime(), the same
// conversion already used for boot-time RTC sync (see HalClock.cpp). Shared by
// live BatterySessionTracker updates (from Rtc::DateTime) and by CSV log replay
// (from parsed timestamp strings) so both produce comparable epochs. year == 0
// is ReadingLogger's RTC-unavailable sentinel and always maps to epoch 0.
inline uint32_t batteryEpochFromParts(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute,
                                      uint8_t second) {
  if (year == 0) return 0;
  struct tm timeinfo = {};
  timeinfo.tm_year = year - 1900;
  timeinfo.tm_mon = month - 1;
  timeinfo.tm_mday = day;
  timeinfo.tm_hour = hour;
  timeinfo.tm_min = minute;
  timeinfo.tm_sec = second;
  const time_t epoch = mktime(&timeinfo);
  return epoch > 0 ? static_cast<uint32_t>(epoch) : 0;
}
