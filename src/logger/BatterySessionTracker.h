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

  // Accumulated active-reading seconds this discharge session, and the epoch of
  // the previous page turn (baseline for accumulating the next delta). Updated
  // on every page turn regardless of whether the battery sample changed.
  uint32_t activeReadSeconds = 0;
  uint32_t lastPageTurnEpoch = 0;

  // Snapshot of activeReadSeconds taken only when an actual battery sample is
  // observed (see observe()). Used as the rate/ETA denominator instead of the
  // continuously-growing activeReadSeconds, so those figures don't drift just
  // from reading pages between two real battery% samples.
  uint32_t activeReadSecondsAtLastSample = 0;

  // Gaps between page turns longer than this indicate the device was idle or
  // asleep, not being actively read, so they're excluded from activeReadSeconds.
  static constexpr uint32_t MAX_ACTIVE_GAP_SECONDS = 600;

  // Called on every page turn (not gated by battery-sample change) to accumulate
  // active-reading time in small increments even when battery% hasn't moved.
  void observePageTurn(uint32_t epoch) {
    if (epoch == 0) return;  // no RTC
    if (lastPageTurnEpoch != 0 && epoch > lastPageTurnEpoch) {
      const uint32_t delta = epoch - lastPageTurnEpoch;
      if (delta <= MAX_ACTIVE_GAP_SECONDS) activeReadSeconds += delta;
    }
    lastPageTurnEpoch = epoch;
  }

  void observe(uint32_t epoch, uint8_t pct, bool charging) {
    if (!hasSample || (lastCharging && !charging)) {  // first sample, or charge->discharge transition
      sessionStartEpoch = epoch;
      sessionStartPct = pct;
      activeReadSeconds = 0;
      lastPageTurnEpoch = epoch;  // fresh baseline so a stale pre-session gap isn't counted
    }
    lastSampleEpoch = epoch;
    lastSamplePct = pct;
    lastCharging = charging;
    hasSample = true;
    activeReadSecondsAtLastSample = activeReadSeconds;
  }

  // Active-reading seconds accumulated this discharge session. 0 if charging or no data yet.
  uint32_t totalReadSeconds() const { return lastCharging ? 0 : activeReadSeconds; }

  // Average discharge rate in percent per active-reading hour, as of the last observed
  // battery sample. 0 if not enough data to estimate. Uses activeReadSecondsAtLastSample
  // rather than totalReadSeconds() so the rate doesn't drift just from reading pages
  // between two real battery% samples.
  float avgDischargePctPerHour() const {
    const uint32_t seconds = lastCharging ? 0 : activeReadSecondsAtLastSample;
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
