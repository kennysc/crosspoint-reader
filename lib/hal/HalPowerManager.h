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
