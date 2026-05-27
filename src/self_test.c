/**
 * self_test.c — THL Power Control PCB self-test firmware
 *
 * Runs each bring-up test stage in sequence, then displays a results screen
 * on the SSD1306 OLED and prints detail over USB serial.
 *
 * Stage 1 — OLED display          (SSD1306,     I2C1 0x3C, GP2/GP3)
 * Stage 2 — EEPROM read/write     (W24C02,      I2C1 0x50, GP2/GP3)
 * Stage 3 — RS-485 HW UART1 loop  (ISL83488IBZ, GP4  TX / GP5  RX)
 * Stage 4 — RS-485 SW UART3 loop  (ISL83488IBZ, GP14 TX / GP15 RX)
 * Stage 5 — RS-232 HW UART0 loop  (ST3232BDR,   GP0  TX / GP1  RX)
 * Stage 6 — RS-232 SW UART2 loop  (ST3232BDR,   GP6  TX / GP7  RX)
 * Stage 7 — GPIO expander P0 I/O  (TCA9555PWR,  I2C1 0x20, GP2/GP3)
 * Stage 8 — ADC conversion check  (MCP3428,     I2C1 0x36, GP2/GP3)
 * Stage 9 — DAC + ADC loopback    (MCP48FVB02T, SPI1 GP10/11/13)
 *
 * ---- Display layout ----
 * The display has 8 pages (rows). Page 0 is always the title "THL PWR CTL";
 * pages 1–7 show up to 7 test results. When there are more than 7 results
 * the loop alternates between screens every 5 seconds, each showing the
 * next 7 results (with the title repeated). This scales to any number of
 * future stages without changing the display logic.
 *
 * ---- RS-485 loopback design note ----
 * The ISL83488IBZ is a full-duplex RS-485 transceiver with DE tied HIGH and
 * RE tied LOW, so the driver and receiver are permanently active. The Y/Z
 * (driver output) and A/B (receiver input) pairs are cross-wired externally.
 * This means RO mirrors DI with ~10 ns propagation — what is driven on TX
 * appears on RX almost instantly.
 *
 * Hardware UART1 (stage 3): the RP2040 UART hardware handles TX and RX on
 * independent paths simultaneously; bytes sent appear in the RX FIFO within
 * one frame time (~1 ms at 9600 baud).
 *
 * Software UART3 (stage 4): exploits the near-zero loopback propagation of
 * the ISL83488IBZ — each bit is driven on GP14 and GP15 is sampled at the
 * midpoint of the same bit period. This tests the full signal chain at 9600
 * baud without needing interrupts, PIO, or concurrent TX/RX threads.
 *
 * ---- RS-232 loopback design note ----
 * The ST3232BDR converts 3.3 V logic to ±RS-232 levels and back. Two DSUB
 * connectors are fitted (one per UART channel) and connected together with a
 * null modem cable (TX↔RX crossed). Unlike the RS-485 self-loopback, TX and
 * RX arrive on physically different GPIO pins via an external cable.
 *
 * The test therefore uses two independent phases per channel:
 *   Phase H: UART0 HW sends "RS232" → ST3232 → DSUB → cable → DSUB →
 *            ST3232 → GP7 (UART2 SW receives via bit-bang edge-detect)
 *   Phase S: GP6 (UART2 SW bit-bang sends) → ST3232 → DSUB → cable →
 *            DSUB → ST3232 → UART0 HW receives
 *
 * RS232-H reports the Phase H result (UART0 TX + UART2 RX path).
 * RS232-S reports the Phase S result (UART2 TX + UART0 RX path).
 *
 * ---- MCP3428 ADC design note ----
 * The MCP3428 is a 4-channel 16-bit delta-sigma ADC. The test selects
 * continuous-conversion mode at 12-bit / 240 sps (config byte 0x8X) and
 * cycles through all four channels. After each channel switch the firmware
 * waits one conversion period (~5 ms) then reads 3 bytes: two data bytes
 * followed by the configuration register. The /RDY bit (bit 7 of the config
 * byte) must be clear, indicating a fresh conversion is available. Because the
 * analog inputs are left unconnected, the actual codes are not checked — only
 * that the device ACKs, accepts the config write, and reports a completed
 * conversion on every channel.
 *
 * ---- MCP48FVB02T DAC + ADC loopback design note ----
 * The MCP48FVB02T is a 2-channel 8-bit SPI DAC with a 3.3 V external Vref.
 * Op-amp circuitry scales the 0–3.3 V output to 0–VUSER. With the VUSER
 * jumper set to 5 V, DAC full scale (0xFF) drives the output to 5 V.
 *
 * Each DAC channel feeds two MCP3428 ADC inputs through a 10 MΩ / 180 kΩ
 * voltage divider (anti-aliasing RC: 180 kΩ + 0.1 µF, f_c ≈ 8.8 Hz):
 *   DAC channel A (address 0x00) → ADC CH1 and CH2
 *   DAC channel B (address 0x01) → ADC CH3 and CH4
 *
 * SPI command frame (24 bits, MSB first):
 *   bits [23:19] — 5-bit register address (0x00 = DAC0, 0x01 = DAC1)
 *   bits [18:17] — command: 0b00 = write, 0b11 = read
 *   bit  [16]    — error flag (0 for writes)
 *   bits [15:8]  — data high byte (don't care for 8-bit DAC, send 0x00)
 *   bits [7:0]   — 8-bit DAC value (0x00 = 0 %, 0xFF = 100 %)
 *
 * Byte 0 = (addr << 3) | 0x00   (cmd=0b00 occupies bits 2:1, err=0 in bit 0)
 *
 * Expected ADC reading at 100 % (5 V output):
 *   Vout = 5 V × 180k / (10M + 180k) ≈ 88.4 mV → ~88 counts at 1 mV/LSB
 *   Pass window: 80–96 counts (±10 %).
 *   At 0 % the ADC must read ≤ 5 counts (noise floor).
 *   RC settling time constant τ = 18 ms; firmware waits 100 ms (≈ 5τ).
 *
 * RESULT_NONE — no response received; component not fitted or not connected.
 * RESULT_FAIL — response received but data mismatch; signal or wiring fault.
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/uart.h"
#include "hardware/spi.h"
#include "ssd1306.h"

/* ---------------------------------------------------------------------------
 * Constants
 * --------------------------------------------------------------------------- */

