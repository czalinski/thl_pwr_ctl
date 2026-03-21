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
 *
 * Display layout (8 pages × 128 px, no blank separator):
 *   Page 0  "THL PWR CTL"    centred title
 *   Page 1  "Self Test"      centred subtitle
 *   Page 2  "OLED:    PASS/FAIL"
 *   Page 3  "EEPROM:  PASS/FAIL/NONE"
 *   Page 4  "RS485-H: PASS/FAIL/NONE"
 *   Page 5  "RS485-S: PASS/FAIL/NONE"
 *   Page 6  "RS232-H: PASS/FAIL/NONE"
 *   Page 7  "RS232-S: PASS/FAIL/NONE"
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
 * midpoint of the same bit. This tests the full signal chain at 9600 baud
 * without needing interrupts, PIO, or concurrent TX/RX threads.
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
 * Both must pass for the full RS-232 signal chain to be verified.
 *
 * RESULT_NONE — no response received; transceiver/connector/cable not fitted
 *               or not connected.
 * RESULT_FAIL — response received but data mismatch; signal integrity or
 *               wiring fault.
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/uart.h"
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
#define RS232_SW_TX_PIN      6
#define RS232_SW_RX_PIN      7
#define RS232_BIT_US         104
#define RS232_HALF_US        52
#define RS232_SW_RX_TIMEOUT_US  50000   /* 50 ms: generous for cable round-trip */

/* Test payload shared by all serial loopback tests */
static const uint8_t SERIAL_TEST_MSG[] = { 'R', 'S', '2', '3', '2' };
#define SERIAL_TEST_LEN  (sizeof(SERIAL_TEST_MSG))

typedef enum { RESULT_PASS, RESULT_FAIL, RESULT_NONE } result_t;

/* ---------------------------------------------------------------------------
 * Display
 * --------------------------------------------------------------------------- */

static uint8_t center_col(const char *s)
{
    uint16_t px = (uint16_t)(strlen(s) * SSD1306_CHAR_W);
    return (uint8_t)(px >= SSD1306_WIDTH ? 0 : (SSD1306_WIDTH - px) / 2);
}

static void draw_results(result_t oled,    result_t eeprom,
                         result_t rs485_hw, result_t rs485_sw,
                         result_t rs232_hw, result_t rs232_sw)
{
    static const char *tag[] = { "PASS", "FAIL", "NONE" };
    char line[22];

    ssd1306_clear();
    ssd1306_draw_string(center_col("THL PWR CTL"), 0, "THL PWR CTL");
    ssd1306_draw_string(center_col("Self Test"),   1, "Self Test");

    snprintf(line, sizeof(line), "OLED:    %s", tag[oled]);
    ssd1306_draw_string(4, 2, line);

    snprintf(line, sizeof(line), "EEPROM:  %s", tag[eeprom]);
    ssd1306_draw_string(4, 3, line);

    snprintf(line, sizeof(line), "RS485-H: %s", tag[rs485_hw]);
    ssd1306_draw_string(4, 4, line);

    snprintf(line, sizeof(line), "RS485-S: %s", tag[rs485_sw]);
    ssd1306_draw_string(4, 5, line);

    snprintf(line, sizeof(line), "RS232-H: %s", tag[rs232_hw]);
    ssd1306_draw_string(4, 6, line);

    snprintf(line, sizeof(line), "RS232-S: %s", tag[rs232_sw]);
    ssd1306_draw_string(4, 7, line);

    ssd1306_flush();
}

/* ---------------------------------------------------------------------------
 * Stage 2 — EEPROM
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
 * Stage 3 — RS-485 hardware UART1 loopback
 *
 * UART1 TX (GP4) drives the ISL83488IBZ DI; the Y/Z outputs are wired to
 * A/B inputs so RO (GP5) mirrors DI with ~10 ns delay. The RP2040 hardware
 * UART manages TX and RX simultaneously on independent paths.
 * --------------------------------------------------------------------------- */
