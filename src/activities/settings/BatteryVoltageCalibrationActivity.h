#pragma once

#include <BatteryMonitor.h>

#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

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

  // Normal: Up/Down browse notches, Left/Right toggle source/step, Confirm
  // enters Adjust on the selected notch. Adjust: Up/Down step editingMv,
  // Confirm commits it, Back discards it (without exiting the activity).
  enum class CalibrationMode : uint8_t { Normal, Adjust };

  static constexpr uint16_t MV_MIN = 0;
  static constexpr uint16_t MV_MAX = 6000;
  static constexpr uint16_t STEP_COARSE_MV = 10;
  static constexpr uint16_t STEP_FINE_MV = 1;

  CalibrationMode mode = CalibrationMode::Normal;
  // Scratch mV value while in Adjust mode; SETTINGS.batteryCustomCurveMv is
  // only written on apply, so Back can discard without touching it.
  uint16_t editingMv = 0;
  bool coarseStep = true;
  ButtonNavigator buttonNavigator;

  uint16_t liveMv = 0;

  // Notch percentage labels ("0%".."100%"), fixed for the activity's lifetime.
  char notchLabels[NOTCH_COUNT][5]{};
  // Notch mV values, re-rendered from SETTINGS.batteryCustomCurveMv (or, for
  // the selected row while adjusting, editingMv) on every buildScreen().
  char notchValues[NOTCH_COUNT][16]{};
  freeink::ui::ListItem rowItems[NOTCH_COUNT]{};
  freeink::ui::ListNav nav;

  static void screenTrampoline(UiScreen& screen, void* user);
  void buildScreen(UiScreen& screen);
  // Moves the selection and pulls the viewport along (mirrors UiListActivity::moveSelectionTo).
  void moveSelection(int index);

  uint16_t currentStepMv() const { return coarseStep ? STEP_COARSE_MV : STEP_FINE_MV; }
  void enterAdjustMode();
  void adjustEditingMv(int delta);
  void applyAdjust();
  void cancelAdjust();
};
