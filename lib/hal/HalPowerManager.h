#pragma once

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <InputManager.h>
#include <Logging.h>
#include <freertos/semphr.h>

#include <cassert>

#include "HalGPIO.h"

class HalPowerManager;
extern HalPowerManager powerManager;  // Singleton

class HalPowerManager {
  int normalFreq = 0;  // MHz
  bool isLowPower = false;

  mutable int _batteryCachedPercent = 0;         // Last read battery percentage (0-100)
  mutable unsigned long _batteryLastPollMs = 0;  // Timestamp of last battery read in milliseconds
  mutable BatteryMonitor::Status _batteryStatusCached = {};
  mutable unsigned long _batteryStatusLastPollMs = 0;

  // Gauge-board percentage source, pushed down from CrossPointSettings each loop
  // (mirrors HalTiltSensor::update()'s SETTINGS-to-HAL push) so this HAL class
  // stays independent of app-level settings headers. False/default curve until
  // the first setBatteryPercentMode() call, matching BatteryPercentMode::Gauge.
  bool _useVoltagePercentMode = false;
  uint16_t _voltageCurveMv[11] = {};

  // Shared debounce state for Voltage-mode percentage, used by both
  // getBatteryPercentage() and getBatteryStatus() (same physical percentage --
  // one shared state, not duplicated per call site). 101 = no history yet.
  mutable uint16_t _voltageDisplayPercent = 101;
  mutable unsigned long _voltagePendingSinceMs = 0;  // 0 = no pending change
  mutable bool _voltagePendingIsIncrease = false;
  static constexpr unsigned long VOLTAGE_PERCENT_CHANGE_DEBOUNCE_MS = 2UL * 60UL * 1000UL;  // 2 min

  // Computes a 1%-resolution Voltage-mode percentage via linear interpolation
  // between the two bracketing _voltageCurveMv[] notches, then debounces it:
  // a change (either direction) must hold continuously for
  // VOLTAGE_PERCENT_CHANGE_DEBOUNCE_MS before it is shown, so a load-transient
  // voltage sag/blip doesn't visibly move the percentage. Shared by
  // getBatteryPercentage() and getBatteryStatus() -- both derive from the same
  // physical voltage.
  uint16_t computeVoltagePercent(uint16_t mv) const;

  enum LockMode { None, NormalSpeed };
  LockMode currentLockMode = None;
  SemaphoreHandle_t modeMutex = nullptr;  // Protect access to currentLockMode

 public:
#if BOARD_HAS_PSRAM
  static constexpr int LOW_POWER_FREQ = 80;  // MHz
#else
  static constexpr int LOW_POWER_FREQ = 10;  // MHz
#endif
  static constexpr unsigned long IDLE_POWER_SAVING_MS = 3000;  // ms
  static constexpr unsigned long BATTERY_POLL_MS = 1500;       // ms

  void begin();

  // Control CPU frequency for power saving
  void setPowerSaving(bool enabled);

  // Setup wake up GPIO and enter deep sleep
  // Should be called inside main loop() to handle the currentLockMode
  void startDeepSleep(HalGPIO& gpio) const;

  // Get battery percentage (range 0-100)
  uint16_t getBatteryPercentage() const;
  BatteryMonitor::Status getBatteryStatus() const;

  // True when the active board has an I2C fuel gauge backend. Callers use this
  // to decide whether a gauge-vs-voltage percentage mode choice is meaningful
  // (ADC-only boards always derive percentage from voltage already).
  bool hasGaugeBackend() const;

  // Push the gauge-board percentage source down from CrossPointSettings. Cheap
  // (a bool and 11 uint16_t copies), safe to call every loop() tick alongside
  // HalTiltSensor::update() -- see CrossPointSettings::batteryPercentMode /
  // batteryCustomCurveMv for what these mean. No-op on boards without a gauge.
  void setBatteryPercentMode(bool useVoltageMode, const uint16_t (&curveMv)[11]);

  // Dumps BQ27220 diagnostic registers to /.crosspoint/bq27220_dump.txt. No-op
  // when deviceIsX3 is false. Call once at boot, after Storage.begin() has
  // succeeded (the gauge I2C bus and the SD card are not ready any earlier).
  void dumpBq27220DiagnosticsToSd(bool deviceIsX3) const;

