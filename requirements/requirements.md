# THL Power Control — Requirements

## Project Overview

Embedded firmware for a custom PCB power control system. The embedded controller is a
**WizNet W5500-EVB-Pico** (RP2040 MCU + W5500 Ethernet) integrated on a custom PCB.
Language: C. Toolchain: Pico SDK (VS Code) or Arduino IDE.

---

## Hardware

| Item | Detail |
|------|--------|
| MCU Board | WizNet W5500-EVB-Pico |
| MCU | Raspberry Pi RP2040 |
| Ethernet | WizNet W5500 (onboard) |
| Host Interface | USB (programming & debug) |
| Target | Custom PCB (power control) |

### GPIO Pin Mapping

All peripheral assignments are fixed by PCB routing. Every firmware target
must use these mappings — do not reassign pins without a hardware revision.

| GPIO | Direction | Function              | Notes                                      |
|------|-----------|-----------------------|--------------------------------------------|
| GP0  | Out       | UART0 TX              |                                            |
| GP1  | In        | UART0 RX              |                                            |
| GP2  | I/O       | I2C1 SDA              | OLED display and future I2C1 peripherals   |
| GP3  | I/O       | I2C1 SCL              | OLED display and future I2C1 peripherals   |
| GP4  | Out       | UART1 TX              |                                            |
| GP5  | In        | UART1 RX              |                                            |
| GP6  | Out       | UART2 TX              |                                            |
| GP7  | In        | UART2 RX              |                                            |
| GP8  | I/O       | I2C0 SDA              |                                            |
| GP9  | I/O       | I2C0 SCL              |                                            |
| GP10 | Out       | SPI1 SCK              |                                            |
| GP11 | Out       | SPI1 MOSI             |                                            |
| GP12 | In        | SPI1 MISO             |                                            |
| GP13 | Out       | SPI1 CS               |                                            |
| GP14 | Out       | UART3 TX              |                                            |
| GP15 | In        | UART3 RX              |                                            |
| GP22 | In        | Factory Reset detect  | Pulled high internally; driven low to request reset |
| GP26 | Out       | PSU 2 enable          | Drive high to enable PSU 2                 |
| GP27 | Out       | PSU 1 enable          | Drive high to enable PSU 1                 |
| GP28 | Out       | Factory Reset LED     | Drive high to illuminate LED               |

### I2C Device Map

#### I2C1 — GP2 (SDA) / GP3 (SCL)

| Address | Device      | Role                  |
|---------|-------------|-----------------------|
| 0x20    | TCA9555PWR  | GPIO expander         |
| 0x3C    | SSD1306     | 128×64 OLED display   |
| 0x50    | W24C02      | EEPROM                |

#### I2C0 — GP8 (SDA) / GP9 (SCL)

| Address | Device  | Role                                      |
|---------|---------|-------------------------------------------|
| 0x36    | MCP3428 | PSU current and voltage ADC measurement   |
| 0x37    | MCP3428 | External analog input ADC                 |

---

## Requirements

### REQ-001 — Factory-Default Network Configuration
> On first power-up (or after a factory reset), the device shall present a static IPv4
> address of **172.22.77.77** with a 16-bit subnet mask (255.255.0.0) on the wired
> LAN port (W5500). The factory-default gateway shall be 172.22.0.1.
> The chosen range (172.22.0.0/16) is RFC-1918 private address space, avoids the
> commonly-defaulted 172.16–17.x and 172.31.x ranges, and provides a full /16
> address space for the device LAN.

### REQ-002 — Web-Based Configuration Interface
> The device shall host an HTTP server on **port 80**. Any browser on the same LAN
> subnet shall be able to navigate to the device IP and reach a configuration page that
> displays the current network settings and allows the operator to modify them.
> Supported browsers must receive a valid HTML response with no JavaScript required
> for the configuration form to function.

### REQ-003 — Persistent Configuration Storage in Flash
> Modified settings shall be written to a dedicated area of the flash memory chip
> accessible to the controller. The stored settings shall be validated on every boot via
> a CRC-32 checksum. If the stored config is absent or corrupt the device shall
> automatically fall back to factory defaults (REQ-001) without operator intervention.

### REQ-004 — Factory Reset via Web Interface
> The web configuration page shall include a **Factory Reset** action. Activating it
> shall erase the stored configuration from flash and reboot the device, restoring all
> network settings to factory defaults.

### REQ-005 — Reboot on Configuration Save
> After the operator submits new network settings through the web interface the device
> shall persist the settings to flash and perform a hardware reset so that the new
> network configuration takes effect immediately.

---

## Self-Test Firmware (`self_test` build target)

### REQ-TEST-001 — OLED Display Check (Iteration 1)
> The self-test firmware shall write the string **"OLED Working"** centred on the
> SSD1306 OLED display (128×64, I2C address 0x3C) and repeat the update every
> **1 second**. Each iteration shall also emit a matching debug line over USB
> serial (`[SELF-TEST] loop=N  OLED Working`).
> If the display does not ACK on I2C the firmware shall print an error over USB
> serial and fast-blink the onboard LED to signal the fault.
>
> I2C mapping: SDA = GP2, SCL = GP3 (I2C1, 400 kHz) — per PCB pin mapping above.

---

## Revision History

| Date | Version | Description |
|------|---------|-------------|
| 2026-03-19 | 0.1 | Initial document created |
| 2026-03-19 | 0.2 | Add REQ-001 through REQ-005: factory IP, web config UI, flash persistence, factory reset, reboot-on-save |
| 2026-03-19 | 0.3 | REQ-001: change factory IP to 172.22.77.77/16 (RFC-1918, uncommon range, /16 mask) |
| 2026-03-20 | 0.4 | Add self-test firmware target and REQ-TEST-001 (OLED check, iteration 1) |
| 2026-03-20 | 0.5 | Add GPIO pin mapping table; fix REQ-TEST-001 OLED I2C pins to GP2/GP3 (I2C1) |
| 2026-03-20 | 0.6 | Add I2C device map: SSD1306 on I2C1, MCP3428 ×2 on I2C0 (0x36, 0x37) |
| 2026-03-20 | 0.7 | Add I2C1 devices: W24C02 EEPROM (0x50), TCA9555PWR GPIO expander (0x20) |