static result_t test_rs485_hw(void)
{
    uart_init(RS485_HW_UART, RS485_BAUD);
    gpio_set_function(RS485_HW_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(RS485_HW_RX_PIN, GPIO_FUNC_UART);

    while (uart_is_readable(RS485_HW_UART))
        uart_getc(RS485_HW_UART);

    uart_write_blocking(RS485_HW_UART, SERIAL_TEST_MSG, SERIAL_TEST_LEN);

    for (size_t i = 0; i < SERIAL_TEST_LEN; i++) {
        uint32_t deadline = time_us_32() + RS485_HW_RX_TIMEOUT_US;
        while (!uart_is_readable(RS485_HW_UART)) {
            if (time_us_32() > deadline) {
                printf("[SELF-TEST] RS485-H: timeout on byte %u — %s\n",
                       (unsigned)i, i == 0 ? "not fitted" : "FAIL");
                return (i == 0) ? RESULT_NONE : RESULT_FAIL;
            }
        }
        uint8_t rx = uart_getc(RS485_HW_UART);
        printf("[SELF-TEST] RS485-H: sent 0x%02X  recv 0x%02X  %s\n",
               SERIAL_TEST_MSG[i], rx,
               rx == SERIAL_TEST_MSG[i] ? "OK" : "MISMATCH");
        if (rx != SERIAL_TEST_MSG[i]) return RESULT_FAIL;
    }
    return RESULT_PASS;
}

/* ---------------------------------------------------------------------------
 * Stage 4 — RS-485 software UART3 loopback (bit-bang, 9600 baud)
 *
 * Exploits the near-zero ISL83488IBZ loopback propagation: each bit is
 * driven on GP14 and GP15 is sampled at the midpoint of the same bit period.
 * This tests the full TX→transceiver→cable→transceiver→RX signal chain at
 * 9600 baud without needing interrupts, PIO, or concurrent TX/RX threads.
 * --------------------------------------------------------------------------- */
static result_t test_rs485_sw(void)
{
    gpio_init(RS485_SW_TX_PIN);
    gpio_set_dir(RS485_SW_TX_PIN, GPIO_OUT);
    gpio_put(RS485_SW_TX_PIN, 1);

    gpio_init(RS485_SW_RX_PIN);
    gpio_set_dir(RS485_SW_RX_PIN, GPIO_IN);
    gpio_pull_up(RS485_SW_RX_PIN);

    sleep_us(200);

    /* Probe: pull TX low and verify RX follows */
    gpio_put(RS485_SW_TX_PIN, 0);
    sleep_us(RS485_SW_HALF_US);
    int probe_rx = gpio_get(RS485_SW_RX_PIN);
    gpio_put(RS485_SW_TX_PIN, 1);
    sleep_us(RS485_SW_BIT_US);

    if (probe_rx != 0) {
        printf("[SELF-TEST] RS485-S: RX did not follow TX low — not fitted\n");
        return RESULT_NONE;
    }

    for (size_t m = 0; m < SERIAL_TEST_LEN; m++) {
        uint8_t byte = SERIAL_TEST_MSG[m];
        bool byte_ok = true;

        /* Start bit */
        gpio_put(RS485_SW_TX_PIN, 0);
        sleep_us(RS485_SW_HALF_US);
        if (gpio_get(RS485_SW_RX_PIN) != 0) byte_ok = false;
        sleep_us(RS485_SW_HALF_US);

        /* 8 data bits, LSB first — drive bit, sample RX at midpoint */
        for (int b = 0; b < 8; b++) {
            uint8_t tx_bit = (byte >> b) & 1u;
            gpio_put(RS485_SW_TX_PIN, tx_bit);
            sleep_us(RS485_SW_HALF_US);
            if ((uint8_t)gpio_get(RS485_SW_RX_PIN) != tx_bit) byte_ok = false;
            sleep_us(RS485_SW_HALF_US);
        }

        /* Stop bit */
        gpio_put(RS485_SW_TX_PIN, 1);
        sleep_us(RS485_SW_BIT_US);

        printf("[SELF-TEST] RS485-S: byte 0x%02X ('%c')  %s\n",
               byte, (byte >= 0x20 && byte < 0x7F) ? (char)byte : '.',
               byte_ok ? "OK" : "MISMATCH");

        if (!byte_ok) return RESULT_FAIL;
    }
    return RESULT_PASS;
}

/* ---------------------------------------------------------------------------
 * RS-232 software UART helpers (bit-bang TX and RX at 9600 baud)
 *
 * Unlike the RS-485 SW test, the RS-232 loopback travels through an external
 * null modem cable between two separate DSUB connectors, so TX and RX are on
 * different physical GPIO pins and cannot be sampled simultaneously.
 * A standard bit-bang receiver is used: detect the falling edge of the start
 * bit, wait 1.5 bit periods to reach the midpoint of bit 0, then sample every
 * bit period for 8 data bits.
 * --------------------------------------------------------------------------- */
static void sw_uart_send_byte(uint pin, uint8_t byte)
{
    gpio_put(pin, 0);           /* start bit */
    sleep_us(RS232_BIT_US);
    for (int b = 0; b < 8; b++) {
        gpio_put(pin, (byte >> b) & 1u);
        sleep_us(RS232_BIT_US);
    }
    gpio_put(pin, 1);           /* stop bit */
    sleep_us(RS232_BIT_US);
}

static bool sw_uart_recv_byte(uint pin, uint8_t *out, uint32_t timeout_us)
{
    /* Wait for falling edge of start bit */
    uint32_t deadline = time_us_32() + timeout_us;
    while (gpio_get(pin) != 0) {
        if (time_us_32() > deadline) return false;
    }

    /* Advance to the midpoint of bit 0: 1.5 × bit period from start of start bit */
    sleep_us(RS232_BIT_US + RS232_HALF_US);

    uint8_t byte = 0;
    for (int b = 0; b < 8; b++) {
        byte |= ((uint8_t)gpio_get(pin) << b);
        sleep_us(RS232_BIT_US);   /* advance to midpoint of next bit */
    }
    /* Now in the stop-bit region; no need to verify it for a basic loopback test */

    *out = byte;
    return true;
}

/* ---------------------------------------------------------------------------
 * Stages 5 & 6 — RS-232 loopback (ST3232BDR, null modem cable)
 *
 * Two-phase cross test:
 *   Phase H (RS232-H): UART0 HW sends → ST3232 → DSUB-0 → null modem →
 *                      DSUB-2 → ST3232 → GP7 (UART2 SW receives)
 *   Phase S (RS232-S): GP6 (UART2 SW sends) → ST3232 → DSUB-2 → null modem →
 *                      DSUB-0 → ST3232 → UART0 HW receives
 *
 * RS232-H covers the UART0 TX and UART2 RX paths.
 * RS232-S covers the UART2 TX and UART0 RX paths.
 * NONE is returned when no data arrives within the timeout (cable not
 * connected or transceiver not fitted). FAIL indicates a data mismatch.
 * --------------------------------------------------------------------------- */
static void test_rs232(result_t *hw_out, result_t *sw_out)
{
    /* Initialise hardware UART0 */
    uart_init(RS232_HW_UART, RS232_BAUD);
    gpio_set_function(RS232_HW_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(RS232_HW_RX_PIN, GPIO_FUNC_UART);

    /* Initialise software UART2 pins */
    gpio_init(RS232_SW_TX_PIN);
    gpio_set_dir(RS232_SW_TX_PIN, GPIO_OUT);
    gpio_put(RS232_SW_TX_PIN, 1);    /* idle high */

    gpio_init(RS232_SW_RX_PIN);
    gpio_set_dir(RS232_SW_RX_PIN, GPIO_IN);
    gpio_pull_up(RS232_SW_RX_PIN);

    sleep_us(500);   /* allow ST3232BDR charge-pump to stabilise */

    /* --- Phase H: UART0 HW sends, UART2 SW receives via bit-bang --- */
    while (uart_is_readable(RS232_HW_UART))
        uart_getc(RS232_HW_UART);   /* drain stale RX data */

    uart_write_blocking(RS232_HW_UART, SERIAL_TEST_MSG, SERIAL_TEST_LEN);

    result_t phase_h = RESULT_PASS;
    for (size_t i = 0; i < SERIAL_TEST_LEN; i++) {
        uint8_t rx = 0;
        uint32_t tmo = (i == 0) ? RS232_SW_RX_TIMEOUT_US : RS232_SW_RX_TIMEOUT_US;
        if (!sw_uart_recv_byte(RS232_SW_RX_PIN, &rx, tmo)) {
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

    /* Brief gap between phases */
    sleep_ms(5);
    while (uart_is_readable(RS232_HW_UART))
        uart_getc(RS232_HW_UART);   /* drain anything from phase H */

    /* --- Phase S: UART2 SW sends via bit-bang, UART0 HW receives --- */
    result_t phase_s = RESULT_PASS;
    for (size_t i = 0; i < SERIAL_TEST_LEN; i++)
        sw_uart_send_byte(RS232_SW_TX_PIN, SERIAL_TEST_MSG[i]);

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
 * Main
 * --------------------------------------------------------------------------- */
int main(void)
{
    stdio_init_all();
    sleep_ms(2000);

    printf("[SELF-TEST] Starting\n");

    /* Stage 1 — OLED */
    result_t oled_result = RESULT_FAIL;
    if (ssd1306_init()) {
        oled_result = RESULT_PASS;
        printf("[SELF-TEST] OLED: PASS\n");
    } else {
        printf("[SELF-TEST] OLED: FAIL (no ACK at 0x%02X)\n", SSD1306_ADDR);
        gpio_init(25); gpio_set_dir(25, GPIO_OUT);
        while (true) { gpio_put(25, 1); sleep_ms(100); gpio_put(25, 0); sleep_ms(100); }
    }

    /* Stage 2 — EEPROM */
    result_t eeprom_result = test_eeprom();
    printf("[SELF-TEST] EEPROM: %s\n",
           eeprom_result == RESULT_PASS ? "PASS" :
           eeprom_result == RESULT_FAIL ? "FAIL" : "NONE (not fitted)");

    /* Stage 3 — RS-485 hardware UART1 */
    result_t rs485_hw_result = test_rs485_hw();
    printf("[SELF-TEST] RS485-H: %s\n",
           rs485_hw_result == RESULT_PASS ? "PASS" :
           rs485_hw_result == RESULT_FAIL ? "FAIL" : "NONE (not fitted)");

    /* Stage 4 — RS-485 software UART3 */
    result_t rs485_sw_result = test_rs485_sw();
    printf("[SELF-TEST] RS485-S: %s\n",
           rs485_sw_result == RESULT_PASS ? "PASS" :
           rs485_sw_result == RESULT_FAIL ? "FAIL" : "NONE (not fitted)");

    /* Stages 5 & 6 — RS-232 (both phases share one init) */
    result_t rs232_hw_result, rs232_sw_result;
    test_rs232(&rs232_hw_result, &rs232_sw_result);
    printf("[SELF-TEST] RS232-H: %s\n",
           rs232_hw_result == RESULT_PASS ? "PASS" :
           rs232_hw_result == RESULT_FAIL ? "FAIL" : "NONE (not fitted)");
    printf("[SELF-TEST] RS232-S: %s\n",
           rs232_sw_result == RESULT_PASS ? "PASS" :
           rs232_sw_result == RESULT_FAIL ? "FAIL" : "NONE (not fitted)");

    /* Show results and loop */
    draw_results(oled_result, eeprom_result,
                 rs485_hw_result, rs485_sw_result,
                 rs232_hw_result, rs232_sw_result);

    uint32_t loop = 0;
    while (true) {
        sleep_ms(5000);
        draw_results(oled_result, eeprom_result,
                     rs485_hw_result, rs485_sw_result,
                     rs232_hw_result, rs232_sw_result);
        printf("[SELF-TEST] loop=%lu  OLED:%s  EEPROM:%s  "
               "RS485-H:%s  RS485-S:%s  RS232-H:%s  RS232-S:%s\n",
               loop++,
               oled_result    == RESULT_PASS ? "PASS" : "FAIL",
               eeprom_result  == RESULT_PASS ? "PASS" : eeprom_result  == RESULT_FAIL ? "FAIL" : "NONE",
               rs485_hw_result== RESULT_PASS ? "PASS" : rs485_hw_result== RESULT_FAIL ? "FAIL" : "NONE",
               rs485_sw_result== RESULT_PASS ? "PASS" : rs485_sw_result== RESULT_FAIL ? "FAIL" : "NONE",
               rs232_hw_result== RESULT_PASS ? "PASS" : rs232_hw_result== RESULT_FAIL ? "FAIL" : "NONE",
               rs232_sw_result== RESULT_PASS ? "PASS" : rs232_sw_result== RESULT_FAIL ? "FAIL" : "NONE");
    }
}
