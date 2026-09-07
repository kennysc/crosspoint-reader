#include <BatteryMonitor.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <PowerManager.h>
#include <Preferences.h>
#include <SPI.h>
#include <Wire.h>
#include <XteinkDetect.h>
#include <esp_sleep.h>

#include <algorithm>
#include <cstring>

// Global HalGPIO instance
HalGPIO gpio;

namespace X3GPIO {

// BQ27220_I2C_BUS_FREE_US applies after every transaction below, success or
// failure, per the TRM S5.3 requirement -- see comment on that constant in
// HalGPIO.h.
bool readI2CReg16LE(uint8_t addr, uint8_t reg, uint16_t* outValue) {
  bool ok = false;
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) == 0) {
    if (Wire.requestFrom(addr, static_cast<uint8_t>(2), static_cast<uint8_t>(true)) >= 2) {
      const uint8_t lo = Wire.read();
      const uint8_t hi = Wire.read();
      *outValue = (static_cast<uint16_t>(hi) << 8) | lo;
      ok = true;
    }
    while (Wire.available()) {
      Wire.read();
    }
  }
  delayMicroseconds(BQ27220_I2C_BUS_FREE_US);
  return ok;
}

// Writes `len` sequential bytes starting at `startReg` in ONE I2C transaction
// (relying on the device's internal auto-increment across a combined
// multi-byte write). TRM S4.6's Note (the "Hibernate I" example) demonstrates
// this working for the Data Memory address+data and checksum+length writes --
// in contrast to the split-single-byte-write requirement found necessary for
// plain Control() subcommand writes (see writeBQ27220Control comment). See
// docs/bq27220-x3-fuel-gauge-findings.md.
bool writeI2CBlock(uint8_t addr, uint8_t startReg, const uint8_t* data, uint8_t len) {
  Wire.beginTransmission(addr);
  Wire.write(startReg);
  for (uint8_t i = 0; i < len; i++) {
    Wire.write(data[i]);
  }
  const bool ok = Wire.endTransmission(true) == 0;
  delayMicroseconds(BQ27220_I2C_BUS_FREE_US);
  return ok;
}

namespace {
// Writes one byte to a single I2C register in one transaction.
bool writeI2CReg8(uint8_t addr, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(value);
  const bool ok = Wire.endTransmission(true) == 0;
  delayMicroseconds(BQ27220_I2C_BUS_FREE_US);
  return ok;
}

// Reads one byte from a single I2C register in one transaction.
bool readI2CReg8(uint8_t addr, uint8_t reg, uint8_t* outValue) {
  bool ok = false;
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) == 0) {
    if (Wire.requestFrom(addr, static_cast<uint8_t>(1), static_cast<uint8_t>(true)) >= 1) {
      *outValue = Wire.read();
      ok = true;
    }
    while (Wire.available()) {
      Wire.read();
    }
  }
  delayMicroseconds(BQ27220_I2C_BUS_FREE_US);
  return ok;
}
}  // namespace

bool readBQ27220CurrentMA(int16_t* outCurrent) {
  uint16_t raw = 0;
  if (!readI2CReg16LE(I2C_ADDR_BQ27220, BQ27220_CUR_REG, &raw)) {
    return false;
  }
  *outCurrent = static_cast<int16_t>(raw);
  return true;
}

namespace {
// Reads one register into an int32_t field, leaving it at -1 (the caller's
// pre-set default) on I2C failure.
void readDiagField(uint8_t reg, int32_t& outField) {
  uint16_t raw = 0;
  if (readI2CReg16LE(I2C_ADDR_BQ27220, reg, &raw)) {
    outField = raw;
  }
}
}  // namespace

void readBQ27220Diagnostics(Bq27220Diagnostics& out) {
  readDiagField(BQ27220_VOLT_REG, out.voltageMv);
  readDiagField(BQ27220_SOC_REG, out.socPercent);
  readDiagField(BQ27220_FULL_CHARGE_CAP_REG, out.fullChargeCapacityMah);
  readDiagField(BQ27220_REMAINING_CAP_REG, out.remainingCapacityMah);
  readDiagField(BQ27220_DESIGN_CAP_REG, out.designCapacityMah);
  readDiagField(BQ27220_OP_STATUS_REG, out.operationStatusRaw);
}

