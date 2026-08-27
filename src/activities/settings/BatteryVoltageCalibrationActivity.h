#pragma once

#include <BatteryMonitor.h>

#include "activities/Activity.h"

// Lets the user build a custom voltage-to-percentage curve on-device for
// gauge-backed boards (BQ27220/CW2017) whose SoC register has proven
// inaccurate. Reached from BatteryStatsActivity when the active board has a
// gauge (see BatteryMonitor::hasGaugeBackend()). Mutates CrossPointSettings::
// batteryPercentMode / batteryCustomCurveMv in place; the caller's
// startActivityForResult() handler persists them on finish(), matching every
// other settings sub-activity in this codebase.
class BatteryVoltageCalibrationActivity final : public Activity {
 public:
  explicit BatteryVoltageCalibrationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BatteryVoltageCalibration", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static constexpr uint8_t NOTCH_COUNT = 11;  // 0%, 10%, ..., 100%

  uint8_t selectedIndex = 0;
  uint16_t liveMv = 0;
};
