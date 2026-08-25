#pragma once

#include "activities/Activity.h"
#include "logger/BatterySessionTracker.h"

// Displays battery discharge stats (avg %/hr, estimated time left, total read
// time since last charge) computed from the cheap CrossPointState snapshot
// that ReadingLogger maintains, plus an on-demand "verify from log" action
// that recomputes the same stats by scanning reading_log.csv for comparison.
class BatteryStatsActivity final : public Activity {
 public:
  explicit BatteryStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BatteryStats", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override { return true; }  // Prevent power-saving mode while verifying
  void render(RenderLock&&) override;

 private:
  enum State { IDLE, VERIFYING, VERIFIED };
  State state = IDLE;

  BatterySessionTracker liveTracker;  // From CrossPointState -- instant, no SD I/O
  BatterySessionTracker logTracker;   // From scanning reading_log.csv -- only after Verify

  // Lifetime aggregates across every charge cycle recorded in the log -- data
  // CrossPointState can never hold, since it resets each cycle. Only after Verify.
  uint32_t logCompletedCycles = 0;
  uint32_t logLifetimeActiveSeconds = 0;

  void goBack() { finish(); }
  void adjustThreshold(int delta);
  void beginVerify();
  void scanLog();
};