// Writes a 16-bit subcommand to Control() (0x00/0x01) as two separate
// single-byte transactions -- this device does not reliably auto-increment
// across a combined multi-byte register write.
bool writeBQ27220Control(uint16_t subcommand) {
  return writeI2CReg8(I2C_ADDR_BQ27220, 0x00, static_cast<uint8_t>(subcommand & 0xFF)) &&
         writeI2CReg8(I2C_ADDR_BQ27220, 0x01, static_cast<uint8_t>((subcommand >> 8) & 0xFF));
}

bool readBQ27220OperationStatus(uint16_t* out) { return readI2CReg16LE(I2C_ADDR_BQ27220, BQ27220_OP_STATUS_REG, out); }

void readBQ27220IdentityDiagnostics(Bq27220IdentityDiagnostics& out) {
  if (!writeBQ27220Control(BQ27220_CTRL_DEVICE_NUMBER)) {
    return;
  }
  delay(2);
  uint16_t deviceNumber = 0;
  out.deviceNumberReadOk = readI2CReg16LE(I2C_ADDR_BQ27220, BQ27220_MACDATA_REG, &deviceNumber);
  if (out.deviceNumberReadOk) {
    out.deviceNumberRaw = deviceNumber;
  }
}

namespace {
// Reads `len` sequential bytes starting at `startReg` in one I2C
// transaction. The device auto-increments its internal read pointer across
// a combined read (already relied on for the 2-byte Standard Command reads
// above); only writes were found to need splitting, per the
// writeBQ27220Control comment.
bool readI2CBlock(uint8_t addr, uint8_t startReg, uint8_t* out, uint8_t len) {
  bool ok = false;
  Wire.beginTransmission(addr);
  Wire.write(startReg);
  if (Wire.endTransmission(false) == 0) {
    if (Wire.requestFrom(addr, len, static_cast<uint8_t>(true)) >= len) {
      for (uint8_t i = 0; i < len; i++) {
        out[i] = Wire.read();
      }
      ok = true;
    }
    while (Wire.available()) {
      Wire.read();
    }
  }
  delayMicroseconds(BQ27220_I2C_BUS_FREE_US);
  return ok;
}
}  // namespace

void readBQ27220RawRegisters(uint8_t* out, uint8_t len) {
  // 32-byte chunks match the MACData() block size already used elsewhere in
  // this file, comfortably within the ESP32 Arduino Wire library's I2C
  // transaction buffer.
  constexpr uint8_t kChunk = 32;
  for (uint8_t offset = 0; offset < len; offset += kChunk) {
    const uint8_t n = std::min<uint8_t>(kChunk, len - offset);
    if (!readI2CBlock(I2C_ADDR_BQ27220, offset, out + offset, n)) {
      memset(out + offset, 0xFF, n);
    }
  }
}

namespace {
// Writes the Data Memory address window pointer, as ONE combined 2-byte
// transaction. See the byte-order note on BQ27220_DM_ADDR_LSB_REG in
// HalGPIO.h. Previously two separate single-byte writes; switched to a
// combined write after TRM S4.6's Note (Hibernate I example) demonstrated
// this pattern entering CFG_UPDATE reliably on the first attempt (vs.
// frequent retries needed with split writes) and returning genuine block
// content on verify-readback (vs. corrupted echo bytes from unrelated
// transactions) -- see docs/bq27220-x3-fuel-gauge-findings.md.
bool writeBQ27220DataMemoryAddress(uint16_t address) {
  const uint8_t addrBytes[2] = {static_cast<uint8_t>(address & 0xFF), static_cast<uint8_t>(address >> 8)};
  return writeI2CBlock(I2C_ADDR_BQ27220, BQ27220_DM_ADDR_LSB_REG, addrBytes, 2);
}
}  // namespace

bool readBQ27220BlockDataAt(uint16_t address, uint8_t out32[32]) {
  if (!writeBQ27220DataMemoryAddress(address)) {
    return false;
  }
  delay(2);
  return readI2CBlock(I2C_ADDR_BQ27220, BQ27220_BLOCKDATA_REG, out32, 32);
}

