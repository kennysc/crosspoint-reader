#include "HalPowerManager.h"

#include <BoardConfig.h>
#include <Logging.h>
#include <PowerManager.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <soc/soc_caps.h>

#include <algorithm>
#include <cassert>

#include "HalClock.h"
#include "HalGPIO.h"
#include "HalStorage.h"
#include <Memory.h>

#if FREEINK_DEVICE_PAPERMONO
#include <M5Pm1.h>
#endif

HalPowerManager powerManager;  // Singleton instance

// GPIO13 controls the X4 battery latch and the X3 SD power rail on the C3
// Xteink boards. Other boards use it for unrelated signals, including the
// X4 Pro display chip select.
static constexpr gpio_num_t XTEINK_C3_GPIO13 = GPIO_NUM_13;

void HalPowerManager::begin() {
  if (BoardConfig::ACTIVE.batteryAdc >= 0) {
    pinMode(BoardConfig::ACTIVE.batteryAdc, INPUT);
  }
  normalFreq = getCpuFrequencyMhz();
  modeMutex = xSemaphoreCreateMutex();
  assert(modeMutex != nullptr);
}

void HalPowerManager::setPowerSaving(bool enabled) {
  if (normalFreq <= 0) {
    return;  // invalid state
  }

  auto wifiMode = WiFi.getMode();
  if (wifiMode != WIFI_MODE_NULL) {
    // Wifi is active, force disabling power saving
    enabled = false;
  }

  // Note: We don't use mutex here to avoid too much overhead,
  // it's not very important if we read a slightly stale value for currentLockMode
  const LockMode mode = currentLockMode;

  if (mode == None && enabled && !isLowPower) {
    LOG_DBG("PWR", "Going to low-power mode");
    if (!setCpuFrequencyMhz(LOW_POWER_FREQ)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", LOW_POWER_FREQ);
      return;
    }
    isLowPower = true;

  } else if ((!enabled || mode != None) && isLowPower) {
    LOG_DBG("PWR", "Restoring normal CPU frequency");
    if (!setCpuFrequencyMhz(normalFreq)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", normalFreq);
      return;
    }
    isLowPower = false;
  }

  // Otherwise, no change needed
}

void HalPowerManager::startDeepSleep(HalGPIO& gpio) const {
#ifdef ENABLE_SERIAL_LOG
  // Tear down HWCDC so the host sees a clean disconnect and the peripheral
  // doesn't hold power domains that interfere with USB-powered GPIO wake.
  // logSerial is the raw HWCDC reference; Serial is the MySerialImpl proxy
  // (which doesn't expose end()).
  logSerial.end();
#endif

#if !SOC_PM_SUPPORT_EXT1_WAKEUP
  if (gpio.isXteinkDevice()) {
    // GPIO13 gates the battery MOSFET on both Xteink C3 boards; driving it low
    // is the battery power-off (the SDK wake source still handles USB power).
    // Release any surviving pad hold first: hold_en survives deep sleep via
    // the SDK's deepSleep() (esp_sleep_config_gpio_isolate +
    // gpio_deep_sleep_hold_en), and a held pad silently ignores the drive.
    gpio_hold_dis(XTEINK_C3_GPIO13);
    gpio_set_direction(XTEINK_C3_GPIO13, GPIO_MODE_OUTPUT);
    gpio_set_level(XTEINK_C3_GPIO13, 0);
    gpio_hold_en(XTEINK_C3_GPIO13);
  }
#endif

  // Hold every configured power-latch pin HIGH through deep sleep. These are
  // keep-alive enables (the X4 Pro's master peripheral rail on GPIO1, the
  // Sticky's PWR_HOLD/PWR_LOCK): deepSleep() isolates all pads
  // (esp_sleep_config_gpio_isolate), so a latch without an armed hold loses its
  // output driver and floats — on the X4 Pro the latch drops as soon as
  // external power leaves (serial/pogo adapter unplugged), and the next power-
  // button press cold-boots instead of fast-waking. holdPowerRails() asserted
  // the latches at boot but arms no sleep hold; arm it here instead. Skips
  // XTEINK_C3_GPIO13: it IS power.latch0 on the C3 Xteink boards, where the
  // block above drives it LOW on purpose (battery power-off).
  for (const int8_t pin : {BoardConfig::ACTIVE.power.latch0, BoardConfig::ACTIVE.power.latch1}) {
    if (pin < 0 || static_cast<gpio_num_t>(pin) == XTEINK_C3_GPIO13) continue;
    const auto g = static_cast<gpio_num_t>(pin);
    // Release any surviving pad hold first: a held pad silently ignores the
    // drive below (same trap as the GPIO13 block above).
    gpio_hold_dis(g);
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
    gpio_hold_en(g);
  }

  // Cut the gated peripheral rails (touch/SD/EPD on boards like the Sticky) and
  // hold the enables off through deep sleep — otherwise the GT911 and SD card
  // stay powered all through "off" and drain the battery. No-op on boards with
  // no switched rails (X4/X3). Trade-off: no touch-to-wake; wake is the power
  // button. Must run after display.deepSleep() so the panel controller gets its
  // deep-sleep command while its rail is still up (enterDeepSleep() in main.cpp
  // guarantees that ordering).
  freeink::PowerManager::powerDownRailsForSleep();

#if FREEINK_DEVICE_PAPERMONO
  // Its power button is behind the M5PM1 PMIC rather than an ESP GPIO, so
  // normal GPIO deep sleep would have no wake source. Ask the PMIC to shut the
  // device down; a button click then restarts it through a cold boot.
  if (freeink::m5pm1::requestShutdown()) {
    delay(1000);  // allow the PMIC firmware time to drop power
  }
#endif

  // Waits for the power button to be physically released (so holding it doesn't
  // immediately wake the device again), then arms the wake source and sleeps.
  freeink::PowerManager::deepSleepUntilPowerButton();
}

