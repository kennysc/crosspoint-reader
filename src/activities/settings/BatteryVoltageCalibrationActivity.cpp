#include "BatteryVoltageCalibrationActivity.h"

#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <I18n.h>

#include <cstdio>
#include <string>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Label left-justified, value right-justified -- mirrors BatteryStatsActivity's
// row layout (see BaseTheme::drawSubHeader for the underlying idiom).
void drawStatRow(const GfxRenderer& renderer, int leftX, int rightEdge, int y, const char* label, const char* value,
                 EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  renderer.drawText(UI_10_FONT_ID, leftX, y, label, true, style);
  const int valueWidth = renderer.getTextWidth(UI_10_FONT_ID, value, style);
  renderer.drawText(UI_10_FONT_ID, rightEdge - valueWidth, y, value, true, style);
}
}  // namespace

void BatteryVoltageCalibrationActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  liveMv = powerManager.getBatteryStatus().millivolts;
  requestUpdate();
}

void BatteryVoltageCalibrationActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    if (selectedIndex > 0) selectedIndex--;
    requestUpdate();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    if (selectedIndex < NOTCH_COUNT - 1) selectedIndex++;
    requestUpdate();
    return;
  }
  // Left/Right pick the source directly (Gauge / Voltage) rather than toggling,
  // matching the button-hint labels shown for each side.
  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    SETTINGS.batteryPercentMode = CrossPointSettings::BatteryPercentMode::Gauge;
    requestUpdate();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    SETTINGS.batteryPercentMode = CrossPointSettings::BatteryPercentMode::Voltage;
    requestUpdate();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    SETTINGS.batteryCustomCurveMv[selectedIndex] = liveMv;
    requestUpdate();
    return;
  }

  // Live-update the reading, but only redraw when it actually changes -- the
  // gauge/ADC read itself is already throttled and cached by HalPowerManager
  // (BATTERY_POLL_MS), so this just avoids a redundant e-ink refresh when the
  // voltage hasn't moved between polls.
  const uint16_t newMv = powerManager.getBatteryStatus().millivolts;
  if (newMv != liveMv) {
    liveMv = newMv;
    requestUpdate();
  }
}

void BatteryVoltageCalibrationActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 tr(STR_BATTERY_VOLTAGE_CALIBRATION));

  const int leftX = metrics.contentSidePadding;
  const int rightEdge = pageWidth - metrics.contentSidePadding;
  int y = metrics.topPadding + metrics.headerHeight + 30;
  constexpr int LINE_H = 28;
  char value[32];

  const bool voltageMode = SETTINGS.batteryPercentMode == CrossPointSettings::BatteryPercentMode::Voltage;
  drawStatRow(renderer, leftX, rightEdge, y, tr(STR_BATTERY_PERCENT_SOURCE),
              voltageMode ? tr(STR_BATTERY_SOURCE_VOLTAGE) : tr(STR_BATTERY_SOURCE_GAUGE), EpdFontFamily::BOLD);
  y += LINE_H;

  const uint16_t previewPct = BatteryMonitor::percentageFromMillivolts(liveMv, SETTINGS.batteryCustomCurveMv);
  snprintf(value, sizeof(value), "%u mV (~%u%%)", liveMv, previewPct);
  drawStatRow(renderer, leftX, rightEdge, y, tr(STR_CURRENT_READING), value);
  y += LINE_H + 10;

  const int listTop = y;
  const int listHeight = pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing - 30;
  GUI.drawList(
      renderer, Rect{0, listTop, pageWidth, listHeight}, NOTCH_COUNT, selectedIndex,
      [](int index) { return std::to_string(index * 10) + "%"; }, nullptr, nullptr,
      [](int index) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%u mV", SETTINGS.batteryCustomCurveMv[index]);
        return std::string(buf);
      },
      true);

  GUI.drawHelpText(
      renderer, Rect{0, pageHeight - metrics.buttonHintsHeight - metrics.contentSidePadding - 15, pageWidth, 20},
      tr(STR_BATTERY_CALIBRATION_HINT));

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_ASSIGN), tr(STR_BATTERY_SOURCE_GAUGE),
                                            tr(STR_BATTERY_SOURCE_VOLTAGE));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
