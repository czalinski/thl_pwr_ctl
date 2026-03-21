# THL Power Control — Board Bring-Up Procedure

This document guides an engineer through the assembly and verification of a new
THL Power Control PCB. Because this is an unproven design and the board is
hand-soldered, a **fault introduced early can damage components added later**.
The bring-up follows a strict incremental sequence: solder a small set of
components, verify correct operation, then proceed to the next stage. Do not
advance to the next stage until the current stage passes.

---

## Tools and Equipment Required

- Soldering iron with fine tip (recommended ≤ 350 °C)
- Solder (lead-free or leaded, 0.3–0.5 mm diameter)
- Flux pen
- Multimeter (continuity and DC voltage)
- USB-A to Micro-USB cable (for powering and programming the Pico)
- Linux, macOS, or Windows host PC with USB port
- Pico SDK toolchain installed (or pre-built `self_test.uf2` binary)

---

## Stage 1 — Pico and OLED Headers

### Components to solder

| Ref | Component | Notes |
|-----|-----------|-------|
| U1  | WizNet W5500-EVB-Pico | Solder 40-pin header to PCB |
| J?  | SSD1306 OLED display | Solder 4-pin I2C header (VCC, GND, SCL, SDA) |

### Before soldering — visual checks

1. Inspect the bare PCB under good lighting. Confirm no visible shorts, lifted
   traces, or manufacturing defects in the area of U1 and the OLED header.
2. Verify the OLED header footprint pin order on the PCB matches the display
   module pin order (VCC · GND · SCL · SDA is common but some modules differ —
   confirm against the actual module before soldering).

### Soldering

1. Solder the **40-pin header** for the W5500-EVB-Pico. Tack two corner pins
   first, verify the header sits flush and square, then solder the remaining
   pins. Inspect each joint under magnification.
2. Solder the **4-pin OLED header**. Same approach — tack, check alignment,
   complete.
3. Seat the W5500-EVB-Pico and the OLED display module onto their headers.

### Post-solder checks — before applying power

1. **Continuity check — power rails:** With the Pico not yet powered, use the
   multimeter in continuity mode. Probe between the OLED VCC pin and the 3.3 V
   rail, and between OLED GND and board GND. Confirm no short between VCC and
   GND.
2. **I2C lines:** Confirm SCL and SDA (GP2 and GP3) are not shorted to each
   other or to GND/VCC.

### Firmware

Flash the `self_test` firmware onto the Pico before powering the board for the
first time.

1. Hold the **BOOTSEL** button on the Pico and connect the USB cable to the
   host PC. The Pico mounts as a USB mass-storage device (`RPI-RP2`).
2. Copy `build/self_test.uf2` to the mounted drive. The Pico reboots
   automatically.

To build the firmware from source:

```bash
cmake -B build
cmake --build build --target self_test
# Output: build/self_test.uf2
```

### Expected behaviour

Once the board is powered over USB the OLED shows a results screen within
3 seconds. At Stage 1 only the OLED is fitted; all other rows show `NONE`:

```
    THL PWR CTL
      Self Test

OLED:    PASS
EEPROM:  NONE
RS485-H: NONE
RS485-S: NONE
```

The display refreshes every 5 seconds to confirm the board is still running.

USB serial output (view on Linux with `minicom -b 115200 -D /dev/ttyACM0`;
device node may differ — check `dmesg` after plugging in):

```
[SELF-TEST] Starting
[SELF-TEST] OLED: PASS
[SELF-TEST] EEPROM: NONE (not fitted)
[SELF-TEST] RS485-H: NONE (not fitted)
[SELF-TEST] RS485-S: NONE (not fitted)
```

### Fault diagnosis

| Symptom | Likely cause | Action |
|---------|--------------|--------|
| OLED blank, no serial output | Pico not running | Re-flash `self_test.uf2`; confirm USB cable carries data (not charge-only) |
| OLED blank, serial shows `ERROR: SSD1306 not found` | I2C wiring fault | Check solder joints on OLED header GP2/GP3; verify OLED VCC is 3.3 V |
| OLED blank, serial shows `ERROR: SSD1306 not found` | OLED pin order mismatch | Confirm VCC/GND/SCL/SDA order matches PCB footprint |
| Display shows garbage or partial pixels | Bad solder joint on SDA or SCL | Reflow GP2 / GP3 header pins |
| Onboard LED fast-blinking, no OLED output | I2C bus fault (no ACK from display) | Same as "not found" above |

### Stage 1 pass criteria

