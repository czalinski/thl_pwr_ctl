/**
 * self_test.c — THL Power Control PCB self-test firmware
 *
 * Runs each bring-up test stage in sequence, then displays a results screen
 * on the SSD1306 OLED and prints detail over USB serial.
 *
 * Stage 1 — OLED display          (SSD1306,    I2C1 0x3C, GP2/GP3)
 * Stage 2 — EEPROM read/write     (W24C02,     I2C1 0x50, GP2/GP3)
 * Stage 3 — RS-485 HW UART1 loop  (ISL83488IBZ, GP4 TX / GP5 RX)
 * Stage 4 — RS-485 SW UART3 loop  (ISL83488IBZ, GP14 TX / GP15 RX)
 *
 * Display layout (8 pages × 128 px):
 *   Page 0  "THL PWR CTL"    centred title
 *   Page 1  "Self Test"      centred subtitle
 *   Page 2  (blank)
 *   Page 3  "OLED:    PASS/FAIL"
 *   Page 4  "EEPROM:  PASS/FAIL/NONE"
 *   Page 5  "RS485-H: PASS/FAIL/NONE"
 *   Page 6  "RS485-S: PASS/FAIL/NONE"
 *   Page 7  (reserved)
 *
 * RS-485 hardware assumptions (both channels):
 *   DE is permanently tied HIGH (driver always enabled).
 *   RE is permanently tied LOW  (receiver always enabled).
 *   Y/Z and A/B are cross-wired for loopback — what is driven on DI
 *   appears on RO with ~10 ns propagation through the transceiver.
 *
 * RESULT_NONE — device/transceiver not present (no response to stimulus).
 * RESULT_FAIL — device present but communication incorrect.
 * RESULT_PASS — all test patterns passed.
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
#define EEPROM_TEST_ADDR  0xF8    /* near end of address space, away from config at 0x00 */
#define EEPROM_WR_MS      5       /* W24C02 write cycle time */

/* Hardware UART1 RS-485 (GP4 TX, GP5 RX) */
#define RS485_HW_UART     uart1
#define RS485_HW_TX_PIN   4
#define RS485_HW_RX_PIN   5
#define RS485_HW_BAUD     9600
#define RS485_HW_RX_TIMEOUT_US  20000   /* 20 ms — well over one byte at 9600 baud */

/* Software UART3 RS-485 (GP14 TX, GP15 RX) */
#define RS485_SW_TX_PIN   14
#define RS485_SW_RX_PIN   15
#define RS485_SW_BIT_US   104     /* 9600 baud: 1 000 000 / 9600 ≈ 104 µs per bit */
#define RS485_SW_HALF_US  52      /* sample point: midpoint of each bit */

/* Test payload used by both RS-485 tests */
static const uint8_t RS485_TEST_MSG[]  = { 'R', 'S', '4', '8', '5' };
#define RS485_TEST_LEN  (sizeof(RS485_TEST_MSG))

typedef enum { RESULT_PASS, RESULT_FAIL, RESULT_NONE } result_t;

/* ---------------------------------------------------------------------------
 * Display
 * --------------------------------------------------------------------------- */

static uint8_t center_col(const char *s)
{
    uint16_t px = (uint16_t)(strlen(s) * SSD1306_CHAR_W);
    return (uint8_t)(px >= SSD1306_WIDTH ? 0 : (SSD1306_WIDTH - px) / 2);
}