// Writes `value` (little-endian) directly to Data Memory address `address`,
// replicating the ACTUAL BQStudio-generated gm.fs protocol (TRM S8.1 Figure
// 8-1 -- a real tool-generated file, not a hand-transcribed doc example):
// address write, full 32-byte data write, and single-byte checksum write are
// three SEPARATE I2C transactions (not combined), the FULL 32-byte block is
// rewritten every time (not just the changed bytes), and there is NO
// length/0x61 write at all in that verified format. This supersedes the
// earlier attempt at replicating TRM S4.6's Note (Hibernate I), which
// combines bytes and writes a length -- that appears to be a different,
// abbreviated partial-block variant that did not work on this hardware
// across several tries; see docs/bq27220-x3-fuel-gauge-findings.md for the
// full history. No address rounding to a 32-byte boundary -- per TRM S6.1's
// DesignCapacity example, the addressed byte lands at BlockData() offset 0
// directly. Gauge must already be in CFG_UPDATE mode; only valid for a field
// whose 2 bytes fit in one window. Caller must ensure the gauge I2C bus is
// already up.
bool writeBQ27220DataMemoryFieldDirect(uint16_t address, uint16_t value, char* outFailDetail, size_t failDetailLen) {
  uint8_t block[32];
  if (!readBQ27220BlockDataAt(address, block)) {
    snprintf(outFailDetail, failDetailLen, "blockdata-read(addr=0x%04X)", address);
    return false;
  }

  // Data VALUE bytes are big-endian (MSB at offset 0, LSB at offset 1) --
  // confirmed both by TRM S6.1's literal pseudocode ("wr 0x40 MSB; wr 0x41
  // LSB") and by a reproducible transposition observed on real hardware
  // (wrote low-then-high, read back high-then-low, twice). This is
  // independent of the address bytes at 0x3E/0x3F, which are little-endian
  // (separately proven -- see BQ27220_DM_ADDR_LSB_REG comment in HalGPIO.h).
  block[0] = static_cast<uint8_t>(value >> 8);
  block[1] = static_cast<uint8_t>(value & 0xFF);

  // 1. Address write (own transaction).
  if (!writeBQ27220DataMemoryAddress(address)) {
    snprintf(outFailDetail, failDetailLen, "address-write(addr=0x%04X)", address);
    return false;
  }

  // 2. Full 32-byte data write (own transaction) -- not just the changed bytes.
  if (!writeI2CBlock(I2C_ADDR_BQ27220, BQ27220_BLOCKDATA_REG, block, 32)) {
    snprintf(outFailDetail, failDetailLen, "blockdata-write(addr=0x%04X)", address);
    return false;
  }

  uint16_t sum = 0;
  for (uint8_t b = 0; b < 32; b++) {
    sum += block[b];
  }
  const uint8_t checksum = static_cast<uint8_t>(255 - (sum % 256));

  // 3. Checksum write: single byte, own transaction, no length write.
  if (!writeI2CReg8(I2C_ADDR_BQ27220, BQ27220_BLOCKDATA_CHECKSUM_REG, checksum)) {
    snprintf(outFailDetail, failDetailLen, "checksum-write(checksum=0x%02X)", checksum);
    return false;
  }

  delay(200);

  // 4. Re-address, then read-and-compare the checksum register, exactly as
  // the verified gm.fs format does (its "C: AA 60 <checksum>" line).
  if (!writeBQ27220DataMemoryAddress(address)) {
    snprintf(outFailDetail, failDetailLen, "verify-address-write(addr=0x%04X)", address);
    return false;
  }
  delay(2);
  uint8_t checksumReadback = 0;
  if (!readI2CReg8(I2C_ADDR_BQ27220, BQ27220_BLOCKDATA_CHECKSUM_REG, &checksumReadback)) {
    snprintf(outFailDetail, failDetailLen, "verify-checksum-read(addr=0x%04X)", address);
    return false;
  }
  if (checksumReadback != checksum) {
    snprintf(outFailDetail, failDetailLen, "checksum-mismatch: wrote 0x%02X read 0x%02X", checksum,
             checksumReadback);
    return false;
  }

  // Extra confidence beyond what the gm.fs format itself checks: also verify
  // the actual data bytes, not just the checksum register.
  uint8_t verifyBlock[32];
  if (!readBQ27220BlockDataAt(address, verifyBlock)) {
    snprintf(outFailDetail, failDetailLen, "verify-read(addr=0x%04X)", address);
    return false;
  }
  if (verifyBlock[0] != block[0] || verifyBlock[1] != block[1]) {
    // block[]/verifyBlock[] are MSB-then-LSB now, so printing them in that
    // order directly gives the human-readable value.
    snprintf(outFailDetail, failDetailLen, "verify-mismatch: wrote %02X%02X read %02X%02X (checksum=0x%02X)",
             block[0], block[1], verifyBlock[0], verifyBlock[1], checksum);
    return false;
  }
  return true;
}

