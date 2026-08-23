#include "BatteryStatsActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "logger/ReadingLogger.h"

namespace {

// Parses one reading_log.csv row ("timestamp,battery_pct,voltage_mv,charging")
// and feeds it into tracker. Silently ignores the header row and any malformed
// line (sscanf field-count mismatch), since both simply fail to match the format.
void parseLogLine(const char* line, BatterySessionTracker& tracker) {
  uint16_t year, mv;
  uint8_t month, day, hour, minute, second, pct;
  int charging;
  if (sscanf(line, "%hu-%hhu-%hhuT%hhu:%hhu:%hhu,%hhu,%hu,%d", &year, &month, &day, &hour, &minute, &second, &pct,
             &mv, &charging) != 9) {
    return;
  }
  if (year == 0) return;  // RTC-unavailable sentinel row

  // Unknown charging reads carry forward the last known state, matching ReadingLogger's rule.
  const bool chargingBool = (charging == -1) ? tracker.lastCharging : (charging == 1);
  tracker.observe(batteryEpochFromParts(year, month, day, hour, minute, second), pct, chargingBool);
}

void formatDuration(char* buf, size_t n, uint32_t seconds) {
  snprintf(buf, n, "%uh %um", seconds / 3600, (seconds % 3600) / 60);
}

void formatRate(char* buf, size_t n, float pctPerHour) {
  if (pctPerHour <= 0.0f) {
    snprintf(buf, n, "%s", tr(STR_NOT_AVAILABLE));
  } else {
    snprintf(buf, n, "%.1f %%/hr", pctPerHour);
  }
}

void formatEta(char* buf, size_t n, int32_t secondsLeft) {
  if (secondsLeft < 0) {
    snprintf(buf, n, "%s", tr(STR_NOT_AVAILABLE));
  } else if (secondsLeft == 0) {
    snprintf(buf, n, "%s", tr(STR_NOW));
  } else {
    formatDuration(buf, n, static_cast<uint32_t>(secondsLeft));
  }
}

// Label left-justified, value right-justified within [leftX, rightEdge] -- matches
// the label/value row layout used across the rest of the UI (see BaseTheme::drawSubHeader).
void drawStatRow(const GfxRenderer& renderer, int leftX, int rightEdge, int y, const char* label, const char* value,
                 EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  renderer.drawText(UI_10_FONT_ID, leftX, y, label, true, style);
  const int valueWidth = renderer.getTextWidth(UI_10_FONT_ID, value, style);
  renderer.drawText(UI_10_FONT_ID, rightEdge - valueWidth, y, value, true, style);
}

}  // namespace

void BatteryStatsActivity::onEnter() {
  Activity::onEnter();
  state = IDLE;

  liveTracker.sessionStartEpoch = APP_STATE.battSessionStartEpoch;
  liveTracker.sessionStartPct = APP_STATE.battSessionStartPct;
  liveTracker.lastSampleEpoch = APP_STATE.battLastSampleEpoch;
  liveTracker.lastSamplePct = APP_STATE.battLastSamplePct;
  liveTracker.lastCharging = APP_STATE.battLastCharging;
  liveTracker.hasSample = APP_STATE.battHasSample;

  requestUpdate();
}

void BatteryStatsActivity::onExit() { Activity::onExit(); }

void BatteryStatsActivity::adjustThreshold(int delta) {
  int v = static_cast<int>(SETTINGS.lowBatteryThresholdPercent) + delta;
  if (v < CrossPointSettings::LOW_BATTERY_THRESHOLD_MIN) v = CrossPointSettings::LOW_BATTERY_THRESHOLD_MIN;
  if (v > CrossPointSettings::LOW_BATTERY_THRESHOLD_MAX) v = CrossPointSettings::LOW_BATTERY_THRESHOLD_MAX;
  SETTINGS.lowBatteryThresholdPercent = static_cast<uint8_t>(v);
  requestUpdate();
}

void BatteryStatsActivity::scanLog() {
  logTracker = BatterySessionTracker();

  HalFile f = Storage.open(ReadingLogger::logPath(), O_RDONLY);
  if (!f) return;

  static constexpr size_t CHUNK_SIZE = 256;
  static constexpr size_t MAX_LINE = 64;
  char chunk[CHUNK_SIZE];
  char lineBuf[MAX_LINE];
  size_t lineLen = 0;
  size_t chunkCount = 0;

  int n;
  while ((n = f.read(chunk, CHUNK_SIZE)) > 0) {
    for (int i = 0; i < n; i++) {
      const char c = chunk[i];
      if (c == '\n') {
        lineBuf[lineLen] = '\0';
        if (lineLen > 0) parseLogLine(lineBuf, logTracker);
        lineLen = 0;
      } else if (lineLen < MAX_LINE - 1) {
        lineBuf[lineLen++] = c;
      }
    }
    if (++chunkCount % 8 == 0) vTaskDelay(1);  // yield periodically for large logs
  }
  if (lineLen > 0) {
    lineBuf[lineLen] = '\0';
    parseLogLine(lineBuf, logTracker);
  }
  f.close();
}

void BatteryStatsActivity::beginVerify() {
  {
    RenderLock lock(*this);
    state = VERIFYING;
  }
  requestUpdateAndWait();
  scanLog();
  state = VERIFIED;
  requestUpdate();
}

void BatteryStatsActivity::loop() {
  if (state == VERIFYING) return;  // beginVerify() runs synchronously to completion

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    goBack();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    adjustThreshold(-1);
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    adjustThreshold(1);
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    beginVerify();
    return;
  }
}

void BatteryStatsActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BATTERY_STATS));

  const int leftX = metrics.contentSidePadding;
  const int rightEdge = pageWidth - metrics.contentSidePadding;
  int y = metrics.topPadding + metrics.headerHeight + 30;
  constexpr int LINE_H = 28;
  char value[24];

  formatRate(value, sizeof(value), liveTracker.avgDischargePctPerHour());
  drawStatRow(renderer, leftX, rightEdge, y, tr(STR_AVG_DISCHARGE_RATE), value);
  y += LINE_H;

  formatEta(value, sizeof(value), liveTracker.estimatedSecondsLeft(SETTINGS.lowBatteryThresholdPercent));
  drawStatRow(renderer, leftX, rightEdge, y, tr(STR_EST_TIME_LEFT), value);
  y += LINE_H;

  formatDuration(value, sizeof(value), liveTracker.totalReadSeconds());
  drawStatRow(renderer, leftX, rightEdge, y, tr(STR_TOTAL_READ_TIME), value);
  y += LINE_H;

  snprintf(value, sizeof(value), "%u%%", SETTINGS.lowBatteryThresholdPercent);
  drawStatRow(renderer, leftX, rightEdge, y, tr(STR_LOW_BATTERY_THRESHOLD), value, EpdFontFamily::BOLD);
  y += LINE_H + 10;

  if (state == VERIFYING) {
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_VERIFYING));
  } else if (state == VERIFIED) {
    renderer.drawText(UI_10_FONT_ID, leftX, y, tr(STR_FROM_LOG), true, EpdFontFamily::BOLD);
    y += LINE_H;

    formatRate(value, sizeof(value), logTracker.avgDischargePctPerHour());
    drawStatRow(renderer, leftX, rightEdge, y, tr(STR_AVG_DISCHARGE_RATE), value);
    y += LINE_H;

    formatDuration(value, sizeof(value), logTracker.totalReadSeconds());
    drawStatRow(renderer, leftX, rightEdge, y, tr(STR_TOTAL_READ_TIME), value);
  }

  if (state != VERIFYING) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_VERIFY_FROM_LOG), "-", "+");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