static void draw_results(result_t oled, result_t eeprom,
                         result_t rs485_hw, result_t rs485_sw)
{
    static const char *tag[] = { "PASS", "FAIL", "NONE" };
    char line[22];

    ssd1306_clear();
    ssd1306_draw_string(center_col("THL PWR CTL"), 0, "THL PWR CTL");
    ssd1306_draw_string(center_col("Self Test"),   1, "Self Test");

    snprintf(line, sizeof(line), "OLED:    %s", tag[oled]);
    ssd1306_draw_string(4, 3, line);

    snprintf(line, sizeof(line), "EEPROM:  %s", tag[eeprom]);
    ssd1306_draw_string(4, 4, line);

    snprintf(line, sizeof(line), "RS485-H: %s", tag[rs485_hw]);
    ssd1306_draw_string(4, 5, line);

    snprintf(line, sizeof(line), "RS485-S: %s", tag[rs485_sw]);
    ssd1306_draw_string(4, 6, line);

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
 * Stage 3 — RS-485 hardware UART1 loopback
 *
 * Sends RS485_TEST_MSG over UART1 (GP4) through the ISL83488IBZ transceiver
 * and reads it back on GP5. DE/RE are assumed permanently tied; the Y/Z and
 * A/B pairs are cross-wired externally.
 * --------------------------------------------------------------------------- */
static result_t test_rs485_hw(void)
{
    uart_init(RS485_HW_UART, RS485_HW_BAUD);
    gpio_set_function(RS485_HW_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(RS485_HW_RX_PIN, GPIO_FUNC_UART);

    /* Drain any stale bytes in the RX FIFO */
    while (uart_is_readable(RS485_HW_UART))
        uart_getc(RS485_HW_UART);

    /* Transmit test message */
    uart_write_blocking(RS485_HW_UART, RS485_TEST_MSG, RS485_TEST_LEN);

    /* Receive and verify each byte with per-byte timeout */
    for (size_t i = 0; i < RS485_TEST_LEN; i++) {
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
               RS485_TEST_MSG[i], rx,
               rx == RS485_TEST_MSG[i] ? "OK" : "MISMATCH");
        if (rx != RS485_TEST_MSG[i]) return RESULT_FAIL;
    }

    return RESULT_PASS;
}

/* ---------------------------------------------------------------------------
 * Stage 4 — RS-485 software UART3 loopback (bit-bang, 9600 baud)
 *
 * Drives GP14 (TX → ISL83488IBZ DI → Y/Z) and samples GP15 (RX → RO ← A/B)
 * at the midpoint of every bit. Because the transceiver loopback has ~10 ns
 * propagation, GP15 mirrors GP14 at any instant — sampling at the midpoint
 * of each driven bit gives a clean read.
 *
 * NONE is returned when the start-bit probe shows GP15 does not follow GP14
 * (transceiver not present or DE/RE not driven correctly).
 * --------------------------------------------------------------------------- */
static result_t test_rs485_sw(void)
{
    /* Configure pins */
    gpio_init(RS485_SW_TX_PIN);
    gpio_set_dir(RS485_SW_TX_PIN, GPIO_OUT);
    gpio_put(RS485_SW_TX_PIN, 1);         /* idle high */

    gpio_init(RS485_SW_RX_PIN);
    gpio_set_dir(RS485_SW_RX_PIN, GPIO_IN);
    gpio_pull_up(RS485_SW_RX_PIN);

    sleep_us(200);                        /* let pins settle */

    /* --- Probe: pull TX low and check RX follows --- */
    gpio_put(RS485_SW_TX_PIN, 0);
    sleep_us(RS485_SW_HALF_US);
    int probe_rx = gpio_get(RS485_SW_RX_PIN);
    gpio_put(RS485_SW_TX_PIN, 1);
    sleep_us(RS485_SW_BIT_US);            /* complete a stop-bit worth of idle */

    if (probe_rx != 0) {
        printf("[SELF-TEST] RS485-S: RX did not follow TX low — not fitted\n");
        return RESULT_NONE;
    }

    /* --- Transmit each test byte and verify RX mirrors TX at each bit --- */
    for (size_t m = 0; m < RS485_TEST_LEN; m++) {
        uint8_t byte = RS485_TEST_MSG[m];
        bool byte_ok = true;

        /* Start bit */
        gpio_put(RS485_SW_TX_PIN, 0);
        sleep_us(RS485_SW_HALF_US);
        if (gpio_get(RS485_SW_RX_PIN) != 0) byte_ok = false;
        sleep_us(RS485_SW_HALF_US);

        /* 8 data bits, LSB first */
        for (int b = 0; b < 8; b++) {
            uint8_t tx_bit = (byte >> b) & 1u;
            gpio_put(RS485_SW_TX_PIN, tx_bit);
            sleep_us(RS485_SW_HALF_US);
            uint8_t rx_bit = (uint8_t)gpio_get(RS485_SW_RX_PIN);
            if (rx_bit != tx_bit) byte_ok = false;
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
 * Main
 * --------------------------------------------------------------------------- */
int main(void)
{
    stdio_init_all();
    sleep_ms(2000);   /* allow USB CDC to enumerate */

    printf("[SELF-TEST] Starting\n");

    /* --- Stage 1: OLED --- */
    result_t oled_result = RESULT_FAIL;
    if (ssd1306_init()) {
        oled_result = RESULT_PASS;
        printf("[SELF-TEST] OLED: PASS\n");
    } else {
        printf("[SELF-TEST] OLED: FAIL (no ACK at 0x%02X)\n", SSD1306_ADDR);
        gpio_init(25);
        gpio_set_dir(25, GPIO_OUT);
        while (true) { gpio_put(25, 1); sleep_ms(100); gpio_put(25, 0); sleep_ms(100); }
    }

    /* --- Stage 2: EEPROM --- */
    result_t eeprom_result = test_eeprom();
    printf("[SELF-TEST] EEPROM: %s\n",
           eeprom_result == RESULT_PASS ? "PASS" :
           eeprom_result == RESULT_FAIL ? "FAIL" : "NONE (not fitted)");

    /* --- Stage 3: RS-485 hardware UART1 --- */
    result_t rs485_hw_result = test_rs485_hw();
    printf("[SELF-TEST] RS485-H: %s\n",
           rs485_hw_result == RESULT_PASS ? "PASS" :
           rs485_hw_result == RESULT_FAIL ? "FAIL" : "NONE (not fitted)");

    /* --- Stage 4: RS-485 software UART3 --- */
    result_t rs485_sw_result = test_rs485_sw();
    printf("[SELF-TEST] RS485-S: %s\n",
           rs485_sw_result == RESULT_PASS ? "PASS" :
           rs485_sw_result == RESULT_FAIL ? "FAIL" : "NONE (not fitted)");

    /* --- Show results and loop --- */
    draw_results(oled_result, eeprom_result, rs485_hw_result, rs485_sw_result);

    uint32_t loop = 0;
    while (true) {
        sleep_ms(5000);
        draw_results(oled_result, eeprom_result, rs485_hw_result, rs485_sw_result);
        printf("[SELF-TEST] loop=%lu  OLED:%s  EEPROM:%s  RS485-H:%s  RS485-S:%s\n",
               loop++,
               oled_result    == RESULT_PASS ? "PASS" : "FAIL",
               eeprom_result  == RESULT_PASS ? "PASS" : eeprom_result  == RESULT_FAIL ? "FAIL" : "NONE",
               rs485_hw_result== RESULT_PASS ? "PASS" : rs485_hw_result== RESULT_FAIL ? "FAIL" : "NONE",
               rs485_sw_result== RESULT_PASS ? "PASS" : rs485_sw_result== RESULT_FAIL ? "FAIL" : "NONE");
    }
}