bool writeBQ27220DataMemoryField(uint16_t address, uint16_t value, char* outFailDetail, size_t failDetailLen,
                                  bool verboseDiagnostics) {
  const uint8_t valueBytes[2] = {static_cast<uint8_t>(value & 0xFF), static_cast<uint8_t>(value >> 8)};
  uint8_t written = 0;
  uint8_t segment = 0;
  while (written < 2) {
    const uint16_t byteAddr = address + written;
    const uint8_t offset = static_cast<uint8_t>(byteAddr % 32);
    const uint16_t blockBase = byteAddr - offset;

    if (!writeBQ27220DataMemoryAddress(blockBase)) {
      snprintf(outFailDetail, failDetailLen, "segment%u address-write(base=0x%04X)", segment, blockBase);
      return false;
    }
    delay(2);  // let the address window settle before BlockData() reflects it
    uint8_t block[32];
    if (!readI2CBlock(I2C_ADDR_BQ27220, BQ27220_BLOCKDATA_REG, block, 32)) {
      snprintf(outFailDetail, failDetailLen, "segment%u blockdata-read(base=0x%04X)", segment, blockBase);
      return false;
    }

    // Ground truth check: read the DEVICE's own checksum of the unmodified
    // block, rather than assuming our software formula (255 - byte-sum mod
    // 256, TRM S6.1) matches what the gauge itself expects. Gated by
    // verboseDiagnostics: an earlier attempt showed register addresses/values
    // from these exact extra reads leaking into the BlockData buffer at
    // unrelated offsets, so they're suspects for corrupting the very write
    // they're meant to help debug.
    uint8_t deviceChecksumBefore = 0;
    bool haveDeviceChecksumBefore = false;
    if (verboseDiagnostics) {
      haveDeviceChecksumBefore = readI2CReg8(I2C_ADDR_BQ27220, BQ27220_BLOCKDATA_CHECKSUM_REG, &deviceChecksumBefore);
    }
    uint16_t sumBefore = 0;
    for (uint8_t b = 0; b < 32; b++) {
      sumBefore += block[b];
    }
    const uint8_t computedChecksumBefore = static_cast<uint8_t>(255 - (sumBefore % 256));

    // Fill as many consecutive bytes of `value` as fit before this block ends;
    // the loop's next iteration picks up any remainder in the following block.
    const uint8_t segStart = offset;
    uint8_t o = offset;
    while (written < 2 && o < 32) {
      block[o] = valueBytes[written];
      written++;
      o++;
    }
    const uint8_t segEnd = o;  // exclusive

    uint16_t sum = 0;
    for (uint8_t b = 0; b < 32; b++) {
      sum += block[b];
    }
    const uint8_t checksum = static_cast<uint8_t>(255 - (sum % 256));

    for (uint8_t w = segStart; w < segEnd; w++) {
      if (!writeI2CReg8(I2C_ADDR_BQ27220, static_cast<uint8_t>(BQ27220_BLOCKDATA_REG + w), block[w])) {
        snprintf(outFailDetail, failDetailLen, "segment%u byte-commit(offset=%u)", segment, w);
        return false;
      }
    }
    if (!writeI2CReg8(I2C_ADDR_BQ27220, BQ27220_BLOCKDATA_CHECKSUM_REG, checksum)) {
      snprintf(outFailDetail, failDetailLen, "segment%u checksum-write", segment);
      return false;
    }
    // 0x24: TRM S6.1's literal worked-example value; 32 (documented max block
    // size, TRM S3.1) was tried first and both were confirmed silently
    // dropped by the verify-readback below on real hardware. See
    // BQ27220_BLOCKDATA_LEN_REG comment in HalGPIO.h.
    if (!writeI2CReg8(I2C_ADDR_BQ27220, BQ27220_BLOCKDATA_LEN_REG, 0x24)) {
      snprintf(outFailDetail, failDetailLen, "segment%u len-write", segment);
      return false;
    }

    // Read back checksum/length immediately (before the settle delay) to see
    // whether the registers themselves actually hold what we just wrote.
    // Same corruption suspicion as deviceChecksumBefore above -- gated off by
    // default now.
    uint8_t checksumReadback = 0;
    uint8_t lenReadback = 0;
    bool haveChecksumReadback = false;
    bool haveLenReadback = false;
    if (verboseDiagnostics) {
      haveChecksumReadback = readI2CReg8(I2C_ADDR_BQ27220, BQ27220_BLOCKDATA_CHECKSUM_REG, &checksumReadback);
      haveLenReadback = readI2CReg8(I2C_ADDR_BQ27220, BQ27220_BLOCKDATA_LEN_REG, &lenReadback);
    }

    delay(5);

    // Verify: re-address the same block and confirm BlockData() now reflects
    // our write. Distinguishes "I2C transaction ACKed" from "gauge actually
    // committed the block" (TRM S3.1: a bad checksum/length is silently
    // ignored, not rejected at the bus level).
    if (!writeBQ27220DataMemoryAddress(blockBase)) {
      snprintf(outFailDetail, failDetailLen, "segment%u verify-address-write", segment);
      return false;
    }
    delay(2);
    uint8_t verifyBlock[32];
    if (!readI2CBlock(I2C_ADDR_BQ27220, BQ27220_BLOCKDATA_REG, verifyBlock, 32)) {
      snprintf(outFailDetail, failDetailLen, "segment%u verify-read", segment);
      return false;
    }
    if (memcmp(verifyBlock + segStart, block + segStart, segEnd - segStart) != 0) {
      char chkReadbackStr[8];
      char lenReadbackStr[8];
      if (haveChecksumReadback) {
        snprintf(chkReadbackStr, sizeof(chkReadbackStr), "0x%02X", checksumReadback);
      } else if (verboseDiagnostics) {
        snprintf(chkReadbackStr, sizeof(chkReadbackStr), "ERR");
      } else {
        snprintf(chkReadbackStr, sizeof(chkReadbackStr), "n/a");
      }
      if (haveLenReadback) {
        snprintf(lenReadbackStr, sizeof(lenReadbackStr), "0x%02X", lenReadback);
      } else if (verboseDiagnostics) {
        snprintf(lenReadbackStr, sizeof(lenReadbackStr), "ERR");
      } else {
        snprintf(lenReadbackStr, sizeof(lenReadbackStr), "n/a");
      }

      // Full block hex, not just the mismatched offsets, so a shift/misalignment
      // elsewhere in the block (not just the target bytes) is visible too.
      char expectedHex[128] = "";
      char actualHex[128] = "";
      size_t expPos = 0, actPos = 0;
      for (uint8_t b = 0; b < 32 && expPos + 3 < sizeof(expectedHex) && actPos + 3 < sizeof(actualHex); b++) {
        expPos += snprintf(expectedHex + expPos, sizeof(expectedHex) - expPos, "%02X ", block[b]);
        actPos += snprintf(actualHex + actPos, sizeof(actualHex) - actPos, "%02X ", verifyBlock[b]);
      }

      const char* checksumConsistentStr;
      if (!verboseDiagnostics) {
        checksumConsistentStr = "not checked";
      } else if (haveDeviceChecksumBefore && deviceChecksumBefore == computedChecksumBefore) {
        checksumConsistentStr = "matches formula";
      } else {
        checksumConsistentStr = "MISMATCH";
      }
      char devChkBeforeStr[8];
      if (verboseDiagnostics && haveDeviceChecksumBefore) {
        snprintf(devChkBeforeStr, sizeof(devChkBeforeStr), "0x%02X", deviceChecksumBefore);
      } else {
        snprintf(devChkBeforeStr, sizeof(devChkBeforeStr), "n/a");
      }
      snprintf(outFailDetail, failDetailLen,
               "segment%u verify-mismatch(off=%u..%u) base=0x%04X devChkBefore=%s(%s) wroteChk=0x%02X "
               "chkReadback=%s lenReadback=%s\nexpected: %s\nactual:   %s",
               segment, segStart, segEnd - 1, blockBase, devChkBeforeStr, checksumConsistentStr, checksum,
               chkReadbackStr, lenReadbackStr, expectedHex, actualHex);
      return false;
    }

    segment++;
  }
  return true;
}

}  // namespace X3GPIO