- [ ] OLED shows `OLED: PASS` within 3 seconds of USB power being applied.
- [ ] Display refreshes every 5 seconds.
- [ ] No hot components, no smell of burning.

**Do not proceed to Stage 2 until all Stage 1 pass criteria are met.**

---

---

## Stage 2 — EEPROM

### Components to solder

| Ref | Component | Notes |
|-----|-----------|-------|
| U?  | W24C02 EEPROM | I2C1, address 0x50, GP2/GP3 |
| C?  | Bypass capacitor (100 nF) | Place as close to EEPROM VCC pin as possible |

### Before soldering — visual checks

1. Confirm Stage 1 pass criteria were met before proceeding.
2. Inspect the EEPROM and bypass capacitor footprints for lifted pads or
   debris.
3. Verify the EEPROM orientation marking against the PCB silkscreen.

### Soldering

1. Solder the **bypass capacitor** first — it is the smaller component and
   easier to place before the EEPROM body is in the way.
2. Solder the **W24C02 EEPROM**. Tack one corner pin, verify alignment, then
   solder remaining pins. Inspect for bridges under magnification.

### Post-solder checks — before applying power

1. **Continuity — power:** Confirm no short between EEPROM VCC and GND.
2. **I2C lines:** Confirm SDA (GP2) and SCL (GP3) remain un-shorted. The OLED
   and EEPROM share the same I2C1 bus; both devices should now be visible on
   it.
3. **Bypass cap orientation:** If polarised, confirm correct polarity.

### Firmware

The same `self_test.uf2` binary is used for all stages. Re-flash if the
binary on the board pre-dates Stage 2 support (i.e., was built before this
stage was added).

```bash
cmake --build build --target self_test
# Copy build/self_test.uf2 to the Pico via BOOTSEL
```

### Expected behaviour

The OLED shows a results screen within 3 seconds of power-up. At Stage 2
the RS-485 transceivers are not yet fitted so those rows show `NONE`:

```
    THL PWR CTL
      Self Test

OLED:    PASS
EEPROM:  PASS
RS485-H: NONE
RS485-S: NONE
```

USB serial output shows each EEPROM write/read-back cycle:

```
[SELF-TEST] EEPROM: wrote 0xAA  read 0xAA  OK
[SELF-TEST] EEPROM: wrote 0x55  read 0x55  OK
[SELF-TEST] EEPROM: wrote 0xA5  read 0xA5  OK
[SELF-TEST] EEPROM: PASS
```

The display refreshes every 5 seconds to confirm the board is still running.

### Fault diagnosis

| Symptom | Likely cause | Action |
|---------|--------------|--------|
| `EEPROM: NONE` on OLED | EEPROM not ACK-ing | Check solder joints on all EEPROM pins; confirm VCC is 3.3 V |
| `EEPROM: NONE` on OLED | Wrong I2C address | Confirm A0/A1/A2 address pins are tied to GND (address = 0x50) |
| `EEPROM: FAIL` on OLED | Write/read mismatch | Check SDA (GP2) and SCL (GP3) joints for cold joints or bridges |
| `EEPROM: FAIL` on OLED | Bus contention | Confirm no other device is holding SDA/SCL low |
| OLED shows `OLED: PASS` but `EEPROM: FAIL` | EEPROM data corruption | Reflow EEPROM joints; replace component if persists |

### Stage 2 pass criteria

- [ ] OLED displays `OLED: PASS` and `EEPROM: PASS` within 3 seconds of power-up.
- [ ] USB serial shows all three write/read-back cycles as `OK`.
- [ ] No hot components, no smell of burning.

**Do not proceed to Stage 3 until all Stage 2 pass criteria are met.**

---

---

## Stage 3 — RS-485 Transceivers

Both RS-485 channels use the **ISL83488IBZ** full-duplex RS-485/RS-422
transceiver. UART1 (hardware) uses GP4/GP5; UART3 (software bit-bang) uses
GP14/GP15. Each transceiver has separate driver outputs (Y/Z) and receiver
inputs (A/B). DE is permanently tied HIGH and RE is permanently tied LOW —
no firmware direction control is needed.

The loopback test wires Y/Z back to A/B on each channel so that every
transmitted bit is immediately received back through the transceiver.

### Components to solder