  // One-off tool: reprograms the BQ27220's Data Memory DesignCapacity and/or
  // FullChargeCapacity seed to the given mAh values, per
  // docs/bq27220-x3-fuel-gauge-findings.md. 0 for either argument skips that
  // field -- FullChargeCapacity (offset 29 of its 32-byte block) is a safe
  // single-block write; DesignCapacity (offset 31) straddles a block
  // boundary and is riskier, so test FullChargeCapacity alone first. No-op
  // when deviceIsX3 is false. NOT gated for general use -- see the
  // BQ27220_REPROGRAM_* build flags at the src/main.cpp call site; this
  // exists to fix one specific device's factory-default (wrong) capacity and
  // is not a general feature. Logs each step, including which exact I2C
  // sub-transaction failed, to /.crosspoint/bq27220_reprogram_log.txt (in
  // addition to LOG_INF/LOG_ERR) since a serial connection may not be
  // available. Call once at boot, after Storage.begin() has succeeded, and
  // call dumpBq27220DiagnosticsToSd() afterward to verify the result.
  void reprogramBq27220Capacity(bool deviceIsX3, uint16_t designCapacityMah, uint16_t fullChargeCapacityMah) const;

  // TEMPORARY diagnostic, not a permanent feature: writes `value` to an
  // arbitrary Data Memory `address` and verifies it round-tripped, to test
  // whether ANY Data Memory write actually commits on this hardware --
  // FullChargeCapacity's write was observed being silently zeroed instead of
  // accepted or left unchanged (docs/bq27220-x3-fuel-gauge-findings.md).
  // Intended to be called with a field written back to its own current
  // value (a no-op if the mechanism works). No-op when deviceIsX3 is false.
  // Logs to /.crosspoint/bq27220_reprogram_log.txt like
  // reprogramBq27220Capacity(); call dumpBq27220DiagnosticsToSd() after to
  // verify.
  void testBq27220DataMemoryWrite(bool deviceIsX3, uint16_t address, uint16_t value) const;

  // TEMPORARY diagnostic, not a permanent feature: replicates TRM S4.6's
  // "Hibernate I" Note verbatim -- the one concrete, fully worked, specific
  // (not generic/illustrative) Data Memory write example in the entire TRM,
  // targeting 0x3E directly for CONFIG_UPDATE entry/exit (not Control()
  // 0x00 as documented everywhere else) and combining each step's bytes into
  // a single I2C transaction rather than separate single-byte writes. Tests
  // this exact literal procedure against its own documented target (Hibernate
  // I, Data Memory address 0x9221, set to 0) before adapting it for
  // DesignCapacity/FullChargeCapacity. See
  // docs/bq27220-x3-fuel-gauge-findings.md. No-op when deviceIsX3 is false.
  // Logs to /.crosspoint/bq27220_reprogram_log.txt; call
  // dumpBq27220DiagnosticsToSd() after to see the resulting full state.
  void testBq27220TiHibernateExample(bool deviceIsX3) const;

  // TEMPORARY diagnostic, not a permanent feature: same combined-transaction,
  // 0x3E-targeted CFG_UPDATE entry/exit mechanism proven more reliable by
  // testBq27220TiHibernateExample(), but writing `value` to an arbitrary
  // Data Memory `address` with a checksum computed fresh for this device's
  // actual block content (X3GPIO::writeBQ27220DataMemoryFieldDirect), rather
  // than TRM S4.6's hardcoded example checksum (0x4C, specific to TI's own
  // test device's block content). See docs/bq27220-x3-fuel-gauge-findings.md.
  // No-op when deviceIsX3 is false. Logs to
  // /.crosspoint/bq27220_reprogram_log.txt; call dumpBq27220DiagnosticsToSd()
  // after to verify.
  void testBq27220DirectWrite(bool deviceIsX3, uint16_t address, uint16_t value) const;

  // RAII helper class to manage power saving locks
  // Usage: create an instance of Lock in a scope to disable power saving, for example when running a task that needs
  // full performance. When the Lock instance is destroyed (goes out of scope), power saving will be re-enabled.
  class Lock {
    friend class HalPowerManager;
    bool valid = false;

   public:
    explicit Lock();
    ~Lock();

    // Non-copyable and non-movable
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    Lock(Lock&&) = delete;
    Lock& operator=(Lock&&) = delete;
  };
};
