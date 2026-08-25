#include "BatteryStatsActivity.h"

#include <GfxRenderer.h>
#include <HalPowerManager.h>
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

// Parses one reading_log.csv row ("timestamp,battery_pct,voltage_mv,charging[,active_read_seconds]")
// and feeds it into tracker, tallying completed charge cycles and their lifetime
// active-reading/discharge totals along the way. The active_read_seconds field is a
// trusted per-row checkpoint (written by ReadingLogger at the same instant), not
// something replayed here -- the log has no per-page-turn timestamps to recompute it
// from. Silently ignores the header row and any malformed line (sscanf field-count
// mismatch below 9), since both simply fail to match the format. Rows written
// before this field existed parse as 9 fields and are tolerated for backward
// compatibility, just without contributing a trustworthy active-time checkpoint.
void parseLogLine(const char* line, BatterySessionTracker& tracker, uint32_t& completedCycles,
                  uint32_t& lifetimeActiveSeconds, uint32_t& lifetimePctDrop, char* firstEntryDate) {
  uint16_t year, mv;
  uint8_t month, day, hour, minute, second, pct;
  int charging;
  uint32_t loggedActiveSeconds = 0;
  const int fields = sscanf(line, "%hu-%hhu-%hhuT%hhu:%hhu:%hhu,%hhu,%hu,%d,%u", &year, &month, &day, &hour, &minute,
                            &second, &pct, &mv, &charging, &loggedActiveSeconds);
  if (fields != 9 && fields != 10) return;
  if (year == 0) return;  // RTC-unavailable sentinel row

  if (firstEntryDate[0] == '\0') {
    snprintf(firstEntryDate, 11, "%04u-%02u-%02u", year, month, day);
  }

  // Unknown charging reads carry forward the last known state, matching ReadingLogger's rule.
  const bool chargingBool = (charging == -1) ? tracker.lastCharging : (charging == 1);
  if (tracker.hasSample && tracker.lastCharging && !chargingBool) {
    // This row starts a new discharge session -- the session that was open going
    // into it just completed; fold its final tallies into the lifetime totals.
    completedCycles++;
    lifetimeActiveSeconds += tracker.activeReadSeconds;
    if (tracker.sessionStartPct > tracker.lastSamplePct) {
      lifetimePctDrop += tracker.sessionStartPct - tracker.lastSamplePct;
    }
  }
  tracker.observe(batteryEpochFromParts(year, month, day, hour, minute, second), pct, chargingBool);
  if (fields == 10) tracker.activeReadSeconds = loggedActiveSeconds;
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

// Average discharge rate in percent per active-reading hour, across all completed
// charge cycles in the log (not just the current session). 0 if no completed cycle
// has been observed yet.
float lifetimeAvgDischargeRate(uint32_t pctDrop, uint32_t activeSeconds) {
  if (activeSeconds == 0) return 0.0f;
  return static_cast<float>(pctDrop) / (static_cast<float>(activeSeconds) / 3600.0f);
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
  state = LOADING;

  liveTracker.sessionStartEpoch = APP_STATE.battSessionStartEpoch;
  liveTracker.sessionStartPct = APP_STATE.battSessionStartPct;
  liveTracker.lastSampleEpoch = APP_STATE.battLastSampleEpoch;
  liveTracker.lastSamplePct = APP_STATE.battLastSamplePct;
  liveTracker.lastCharging = APP_STATE.battLastCharging;
  liveTracker.hasSample = APP_STATE.battHasSample;
  liveTracker.activeReadSeconds = APP_STATE.battActiveReadSeconds;
  liveTracker.lastPageTurnEpoch = APP_STATE.battLastPageTurnEpoch;

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
  logCompletedCycles = 0;
  logLifetimeActiveSeconds = 0;
  logCompletedActiveSeconds = 0;
  logLifetimePctDrop = 0;
  logFirstEntryDate[0] = '\0';

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
        if (lineLen > 0) {
          parseLogLine(lineBuf, logTracker, logCompletedCycles, logLifetimeActiveSeconds, logLifetimePctDrop,
                       logFirstEntryDate);
        }
        lineLen = 0;
      } else if (lineLen < MAX_LINE - 1) {
        lineBuf[lineLen++] = c;
      }
    }
    if (++chunkCount % 8 == 0) vTaskDelay(1);  // yield periodically for large logs
  }
  if (lineLen > 0) {
    lineBuf[lineLen] = '\0';
    parseLogLine(lineBuf, logTracker, logCompletedCycles, logLifetimeActiveSeconds, logLifetimePctDrop,
                 logFirstEntryDate);
  }
  f.close();

  // logCompletedActiveSeconds reflects only completed cycles (the denominator for the
  // lifetime avg discharge rate); logLifetimeActiveSeconds additionally folds in the
  // still-open session's reading time so far, since that's part of the lifetime total.
  logCompletedActiveSeconds = logLifetimeActiveSeconds;
  logLifetimeActiveSeconds += logTracker.activeReadSeconds;
}

