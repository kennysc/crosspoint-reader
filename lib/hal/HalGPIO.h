#pragma once

#include <Arduino.h>
#include <InputManager.h>

// Display SPI pins (custom pins for XteinkX4, not hardware SPI defaults)
#define EPD_SCLK 8   // SPI Clock
#define EPD_MOSI 10  // SPI MOSI (Master Out Slave In)
#define EPD_CS 21    // Chip Select
#define EPD_DC 4     // Data/Command
#define EPD_RST 5    // Reset
#define EPD_BUSY 6   // Busy

#define SPI_MISO 7  // SPI MISO, shared between SD card and display (Master In Slave Out)

#define BAT_GPIO0 0  // Battery voltage

#define UART0_RXD 20  // Used for USB connection detection

// Xteink X3 Hardware
#define X3_I2C_SDA 20
#define X3_I2C_SCL 0
#define X3_I2C_FREQ 400000

// TI BQ27220 Fuel gauge I2C. Addresses verified against the TI BQ27220
// Technical Reference Manual (SLUUBD4A) Standard Commands table.
//
// TRM Section 5.3 "I2C Command Waiting Time": "To ensure proper operation at
// 400 kHz, a t(BUF) >= 66 us bus-free waiting time must be inserted between
// all packets addressed to the fuel gauge." X3_I2C_FREQ is 400 kHz. The
// low-level I2C primitives below (readI2CReg16LE, writeI2CReg8, readI2CReg8,
// readI2CBlock) enforce this after every transaction; 100 us gives margin
// over the documented 66 us minimum.
#define BQ27220_I2C_BUS_FREE_US 100
#define I2C_ADDR_BQ27220 0x55            // Fuel gauge I2C address
#define BQ27220_SOC_REG 0x2C             // StateOfCharge() command code (%)
#define BQ27220_CUR_REG 0x0C             // Current() command code (signed mA)
#define BQ27220_VOLT_REG 0x08            // Voltage() command code (mV)
#define BQ27220_REMAINING_CAP_REG 0x10   // RemainingCapacity() command code (mAh)
#define BQ27220_FULL_CHARGE_CAP_REG 0x12 // FullChargeCapacity() command code (mAh)
#define BQ27220_DESIGN_CAP_REG 0x3C      // DesignCapacity() command code (mAh, read-only)
// OperationStatus() command code (flags), per TRM Table 2-7:
// High byte: [2]=CFGUPDATE (gauge is in CONFIG_UPDATE mode, gauging suspended), rest RSVD.
// Low byte: [7]=BTPINT [6]=SMTH [5]=INITCOMP [4]=VDQ [3]=EDV2 [2:1]=SEC[1:0] [0]=CALMD.
// SEC[1:0] is the seal state: 11=Sealed, 10=Unsealed, 01=Full Access.
#define BQ27220_OP_STATUS_REG 0x3A

// Control() (0x00/0x01) subcommand identity check -- independent of the
// still-unresolved Data Memory (BlockData) access issue, since this only
// uses Control() writes and MACData() reads, both already proven to work
// correctly (the CONFIG_UPDATE mode bit was observed to flip and clear via
// this exact write mechanism). Per TRM SLUUBD4A Section 2.2, "Any
// subcommand that has a data response will be read back on MACData()"
// (0x40/0x41 for a 2-byte response). DEVICE_NUMBER() must echo 0x0220 on a
// genuine BQ27220 (Section 2.2.2); a mismatch here would mean the whole
// MACData() response mechanism is suspect, not just Data Memory addressing.
#define BQ27220_CTRL_DEVICE_NUMBER 0x0001
#define BQ27220_MACDATA_REG 0x40

// Data Memory access, per TRM Section 3.1 and the worked example in Section
// 6.1. Writing a 16-bit address to 0x3E/0x3F makes that address's containing
// 32-byte Data Memory block readable/writable via BlockData() (0x40-0x5F, the
// same command-code range as MACData() -- whichever was addressed last).
// IMPORTANT: TRM Section 6.1's own prose has the byte order backwards ("write
// 0x9F to 0x3E for the MSB, 0x92 to 0x3F for the LSB" -- literally that gives
// address 0x9F92, not DesignCapacity's real address 0x929F). The byte order
// is little-endian like every other 16-bit register on this device: low byte
// at 0x3E, high byte at 0x3F. Confirmed both by reconciling the TRM's own
// example and by a live leftover pointer value captured from this exact
// device (0x3E/0x3F read A3 92 = address 0x92A3 = DesignVoltage's documented
// address). See docs/bq27220-x3-fuel-gauge-findings.md Finding 4.
#define BQ27220_DM_ADDR_LSB_REG 0x3E
#define BQ27220_DM_ADDR_MSB_REG 0x3F
#define BQ27220_BLOCKDATA_REG 0x40           // 32-byte window into the addressed Data Memory block
#define BQ27220_BLOCKDATA_CHECKSUM_REG 0x60  // 255 - (sum of the 32 BlockData bytes mod 256)
#define BQ27220_BLOCKDATA_LEN_REG 0x61       // block length to commit -- on real hardware, 32 (the
                                              // documented max block size, TRM S3.1) writes ACK at the
                                              // I2C level but the block silently never commits (caught
                                              // by writeBQ27220DataMemoryField()'s verify-readback);
                                              // TRM S6.1's literal worked-example value 0x24 is used
                                              // instead, despite exceeding that documented max.