namespace {
constexpr char HW_NAMESPACE[] = "cphw";
constexpr char NVS_KEY_DEV_OVERRIDE[] = "dev_ovr";  // 0=auto, 1=x4, 2=x3
constexpr char NVS_KEY_DEV_CACHED[] = "dev_det";    // 0=unknown, 1=x4, 2=x3

enum class NvsDeviceValue : uint8_t { Unknown = 0, X4 = 1, X3 = 2 };

NvsDeviceValue readNvsDeviceValue(const char* key, NvsDeviceValue defaultValue) {
  Preferences prefs;
  if (!prefs.begin(HW_NAMESPACE, true)) {
    return defaultValue;
  }
  const uint8_t raw = prefs.getUChar(key, static_cast<uint8_t>(defaultValue));
  prefs.end();
  if (raw > static_cast<uint8_t>(NvsDeviceValue::X3)) {
    return defaultValue;
  }
  return static_cast<NvsDeviceValue>(raw);
}

void writeNvsDeviceValue(const char* key, NvsDeviceValue value) {
  Preferences prefs;
  if (!prefs.begin(HW_NAMESPACE, false)) {
    return;
  }
  prefs.putUChar(key, static_cast<uint8_t>(value));
  prefs.end();
}

HalGPIO::DeviceType nvsToDeviceType(NvsDeviceValue value) {
  return value == NvsDeviceValue::X3 ? HalGPIO::DeviceType::X3 : HalGPIO::DeviceType::X4;
}

HalGPIO::DeviceType detectDeviceTypeWithFingerprint() {
  // Explicit override for recovery/support:
  // 0 = auto, 1 = force X4, 2 = force X3
  const NvsDeviceValue overrideValue = readNvsDeviceValue(NVS_KEY_DEV_OVERRIDE, NvsDeviceValue::Unknown);
  if (overrideValue == NvsDeviceValue::X3 || overrideValue == NvsDeviceValue::X4) {
    LOG_INF("HW", "Device override active: %s", overrideValue == NvsDeviceValue::X3 ? "X3" : "X4");
    return nvsToDeviceType(overrideValue);
  }

  const NvsDeviceValue cachedValue = readNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::Unknown);
  if (cachedValue == NvsDeviceValue::X3 || cachedValue == NvsDeviceValue::X4) {
    LOG_INF("HW", "Using cached device type: %s", cachedValue == NvsDeviceValue::X3 ? "X3" : "X4");
    return nvsToDeviceType(cachedValue);
  }