uint16_t HalPowerManager::getBatteryPercentage() const {
  static const BatteryMonitor battery;
  if (BoardConfig::ACTIVE.batteryGauge.gaugeAddr != 0) {
    const unsigned long now = millis();
    const bool firstPoll = _batteryLastPollMs == 0;
    if (!firstPoll && (now - _batteryLastPollMs) < BATTERY_POLL_MS) {
      return _batteryCachedPercent;
    }
    _batteryLastPollMs = now;

    // Voltage mode: the gauge's own SoC register has proven inaccurate on some
    // units, so derive percentage from its raw voltage register against a
    // user-calibrated curve instead. See CrossPointSettings::batteryPercentMode.
    if (_useVoltagePercentMode) {
      const uint16_t mv = battery.readMillivolts();
      _batteryCachedPercent = computeVoltagePercent(mv);
      return _batteryCachedPercent;
    }

    uint16_t percent = 0;
    if (!battery.readPercentageChecked(percent)) {
      return _batteryCachedPercent;
    }
    _batteryCachedPercent = percent;
    return _batteryCachedPercent;
  }

  // smooth the battery %.
  if (_batteryCachedPercent == 0) {
    _batteryCachedPercent = 10 * battery.readPercentage();
  } else {
    _batteryCachedPercent = (_batteryCachedPercent * 9 + battery.readPercentage() * 10) / 10;
  }
  return _batteryCachedPercent / 10;
}

BatteryMonitor::Status HalPowerManager::getBatteryStatus() const {
  static const BatteryMonitor battery;
  const unsigned long now = millis();
  if (_batteryStatusLastPollMs != 0 && (now - _batteryStatusLastPollMs) < BATTERY_POLL_MS) {
    return _batteryStatusCached;
  }
  _batteryStatusLastPollMs = now;
  _batteryStatusCached = battery.readStatus();

  // Voltage mode: override the gauge's SoC-derived percentage with one computed
  // from its raw voltage against the user-calibrated curve, matching
  // getBatteryPercentage(). See CrossPointSettings::batteryPercentMode.
  if (BoardConfig::ACTIVE.batteryGauge.gaugeAddr != 0 && _useVoltagePercentMode &&
      _batteryStatusCached.millivoltsKnown) {
    _batteryStatusCached.percentage = computeVoltagePercent(_batteryStatusCached.millivolts);
    _batteryStatusCached.percentageKnown = true;
  }
  return _batteryStatusCached;
}

uint16_t HalPowerManager::computeVoltagePercent(const uint16_t mv) const {
  // 1) Raw 1%-resolution value via linear interpolation between the bracketing
  // curve notches.
  uint16_t raw;
  if (mv >= _voltageCurveMv[10]) {
    raw = 100;
  } else if (mv <= _voltageCurveMv[0]) {
    raw = 0;
  } else {
    uint8_t i = 1;
    while (i < 10 && mv >= _voltageCurveMv[i]) ++i;
    const uint16_t lo = _voltageCurveMv[i - 1];
    const uint16_t hi = _voltageCurveMv[i];
    if (hi <= lo) {
      // Non-ascending/degenerate segment (e.g. mid-edit in the calibration
      // screen): fall back to the floor notch rather than dividing.
      raw = static_cast<uint16_t>((i - 1) * 10);
    } else {
      const uint32_t span = hi - lo;
      const uint32_t offset = mv - lo;
      raw = static_cast<uint16_t>((i - 1) * 10 + (offset * 10) / span);
    }
  }

  // 2) Debounce: a change (either direction) must hold continuously for
  // VOLTAGE_PERCENT_CHANGE_DEBOUNCE_MS before it is shown. Any tick where raw
  // returns to the currently displayed value, or reverses direction, clears
  // the pending timer.
  const unsigned long now = millis();
  if (_voltageDisplayPercent > 100) {
    _voltageDisplayPercent = raw;
    _voltagePendingSinceMs = 0;
    return _voltageDisplayPercent;
  }
  if (raw == _voltageDisplayPercent) {
    _voltagePendingSinceMs = 0;
    return _voltageDisplayPercent;
  }
  const bool pendingIncrease = raw > _voltageDisplayPercent;
  if (_voltagePendingSinceMs == 0 || pendingIncrease != _voltagePendingIsIncrease) {
    _voltagePendingSinceMs = now;
    _voltagePendingIsIncrease = pendingIncrease;
  } else if (now - _voltagePendingSinceMs >= VOLTAGE_PERCENT_CHANGE_DEBOUNCE_MS) {
    _voltageDisplayPercent = raw;
    _voltagePendingSinceMs = now;  // restart in case the trend keeps going
  }
  return _voltageDisplayPercent;
}