/* W24C02 EEPROM */
#define EEPROM_ADDR       0x50
#define EEPROM_TEST_ADDR  0xF8
#define EEPROM_WR_MS      5

/* RS-485 hardware UART1 (GP4 TX, GP5 RX) */
#define RS485_HW_UART           uart1
#define RS485_HW_TX_PIN         4
#define RS485_HW_RX_PIN         5
#define RS485_BAUD              9600
#define RS485_HW_RX_TIMEOUT_US  20000

/* RS-485 software UART3 (GP14 TX, GP15 RX) */
#define RS485_SW_TX_PIN   14
#define RS485_SW_RX_PIN   15
#define RS485_SW_BIT_US   104
#define RS485_SW_HALF_US  52

/* RS-232 hardware UART0 (GP0 TX, GP1 RX) */
#define RS232_HW_UART           uart0
#define RS232_HW_TX_PIN         0
#define RS232_HW_RX_PIN         1
#define RS232_BAUD              9600
#define RS232_HW_RX_TIMEOUT_US  20000

/* RS-232 software UART2 (GP6 TX, GP7 RX) */
#define RS232_SW_TX_PIN         6
#define RS232_SW_RX_PIN         7
#define RS232_BIT_US            104
#define RS232_HALF_US           52
#define RS232_SW_RX_TIMEOUT_US  50000

/* TCA9555PWR GPIO expander (I2C1, address 0x20) */
#define TCA9555_ADDR     0x20
#define TCA9555_IN0      0x00   /* input  port 0 register */
#define TCA9555_OUT0     0x02   /* output port 0 register */
#define TCA9555_CFG0     0x06   /* config port 0 register (1=input, 0=output) */
#define TCA9555_CFG1     0x07   /* config port 1 register */
/* P00–P03 outputs (0), P04–P07 inputs (1) → 0xF0 */
#define TCA9555_P0_CFG   0xF0
/* P10–P17 relay outputs — configure as inputs for safety during P0 test */
#define TCA9555_P1_CFG_SAFE  0xFF

/* MCP3428 4-channel ADC (I2C1, address 0x36) */
#define MCP3428_ADDR     0x36
#define MCP3428_CONV_MS  5       /* 240 sps → ~4.2 ms; 5 ms gives margin */
/* Config byte: /RDY | C1 | C0 | /OC | S1 | S0 | G1 | G0
 * /RDY=1 (triggers one-shot or has no effect in continuous), /OC=0 (continuous),
 * S1:S0=00 (12-bit, 240 sps), G1:G0=00 (gain = 1×).
 * Channel is selected by C1:C0 (bits 6–5). */
#define MCP3428_CFG_BASE 0x80    /* continuous, 12-bit, 1×, ch1 */
#define MCP3428_RDY_BIT  0x80    /* /RDY in read config byte: 0 = conversion ready */

/* MCP48FVB02T 2-channel 8-bit SPI DAC (SPI1) */
#define DAC_SPI_PORT     spi1
#define DAC_SCK_PIN      10
#define DAC_MOSI_PIN     11
#define DAC_CS_PIN       13
#define DAC_SPI_BAUD     1000000
/* Byte 0 of the 24-bit frame: (addr << 3) | cmd<<1 | err
 * cmd = 0b00 (write) → bit[1:0] of addr-shifted byte = 0x00 */
#define DAC_CH0_ADDR     0x00   /* volatile DAC0 register */
#define DAC_CH1_ADDR     0x01   /* volatile DAC1 register */