  // No cache yet: use FreeInk's canonical two-pass X3 fingerprint and persist
  // only confirmed results. Inconclusive probes deliberately remain uncached.
  uint8_t score1 = 0;
  uint8_t score2 = 0;
  const freeink::XteinkVerdict verdict = freeink::detectXteinkVerdict(&score1, &score2);
  LOG_INF("HW", "Xteink probe scores: pass1=%u pass2=%u verdict=%u", score1, score2, static_cast<unsigned>(verdict));

  if (verdict == freeink::XteinkVerdict::X3Confirmed) {
    writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X3);
    return HalGPIO::DeviceType::X3;
  }

  if (verdict == freeink::XteinkVerdict::X4Confirmed) {
    writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X4);
    return HalGPIO::DeviceType::X4;
  }

  // Conservative fallback for first boot with inconclusive probes.
  return HalGPIO::DeviceType::X4;
}

}  // namespace

void HalGPIO::begin() {
#if FREEINK_MCU_C3
  _deviceType = detectDeviceTypeWithFingerprint();
  BoardConfig::selectDevice(deviceIsX3() ? BoardConfig::Board::XteinkX3 : BoardConfig::Board::XteinkX4);

  // Resolve the per-batch controller before SPI owns the display pins. FreeInk
  // checks the OEM hw_calib/screenType value first, then falls back to its
  // two-pass display-bus probe. X3's facade keys panel selection off the sibling
  // board profile, so preserve a detected UC8279 through setDisplayX3().
  freeink::applyXteinkDisplayController();
  if (deviceIsX3() && BoardConfig::ACTIVE.displayController == BoardConfig::DisplayController::UC8279) {
    BoardConfig::selectDevice(BoardConfig::Board::XteinkX3Uc8279);
  }

  SPI.begin(EPD_SCLK, SPI_MISO, EPD_MOSI, EPD_CS);

  if (deviceIsX4()) {
    pinMode(BAT_GPIO0, INPUT);
    pinMode(UART0_RXD, INPUT);
  }
#else
  _deviceType = DeviceType::X4;
#endif
  inputMgr.begin();
}