// Control() subcommands used for Data Memory access (TRM Table 2-2, Section 6.1).
// 0xFFFF is confirmed correct by two independent TRM references (S6.1 step 2
// and S8.5 step 3, "See Chapter 6 for the procedure to enter FULL ACCESS") --
// reverted to it after 0x8000 (TRM S3.3's Sealed->Unsealed key, tried as a
// guess) also failed to elevate OperationStatus() SEC[1:0] past Unsealed
// (10). Neither key has been observed reaching Full Access (01) on this
// device. See docs/bq27220-x3-fuel-gauge-findings.md.
#define BQ27220_CTRL_FULL_ACCESS_KEY 0xFFFF
#define BQ27220_CTRL_ENTER_CFG_UPDATE 0x0090
#define BQ27220_CTRL_EXIT_CFG_UPDATE_REINIT 0x0091
// OperationStatus() high-byte bit2 (TRM Table 2-7): gauge is in CONFIG_UPDATE
// mode, gauging suspended, Data Memory writable.
#define BQ27220_OP_STATUS_CFGUPDATE_MASK 0x0400

// Data Memory addresses, TRM Table 3-2 (Class=Gas Gauging, Subclass=CEDV
// Profile 1). FullChargeCapacity sits at offset 29 of its 32-byte block (no
// boundary crossing); DesignCapacity sits at offset 31, so its second byte
// falls in the next block -- writeBQ27220DataMemoryField() handles this.
#define BQ27220_DM_ADDR_FULL_CHARGE_CAPACITY 0x929D
#define BQ27220_DM_ADDR_DESIGN_CAPACITY 0x929F
// Configuration/BTP class, "Init Discharge Set" default 150 mAh, offset 14 of
// its block (no boundary crossing). Used only as a write-mechanism sanity
// check (written back to its own current value) -- see
// docs/bq27220-x3-fuel-gauge-findings.md.
#define BQ27220_DM_ADDR_BTP_DISCHARGE_SET 0x920E
// Configuration/Power class, "Hibernate I" default 8 mA. TRM S4.6's Note
// (the one concrete, fully worked Data Memory write example in the TRM)
// demonstrates setting this to 0 -- used as this codebase's mechanism
// validation target. See docs/bq27220-x3-fuel-gauge-findings.md.
#define BQ27220_DM_ADDR_HIBERNATE_I 0x9221

// Analog DS3231 RTC I2C
#define I2C_ADDR_DS3231 0x68  // RTC I2C address
#define DS3231_SEC_REG 0x00   // Seconds command code (BCD)

// QST QMI8658 IMU I2C
#define I2C_ADDR_QMI8658 0x6B        // IMU I2C address
#define I2C_ADDR_QMI8658_ALT 0x6A    // IMU I2C fallback address
#define QMI8658_WHO_AM_I_REG 0x00    // WHO_AM_I command code
#define QMI8658_WHO_AM_I_VALUE 0x05  // WHO_AM_I expected value

