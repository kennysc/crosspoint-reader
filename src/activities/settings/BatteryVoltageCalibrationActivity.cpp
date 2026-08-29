#include "BatteryVoltageCalibrationActivity.h"

#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <I18n.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

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
  for (uint8_t i = 0; i < NOTCH_COUNT; ++i) {
    snprintf(notchLabels[i], sizeof(notchLabels[i]), "%u%%", i * 10);
  }
  nav.reset(0);
  liveMv = powerManager.getBatteryStatus().millivolts;
  resetUi();
  app.setScreen(&BatteryVoltageCalibrationActivity::screenTrampoline, this);
  requestUpdate();
}

void BatteryVoltageCalibrationActivity::moveSelection(const int index) {
  {
    // Matches UiListActivity::moveSelectionTo -- the render task reads nav
    // mid-build (syncToProps, layout feedback); a press landing during a
    // render would otherwise tear selection/viewport.
    RenderLock lock(*this);
    nav.selected = index;
    nav.follow(NOTCH_COUNT);
  }
  requestUpdate();
}

void BatteryVoltageCalibrationActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    if (nav.selected > 0) moveSelection(nav.selected - 1);
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    if (nav.selected < NOTCH_COUNT - 1) moveSelection(nav.selected + 1);
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
    SETTINGS.batteryCustomCurveMv[nav.selected] = liveMv;
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

  renderUi();

  GUI.drawHelpText(
      renderer, Rect{0, pageHeight - metrics.buttonHintsHeight - metrics.contentSidePadding - 15, pageWidth, 20},
      tr(STR_BATTERY_CALIBRATION_HINT));

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_ASSIGN), tr(STR_BATTERY_SOURCE_GAUGE),
                                            tr(STR_BATTERY_SOURCE_VOLTAGE));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void BatteryVoltageCalibrationActivity::screenTrampoline(UiScreen& screen, void* user) {
  static_cast<BatteryVoltageCalibrationActivity*>(user)->buildScreen(screen);
}

void BatteryVoltageCalibrationActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageHeight = renderer.getScreenHeight();

  // Mirrors the stat-row geometry in render() -- the list starts right below
  // the two rows drawn there.
  constexpr int LINE_H = 28;
  const int listTop = metrics.topPadding + metrics.headerHeight + 30 + LINE_H + LINE_H + 10;
  const int listBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - 30;
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(listTop), 0,
                                      static_cast<int16_t>(pageHeight - listBottom), 0});

  for (uint8_t i = 0; i < NOTCH_COUNT; ++i) {
    snprintf(notchValues[i], sizeof(notchValues[i]), "%u mV", SETTINGS.batteryCustomCurveMv[i]);
    rowItems[i].label = notchLabels[i];
    rowItems[i].value = notchValues[i];
  }

  int16_t rowHeight = screen.theme().rowHeight;
  if (!mappedInput.hasTouch()) {
    // Non-touch hardware (X3/X4) keeps the original, denser per-theme row
    // height instead of FreeInkUI's touch-target-sized default.
    rowHeight = static_cast<int16_t>(metrics.listRowHeight);
  }

  fui::ListProps props;
  props.items = rowItems;
  props.count = NOTCH_COUNT;
  props.rowHeight = rowHeight;
  props.inputMask = fui::InputNone;  // physical buttons handled in loop()
  nav.syncToProps(screen.body(), rowHeight, screen.theme().listRowGap, NOTCH_COUNT, props);
  screen.list(props);
}