void HalGPIO::update() {
  inputMgr.update();
  const bool connected = isUsbConnected();
  usbStateChanged = (connected != lastUsbConnected);
  lastUsbConnected = connected;
}

bool HalGPIO::wasUsbStateChanged() const { return usbStateChanged; }

bool HalGPIO::isPressed(uint8_t buttonIndex) const { return inputMgr.isPressed(buttonIndex); }

bool HalGPIO::wasPressed(uint8_t buttonIndex) const { return inputMgr.wasPressed(buttonIndex); }

bool HalGPIO::wasAnyPressed() const { return inputMgr.wasAnyPressed(); }

bool HalGPIO::wasReleased(uint8_t buttonIndex) const { return inputMgr.wasReleased(buttonIndex); }

bool HalGPIO::wasAnyReleased() const { return inputMgr.wasAnyReleased(); }

unsigned long HalGPIO::getHeldTime() const { return inputMgr.getHeldTime(); }

unsigned long HalGPIO::getPowerButtonHeldTime() const { return inputMgr.getPowerButtonHeldTime(); }

bool HalGPIO::hasTouch() const { return inputMgr.hasTouch(); }

bool HalGPIO::hasHomeKey() const { return BoardConfig::hasHomeKey(); }

bool HalGPIO::wasHomeKeyTapped() const { return inputMgr.wasHomeKeyTapped(); }

bool HalGPIO::wasHomeKeyLongPressed() const { return inputMgr.wasHomeKeyLongPressed(); }

bool HalGPIO::wasTouchTap(float& nx, float& ny) const { return inputMgr.wasTouchTap(nx, ny); }

bool HalGPIO::wasTouchDown(float& nx, float& ny) const { return inputMgr.wasTouchPressedAt(nx, ny); }

bool HalGPIO::wasTouchReleased() const { return inputMgr.wasTouchReleased(); }

bool HalGPIO::isTouchTapCandidate(float& nx, float& ny, unsigned long& heldMs) const {
  return inputMgr.isTouchTapCandidate(nx, ny, heldMs);
}

bool HalGPIO::isTouchHeldAt(float& nx, float& ny) const { return inputMgr.isTouchHeldAt(nx, ny); }

bool HalGPIO::wasTouchLongPress(float& nx, float& ny) const { return inputMgr.wasTouchLongPress(nx, ny); }

void HalGPIO::suppressTouchContact() { inputMgr.suppressTouchContact(); }

unsigned long HalGPIO::lastTouchHeldMs() const { return inputMgr.lastTouchHeldMs(); }

bool HalGPIO::wasSwipe(float& nxStart, float& nyStart, float& nxEnd, float& nyEnd) const {
  return inputMgr.wasSwipe(nxStart, nyStart, nxEnd, nyEnd);
}