namespace X3GPIO {

// Snapshot of the BQ27220 diagnostic registers for the SD dump. -1 marks a
// register whose I2C read failed; every real value is non-negative (all are
// unsigned 16-bit standard commands per the TRM), so -1 is an unambiguous
// failure sentinel.
struct Bq27220Diagnostics {
  int32_t voltageMv = -1;
  int32_t socPercent = -1;
  int32_t fullChargeCapacityMah = -1;
  int32_t remainingCapacityMah = -1;
  int32_t designCapacityMah = -1;
  int32_t operationStatusRaw = -1;  // raw flags word; see BQ27220_OP_STATUS_REG comment for bit layout
};

// Reads all six diagnostic registers. Caller must ensure the gauge I2C bus is
// already up (e.g. via a prior HalPowerManager battery read) before calling.
void readBQ27220Diagnostics(Bq27220Diagnostics& out);

// See BQ27220_CTRL_DEVICE_NUMBER comment above.
struct Bq27220IdentityDiagnostics {
  bool deviceNumberReadOk = false;
  int32_t deviceNumberRaw = -1;  // expect 0x0220 on a genuine BQ27220 (TRM Section 2.2.2)
};

// Reads DEVICE_NUMBER() via Control()/MACData(). Caller must ensure the
// gauge I2C bus is already up.
void readBQ27220IdentityDiagnostics(Bq27220IdentityDiagnostics& out);

// Raw byte-for-byte dump of the gauge's command address space, starting at
// register 0x00, for offline decoding against the TRM (or against a clone
// chip's actual behavior when it diverges from the TRM). Covers the
// Standard Commands block, the MACData() response buffer, and
// MACDataSum()/MACDataLen() with margin. A byte whose containing I2C
// transaction failed reads back as 0xFF -- indistinguishable from a real
// 0xFF, but distinguishable from a systematic failure by cross-referencing
// against BQ27220_MACDATA_REG etc. reads that succeeded. Caller must ensure
// the gauge I2C bus is already up; call after any MACData()-populating
// subcommand (e.g. readBQ27220IdentityDiagnostics) so the buffer reflects
// its response. Must stay a multiple of 16 -- the SD dump's hex table walks
// it in fixed 16-byte rows with no per-row bounds check.
constexpr uint8_t BQ27220_RAW_DUMP_LEN = 0x80;
void readBQ27220RawRegisters(uint8_t* out, uint8_t len);

// Writes a 16-bit subcommand to Control() (0x00/0x01). Used both for MAC
// subcommands (e.g. BQ27220_CTRL_DEVICE_NUMBER) and for the FULL ACCESS
// unseal key / CFG_UPDATE entry-exit subcommands. Caller must ensure the
// gauge I2C bus is already up.
bool writeBQ27220Control(uint16_t subcommand);

// Writes `len` sequential bytes starting at `startReg` in ONE I2C transaction.
// See the comment on this function in HalGPIO.cpp for why this differs from
// writeBQ27220Control's split-single-byte-write approach. Caller must ensure
// the gauge I2C bus is already up.
bool writeI2CBlock(uint8_t addr, uint8_t startReg, const uint8_t* data, uint8_t len);

// Addresses `address` via 0x3E/0x3F then reads the resulting 32-byte
// BlockData() window into out32. Caller must ensure the gauge I2C bus is
// already up.
bool readBQ27220BlockDataAt(uint16_t address, uint8_t out32[32]);

// Reads OperationStatus() (0x3A/0x3B). Caller must ensure the gauge I2C bus
// is already up.
bool readBQ27220OperationStatus(uint16_t* out);

// Writes `value` (little-endian) directly to Data Memory address `address`,
// replicating TRM S4.6's Note (Hibernate I example) mechanism: address+data
// and checksum+length each combined into one I2C transaction, no 32-byte
// block-alignment (the addressed byte lands at BlockData() offset 0
// directly), checksum computed fresh for this device's actual block content.
// Confirmed more reliable on real hardware than writeBQ27220DataMemoryField's
// split-write approach -- see docs/bq27220-x3-fuel-gauge-findings.md. Only
// valid for a field whose 2 bytes fit in one window. Gauge must already be
// in CFG_UPDATE mode; caller must ensure the gauge I2C bus is already up.
bool writeBQ27220DataMemoryFieldDirect(uint16_t address, uint16_t value, char* outFailDetail, size_t failDetailLen);

// Writes `value` (mAh, little-endian) into the Data Memory field at
// `address` via the BlockData() block-window mechanism (TRM S3.1/S6.1, byte
// order corrected per the BQ27220_DM_ADDR_LSB_REG comment). Handles the
// field's two bytes straddling a 32-byte block boundary by committing each
// affected block separately, and reads each block back afterward to confirm
// the gauge actually committed it (a bad checksum/length is silently
// ignored per TRM S3.1, not rejected at the I2C bus level, so a bus-level
// ACK alone doesn't prove the write took). On failure, writes a short
// description of which step failed (e.g. "segment1 verify-mismatch(offset=0..0)")
// into outFailDetail (must be non-null, failDetailLen bytes). Gauge must
// already be UNSEALED to FULL ACCESS and in CFG_UPDATE mode before calling;
// caller must ensure the gauge I2C bus is already up.
//
// verboseDiagnostics adds extra ground-truth reads (the device's own
// checksum before modification, and a checksum/length readback immediately
// after committing) to a failure's detail message. Real-hardware testing
// showed register addresses/values from exactly these extra reads leaking
// into the BlockData buffer at unrelated offsets -- they are suspects for
// corrupting the very write they're meant to help debug, so default this to
// false for actual write attempts; only enable for isolated debugging.
bool writeBQ27220DataMemoryField(uint16_t address, uint16_t value, char* outFailDetail, size_t failDetailLen,
                                  bool verboseDiagnostics);

}  // namespace X3GPIO