| Ref | Component | Notes |
|-----|-----------|-------|
| U?  | ISL83488IBZ — UART1 channel | GP4 → DI, GP5 ← RO; DE tied HIGH, RE tied LOW |
| U?  | ISL83488IBZ — UART3 channel | GP14 → DI, GP15 ← RO; DE tied HIGH, RE tied LOW |
| C?  | Bypass capacitor ×2 (100 nF) | One per transceiver, as close to VCC as possible |
| R?  | 120 Ω termination resistor ×2 | One per channel — between Y and Z, and between A and B |
| J?  | Termination jumpers | Connect both jumpers to enable termination on each side |
| J?  | Loopback wiring / headers | Wire Y to A and Z to B on each channel |

### Before soldering — visual checks

1. Confirm Stage 2 pass criteria were met before proceeding.
2. Inspect ISL83488IBZ footprints and verify pin 1 orientation against PCB
   silkscreen. Incorrect orientation will not damage the device at 3.3 V but
   will cause the test to fail.
3. Confirm DE and RE tie-off resistors/traces are present on the PCB. DE must
   be pulled to VCC and RE must be pulled to GND for full-duplex operation.

### Soldering

1. Solder **bypass capacitors** first.
2. Solder both **ISL83488IBZ** transceivers. Tack one corner pin per IC,
   verify alignment, then complete. Inspect closely for bridges — this is an
   SOIC package with fine pitch.
3. Solder the **120 Ω termination resistors**.
4. Connect the **termination jumpers** (both channels, both ends).

### Post-solder checks — before applying power

1. **DE and RE:** Confirm DE is pulled to VCC (≈3.3 V) and RE to GND on both
   ICs. A misconnected DE or RE will cause NONE or FAIL in firmware.
2. **Termination:** Confirm 120 Ω measures between Y and Z, and between A and
   B, on each channel with the jumpers connected.
3. **Loopback wiring:** Confirm Y is connected to A, and Z to B, on each
   channel. Swapping Y/Z or A/B will cause a FAIL (inverted signal).
4. **Power:** No short between VCC and GND on either IC.

### Firmware

Re-flash `self_test.uf2` — this stage requires the firmware built after
Stage 3 was added.

```bash
cmake --build build --target self_test
# Copy build/self_test.uf2 to the Pico via BOOTSEL
```

### Expected behaviour

```
    THL PWR CTL
      Self Test

OLED:    PASS
EEPROM:  PASS
RS485-H: PASS
RS485-S: PASS
```

USB serial shows each byte of the loopback sequence for both channels:

```
[SELF-TEST] RS485-H: sent 0x52  recv 0x52  OK
[SELF-TEST] RS485-H: sent 0x53  recv 0x53  OK
[SELF-TEST] RS485-H: sent 0x34  recv 0x34  OK
[SELF-TEST] RS485-H: sent 0x38  recv 0x38  OK
[SELF-TEST] RS485-H: sent 0x35  recv 0x35  OK
[SELF-TEST] RS485-H: PASS
[SELF-TEST] RS485-S: byte 0x52 ('R')  OK
[SELF-TEST] RS485-S: byte 0x53 ('S')  OK
[SELF-TEST] RS485-S: byte 0x34 ('4')  OK
[SELF-TEST] RS485-S: byte 0x38 ('8')  OK
[SELF-TEST] RS485-S: byte 0x35 ('5')  OK
[SELF-TEST] RS485-S: PASS
```

### Fault diagnosis

| Symptom | Likely cause | Action |
|---------|--------------|--------|
| `RS485-H: NONE` | HW UART1 loopback open | Check Y→A and Z→B wiring on UART1 channel; confirm DE is high |
| `RS485-S: NONE` | SW UART3 loopback open | Check Y→A and Z→B wiring on UART3 channel; confirm DE is high |
| `RS485-H: FAIL` | Byte mismatch on UART1 | Check for Y/Z or A/B swap; inspect ISL83488IBZ solder joints on GP4/GP5 side |
| `RS485-S: FAIL` | Bit mismatch on UART3 | Check GP14/GP15 ISL83488IBZ joints; confirm RE is tied LOW |
| Either channel `FAIL`, inverted data | Y/Z or A/B polarity swapped | Swap the loopback wires (Y↔Z or A↔B) |
| Correct bytes received but intermittent | Poor solder joint or marginal termination | Reflow joints; confirm 120 Ω termination on both sides |

### Stage 3 pass criteria

- [ ] OLED shows `RS485-H: PASS` and `RS485-S: PASS`.
- [ ] USB serial shows all five bytes `OK` for both channels.
- [ ] No hot components, no smell of burning.

**Do not proceed to Stage 4 until all Stage 3 pass criteria are met.**

---

*Further stages will be added as components are verified and added to the build-up sequence.*