bool HalGPIO::wasTouchActivity() const { return inputMgr.wasTouchActivity(); }

void HalGPIO::setSharedConfirmPowerShortPressEmitsPower(const bool enabled) {
  InputManager::setSharedConfirmPowerShortPressEmitsPower(enabled);
}

bool HalGPIO::hasEdgeSideButtons() const {
  return BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3Uc8279 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX4Pro ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX4Classic;
}

bool HalGPIO::isXteinkDevice() const {
  return BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3Uc8279 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX4;
}

bool HalGPIO::verifyPowerButtonWakeup() {
  // M5Paper v1.1: the classic ESP32's reset-to-setup() latency exceeds a normal
  // wheel click, so a click wake is always released before this samples and
  // verification would re-sleep on every wake. Its wheel has hard external
  // pull-ups, so the ghost-wake debounce this implements is not needed.
  if (BoardConfig::isPaperMono() || BoardConfig::isM5PaperV11() || BoardConfig::ACTIVE.input.power < 0) {
    return true;
  }

  constexpr unsigned long POWER_WAKE_STABILITY_MS = 10;
  const bool heldAtFirstSample = inputMgr.isPowerButtonPhysicallyPressed();
  const unsigned long sampleStart = millis();
  inputMgr.update();
  while (millis() - sampleStart < POWER_WAKE_STABILITY_MS || inputMgr.isDebouncePending()) {
    delay(1);
    inputMgr.update();
  }
  return heldAtFirstSample && inputMgr.isPowerButtonPhysicallyPressed();
}

bool HalGPIO::isUsbConnected() const {
  if (deviceIsX3()) {
    // X3: infer USB/charging via BQ27220 Current() register (0x0C, signed mA).
    // Positive current means charging.
    for (uint8_t attempt = 0; attempt < 2; ++attempt) {
      int16_t currentMa = 0;
      if (X3GPIO::readBQ27220CurrentMA(&currentMa)) {
        return currentMa > 0;
      }
      delay(2);
    }
    return false;
  }
  if (BoardConfig::ACTIVE.usbDetect >= 0) {
    return digitalRead(BoardConfig::ACTIVE.usbDetect) == HIGH;
  }
  // No digital USB-detect line (e.g. Sticky, whose PWR_IN_VOLT is an analog
  // divider): infer external power from charging state instead. BatteryMonitor
  // picks the board's best source — charger IC status, gauge Current() sign, or
  // a /STAT pin — and reports false on boards with no battery telemetry at all.
  // Caveat: charge termination at 100% reads as "not connected".
  static const BatteryMonitor battery;
  return battery.isCharging();
}

bool HalGPIO::coldBootImpliesPowerButton() const {
  // Xteink-style power topology: the power button energizes the rail until
  // firmware latches it, so a no-USB POWERON can only be a still-held button
  // boot, and plugging USB into an off device should charge-sleep, not boot.
  // Everything else boots on any cold boot: boards with no USB detection at
  // all (M5Paper v1.1, PaperColor, Murphy, de-link) would misread USB and
  // post-flash boots as battery button boots, and STAT-only boards like the
  // EEGO A4 misread them the same way once the charger terminates at 100%
  // (STAT inactive reads as "no USB").
  return isXteinkDevice() || BoardConfig::isPaperMono() || BoardConfig::isSticky();
}

HalGPIO::WakeupReason HalGPIO::getWakeupReason() const {
  const auto wakeupCause = esp_sleep_get_wakeup_cause();
  const auto resetReason = esp_reset_reason();

  const bool usbConnected = isUsbConnected();

  if (resetReason == ESP_RST_DEEPSLEEP &&
      (wakeupCause == ESP_SLEEP_WAKEUP_GPIO || wakeupCause == ESP_SLEEP_WAKEUP_EXT1)) {
    return WakeupReason::PowerButton;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && !usbConnected &&
      coldBootImpliesPowerButton()) {
    return WakeupReason::PowerButton;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_UNKNOWN && usbConnected) {
    return WakeupReason::AfterFlash;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && usbConnected) {
    return WakeupReason::AfterUSBPower;
  }
  return WakeupReason::Other;
}