class HalGPIO {
#if CROSSPOINT_EMULATED == 0
  InputManager inputMgr;
#endif

  bool lastUsbConnected = false;
  bool usbStateChanged = false;

 public:
  enum class DeviceType : uint8_t { X4, X3 };

 private:
  DeviceType _deviceType = DeviceType::X4;

 public:
  HalGPIO() = default;

  // Inline device type helpers for cleaner downstream checks
  inline bool deviceIsX3() const { return _deviceType == DeviceType::X3; }
  inline bool deviceIsX4() const { return _deviceType == DeviceType::X4; }
  bool isXteinkDevice() const;

  // True when the board's page buttons sit on the left/right screen edges
  // (X3, X4 Pro) rather than an off-screen vertical rocker. Drives side-hint
  // placement and the flipped large-step direction in selection activities.
  // Keyed off the active BoardConfig profile, not the X3/X4 runtime detection.
  bool hasEdgeSideButtons() const;

  // Start button GPIO and setup SPI for screen and SD card
  void begin();

  // Button input methods
  void update();
  bool isPressed(uint8_t buttonIndex) const;
  bool wasPressed(uint8_t buttonIndex) const;
  bool wasAnyPressed() const;
  bool wasReleased(uint8_t buttonIndex) const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;
  unsigned long getPowerButtonHeldTime() const;
  bool hasTouch() const;
  // Capacitive Home key reported by the touch controller (X4 Pro). The tap
  // event fires on release and excludes a long hold.
  bool hasHomeKey() const;
  bool wasHomeKeyTapped() const;
  bool wasHomeKeyLongPressed() const;
  bool wasTouchTap(float& nx, float& ny) const;
  bool wasTouchDown(float& nx, float& ny) const;
  // Raw release edge, reported even when the contact was not a tap (swipe end,
  // drag-off). Snapshot builders forward it so interaction routing can clear
  // pressed state.
  bool wasTouchReleased() const;
  bool isTouchTapCandidate(float& nx, float& ny, unsigned long& heldMs) const;
  bool isTouchHeldAt(float& nx, float& ny) const;
  // One-shot long-press, fired by the SDK classifier while the finger is still
  // down (stationary contact held past its threshold). Position = touch-down
  // point. Callers that act on it should suppressTouchContact() so the lift
  // cannot also tap.
  bool wasTouchLongPress(float& nx, float& ny) const;
  // Ignore the remainder of the current contact (its continued hold and its
  // release edge). Self-clears once the contact ends.
  void suppressTouchContact();
  unsigned long lastTouchHeldMs() const;
  bool wasSwipe(float& nxStart, float& nyStart, float& nxEnd, float& nyEnd) const;
  bool wasTouchActivity() const;
  void setSharedConfirmPowerShortPressEmitsPower(bool enabled);

  // Verify that the physical power button remains held through input debounce.
  // Returns true if verification succeeded, false if device should return to sleep.
  // Should only be called when wakeup reason is PowerButton.
  bool verifyPowerButtonWakeup();

  // Check if USB is connected
  bool isUsbConnected() const;

  // Whether a cold boot with no USB detected can be trusted to mean a held
  // power button (Xteink-style button-energized rail with reliable USB
  // detection). When false, cold boots always proceed to a normal boot.
  bool coldBootImpliesPowerButton() const;

  // Returns true once per edge (plug or unplug) since the last update()
  bool wasUsbStateChanged() const;

  enum class WakeupReason { PowerButton, AfterFlash, AfterUSBPower, Other };

  WakeupReason getWakeupReason() const;

  // Button indices
  static constexpr uint8_t BTN_BACK = 0;
  static constexpr uint8_t BTN_CONFIRM = 1;
  static constexpr uint8_t BTN_LEFT = 2;
  static constexpr uint8_t BTN_RIGHT = 3;
  static constexpr uint8_t BTN_UP = 4;
  static constexpr uint8_t BTN_DOWN = 5;
  static constexpr uint8_t BTN_POWER = 6;
};

extern HalGPIO gpio;
