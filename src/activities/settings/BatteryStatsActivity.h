#pragma once

#include "activities/Activity.h"
#include "logger/BatterySessionTracker.h"

// Displays battery discharge stats (avg %/hr, estimated time left, total read
// time since last charge) computed from the cheap CrossPointState snapshot
// that ReadingLogger maintains, plus a second pass over reading_log.csv, run
// automatically on entry, for lifetime stats CrossPointState can't hold.
class BatteryStatsActivity final : public Activity {
 public:
  explicit BatteryStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BatteryStats", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override { return true; }  // Prevent power-saving mode while scanning the log
  void render(RenderLock&&) override;

 private:
  enum State { LOADING, READY };
  State state = LOADING;

  BatterySessionTracker liveTracker;  // From CrossPointState -- instant, no SD I/O
  BatterySessionTracker logTracker;   // From scanning reading_log.csv, on entry

  // Lifetime aggregates across every charge cycle recorded in the log -- data
  // CrossPointState can never hold, since it resets each cycle.
  uint32_t logCompletedCycles = 0;
  uint32_t logLifetimeActiveSeconds = 0;  // completed cycles + the still-open one

  // Denominator/numerator for the lifetime avg discharge rate -- completed cycles only,
  // since the still-open session hasn't finished discharging by a comparable amount yet.
  uint32_t logCompletedActiveSeconds = 0;
  uint32_t logLifetimePctDrop = 0;

  char logFirstEntryDate[11] = "";  // "YYYY-MM-DD" of the first log row, "" if no log yet

  void goBack() { finish(); }
  void adjustThreshold(int delta);
  void scanLog();
};
