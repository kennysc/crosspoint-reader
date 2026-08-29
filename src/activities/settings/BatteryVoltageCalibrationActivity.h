#pragma once

#include <BatteryMonitor.h>

#include "activities/Activity.h"
#include "components/UiAppHost.h"

// Lets the user build a custom voltage-to-percentage curve on-device for
// gauge-backed boards (BQ27220/CW2017) whose SoC register has proven
// inaccurate. Reached from BatteryStatsActivity when the active board has a
// gauge (see BatteryMonitor::hasGaugeBackend()). Mutates CrossPointSettings::
// batteryPercentMode / batteryCustomCurveMv in place; the caller's
// startActivityForResult() handler persists them on finish(), matching every
// other settings sub-activity in this codebase.
class BatteryVoltageCalibrationActivity final : public Activity, private UiAppHost {
 public:
  explicit BatteryVoltageCalibrationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BatteryVoltageCalibration", renderer, mappedInput), UiAppHost(renderer) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static constexpr uint8_t NOTCH_COUNT = 11;  // 0%, 10%, ..., 100%

  uint16_t liveMv = 0;

  // Notch percentage labels ("0%".."100%"), fixed for the activity's lifetime.
  char notchLabels[NOTCH_COUNT][5]{};
  // Notch mV values, re-rendered from SETTINGS.batteryCustomCurveMv on every buildScreen().
  char notchValues[NOTCH_COUNT][16]{};
  freeink::ui::ListItem rowItems[NOTCH_COUNT]{};
  freeink::ui::ListNav nav;

  static void screenTrampoline(UiScreen& screen, void* user);
  void buildScreen(UiScreen& screen);
  // Moves the selection and pulls the viewport along (mirrors UiListActivity::moveSelectionTo).
  void moveSelection(int index);
};