void BatteryStatsActivity::loop() {
  if (state == LOADING) {
    requestUpdateAndWait();  // paint the "Loading..." screen before blocking on the SD scan
    scanLog();
    state = READY;
    requestUpdate();
    return;
  }

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

  const auto battStatus = powerManager.getBatteryStatus();
  if (battStatus.millivoltsKnown) {
    snprintf(value, sizeof(value), "%u mV", battStatus.millivolts);
  } else {
    snprintf(value, sizeof(value), "%s", tr(STR_NOT_AVAILABLE));
  }
  drawStatRow(renderer, leftX, rightEdge, y, tr(STR_BATTERY_VOLTAGE), value);
  y += LINE_H;

  snprintf(value, sizeof(value), "%u%%", SETTINGS.lowBatteryThresholdPercent);
  drawStatRow(renderer, leftX, rightEdge, y, tr(STR_LOW_BATTERY_THRESHOLD), value, EpdFontFamily::BOLD);
  y += LINE_H + 10;

  if (state == LOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_LOADING));
  } else {
    renderer.drawText(UI_10_FONT_ID, leftX, y, tr(STR_FROM_LOG), true, EpdFontFamily::BOLD);
    y += LINE_H;

    formatRate(value, sizeof(value), logTracker.avgDischargePctPerHour());
    drawStatRow(renderer, leftX, rightEdge, y, tr(STR_AVG_DISCHARGE_RATE), value);
    y += LINE_H;

    formatDuration(value, sizeof(value), logTracker.totalReadSeconds());
    drawStatRow(renderer, leftX, rightEdge, y, tr(STR_TOTAL_READ_TIME), value);
    y += LINE_H;

    snprintf(value, sizeof(value), "%u", logCompletedCycles);
    drawStatRow(renderer, leftX, rightEdge, y, tr(STR_CHARGE_CYCLES_LOGGED), value);
    y += LINE_H;

    formatDuration(value, sizeof(value), logLifetimeActiveSeconds);
    drawStatRow(renderer, leftX, rightEdge, y, tr(STR_LIFETIME_READ_TIME), value);
    y += LINE_H;

    formatRate(value, sizeof(value), lifetimeAvgDischargeRate(logLifetimePctDrop, logCompletedActiveSeconds));
    drawStatRow(renderer, leftX, rightEdge, y, tr(STR_LIFETIME_AVG_DISCHARGE_RATE), value);
    y += LINE_H;

    snprintf(value, sizeof(value), "%s", logFirstEntryDate[0] ? logFirstEntryDate : tr(STR_NOT_AVAILABLE));
    drawStatRow(renderer, leftX, rightEdge, y, tr(STR_LOGGING_SINCE), value);
  }

  if (state != LOADING) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "-", "+");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
