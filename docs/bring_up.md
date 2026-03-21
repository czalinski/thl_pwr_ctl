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

Once the board is powered over USB:

- The OLED display illuminates and shows **"OLED Working"** centred on the
  screen.
- The message refreshes every **1 second**.
- A matching debug line is printed over USB serial each second:
  ```
  [SELF-TEST] loop=0  OLED Working
  [SELF-TEST] loop=1  OLED Working
  ...
  ```
  To view the serial output on Linux: `minicom -b 115200 -D /dev/ttyACM0`
  (device node may differ — check `dmesg` after plugging in).

### Fault diagnosis

| Symptom | Likely cause | Action |
|---------|--------------|--------|
| OLED blank, no serial output | Pico not running | Re-flash `self_test.uf2`; confirm USB cable carries data (not charge-only) |
| OLED blank, serial shows `ERROR: SSD1306 not found` | I2C wiring fault | Check solder joints on OLED header GP2/GP3; verify OLED VCC is 3.3 V |
| OLED blank, serial shows `ERROR: SSD1306 not found` | OLED pin order mismatch | Confirm VCC/GND/SCL/SDA order matches PCB footprint |
| Display shows garbage or partial pixels | Bad solder joint on SDA or SCL | Reflow GP2 / GP3 header pins |
| Onboard LED fast-blinking, no OLED output | I2C bus fault (no ACK from display) | Same as "not found" above |

### Stage 1 pass criteria

- [ ] OLED displays **"OLED Working"** within 3 seconds of USB power being applied.
- [ ] Message refreshes visibly every second.
- [ ] No hot components, no smell of burning.

**Do not proceed to Stage 2 until all Stage 1 pass criteria are met.**

---

*Further stages will be added as components are verified and added to the build-up sequence.*