/* ADC loopback thresholds — 10 MΩ / 180 kΩ divider, VUSER = 5 V, 12-bit gain-1×
 * Vout = 5 V × 180k/(10M+180k) ≈ 88.4 mV → ~88 counts (1 mV/LSB) */
#define ADC_EXPECT_FULL  88
#define ADC_TOLERANCE     8     /* 10 % of 88 */
#define ADC_NOISE_FLOOR   5     /* max counts acceptable at 0 V */
#define DAC_SETTLE_MS   100     /* 5 × RC time constant (τ = 18 ms) */

/* Test payload shared by all serial loopback tests */
static const uint8_t SERIAL_TEST_MSG[] = { 'R', 'S', '2', '3', '2' };
#define SERIAL_TEST_LEN  (sizeof(SERIAL_TEST_MSG))

typedef enum { RESULT_PASS, RESULT_FAIL, RESULT_NONE } result_t;

/* ---------------------------------------------------------------------------
 * Result store and display
 *
 * Results are accumulated in order of testing. The display loop shows up to
 * 7 results per screen (pages 1–7, with the title on page 0) and alternates
 * between screens when there are more than 7 results.
 * --------------------------------------------------------------------------- */
#define MAX_RESULTS 16

static const char *result_label[MAX_RESULTS];
static result_t    result_value[MAX_RESULTS];
static int         result_count = 0;

static void record_result(const char *label, result_t r)
{
    if (result_count < MAX_RESULTS) {
        result_label[result_count] = label;
        result_value[result_count] = r;
        result_count++;
    }
}

static uint8_t center_col(const char *s)
{
    uint16_t px = (uint16_t)(strlen(s) * SSD1306_CHAR_W);
    return (uint8_t)(px >= SSD1306_WIDTH ? 0 : (SSD1306_WIDTH - px) / 2);
}

/** Draw one screen of up to 7 results starting at result index 'first'. */
static void draw_screen(int first)
{
    static const char *tag[] = { "PASS", "FAIL", "NONE" };
    char line[22];

    ssd1306_clear();
    ssd1306_draw_string(center_col("THL PWR CTL"), 0, "THL PWR CTL");

    for (int i = 0; i < 7 && (first + i) < result_count; i++) {
        snprintf(line, sizeof(line), "%s%s",
                 result_label[first + i],
                 tag[result_value[first + i]]);
        ssd1306_draw_string(4, 1 + i, line);
    }
    ssd1306_flush();
}

/* ---------------------------------------------------------------------------
 * Stage 2 — EEPROM
 * I2C1 is already initialised by ssd1306_init(); no re-init needed.
 * --------------------------------------------------------------------------- */
static result_t test_eeprom(void)
{
    static const uint8_t patterns[] = { 0xAA, 0x55, 0xA5 };

    uint8_t probe[2] = { EEPROM_TEST_ADDR, 0x00 };
    if (i2c_write_blocking(i2c1, EEPROM_ADDR, probe, 2, false) != 2) {
        printf("[SELF-TEST] EEPROM: no ACK at 0x%02X — not fitted\n", EEPROM_ADDR);
        return RESULT_NONE;
    }
    sleep_ms(EEPROM_WR_MS);

    for (size_t i = 0; i < sizeof(patterns); i++) {
        uint8_t wr[2] = { EEPROM_TEST_ADDR, patterns[i] };
        if (i2c_write_blocking(i2c1, EEPROM_ADDR, wr, 2, false) != 2) {
            printf("[SELF-TEST] EEPROM: write error (pattern 0x%02X)\n", patterns[i]);
            return RESULT_FAIL;
        }
        sleep_ms(EEPROM_WR_MS);

        uint8_t addr = EEPROM_TEST_ADDR, rb = 0;
        if (i2c_write_blocking(i2c1, EEPROM_ADDR, &addr, 1, true)  != 1 ||
            i2c_read_blocking (i2c1, EEPROM_ADDR, &rb,   1, false) != 1) {
            printf("[SELF-TEST] EEPROM: read error (pattern 0x%02X)\n", patterns[i]);
            return RESULT_FAIL;
        }
        printf("[SELF-TEST] EEPROM: wrote 0x%02X  read 0x%02X  %s\n",
               patterns[i], rb, rb == patterns[i] ? "OK" : "MISMATCH");
        if (rb != patterns[i]) return RESULT_FAIL;
    }
    return RESULT_PASS;
}

/* ---------------------------------------------------------------------------
 * RS-485 GPIO signal-path test (shared helper)
 *
 * Tests the physical signal path through the ISL83488IBZ transceiver and
 * the external Y/Z→A/B loopback wiring using raw GPIO — not UART framing.
 *
 * With DE tied HIGH and RE tied LOW the driver and receiver are permanently
 * active. Driving DI (TX pin) low pulls Y/Z low; through the loopback this
 * drives A/B low, pulling RO (RX pin) low. A floating or pull-up-only RX
 * line stays high regardless of TX state — so TX=low → RX=low is the
 * definitive wired-loopback check.
 *
 * NONE — RX did not follow TX low: loopback not wired
 * FAIL — unexpected state when TX is driven high
 * PASS — RX follows TX both high and low
 * --------------------------------------------------------------------------- */
