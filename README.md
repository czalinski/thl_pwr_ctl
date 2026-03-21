# THL Power Control Firmware

Embedded C firmware for a custom PCB power control system built around the
[WizNet W5500-EVB-Pico](https://docs.wiznet.io/Product/iEthernet/W5500/w5500-evb-pico)
(RP2040 + W5500 Ethernet).

## Repository Structure

```
thl_pwr_ctl/
├── requirements/       # Project requirements documentation
│   └── requirements.md
├── src/                # Application source files (.c)
├── include/            # Header files (.h)
├── lib/                # Third-party / vendor libraries
├── docs/               # Schematics, datasheets, design notes
├── build/              # Build output (git-ignored)
├── CMakeLists.txt      # Pico SDK build (VS Code)
└── README.md
```

## Toolchain Options

### VS Code + Pico SDK (recommended for C development)
1. Install the [Raspberry Pi Pico extension](https://marketplace.visualstudio.com/items?itemName=raspberry-pi.raspberry-pi-pico)
2. Open this folder in VS Code
3. Build via `cmake` / the extension UI

### Arduino IDE
- Board: **Raspberry Pi Pico / RP2040** (Earle Philhower core)
- Sketch entry point lives in `src/`

## Hardware

| Component | Detail |
|-----------|--------|
| Board | WizNet W5500-EVB-Pico |
| MCU | RP2040 (dual-core Cortex-M0+, 133 MHz) |
| Ethernet | W5500 hardwired TCP/IP offload |
| Flash | 2 MB onboard |
| Interface | USB (UF2 drag-and-drop or picotool) |