bool HalPowerManager::hasGaugeBackend() const {
  static const BatteryMonitor battery;
  return battery.hasGaugeBackend();
}

void HalPowerManager::setBatteryPercentMode(const bool useVoltageMode, const uint16_t (&curveMv)[11]) {
  const bool modeChanged = useVoltageMode != _useVoltagePercentMode;
  const bool curveChanged = !std::equal(std::begin(curveMv), std::end(curveMv), std::begin(_voltageCurveMv));
  _useVoltagePercentMode = useVoltageMode;
  std::copy(std::begin(curveMv), std::end(curveMv), std::begin(_voltageCurveMv));
  if (modeChanged || curveChanged) {
    // Curve/mode actually changed (mid-edit in the calibration screen, or a
    // Gauge/Voltage flip) -- drop the debounce baseline computed under the
    // old curve so the next read isn't held back by it.
    _voltageDisplayPercent = 101;
    _voltagePendingSinceMs = 0;
  }
}

void HalPowerManager::dumpBq27220DiagnosticsToSd(const bool deviceIsX3) const {
  if (!deviceIsX3) {
    return;
  }

  // Bring the gauge I2C bus up through the existing HAL surface -- the result is
  // unused here, this call's only purpose is BatteryMonitor::ensureWire()'s side
  // effect inside the freeink-sdk submodule.
  (void)getBatteryStatus();

  X3GPIO::Bq27220Diagnostics diag;
  X3GPIO::readBQ27220Diagnostics(diag);

  X3GPIO::Bq27220IdentityDiagnostics identity;
  X3GPIO::readBQ27220IdentityDiagnostics(identity);

  Rtc::DateTime dt;
  const bool haveTime = halClock.isAvailable() && halClock.getDateTime(dt);

  Storage.mkdir("/.crosspoint");
  HalFile file;
  if (!Storage.openFileForWrite("PWR", "/.crosspoint/bq27220_dump.txt", file)) {
    LOG_ERR("PWR", "Failed to open bq27220_dump.txt for writing");
    return;
  }

  if (haveTime) {
    file.printf("Timestamp: %04u-%02u-%02u %02u:%02u:%02u\n", dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
  }
  file.printf("Voltage_mV: %ld\n", static_cast<long>(diag.voltageMv));
  file.printf("StateOfCharge_pct: %ld\n", static_cast<long>(diag.socPercent));
  file.printf("FullChargeCapacity_mAh: %ld\n", static_cast<long>(diag.fullChargeCapacityMah));
  file.printf("RemainingCapacity_mAh: %ld\n", static_cast<long>(diag.remainingCapacityMah));
  file.printf("DesignCapacity_mAh: %ld\n", static_cast<long>(diag.designCapacityMah));
  if (diag.operationStatusRaw >= 0) {
    file.printf("OperationStatus_raw: 0x%04X\n", static_cast<unsigned>(diag.operationStatusRaw));
  } else {
    file.printf("OperationStatus_raw: -1\n");
  }
  // Independent of the Data Memory access issue -- see BQ27220_CTRL_DEVICE_NUMBER
  // comment in HalGPIO.h. Expect 0x0220 on a genuine BQ27220.
  if (identity.deviceNumberReadOk) {
    file.printf("DeviceNumber_raw: 0x%04X\n", static_cast<unsigned>(identity.deviceNumberRaw));
  } else {
    file.printf("DeviceNumber_raw: -1\n");
  }

  // Raw hex dump of the full command address space (see BQ27220_RAW_DUMP_LEN
  // comment in HalGPIO.h) for offline decoding: the named fields above only
  // cover the registers this codebase already knows about, and won't show
  // where a clone chip's actual layout diverges from the TRM.
  auto rawRegs = makeUniqueNoThrow<uint8_t[]>(X3GPIO::BQ27220_RAW_DUMP_LEN);
  if (rawRegs) {
    X3GPIO::readBQ27220RawRegisters(rawRegs.get(), X3GPIO::BQ27220_RAW_DUMP_LEN);
    file.printf("\nRaw register dump (addr 0x00-0x%02X, 0xFF marks a failed I2C read):\n",
                X3GPIO::BQ27220_RAW_DUMP_LEN - 1);
    for (uint8_t row = 0; row < X3GPIO::BQ27220_RAW_DUMP_LEN; row += 16) {
      file.printf("%02X:", row);
      for (uint8_t col = 0; col < 16; col++) {
        file.printf(" %02X", rawRegs[row + col]);
      }
      file.printf("\n");
    }
  } else {
    LOG_ERR("PWR", "OOM: %d bytes for BQ27220 raw dump buffer", X3GPIO::BQ27220_RAW_DUMP_LEN);
  }

  LOG_INF("PWR", "Wrote BQ27220 diagnostic dump to SD");
}

namespace {
// Polls OperationStatus() until CFGUPDATE reaches `wantSet`, or ~1s elapses
// (TRM S2.2.21/S6.1 steps 4 and 15).
bool waitForBq27220CfgUpdate(const bool wantSet) {
  const unsigned long deadline = millis() + 1000;
  do {
    uint16_t status = 0;
    if (X3GPIO::readBQ27220OperationStatus(&status)) {
      if (((status & BQ27220_OP_STATUS_CFGUPDATE_MASK) != 0) == wantSet) {
        return true;
      }
    }
    delay(20);
  } while (millis() < deadline);
  return false;
}
}  // namespace

void HalPowerManager::reprogramBq27220Capacity(const bool deviceIsX3, const uint16_t designCapacityMah,
                                                const uint16_t fullChargeCapacityMah) const {
  if (!deviceIsX3) {
    return;
  }

  (void)getBatteryStatus();  // bring up the gauge I2C bus, see dumpBq27220DiagnosticsToSd

  // Mirrors every step to /.crosspoint/bq27220_reprogram_log.txt in addition to
  // LOG_INF/LOG_ERR, since this device may not have a serial connection --
  // the SD card is the only way to see which step failed if one does.
  Storage.mkdir("/.crosspoint");
  HalFile logFile;
  const bool haveLogFile = Storage.openFileForWrite("PWR", "/.crosspoint/bq27220_reprogram_log.txt", logFile);
  if (!haveLogFile) {
    LOG_ERR("PWR", "BQ27220 reprogram: failed to open reprogram log file (continuing without it)");
  }
  auto logStep = [&](const char* message) {
    LOG_INF("PWR", "%s", message);
    if (haveLogFile) {
      logFile.printf("%s\n", message);
    }
  };

  // Sized to hold "Write FullChargeCapacity/DesignCapacity: " plus the
  // longest writeBQ27220DataMemoryField() failDetail message below.
  char msg[192];
  snprintf(msg, sizeof(msg), "Target: DesignCapacity=%u FullChargeCapacity=%u mAh", designCapacityMah,
           fullChargeCapacityMah);
  logStep(msg);

  // UNSEALED (this gauge's power-on default, see docs/bq27220-x3-fuel-gauge-findings.md
  // Finding 3) does not permit Data Memory writes -- unseal to FULL ACCESS first.
  const bool unsealed = X3GPIO::writeBQ27220Control(BQ27220_CTRL_FULL_ACCESS_KEY) &&
                         X3GPIO::writeBQ27220Control(BQ27220_CTRL_FULL_ACCESS_KEY);
  logStep(unsealed ? "FULL ACCESS unseal: ok" : "FULL ACCESS unseal: FAILED");
  if (!unsealed) {
    return;
  }
  delay(10);

  // "ok" above only means the key writes ACKed at the I2C bus level -- it
  // does NOT confirm the gauge actually elevated permission. Every dump
  // collected so far has shown SEC[1:0]=10 (Unsealed), never 01 (Full
  // Access), even right after this exact unseal sequence. TRM S6.1 step 2
  // requires Full Access for Data Memory writes, so if this key exchange
  // isn't actually working, every write attempt below is doomed regardless
  // of checksum/length/timing correctness. Verify directly instead of
  // assuming.
  uint16_t opStatusAfterUnseal = 0;
  if (X3GPIO::readBQ27220OperationStatus(&opStatusAfterUnseal)) {
    const uint8_t sec = (opStatusAfterUnseal >> 1) & 0x3;
    const char* secName;
    if (sec == 0b01) {
      secName = "Full Access";
    } else if (sec == 0b10) {
      secName = "Unsealed (NOT Full Access)";
    } else if (sec == 0b11) {
      secName = "Sealed";
    } else {
      secName = "unknown(00)";
    }
    snprintf(msg, sizeof(msg), "Post-unseal OperationStatus SEC[1:0]=%u%u -> %s", (sec >> 1) & 1, sec & 1, secName);
    logStep(msg);
  } else {
    logStep("Post-unseal OperationStatus read failed -- could not verify SEC state");
  }

  // Observed intermittently flaky in practice (this exact write succeeds on
  // some boots, silently doesn't take on others) -- retry a few times with a
  // short backoff rather than treating one miss as a hard failure.
  constexpr uint8_t kMaxCfgUpdateAttempts = 3;
  bool enteredCfgUpdate = false;
  for (uint8_t attempt = 1; attempt <= kMaxCfgUpdateAttempts && !enteredCfgUpdate; attempt++) {
    if (!X3GPIO::writeBQ27220Control(BQ27220_CTRL_ENTER_CFG_UPDATE)) {
      snprintf(msg, sizeof(msg), "ENTER_CFG_UPDATE attempt %u: I2C write failed", attempt);
      logStep(msg);
      delay(50);
      continue;
    }
    enteredCfgUpdate = waitForBq27220CfgUpdate(true);
    if (!enteredCfgUpdate) {
      snprintf(msg, sizeof(msg), "ENTER_CFG_UPDATE attempt %u: write ok, CFGUPDATE bit never set within 1s", attempt);
      logStep(msg);
      delay(50);
    }
  }
  logStep(enteredCfgUpdate ? "ENTER_CFG_UPDATE: ok" : "ENTER_CFG_UPDATE: FAILED after retries");
  if (!enteredCfgUpdate) {
    return;
  }

  // 0 means "skip this field" -- lets a build attempt just the single-block
  // FullChargeCapacity write (no boundary crossing) before risking the
  // two-block DesignCapacity write. A failure's diagnostic detail (full
  // before/after block hex, not just the mismatched offsets) can run past
  // 300 bytes, so it's heap-allocated per the project's <256B stack-local
  // rule, with a smaller stack fallback if that allocation fails.
  auto writeField = [&](uint16_t address, uint16_t valueMah, const char* fieldName) -> bool {
    constexpr size_t kDetailBufSize = 512;
    auto heapDetail = makeUniqueNoThrow<char[]>(kDetailBufSize);
    char smallFallback[128] = "";
    char* detailBuf = heapDetail ? heapDetail.get() : smallFallback;
    const size_t detailBufLen = heapDetail ? kDetailBufSize : sizeof(smallFallback);
    if (!heapDetail) {
      LOG_ERR("PWR", "OOM: %u bytes for BQ27220 write diagnostic buffer, using smaller fallback",
              static_cast<unsigned>(kDetailBufSize));
    }
    detailBuf[0] = '\0';
    // verboseDiagnostics=false: its extra reads were found to corrupt this
    // exact write on real hardware -- see writeBQ27220DataMemoryField() comment.
    const bool ok = X3GPIO::writeBQ27220DataMemoryField(address, valueMah, detailBuf, detailBufLen, false);
    snprintf(msg, sizeof(msg), "Write %s: %s", fieldName, ok ? "ok" : "FAILED, detail follows:");
    logStep(msg);
    if (!ok) {
      logStep(detailBuf);
    }
    return ok;
  };

  bool wroteFcc = true;
  if (fullChargeCapacityMah != 0) {
    wroteFcc = writeField(BQ27220_DM_ADDR_FULL_CHARGE_CAPACITY, fullChargeCapacityMah, "FullChargeCapacity");
  } else {
    logStep("Write FullChargeCapacity: skipped");
  }

  bool wroteDesign = true;
  if (designCapacityMah != 0) {
    wroteDesign = writeField(BQ27220_DM_ADDR_DESIGN_CAPACITY, designCapacityMah, "DesignCapacity");
  } else {
    logStep("Write DesignCapacity: skipped");
  }

  // EXIT_CFG_UPDATE_REINIT (rather than plain EXIT_CFG_UPDATE) forces the gauge
  // to reseed RemainingCapacity/FullChargeCapacity from the new DesignCapacity.
  const bool exitedCfgUpdate = X3GPIO::writeBQ27220Control(BQ27220_CTRL_EXIT_CFG_UPDATE_REINIT);
  logStep(exitedCfgUpdate ? "EXIT_CFG_UPDATE_REINIT write: ok" : "EXIT_CFG_UPDATE_REINIT write: FAILED");

  const bool cfgUpdateCleared = waitForBq27220CfgUpdate(false);
  logStep(cfgUpdateCleared ? "CFGUPDATE cleared: ok" : "CFGUPDATE cleared: FAILED -- gauging may remain suspended");

  snprintf(msg, sizeof(msg), "Sequence complete: FCC=%d Design=%d exit=%d cleared=%d -- verify via bq27220_dump.txt",
           wroteFcc, wroteDesign, exitedCfgUpdate, cfgUpdateCleared);
  logStep(msg);
}

void HalPowerManager::testBq27220DataMemoryWrite(const bool deviceIsX3, const uint16_t address,
                                                  const uint16_t value) const {
  if (!deviceIsX3) {
    return;
  }

  (void)getBatteryStatus();  // bring up the gauge I2C bus, see dumpBq27220DiagnosticsToSd

  Storage.mkdir("/.crosspoint");
  HalFile logFile;
  const bool haveLogFile = Storage.openFileForWrite("PWR", "/.crosspoint/bq27220_reprogram_log.txt", logFile);
  if (!haveLogFile) {
    LOG_ERR("PWR", "BQ27220 test write: failed to open reprogram log file (continuing without it)");
  }
  auto logStep = [&](const char* message) {
    LOG_INF("PWR", "%s", message);
    if (haveLogFile) {
      logFile.printf("%s\n", message);
    }
  };

  char msg[192];
  snprintf(msg, sizeof(msg), "Sanity test: write 0x%04X to Data Memory address 0x%04X (write mechanism check, see "
                              "docs/bq27220-x3-fuel-gauge-findings.md)",
           value, address);
  logStep(msg);

  const bool unsealed = X3GPIO::writeBQ27220Control(BQ27220_CTRL_FULL_ACCESS_KEY) &&
                         X3GPIO::writeBQ27220Control(BQ27220_CTRL_FULL_ACCESS_KEY);
  logStep(unsealed ? "FULL ACCESS unseal: ok" : "FULL ACCESS unseal: FAILED");
  if (!unsealed) {
    return;
  }
  delay(10);

  // See the matching comment in reprogramBq27220Capacity(): "ok" above only
  // confirms the bus-level ACK, not that permission actually elevated.
  uint16_t opStatusAfterUnseal = 0;
  if (X3GPIO::readBQ27220OperationStatus(&opStatusAfterUnseal)) {
    const uint8_t sec = (opStatusAfterUnseal >> 1) & 0x3;
    const char* secName;
    if (sec == 0b01) {
      secName = "Full Access";
    } else if (sec == 0b10) {
      secName = "Unsealed (NOT Full Access)";
    } else if (sec == 0b11) {
      secName = "Sealed";
    } else {
      secName = "unknown(00)";
    }
    snprintf(msg, sizeof(msg), "Post-unseal OperationStatus SEC[1:0]=%u%u -> %s", (sec >> 1) & 1, sec & 1, secName);
    logStep(msg);
  } else {
    logStep("Post-unseal OperationStatus read failed -- could not verify SEC state");
  }

  constexpr uint8_t kMaxCfgUpdateAttempts = 3;
  bool enteredCfgUpdate = false;
  for (uint8_t attempt = 1; attempt <= kMaxCfgUpdateAttempts && !enteredCfgUpdate; attempt++) {
    if (!X3GPIO::writeBQ27220Control(BQ27220_CTRL_ENTER_CFG_UPDATE)) {
      snprintf(msg, sizeof(msg), "ENTER_CFG_UPDATE attempt %u: I2C write failed", attempt);
      logStep(msg);
      delay(50);
      continue;
    }
    enteredCfgUpdate = waitForBq27220CfgUpdate(true);
    if (!enteredCfgUpdate) {
      snprintf(msg, sizeof(msg), "ENTER_CFG_UPDATE attempt %u: write ok, CFGUPDATE bit never set within 1s", attempt);
      logStep(msg);
      delay(50);
    }
  }
  logStep(enteredCfgUpdate ? "ENTER_CFG_UPDATE: ok" : "ENTER_CFG_UPDATE: FAILED after retries");
  if (!enteredCfgUpdate) {
    return;
  }

  constexpr size_t kDetailBufSize = 512;
  auto heapDetail = makeUniqueNoThrow<char[]>(kDetailBufSize);
  char smallFallback[128] = "";
  char* detailBuf = heapDetail ? heapDetail.get() : smallFallback;
  const size_t detailBufLen = heapDetail ? kDetailBufSize : sizeof(smallFallback);
  if (!heapDetail) {
    LOG_ERR("PWR", "OOM: %u bytes for BQ27220 write diagnostic buffer, using smaller fallback",
            static_cast<unsigned>(kDetailBufSize));
  }
  detailBuf[0] = '\0';
  // verboseDiagnostics=false: testing whether removing its extra reads (found
  // to leak into the BlockData buffer on the previous attempt) lets this
  // write actually commit -- see writeBQ27220DataMemoryField() comment.
  const bool wroteOk = X3GPIO::writeBQ27220DataMemoryField(address, value, detailBuf, detailBufLen, false);
  logStep(wroteOk ? "Test write: ok" : "Test write: FAILED, detail follows:");
  if (!wroteOk) {
    logStep(detailBuf);
  }

  const bool exitedCfgUpdate = X3GPIO::writeBQ27220Control(BQ27220_CTRL_EXIT_CFG_UPDATE_REINIT);
  logStep(exitedCfgUpdate ? "EXIT_CFG_UPDATE_REINIT write: ok" : "EXIT_CFG_UPDATE_REINIT write: FAILED");
  const bool cfgUpdateCleared = waitForBq27220CfgUpdate(false);
  logStep(cfgUpdateCleared ? "CFGUPDATE cleared: ok" : "CFGUPDATE cleared: FAILED -- gauging may remain suspended");

  snprintf(msg, sizeof(msg), "Sanity test complete: write=%d -- verify via bq27220_dump.txt", wroteOk);
  logStep(msg);
}

void HalPowerManager::testBq27220TiHibernateExample(const bool deviceIsX3) const {
  if (!deviceIsX3) {
    return;
  }

  (void)getBatteryStatus();  // bring up the gauge I2C bus, see dumpBq27220DiagnosticsToSd

  Storage.mkdir("/.crosspoint");
  HalFile logFile;
  const bool haveLogFile = Storage.openFileForWrite("PWR", "/.crosspoint/bq27220_reprogram_log.txt", logFile);
  if (!haveLogFile) {
    LOG_ERR("PWR", "BQ27220 TI-example test: failed to open reprogram log file (continuing without it)");
  }
  auto logStep = [&](const char* message) {
    LOG_INF("PWR", "%s", message);
    if (haveLogFile) {
      logFile.printf("%s\n", message);
    }
  };

  char msg[192];
  logStep("TRM S4.6 Note literal replication test: Hibernate I -> 0 at Data Memory 0x9221");

  const bool unsealed = X3GPIO::writeBQ27220Control(BQ27220_CTRL_FULL_ACCESS_KEY) &&
                         X3GPIO::writeBQ27220Control(BQ27220_CTRL_FULL_ACCESS_KEY);
  logStep(unsealed ? "FULL ACCESS unseal: ok" : "FULL ACCESS unseal: FAILED");
  if (!unsealed) {
    return;
  }
  delay(10);

  uint16_t opStatusAfterUnseal = 0;
  if (X3GPIO::readBQ27220OperationStatus(&opStatusAfterUnseal)) {
    const uint8_t sec = (opStatusAfterUnseal >> 1) & 0x3;
    snprintf(msg, sizeof(msg), "Post-unseal OperationStatus=0x%04X SEC[1:0]=%u%u", opStatusAfterUnseal,
             (sec >> 1) & 1, sec & 1);
    logStep(msg);
  }

  // Step 1 (TRM S4.6 Note, literal): "Write 0x0090 to 0x3E (enter CONFIG UPDATE
  // mode), and wait 1100 ms" -- targets 0x3E directly, not Control() (0x00) as
  // documented everywhere else in the TRM. Testing literally, as one combined
  // 2-byte write (low byte first, per this codebase's established byte order).
  const uint8_t step1[2] = {0x90, 0x00};
  const bool step1Ok = X3GPIO::writeI2CBlock(I2C_ADDR_BQ27220, BQ27220_DM_ADDR_LSB_REG, step1, 2);
  logStep(step1Ok ? "Step1 (0x0090 -> 0x3E, combined write): ok" : "Step1 (0x0090 -> 0x3E): FAILED");
  delay(1100);

  uint16_t opStatusAfterStep1 = 0;
  if (X3GPIO::readBQ27220OperationStatus(&opStatusAfterStep1)) {
    snprintf(msg, sizeof(msg), "After step1+1100ms: OperationStatus=0x%04X (CFGUPDATE %s)", opStatusAfterStep1,
             (opStatusAfterStep1 & BQ27220_OP_STATUS_CFGUPDATE_MASK) ? "SET" : "clear");
    logStep(msg);
  } else {
    logStep("After step1: OperationStatus read failed");
  }

  // Step 2 (literal): "Write (hex) 21 92 00, starting at 0x3E" -- one combined
  // 3-byte transaction: address low=0x21, high=0x92 (-> Data Memory address
  // 0x9221, little-endian), then the first BlockData() byte = 0x00.
  const uint8_t step2[3] = {0x21, 0x92, 0x00};
  const bool step2Ok = X3GPIO::writeI2CBlock(I2C_ADDR_BQ27220, BQ27220_DM_ADDR_LSB_REG, step2, 3);
  logStep(step2Ok ? "Step2 (21 92 00 -> 0x3E, combined write): ok" : "Step2: FAILED");

  // Step 3 (literal): "Write (hex) 4C 05, starting at 0x61" -- testing exactly
  // as documented, even though Table 2-1 names 0x60=MACDataSum()/checksum and
  // 0x61=MACDataLen()/length (checksum then length), while this targets 0x61
  // first as one combined 2-byte write.
  const uint8_t step3[2] = {0x4C, 0x05};
  const bool step3Ok = X3GPIO::writeI2CBlock(I2C_ADDR_BQ27220, BQ27220_BLOCKDATA_LEN_REG, step3, 2);
  logStep(step3Ok ? "Step3 (4C 05 -> 0x61, combined write): ok" : "Step3: FAILED");

  delay(5);

  // Verify: re-address 0x9221 and read back BlockData() -- offset 0 should now
  // read 0x00 if this exact procedure actually committed the write.
  uint8_t verifyBlock[32];
  if (X3GPIO::readBQ27220BlockDataAt(0x9221, verifyBlock)) {
    snprintf(msg, sizeof(msg), "Verify @0x9221: offset0=0x%02X (want 0x00) offset1=0x%02X offset2=0x%02X",
             verifyBlock[0], verifyBlock[1], verifyBlock[2]);
    logStep(msg);
  } else {
    logStep("Verify @0x9221: block read failed");
  }

  // Step 4 (literal): "Write 0x0091 to 0x3E (exit CONFIG UPDATE reinit)."
  const uint8_t step4[2] = {0x91, 0x00};
  const bool step4Ok = X3GPIO::writeI2CBlock(I2C_ADDR_BQ27220, BQ27220_DM_ADDR_LSB_REG, step4, 2);
  logStep(step4Ok ? "Step4 (0x0091 -> 0x3E, combined write): ok" : "Step4: FAILED");

  const bool cfgUpdateCleared = waitForBq27220CfgUpdate(false);
  logStep(cfgUpdateCleared ? "CFGUPDATE cleared: ok" : "CFGUPDATE cleared: FAILED -- gauging may remain suspended");

  logStep("TRM S4.6 literal replication test complete -- verify via bq27220_dump.txt");
}

void HalPowerManager::testBq27220DirectWrite(const bool deviceIsX3, const uint16_t address,
                                              const uint16_t value) const {
  if (!deviceIsX3) {
    return;
  }

  (void)getBatteryStatus();  // bring up the gauge I2C bus, see dumpBq27220DiagnosticsToSd

  Storage.mkdir("/.crosspoint");
  HalFile logFile;
  const bool haveLogFile = Storage.openFileForWrite("PWR", "/.crosspoint/bq27220_reprogram_log.txt", logFile);
  if (!haveLogFile) {
    LOG_ERR("PWR", "BQ27220 direct-write test: failed to open reprogram log file (continuing without it)");
  }
  auto logStep = [&](const char* message) {
    LOG_INF("PWR", "%s", message);
    if (haveLogFile) {
      logFile.printf("%s\n", message);
    }
  };

  char msg[192];
  snprintf(msg, sizeof(msg), "Direct-write test (combined transactions, computed checksum): 0x%04X -> addr 0x%04X",
           value, address);
  logStep(msg);

  const bool unsealed = X3GPIO::writeBQ27220Control(BQ27220_CTRL_FULL_ACCESS_KEY) &&
                         X3GPIO::writeBQ27220Control(BQ27220_CTRL_FULL_ACCESS_KEY);
  logStep(unsealed ? "FULL ACCESS unseal: ok" : "FULL ACCESS unseal: FAILED");
  if (!unsealed) {
    return;
  }
  delay(10);

  uint16_t opStatusAfterUnseal = 0;
  if (X3GPIO::readBQ27220OperationStatus(&opStatusAfterUnseal)) {
    const uint8_t sec = (opStatusAfterUnseal >> 1) & 0x3;
    snprintf(msg, sizeof(msg), "Post-unseal OperationStatus=0x%04X SEC[1:0]=%u%u (key=0x%04X)", opStatusAfterUnseal,
             (sec >> 1) & 1, sec & 1, BQ27220_CTRL_FULL_ACCESS_KEY);
    logStep(msg);
  }

  // ENTER_CFG_UPDATE via 0x3E (combined write) -- see testBq27220TiHibernateExample():
  // confirmed reliable on the first attempt, unlike the Control()-based approach.
  const uint8_t enter[2] = {0x90, 0x00};
  const bool enterOk = X3GPIO::writeI2CBlock(I2C_ADDR_BQ27220, BQ27220_DM_ADDR_LSB_REG, enter, 2);
  logStep(enterOk ? "ENTER_CFG_UPDATE (0x0090 -> 0x3E, combined write): ok" : "ENTER_CFG_UPDATE: FAILED");
  delay(1100);

  uint16_t opStatus = 0;
  if (X3GPIO::readBQ27220OperationStatus(&opStatus)) {
    snprintf(msg, sizeof(msg), "After enter+1100ms: OperationStatus=0x%04X (CFGUPDATE %s)", opStatus,
             (opStatus & BQ27220_OP_STATUS_CFGUPDATE_MASK) ? "SET" : "clear");
    logStep(msg);
    if (!(opStatus & BQ27220_OP_STATUS_CFGUPDATE_MASK)) {
      logStep("Aborting: CFGUPDATE not set, Data Memory write would be unsafe");
      return;
    }
  } else {
    logStep("After enter: OperationStatus read failed, aborting");
    return;
  }

  constexpr size_t kDetailBufSize = 512;
  auto heapDetail = makeUniqueNoThrow<char[]>(kDetailBufSize);
  char smallFallback[128] = "";
  char* detailBuf = heapDetail ? heapDetail.get() : smallFallback;
  const size_t detailBufLen = heapDetail ? kDetailBufSize : sizeof(smallFallback);
  if (!heapDetail) {
    LOG_ERR("PWR", "OOM: %u bytes for BQ27220 write diagnostic buffer, using smaller fallback",
            static_cast<unsigned>(kDetailBufSize));
  }
  detailBuf[0] = '\0';
  const bool wroteOk = X3GPIO::writeBQ27220DataMemoryFieldDirect(address, value, detailBuf, detailBufLen);
  logStep(wroteOk ? "Direct write: ok" : "Direct write: FAILED, detail follows:");
  if (!wroteOk) {
    logStep(detailBuf);
  }

  // EXIT_CFG_UPDATE_REINIT via 0x3E (combined write), matching entry's mechanism.
  const uint8_t exit[2] = {0x91, 0x00};
  const bool exitOk = X3GPIO::writeI2CBlock(I2C_ADDR_BQ27220, BQ27220_DM_ADDR_LSB_REG, exit, 2);
  logStep(exitOk ? "EXIT_CFG_UPDATE_REINIT (0x0091 -> 0x3E, combined write): ok" : "EXIT_CFG_UPDATE_REINIT: FAILED");
  const bool cfgUpdateCleared = waitForBq27220CfgUpdate(false);
  logStep(cfgUpdateCleared ? "CFGUPDATE cleared: ok" : "CFGUPDATE cleared: FAILED -- gauging may remain suspended");

  snprintf(msg, sizeof(msg), "Direct-write test complete: write=%d -- verify via bq27220_dump.txt", wroteOk);
  logStep(msg);
}

HalPowerManager::Lock::Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  // Current limitation: only one lock at a time
  if (powerManager.currentLockMode != None) {
    LOG_ERR("PWR", "Lock already held, ignore");
    valid = false;
  } else {
    powerManager.currentLockMode = NormalSpeed;
    valid = true;
  }
  xSemaphoreGive(powerManager.modeMutex);
  if (valid) {
    // Immediately restore normal CPU frequency if currently in low-power mode
    powerManager.setPowerSaving(false);
  }
}

HalPowerManager::Lock::~Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  if (valid) {
    powerManager.currentLockMode = None;
  }
  xSemaphoreGive(powerManager.modeMutex);
}