static result_t test_rs485_gpio(uint tx_pin, uint rx_pin)
{
    gpio_init(tx_pin);
    gpio_set_dir(tx_pin, GPIO_OUT);
    gpio_put(tx_pin, 1);

    gpio_init(rx_pin);
    gpio_set_dir(rx_pin, GPIO_IN);
    gpio_pull_up(rx_pin);
    sleep_us(200);   /* allow transceiver and pull-up to settle */

    gpio_put(tx_pin, 1);
    sleep_us(10);
    int hi1 = gpio_get(rx_pin);

    gpio_put(tx_pin, 0);
    sleep_us(10);
    int lo = gpio_get(rx_pin);

    gpio_put(tx_pin, 1);
    sleep_us(10);
    int hi2 = gpio_get(rx_pin);

    printf("[SELF-TEST] RS485 GP%u->GP%u: TX=H RX=%d  TX=L RX=%d  TX=H RX=%d\n",
           tx_pin, rx_pin, hi1, lo, hi2);

    if (lo != 0)
        return RESULT_NONE;   /* RX did not follow TX low — loopback not wired */
    if (hi1 != 1 || hi2 != 1)
        return RESULT_FAIL;
    return RESULT_PASS;
}

/* ---------------------------------------------------------------------------
 * Stage 3 — RS-485 channel 1 (GP4 TX / GP5 RX, UART1 pins)
 * Stage 4 — RS-485 channel 3 (GP14 TX / GP15 RX, UART3 pins)
 * --------------------------------------------------------------------------- */
static result_t test_rs485_hw(void)
{
    return test_rs485_gpio(RS485_HW_TX_PIN, RS485_HW_RX_PIN);
}

static result_t test_rs485_sw(void)
{
    return test_rs485_gpio(RS485_SW_TX_PIN, RS485_SW_RX_PIN);
}

/* ---------------------------------------------------------------------------
 * RS-232 software UART helpers (bit-bang TX and RX at 9600 baud)
 *
 * Unlike the RS-485 SW test, the RS-232 loopback travels through an external
 * null modem cable between two separate DSUB connectors. TX and RX are on
 * different physical GPIO pins and cannot be sampled simultaneously. A
 * standard bit-bang receiver is therefore used: detect the falling edge of
 * the start bit, wait 1.5 bit periods to the midpoint of bit 0, then sample
 * every bit period for 8 data bits.
 * --------------------------------------------------------------------------- */
static void sw_uart_send_byte(uint pin, uint8_t byte)
{
    gpio_put(pin, 0);                   /* start bit */
    sleep_us(RS232_BIT_US);
    for (int b = 0; b < 8; b++) {
        gpio_put(pin, (byte >> b) & 1u);
        sleep_us(RS232_BIT_US);
    }
    gpio_put(pin, 1);                   /* stop bit */
    sleep_us(RS232_BIT_US);
}

static bool sw_uart_recv_byte(uint pin, uint8_t *out, uint32_t timeout_us)
{
    uint32_t deadline = time_us_32() + timeout_us;
    while (gpio_get(pin) != 0) {       /* wait for falling edge of start bit */
        if (time_us_32() > deadline) return false;
    }
    /* Advance to midpoint of bit 0: 1.5 × bit period from start of start bit */
    sleep_us(RS232_BIT_US + RS232_HALF_US);
    uint8_t byte = 0;
    for (int b = 0; b < 8; b++) {
        byte |= ((uint8_t)gpio_get(pin) << b);
        sleep_us(RS232_BIT_US);
    }
    *out = byte;
    return true;
}

/* ---------------------------------------------------------------------------
 * Stages 5 & 6 — RS-232 loopback (ST3232BDR, null modem cable)
 *
 * Two-phase cross test — see file header for full description.
 * RS232-H covers the UART0 TX and UART2 RX paths.
 * RS232-S covers the UART2 TX and UART0 RX paths.
 * --------------------------------------------------------------------------- */
