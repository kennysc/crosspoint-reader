# BQ27220 Fuel Gauge Findings (Xteink X3)

Investigation into premature-shutdown reports on X3 hardware, driven by a raw
I2C register dump of the BQ27220 fuel gauge (`X3GPIO::readBQ27220RawRegisters`,
`lib/hal/HalGPIO.cpp`, branch `bq27220-diagnostic-dump`) decoded against the
official TI BQ27220 Technical Reference Manual (SLUUBD4A,
<https://www.ti.com/lit/ug/sluubd4a/sluubd4a.pdf>).

## Method

`dumpBq27220DiagnosticsToSd` (`lib/hal/HalPowerManager.cpp`) writes
`/.crosspoint/bq27220_dump.txt` at boot on X3 devices: named Standard Command
fields, then a raw hex dump of I2C addresses `0x00`-`0x7F` in 16-byte rows.
Every byte below was decoded against TRM Table 2-1 (Standard Commands),
Table 2-6 (`BatteryStatus()` bits), Table 2-7 (`OperationStatus()` bits), and
Table 3-2 (Data Memory addresses/defaults), then cross-checked for internal
consistency (e.g. `RemainingCapacity() / StandbyCurrent() == StandbyTimeToEmpty()`
to the minute, `RawVoltage() ≈ Voltage()`). All decoded values below are
self-consistent, which rules out a corrupted or misaligned dump.

## Captured sample

```
Voltage_mV: 3734
StateOfCharge_pct: 86
FullChargeCapacity_mAh: 3000
RemainingCapacity_mAh: 2558
DesignCapacity_mAh: 3000
OperationStatus_raw: 0x00A4
DeviceNumber_raw: 0x740E

00: 40 00 00 3A FF FF B0 0B 96 0E 29 40 E4 FF FF FF
10: FE 09 B8 0B D7 FF 9F 0E FF FF F3 FF 1E 2E 18 FC
20: 99 00 C8 01 F1 FF FF FF B0 0B 00 00 56 00 64 00
30: 68 10 C8 00 96 00 AF 00 FF FF A4 00 B8 0B A3 92
40: 0E 74 00 64 0E 9F 00 95 03 63 0F BE 01 3C 09 00
50: 00 0B D7 01 0D 39 01 0D AD 01 10 4D 0F CB 0F 55
60: A9 24 FF FF FF FF FF FF FF FF FF FF FF FF FF FF
70: FF FF FF FF FF FF FF FF FF 37 74 00 98 0E B0 0B
```

## Decoded register map

| Addr | Bytes | Value | Command (TRM Table 2-1) | Notes |
|---|---|---|---|---|
| 0x00/01 | 40 00 | 0x0040 | `Control()`/`CONTROL_STATUS()` | low-byte bit6 set; Table 2-3 marks bit6 RSVD (low-confidence anomaly) |
| 0x02/03 | 00 3A | 14848 mA | `AtRate()` | TRM: default is 0; nonzero here with no host ever calling `AtRate()` |
| 0x04/05 | FF FF | 65535 | `AtRateTimeToEmpty()` | sentinel means `AtRate()=0` per TRM — **contradicts** 0x02/03 above |
| 0x06/07 | B0 0B | 2992 (299.2K = 26.05°C) | `Temperature()` | plausible ambient reading |
| 0x08/09 | 96 0E | 3734 mV | `Voltage()` | matches named field |
| 0x0A/0B | 29 40 | 0x4029 | `BatteryStatus()` | OCVCOMP, OCVGD, BATTPRES, DSG set — fully consistent with a discharging, present battery |
| 0x0C/0D | E4 FF | -28 mA | `Current()` | matches DSG bit above |
| 0x0E/0F | FF FF | — | *(reserved, undocumented)* | correctly `0xFFFF` |
| 0x10/11 | FE 09 | 2558 mAh | `RemainingCapacity()` | matches named field |
| 0x12/13 | B8 0B | 3000 mAh | `FullChargeCapacity()` | matches named field; **= TRM stock default** (Table 3-2, addr `0x929D`) |
| 0x14/15 | D7 FF | -41 mA | `AverageCurrent()` | |
| 0x16/17 | 9F 0E | 3743 min | `TimeToEmpty()` | = `RemainingCapacity()/AverageCurrent()` to within rounding |
| 0x18/19 | FF FF | 65535 | `TimeToFull()` | not charging |
| 0x1A/1B | F3 FF | -13 mA | `StandbyCurrent()` | |
| 0x1C/1D | 1E 2E | 11806 min | `StandbyTimeToEmpty()` | = `RemainingCapacity()/StandbyCurrent()` exactly |
| 0x1E/1F | 18 FC | -1000 mA | `MaxLoadCurrent()` | |
| 0x20/21 | 99 00 | 153 min | `MaxLoadTimeToEmpty()` | = `RemainingCapacity()/MaxLoadCurrent()` exactly |
| 0x22/23 | C8 01 | 456 | `RawCoulombCount()` | |
| 0x24/25 | F1 FF | -15 mW | `AveragePower()` | negative = discharging, consistent |
| 0x26/27 | FF FF | — | *(reserved, undocumented)* | correctly `0xFFFF` |
| 0x28/29 | B0 0B | 2992 | `InternalTemperature()` | = `Temperature()`, same sensor |
| 0x2A/2B | 00 00 | 0 | `CycleCount()` | **gauge has never completed a learning cycle** |
| 0x2C/2D | 56 00 | 86 | `RelativeStateOfCharge()` | matches named field |
| 0x2E/2F | 64 00 | 100 | `StateOfHealth()` | = `FullChargeCapacity()/DesignCapacity()`, consistent w/ CycleCount=0 |
| 0x30/31 | 68 10 | 4200 mV | `ChargeVoltage()` | **= TRM stock default** (Table 3-2, addr `0x91FD`) |
| 0x32/33 | C8 00 | 200 mA | `ChargeCurrent()` | **= TRM stock default** (Table 3-2, addr `0x91FB`) |
| 0x34/35 | 96 00 | 150 mAh | `BTPDischargeSet()` | **= TRM stock default** (Table 3-2, addr `0x920E`) |
| 0x36/37 | AF 00 | 175 mAh | `BTPChargeSet()` | **= TRM stock default** (Table 3-2, addr `0x9210`) |
| 0x38/39 | FF FF | — | *(reserved, undocumented)* | correctly `0xFFFF` |
| 0x3A/3B | A4 00 | 0x00A4 | `OperationStatus()` | BTPINT, INITCOMP set; SEC[1:0]=`10` = **gauge is UNSEALED** |
| 0x3C/3D | B8 0B | 3000 mAh | `DesignCapacity()` | matches named field; **= TRM stock default** (Table 3-2, addr `0x929F`); **actual battery is 650mAh** |
| 0x3E/3F | A3 92 | address `0x92A3` | *(undocumented as a Standard Command)* | see Finding 4 |
| 0x40-5F | (32-byte block) | `DeviceNumber` = `0x740E` at 0x40/41 | `MACData()` | see Finding 2; remaining bytes are leftover MAC buffer content, not decodable without controlled sequencing |
| 0x60/61 | A9 24 | checksum=0xA9, len=0x24 (36) | `MACDataSum()`/`MACDataLen()` | of the last MAC transaction |
| 0x62-78 | FF... | — | *(reserved, undocumented)* | correctly `0xFFFF` |
| 0x79 | 37 | 55 | `AnalogCount()` | |
| 0x7A/7B | 74 00 | 116 | `RawCurrent()` | |
| 0x7C/7D | 98 0E | 3736 | `RawVoltage()` | ≈ `Voltage()` (3734), raw ADC vs. compensated |
| 0x7E/7F | B0 0B | 2992 | `RawIntTemp()` | = `InternalTemperature()`/`Temperature()` |

## Findings

### 1. CEDV profile is unmodified TI factory defaults (primary finding)

`ChargeVoltage` (4200 mV), `ChargeCurrent` (200 mA), `BTPDischargeSet` (150 mAh),
`BTPChargeSet` (175 mAh), and `DesignCapacity`/seed `FullChargeCapacity`
(3000 mAh) all read back *exactly* TI's documented factory defaults
(TRM Table 3-2). `CycleCount()=0` confirms the gauge has never completed a
qualified learning cycle since these were seeded.

The real battery on this unit is **650 mAh**, not 3000 mAh.
`StateOfCharge() = RemainingCapacity() / FullChargeCapacity()`, so the gauge
is computing SOC% against a reference cell ~4.6x larger than the physical
pack. This means SOC% reads far higher than actual remaining energy, and the
pack hits its real empty-voltage point well before SOC% reaches 0% —
consistent with reported premature shutdowns.

`DesignVoltage` (Table 3-2, addr `0x92A3`, default 3700 mV) is a chemistry
property, not capacity-dependent, and does not need to change for a standard
1S Li-ion/LiPo cell.

### 2. `DeviceNumber` mismatch

`DEVICE_NUMBER()` returns `0x740E`. TRM p.19: "This command instructs the
fuel gauge to return the device type `0x0220` to `MACData()`." Table 3-2
confirms `Device Type` (Configuration/Registers, addr `0x9212`) defaults to
`0x0220`. `Device Type` is a writable Data Memory field, so this alone isn't
proof of non-genuine silicon — an OEM could have deliberately reprogrammed
it — but it's unexplained and worth flagging as a live possibility.

### 3. Gauge boots UNSEALED

`OperationStatus()` SEC[1:0] = `10` (Unsealed). TRM p.27 confirms this is
the documented power-on default ("The default access mode of the fuel gauge
is UNSEALED, so the system processor must send a SEALED subcommand after a
gauge reset to utilize this data protection feature"). Nothing in this
firmware currently seals the gauge. This resolves — rather than causes — the
"unresolved Data Memory (BlockData) access issue" noted in this branch's
original commit: Data Memory access requires UNSEALED-or-better, and the
gauge is already sitting in that state at boot, so seal state was not the
blocker.

### 4. `0x3E`/`0x3F` holds a live Data Memory address, and the TRM's own worked example has a byte-order error

TRM lists no Standard Command at `0x3E`/`0x3F` — it should read `0xFFFF`
like every other genuine gap (`0x0E/0F`, `0x26/27`, `0x38/39`, all correctly
`0xFFFF` in this dump). Instead it holds `A3 92`.

TRM §3.1 explains the mechanism: writing a 16-bit Data Memory address to
`0x3E`/`0x3F` makes that address's containing 32-byte block accessible via
`BlockData()` (`0x40`-`0x5F`), at command offset `0x40 + (address mod 32)`.

Decoding `0x3E`/`0x3F` little-endian — byte@`0x3E` = low byte, byte@`0x3F` =
high byte, consistent with every other 16-bit register on this device —
`A3 92` = address `0x92A3`, which is exactly `DesignVoltage`'s documented
Data Memory address (Table 3-2). This is strong empirical confirmation, from
live silicon, that (a) this addressing mechanism is functional on this
device, and (b) the byte order is little-endian.

This directly contradicts TRM Chapter 6.1's own worked example, which
describes writing `DesignCapacity`'s address as "Write `0x9F` to `0x3E` to
access the **MSB**" / "Write `0x92` to `0x3F` to access the **LSB**" — i.e.
big-endian (`0x3E`=high, `0x3F`=low), which would produce address `0x9F92`.
Taken literally, that does not match `DesignCapacity`'s documented address
(`0x929F`, Table 3-2). Decoding the *same* example bytes little-endian
(`0x3E`=low=`0x9F`, `0x3F`=high=`0x92`) reconstructs `0x929F` exactly — the
correct, documented address. **TRM Chapter 6.1's MSB/LSB labels for
`0x3E`/`0x3F` are backwards; the actual byte order is little-endian**,
confirmed both by reconciling the TRM's own example and by this device's
live leftover pointer value. Anyone implementing this procedure from the TRM
text literally will address the wrong Data Memory location.

## Data Memory write attempt: failed, root cause unresolved

An attempt to reprogram `DesignCapacity`/`FullChargeCapacity` to 650 mAh via
the TRM §6.1 Data Memory update procedure (`X3GPIO::writeBQ27220DataMemoryField`,
`lib/hal/HalGPIO.cpp`) was made and **did not succeed** on real hardware,
across ~9 flash/boot iterations. Findings, in the order established:

1. **`FullChargeCapacity` write (single block, offset 29, no boundary
   crossing)**: every individual I2C transaction (address-window write,
   `BlockData()` byte commits, checksum write, length write) ACKed
   successfully, but a post-write readback showed the target bytes reverted
   to `00 00` — neither the old value (`B8 0B` = 3000) nor the new one
   (`8A 02` = 650). `BlockDataLen()` was tried as both `32` (documented max
   block size, TRM §3.1) and `0x24`/36 (TRM §6.1's literal worked-example
   value, which itself exceeds that documented max — likely a TRM erratum);
   neither committed the write.
2. **Checksum formula validated independently**: the device's own
   `MACDataSum()`/`BlockDataSum()` (`0x60`) readback of the *unmodified*
   block matched this codebase's software computation
   (`255 - (byte-sum mod 256)`) exactly. The checksum algorithm itself is not
   the problem.
3. **Sanity check on a non-capacity field**: `BTPDischargeSet` (`0x920E`,
   offset 14, single block) was written back to its own current value (150
   mAh — a no-op if the mechanism worked), to isolate "capacity fields are
   locked" from "the write mechanism doesn't work at all." It also failed:
   the low byte (offset 14) landed correctly, but offset 15 (the intended
   high byte, `0x00`) read back as `0x4F` — the register's *own address*
   (`0x40 + 15 = 0x4F`) — and offsets 17-19 read back `60 3E 61`: the
   checksum register address, the checksum value that was sent, and the
   length register address, i.e. bytes from the write's own later
   transactions appearing inside the block at unrelated offsets.
4. **Ruled out**: this codebase's own extra diagnostic readback reads
   (added to investigate point 3) as the cause. Repeating the identical
   `BTPDischargeSet` test with those reads removed (`verboseDiagnostics=false`
   parameter added to `writeBQ27220DataMemoryField`) reproduced the exact
   same corruption pattern at the exact same offsets.
5. **Most significant finding**: comparing the pre-write block snapshot
   (`expected`, i.e. what `readI2CBlock()` returned *before* any
   modification) between two otherwise-identical `BTPDischargeSet` test runs
   showed offsets 17-19 differing between runs (`00 00 00` vs. `60 3F 61`) —
   **before this codebase had written anything in either run.** This means
   the anomalous content is not something the write sequence corrupts; it's
   already unstable/non-deterministic at that memory location on read,
   independent of any write attempt.

Taken together, this points to memory at these Data Memory addresses not
behaving as genuine, stable, TI-documented calibration storage on this
specific chip — plausibly uninitialized/floating memory, or an aliased
internal debug/trace buffer, rather than a Data Memory read/write bug
fixable by adjusting checksum, length, or timing values from firmware.
Confirming the actual cause would need hardware-level I2C bus analysis
(logic analyzer), which is out of scope for a firmware-only investigation.

**Nothing was corrupted or lost by these attempts.** `BTPDischargeSet`,
`ChargeVoltage`, `ChargeCurrent`, `DesignCapacity`, and `FullChargeCapacity`
all still read their original, unmodified factory-default values after every
attempt — TRM §3.1's "written data is not persistent" behavior held, and
`EXIT_CFG_UPDATE_REINIT` cleanly discarded the failed staging attempts each
time.

## Second investigation round: full TRM re-read, TI's actual tool-generated protocol, permission escalation

A second round (~15 further flash/boot iterations) started from a full,
complete re-read of the TRM (all 89 pages, including Chapter 4 "Functional
Description," Chapter 8 "Updating BQ27220 Configuration Parameters," and the
Revision History appendix, none of which had been read in the first round).
This surfaced real, concrete new procedure details, but still did not
achieve a successful write.

1. **TRM §5.3 "I2C Command Waiting Time"**: at 400 kHz (this device's
   `X3_I2C_FREQ`), a documented `t(BUF) >= 66 us` bus-free gap is required
   between every I2C packet addressed to the gauge. The original
   implementation had no such gap between many consecutive transactions.
   Fixed: every low-level I2C primitive in `HalGPIO.cpp`
   (`readI2CReg16LE`, `writeI2CReg8`, `readI2CReg8`, `readI2CBlock`,
   `writeI2CBlock`) now calls `delayMicroseconds(BQ27220_I2C_BUS_FREE_US)`
   (100 us) after every transaction, success or failure. This is a real fix,
   independently worth keeping regardless of the outcome below.
2. **TRM §4.6, Note ("Hibernate I" example)**: a second, much more specific
   worked example than Chapter 6.1's — TI support-note style, concrete hex
   bytes, phrased as tested guidance ("it is highly recommended to..."), not
   generic/illustrative text. It targets Control() subcommands (`0x0090`
   ENTER_CFG_UPDATE, `0x0091` EXIT_CFG_UPDATE_REINIT) by writing directly to
   `0x3E`, not to `Control()` (`0x00`) as documented everywhere else in the
   TRM. Replicating this exact mechanism (`X3GPIO::writeI2CBlock` combined
   multi-byte transactions instead of split single-byte writes) measurably
   improved reliability: `CFGUPDATE` entered on the first attempt every time
   thereafter, vs. frequent retries needed with the Control()-based approach,
   and `BlockData()` readbacks became stable, plausible-looking data instead
   of corrupted bytes that looked like echoed register addresses (`60 3E 61`,
   matching addresses used in nearby transactions).
3. **TI's checksum in that Note (`0x4C`) is example-specific, not
   universal**: it depends on the full 32-byte block's exact prior content,
   which differs per device. Computing it fresh
   (`X3GPIO::writeBQ27220DataMemoryFieldDirect`) was verified correct two
   ways: it matched the device's own `MACDataSum()` readback of the
   unmodified block exactly, and independently matches TRM §6.1's own
   incremental-checksum pseudocode algebraically re-derived
   (`New_Chksum = 255 - sum(new 32 bytes) mod 256`). The checksum was never
   the problem in either investigation round.
4. **Systematically varied every other plausible protocol variable against
   this now-reliable mechanism (Hibernate I, `0x9221`, target value 0) with
   no effect on the outcome**: checksum+length combined write targeting
   `0x61` (TRM §4.6's literal text) vs. `0x60` (Table 2-1's actual
   naming) — no difference; adding TRM §3.1's described `BlockDataControl()`
   "set-up" write (`0x00` to `0x61` before addressing) — no difference;
   extending the post-checksum-write delay from 5 ms to 200 ms — no
   difference. All four variants read back the exact same unchanged bytes.
5. **Retargeted the mechanism at the real goal, `FullChargeCapacity`
   (`0x929D`), instead of continuing to iterate on Hibernate I** — same
   result: write silently does not commit.
6. **TRM §8.1, Figure 8-1**: an actual BQStudio-generated `gm.fs` file (the
   real tool-verified protocol, not a hand-written doc example). It shows
   address write, full 32-byte `BlockData()` write, and single-byte checksum
   write as three separate I2C transactions (not combined), the *entire*
   32-byte block rewritten every time (not just the changed bytes), and
   **no length/`0x61` write at all** for a full-block rewrite — contradicting
   this investigation's working assumption (from TRM §4.6's abbreviated
   partial-write example) that a length byte was required. Verification in
   the real file is done by re-addressing and reading back just the checksum
   register. `writeBQ27220DataMemoryFieldDirect` was rewritten to match this
   exactly. Still failed: the checksum register read back a stable,
   unrelated value (`0x3D`) regardless of what was written to it (`0x6C`,
   reproduced identically across repeated attempts).
7. **Byte-order theory raised and retracted**: an initial `FullChargeCapacity`
   attempt showed offsets 0/1 transposed relative to what was sent (wrote
   `B8 0B`, read `0B B8`), which looked consistent with TRM §6.1's literal
   MSB-first pseudocode for *data* bytes (as opposed to the already-corrected
   LSB-first *address* bytes at `0x3E`/`0x3F` — a genuinely different
   convention TI's own worked example uses for the two). Fixing the write to
   send MSB-first did **not** change the result: the readback was
   byte-for-byte identical to the previous (LSB-first) attempt's readback.
   Since a write that never commits leaves the same stale bytes regardless of
   what order they were sent in, this proves the earlier "transposition" was
   coincidental stale content, not evidence of a real byte-order bug.
8. **Permission (`SEC[1:0]`) checked directly, and never once observed
   reaching Full Access.** TRM §6.1 step 2 states Data Memory writes require
   Full Access (not just Unsealed), and gives the key sequence `0xFFFF`
   (sent twice to Control()). This is independently confirmed elsewhere in
   the TRM: §8.5 step 3 says "Enter FULL ACCESS mode to gain access to the
   Data Memory. See Chapter 6 for the procedure" — i.e. the same `0xFFFF`
   key, confirmed correct by two unrelated sections written for different
   audiences (a worked I2C example vs. a BQStudio setup guide). Reading
   `OperationStatus()` immediately after sending this key, across every
   attempt this was checked, showed `SEC[1:0] = 10` (Unsealed) — never `01`
   (Full Access). The alternate key documented elsewhere in the TRM (`0x8000`,
   §3.3's Sealed→Unsealed key) was also tried, on the chance this device's
   actual Full Access key differs from either documented value — same
   result, `SEC[1:0]` stayed at `10`.

**Conclusion**: this is now believed to be the root cause. Every write
attempt in both investigation rounds happened while the gauge was only
Unsealed, never Full Access, which TRM §6.1 explicitly states is
insufficient for Data Memory writes. Both keys the TRM documents for
reaching Full Access were tried and neither worked on this device, and the
full protocol (address/data structure, checksum algorithm, timing,
BlockDataControl() setup, and the confirmed-correct tool-generated wire
format from Chapter 8) has otherwise been verified correct in every part
that's independently checkable from firmware alone. This is consistent with
this being non-genuine or otherwise non-standard silicon (see Finding 2,
`DeviceNumber` mismatch) that does not honor the documented Full Access key
exchange. Confirming or fixing this would need either genuine TI tooling
(EV2300/BQStudio, to test with a known-good reference implementation and
rule out this codebase's I2C driver as a variable) or hardware-level I2C bus
analysis — both out of scope for further firmware-only iteration.

**Nothing was corrupted or lost across either round.** `BTPDischargeSet`,
`ChargeVoltage`, `ChargeCurrent`, `Hibernate I`, `DesignCapacity`, and
`FullChargeCapacity` all still read their original, unmodified
factory-default values after every attempt in both rounds.

## Recommendation

**Do not pursue the Data Memory write path further without genuine TI
tooling (EV2300/BQStudio) or hardware-level I2C bus analysis** — see both
investigation rounds above; the permission/Full-Access finding in
particular cannot be resolved through further protocol guessing. The
capacity mismatch (Finding 1) is real and still needs a fix, but not this
one.

Instead, use this codebase's existing **gauge-independent** voltage-curve
battery percentage calibration: `CrossPointSettings::batteryPercentMode` /
`batteryCustomCurveMv` (`src/CrossPointSettings.h`), configured via the
existing `BatteryVoltageCalibrationActivity`
(`src/activities/settings/BatteryVoltageCalibrationActivity.cpp`) and
applied through `HalPowerManager::setBatteryPercentMode()`. This computes
the reported percentage from a directly-calibrated voltage curve instead of
the gauge chip's internal (in this case unreliable-to-reprogram) `SOC%`/
`FullChargeCapacity` math, so it sidesteps the Data Memory problem entirely
and requires no further I2C risk. Calibrate the curve against the real
650 mAh cell's actual discharge behavior.

If the Data Memory write is revisited later with proper bus-analysis
tooling: `DesignCapacity` (`0x929F`) sits at offset 31 of its 32-byte Data
Memory block — its second byte (`0x92A0`) falls in the *next* block, so
writing it needs two separate address/`BlockData()`/checksum passes.
`FullChargeCapacity` (`0x929D`, offset 29) does not cross a block boundary.
Do not change `ChargeVoltage`/`ChargeCurrent`/BTP thresholds (safe generic
defaults, not capacity-dependent) or `DesignVoltage` (chemistry property,
not capacity). Full CEDV re-characterization (Qmax, R0/R1/EMF impedance
model, EDV0/1/2 thresholds, DOD voltage curve) would in any case need TI's
BQStudio/EV2300 characterization flow against the real cell, independent of
whether the Data Memory write mechanism itself can be made to work.