static void test_rs232(result_t *hw_out, result_t *sw_out)
{
    uart_init(RS232_HW_UART, RS232_BAUD);
    gpio_set_function(RS232_HW_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(RS232_HW_RX_PIN, GPIO_FUNC_UART);

    gpio_init(RS232_SW_TX_PIN);
    gpio_set_dir(RS232_SW_TX_PIN, GPIO_OUT);
    gpio_put(RS232_SW_TX_PIN, 1);

    gpio_init(RS232_SW_RX_PIN);
    gpio_set_dir(RS232_SW_RX_PIN, GPIO_IN);
    gpio_pull_up(RS232_SW_RX_PIN);

    sleep_us(500);   /* allow ST3232BDR charge-pump to stabilise */

    /* Phase H: UART0 HW sends, UART2 SW receives */
    while (uart_is_readable(RS232_HW_UART)) uart_getc(RS232_HW_UART);
    uart_write_blocking(RS232_HW_UART, SERIAL_TEST_MSG, SERIAL_TEST_LEN);

    result_t phase_h = RESULT_PASS;
    for (size_t i = 0; i < SERIAL_TEST_LEN; i++) {
        uint8_t rx = 0;
        if (!sw_uart_recv_byte(RS232_SW_RX_PIN, &rx, RS232_SW_RX_TIMEOUT_US)) {
            printf("[SELF-TEST] RS232-H: timeout on byte %u — %s\n",
                   (unsigned)i, i == 0 ? "not fitted" : "FAIL");
            phase_h = (i == 0) ? RESULT_NONE : RESULT_FAIL;
            break;
        }
        printf("[SELF-TEST] RS232-H: sent 0x%02X  recv 0x%02X  %s\n",
               SERIAL_TEST_MSG[i], rx,
               rx == SERIAL_TEST_MSG[i] ? "OK" : "MISMATCH");
        if (rx != SERIAL_TEST_MSG[i]) { phase_h = RESULT_FAIL; break; }
    }
    *hw_out = phase_h;

    sleep_ms(5);
    while (uart_is_readable(RS232_HW_UART)) uart_getc(RS232_HW_UART);

    /* Phase S: UART2 SW sends, UART0 HW receives */
    for (size_t i = 0; i < SERIAL_TEST_LEN; i++)
        sw_uart_send_byte(RS232_SW_TX_PIN, SERIAL_TEST_MSG[i]);

    result_t phase_s = RESULT_PASS;
    for (size_t i = 0; i < SERIAL_TEST_LEN; i++) {
        uint32_t deadline = time_us_32() + RS232_HW_RX_TIMEOUT_US;
        while (!uart_is_readable(RS232_HW_UART)) {
            if (time_us_32() > deadline) {
                printf("[SELF-TEST] RS232-S: timeout on byte %u — %s\n",
                       (unsigned)i, i == 0 ? "not fitted" : "FAIL");
                phase_s = (i == 0) ? RESULT_NONE : RESULT_FAIL;
                goto phase_s_done;
            }
        }
        uint8_t rx = uart_getc(RS232_HW_UART);
        printf("[SELF-TEST] RS232-S: sent 0x%02X  recv 0x%02X  %s\n",
               SERIAL_TEST_MSG[i], rx,
               rx == SERIAL_TEST_MSG[i] ? "OK" : "MISMATCH");
        if (rx != SERIAL_TEST_MSG[i]) { phase_s = RESULT_FAIL; goto phase_s_done; }
    }
phase_s_done:
    *sw_out = phase_s;
}

/* ---------------------------------------------------------------------------
 * Stage 7 — TCA9555PWR GPIO expander, port 0
 *
 * Port 0 configuration (register 0x06 = 0xF0):
 *   P00–P03: outputs (config bits 0–3 = 0)
 *   P04–P07: inputs  (config bits 4–7 = 1)
 *   Wired:   P00→P04, P01→P05, P02→P06, P03→P07
 *
 * Port 1 (P10–P17, relay drivers) is configured as all-inputs during this
 * test to prevent accidental relay activation.
 *
 * Test drives each output pattern into the lower nibble and verifies the
 * upper nibble of the input register reflects the expected state.
 * --------------------------------------------------------------------------- */
static result_t test_gpio_p0(void)
{
    /* Probe — check for ACK and configure port directions */
    uint8_t cfg0[2] = { TCA9555_CFG0, TCA9555_P0_CFG };
    if (i2c_write_blocking(i2c1, TCA9555_ADDR, cfg0, 2, false) != 2) {
        printf("[SELF-TEST] GPIO-0: no ACK at 0x%02X — not fitted\n", TCA9555_ADDR);
        return RESULT_NONE;
    }

    /* Configure port 1 as all-inputs for safety (relay pins, not tested here) */
    uint8_t cfg1[2] = { TCA9555_CFG1, TCA9555_P1_CFG_SAFE };
    i2c_write_blocking(i2c1, TCA9555_ADDR, cfg1, 2, false);

    /* Test patterns: drive lower nibble, expect upper nibble = lower << 4 */
    static const uint8_t out_pat[] = { 0x00, 0x01, 0x02, 0x04, 0x08, 0x0F };
    static const uint8_t exp_pat[] = { 0x00, 0x10, 0x20, 0x40, 0x80, 0xF0 };

    for (size_t i = 0; i < sizeof(out_pat); i++) {
        /* Write output latch */
        uint8_t wr[2] = { TCA9555_OUT0, out_pat[i] };
        if (i2c_write_blocking(i2c1, TCA9555_ADDR, wr, 2, false) != 2) {
            printf("[SELF-TEST] GPIO-0: output write error\n");
            return RESULT_FAIL;
        }
        sleep_ms(1);   /* allow output to propagate through wiring */

        /* Read input port */
        uint8_t reg = TCA9555_IN0, in_val = 0;
        if (i2c_write_blocking(i2c1, TCA9555_ADDR, &reg, 1, true)  != 1 ||
            i2c_read_blocking (i2c1, TCA9555_ADDR, &in_val, 1, false) != 1) {
            printf("[SELF-TEST] GPIO-0: input read error\n");
            return RESULT_FAIL;
        }

        uint8_t got = in_val & 0xF0;
        printf("[SELF-TEST] GPIO-0: out=0x%02X  in=0x%02X  exp_hi=0x%02X  %s\n",
               out_pat[i], in_val, exp_pat[i],
               got == exp_pat[i] ? "OK" : "MISMATCH");

        if (got != exp_pat[i]) return RESULT_FAIL;
    }
    return RESULT_PASS;
}

/* ---------------------------------------------------------------------------
 * Stage 8 — MCP3428 ADC, 4-channel conversion check
 *
 * Inputs are unconnected (floating); the test does not validate the ADC
 * codes — only that the device ACKs, accepts a config write, and reports
 * a completed conversion (/RDY = 0) for each of the four channels.
 *
 * Read format (12-bit mode): [data_MSB, data_LSB, config_byte]
 * The upper 4 bits of data_MSB are sign-extended by the device, so casting
 * the two data bytes directly to int16_t gives the correct signed value.
 * --------------------------------------------------------------------------- */
static result_t test_adc(void)
{
    /* Probe — attempt a read to check for ACK */
    uint8_t buf[3];
    if (i2c_read_blocking(i2c1, MCP3428_ADDR, buf, 3, false) != 3) {
        printf("[SELF-TEST] ADC: no ACK at 0x%02X — not fitted\n", MCP3428_ADDR);
        return RESULT_NONE;
    }

    for (int ch = 0; ch < 4; ch++) {
        /* Select channel (C1:C0 in bits 6–5), continuous mode, 12-bit, 1× */
        uint8_t cfg = (uint8_t)(MCP3428_CFG_BASE | ((uint8_t)ch << 5));
        if (i2c_write_blocking(i2c1, MCP3428_ADDR, &cfg, 1, false) != 1) {
            printf("[SELF-TEST] ADC: config write error ch%d\n", ch + 1);
            return RESULT_FAIL;
        }
        sleep_ms(MCP3428_CONV_MS);

        if (i2c_read_blocking(i2c1, MCP3428_ADDR, buf, 3, false) != 3) {
            printf("[SELF-TEST] ADC: read error ch%d\n", ch + 1);
            return RESULT_FAIL;
        }

        if (buf[2] & MCP3428_RDY_BIT) {
            printf("[SELF-TEST] ADC: ch%d conversion not ready (cfg=0x%02X)\n",
                   ch + 1, buf[2]);
            return RESULT_FAIL;
        }

        int16_t raw = (int16_t)(((uint16_t)buf[0] << 8) | buf[1]);
        printf("[SELF-TEST] ADC: ch%d raw=0x%04X (%d)  cfg=0x%02X  OK\n",
               ch + 1, (uint16_t)raw, raw, buf[2]);
    }
    return RESULT_PASS;
}

/* ---------------------------------------------------------------------------
 * Stage 9 helpers — SPI DAC write and MCP3428 single-channel read
 * --------------------------------------------------------------------------- */
static void dac_write_ch(uint8_t addr, uint8_t value)
{
    /* 24-bit frame: byte0 = addr<<3 (cmd=00, err=0), byte1 = 0x00, byte2 = value */
    uint8_t buf[3] = { (uint8_t)(addr << 3), 0x00, value };
    gpio_put(DAC_CS_PIN, 0);
    spi_write_blocking(DAC_SPI_PORT, buf, 3);
    gpio_put(DAC_CS_PIN, 1);
}

/* ch: 0–3 maps to MCP3428 CH1–CH4. Returns false on I2C error or timeout. */
static bool adc_read_ch(int ch, int16_t *out)
{
    uint8_t cfg = (uint8_t)(MCP3428_CFG_BASE | ((uint8_t)ch << 5));
    if (i2c_write_blocking(i2c1, MCP3428_ADDR, &cfg, 1, false) != 1)
        return false;
    sleep_ms(MCP3428_CONV_MS);
    uint8_t buf[3];
    if (i2c_read_blocking(i2c1, MCP3428_ADDR, buf, 3, false) != 3)
        return false;
    if (buf[2] & MCP3428_RDY_BIT)
        return false;
    *out = (int16_t)(((uint16_t)buf[0] << 8) | buf[1]);
    return true;
}

/* ---------------------------------------------------------------------------
 * Stage 9 — MCP48FVB02T DAC loopback through MCP3428 ADC
 *
 * Both DAC channels are driven to 0 % then 100 % in a single pass. The ADC
 * is read after each transition (100 ms settling for the RC filter). Channel
 * A uses ADC CH1+CH2; channel B uses ADC CH3+CH4.
 *
 * RESULT_NONE: 100 % reading ≤ noise floor on both ADC inputs — DAC or signal
 *              path not present.
 * RESULT_FAIL: 0 % reading above noise floor, or 100 % reading outside the
 *              ±10 % window around the expected 88 counts.
 * --------------------------------------------------------------------------- */
static void test_dac(result_t *ch_a, result_t *ch_b)
{
    /* Initialise SPI1 */
    spi_init(DAC_SPI_PORT, DAC_SPI_BAUD);
    spi_set_format(DAC_SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(DAC_SCK_PIN,  GPIO_FUNC_SPI);
    gpio_set_function(DAC_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_init(DAC_CS_PIN);
    gpio_set_dir(DAC_CS_PIN, GPIO_OUT);
    gpio_put(DAC_CS_PIN, 1);
    sleep_us(100);

    /* ---- 0 % ---- */
    dac_write_ch(DAC_CH0_ADDR, 0x00);
    dac_write_ch(DAC_CH1_ADDR, 0x00);
    sleep_ms(DAC_SETTLE_MS);

    int16_t z0 = 0, z1 = 0, z2 = 0, z3 = 0;
    if (!adc_read_ch(0, &z0) || !adc_read_ch(1, &z1) ||
        !adc_read_ch(2, &z2) || !adc_read_ch(3, &z3)) {
        printf("[SELF-TEST] DAC: ADC read error at 0%%\n");
        *ch_a = *ch_b = RESULT_FAIL;
        return;
    }
    printf("[SELF-TEST] DAC:   0%%: ch1=%d ch2=%d ch3=%d ch4=%d  (floor<=%d)\n",
           z0, z1, z2, z3, ADC_NOISE_FLOOR);

    /* ---- 100 % ---- */
    dac_write_ch(DAC_CH0_ADDR, 0xFF);
    dac_write_ch(DAC_CH1_ADDR, 0xFF);
    sleep_ms(DAC_SETTLE_MS);

    int16_t f0 = 0, f1 = 0, f2 = 0, f3 = 0;
    if (!adc_read_ch(0, &f0) || !adc_read_ch(1, &f1) ||
        !adc_read_ch(2, &f2) || !adc_read_ch(3, &f3)) {
        printf("[SELF-TEST] DAC: ADC read error at 100%%\n");
        *ch_a = *ch_b = RESULT_FAIL;
        return;
    }
    printf("[SELF-TEST] DAC: 100%%: ch1=%d ch2=%d ch3=%d ch4=%d  exp=%d+/-%d\n",
           f0, f1, f2, f3, ADC_EXPECT_FULL, ADC_TOLERANCE);

    /* Evaluate channel A (DAC0 → CH1, CH2) */
    if (f0 <= ADC_NOISE_FLOOR && f1 <= ADC_NOISE_FLOOR) {
        *ch_a = RESULT_NONE;
    } else if (z0 <= ADC_NOISE_FLOOR && z1 <= ADC_NOISE_FLOOR &&
               f0 >= ADC_EXPECT_FULL - ADC_TOLERANCE &&
               f0 <= ADC_EXPECT_FULL + ADC_TOLERANCE &&
               f1 >= ADC_EXPECT_FULL - ADC_TOLERANCE &&
               f1 <= ADC_EXPECT_FULL + ADC_TOLERANCE) {
        *ch_a = RESULT_PASS;
    } else {
        *ch_a = RESULT_FAIL;
    }

    /* Evaluate channel B (DAC1 → CH3, CH4) */
    if (f2 <= ADC_NOISE_FLOOR && f3 <= ADC_NOISE_FLOOR) {
        *ch_b = RESULT_NONE;
    } else if (z2 <= ADC_NOISE_FLOOR && z3 <= ADC_NOISE_FLOOR &&
               f2 >= ADC_EXPECT_FULL - ADC_TOLERANCE &&
               f2 <= ADC_EXPECT_FULL + ADC_TOLERANCE &&
               f3 >= ADC_EXPECT_FULL - ADC_TOLERANCE &&
               f3 <= ADC_EXPECT_FULL + ADC_TOLERANCE) {
        *ch_b = RESULT_PASS;
    } else {
        *ch_b = RESULT_FAIL;
    }
}

/* ---------------------------------------------------------------------------
 * Main
 * --------------------------------------------------------------------------- */
int main(void)
{
    stdio_init_all();
    sleep_ms(3000);  /* wait for USB CDC to enumerate on the host */

    printf("[SELF-TEST] Starting\n");

    /* I2C1 probe — runs before ssd1306_init so no prior I2C activity can interfere.
     * Checks OLED, GPIO expander, external ADC, and EEPROM. Results reprinted
     * each loop so they are always visible regardless of when the port is opened. */
    static char i2c_scan_result[128];
    {
        i2c_init(i2c1, 400000);
        gpio_set_function(2, GPIO_FUNC_I2C);
        gpio_set_function(3, GPIO_FUNC_I2C);
        gpio_pull_up(2);
        gpio_pull_up(3);
        sleep_ms(5);
        uint8_t oled_probe[2] = {0x00, 0xAE};
        uint8_t d = 0;
        int r3c = i2c_write_blocking(i2c1, 0x3C, oled_probe, 2, false);
        int r20 = i2c_write_blocking(i2c1, 0x20, &d, 1, false);
        int r37 = i2c_write_blocking(i2c1, 0x37, &d, 1, false);
        int r50 = i2c_write_blocking(i2c1, 0x50, &d, 1, false);
        snprintf(i2c_scan_result, sizeof(i2c_scan_result),
                 "I2C1: 0x3C=%d 0x20=%d 0x37=%d 0x50=%d |",
                 r3c, r20, r37, r50);
    }

    /* Stage 1 — OLED */
    result_t r;
    if (ssd1306_init()) {
        r = RESULT_PASS;
        printf("[SELF-TEST] OLED: PASS\n");
    } else {
        r = RESULT_FAIL;
        printf("[SELF-TEST] OLED: FAIL (no ACK at 0x%02X)\n", SSD1306_ADDR);
    }
    record_result("OLED:    ", r);

    /* Stage 2 — EEPROM */
    r = test_eeprom();
    printf("[SELF-TEST] EEPROM: %s\n",
           r == RESULT_PASS ? "PASS" : r == RESULT_FAIL ? "FAIL" : "NONE (not fitted)");
    record_result("EEPROM:  ", r);

    /* Stage 3 — RS-485 hardware UART1 */
    r = test_rs485_hw();
    printf("[SELF-TEST] RS485-H: %s\n",
           r == RESULT_PASS ? "PASS" : r == RESULT_FAIL ? "FAIL" : "NONE (not fitted)");
    record_result("RS485-H: ", r);

    /* Stage 4 — RS-485 software UART3 */
    r = test_rs485_sw();
    printf("[SELF-TEST] RS485-S: %s\n",
           r == RESULT_PASS ? "PASS" : r == RESULT_FAIL ? "FAIL" : "NONE (not fitted)");
    record_result("RS485-S: ", r);

    /* Stages 5 & 6 — RS-232 */
    result_t rs232_hw, rs232_sw;
    test_rs232(&rs232_hw, &rs232_sw);
    printf("[SELF-TEST] RS232-H: %s\n",
           rs232_hw == RESULT_PASS ? "PASS" : rs232_hw == RESULT_FAIL ? "FAIL" : "NONE (not fitted)");
    printf("[SELF-TEST] RS232-S: %s\n",
           rs232_sw == RESULT_PASS ? "PASS" : rs232_sw == RESULT_FAIL ? "FAIL" : "NONE (not fitted)");
    record_result("RS232-H: ", rs232_hw);
    record_result("RS232-S: ", rs232_sw);

    /* Stage 7 — GPIO expander port 0 */
    r = test_gpio_p0();
    printf("[SELF-TEST] GPIO-0:  %s\n",
           r == RESULT_PASS ? "PASS" : r == RESULT_FAIL ? "FAIL" : "NONE (not fitted)");
    record_result("GPIO-0:  ", r);

    /* Stage 8 — MCP3428 ADC */
    r = test_adc();
    printf("[SELF-TEST] ADC:     %s\n",
           r == RESULT_PASS ? "PASS" : r == RESULT_FAIL ? "FAIL" : "NONE (not fitted)");
    record_result("ADC:     ", r);

    /* Stage 9 — MCP48FVB02T DAC loopback */
    result_t dac_a, dac_b;
    test_dac(&dac_a, &dac_b);
    printf("[SELF-TEST] DAC-A:   %s\n",
           dac_a == RESULT_PASS ? "PASS" : dac_a == RESULT_FAIL ? "FAIL" : "NONE (not fitted)");
    printf("[SELF-TEST] DAC-B:   %s\n",
           dac_b == RESULT_PASS ? "PASS" : dac_b == RESULT_FAIL ? "FAIL" : "NONE (not fitted)");
    record_result("DAC-A:   ", dac_a);
    record_result("DAC-B:   ", dac_b);

    /* Show first screen and enter display loop */
    draw_screen(0);

    int screen    = 0;
    int n_screens = (result_count + 6) / 7;   /* 7 results per screen */
    uint32_t loop = 0;

    while (true) {
        sleep_ms(5000);
        screen = (screen + 1) % n_screens;
        draw_screen(screen * 7);

        printf("[SELF-TEST] loop=%lu  %s", loop++, i2c_scan_result);
        for (int i = 0; i < result_count; i++) {
            printf("  %s%s", result_label[i],
                   result_value[i] == RESULT_PASS ? "PASS" :
                   result_value[i] == RESULT_FAIL ? "FAIL" : "NONE");
        }
        printf("\n");
    }
}
