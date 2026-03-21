/**
 * self_test.c — THL Power Control PCB self-test firmware
 *
 * Runs each bring-up test stage in sequence, then displays a results screen
 * on the SSD1306 OLED and prints detail over USB serial.
 *
 * Stage 1 — OLED display      (SSD1306, I2C1 0x3C, GP2/GP3)
 * Stage 2 — EEPROM read/write (W24C02,  I2C1 0x50, GP2/GP3)
 *
 * Display layout (8 pages × 128 px):
 *   Page 0  "THL PWR CTL"   centred title
 *   Page 1  "Self Test"     centred subtitle
 *   Page 2  (blank)
 *   Page 3  "OLED:    PASS/FAIL"
 *   Page 4  "EEPROM:  PASS/FAIL/NONE"
 *   Page 5+ reserved for future stages
 *
 * A test returns NONE when the device did not ACK on I2C (not yet soldered).
 * A test returns FAIL when the device is present but behaves incorrectly.
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "ssd1306.h"

/* W24C02 EEPROM — on I2C1, already initialised by ssd1306_init() */
#define EEPROM_ADDR      0x50
#define EEPROM_TEST_ADDR 0xF8   /* near end of address space, away from config at 0x00 */
#define EEPROM_WR_MS     5      /* W24C02 write cycle time */

typedef enum { RESULT_PASS, RESULT_FAIL, RESULT_NONE } result_t;

/* ---------------------------------------------------------------------------
 * Display helpers
 * --------------------------------------------------------------------------- */

static uint8_t center_col(const char *s)
{
    uint16_t px = (uint16_t)(strlen(s) * SSD1306_CHAR_W);
    return (uint8_t)(px >= SSD1306_WIDTH ? 0 : (SSD1306_WIDTH - px) / 2);
}

static void draw_results(result_t oled, result_t eeprom)
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

    ssd1306_flush();
}

/* ---------------------------------------------------------------------------
 * EEPROM test
 * I2C1 is already initialised by ssd1306_init(); no re-init needed.
 * Writes three distinct patterns to address 0xF8 and reads each back.
 * Returns NONE if the device does not ACK, FAIL on any mismatch, PASS otherwise.
 * --------------------------------------------------------------------------- */
static result_t test_eeprom(void)
{
    static const uint8_t patterns[] = { 0xAA, 0x55, 0xA5 };

    /* Probe — check for ACK on a write */
    uint8_t probe[2] = { EEPROM_TEST_ADDR, 0x00 };
    if (i2c_write_blocking(i2c1, EEPROM_ADDR, probe, 2, false) != 2) {
        printf("[SELF-TEST] EEPROM: no ACK at 0x%02X — not fitted\n", EEPROM_ADDR);
        return RESULT_NONE;
    }
    sleep_ms(EEPROM_WR_MS);

    for (size_t i = 0; i < sizeof(patterns); i++) {
        /* Write */
        uint8_t wr[2] = { EEPROM_TEST_ADDR, patterns[i] };
        if (i2c_write_blocking(i2c1, EEPROM_ADDR, wr, 2, false) != 2) {
            printf("[SELF-TEST] EEPROM: write error (pattern 0x%02X)\n", patterns[i]);
            return RESULT_FAIL;
        }
        sleep_ms(EEPROM_WR_MS);

        /* Read back */
        uint8_t addr = EEPROM_TEST_ADDR;
        uint8_t rb   = 0;
        if (i2c_write_blocking(i2c1, EEPROM_ADDR, &addr, 1, true)  != 1 ||
            i2c_read_blocking (i2c1, EEPROM_ADDR, &rb,   1, false) != 1) {
            printf("[SELF-TEST] EEPROM: read error (pattern 0x%02X)\n", patterns[i]);
            return RESULT_FAIL;
        }

        printf("[SELF-TEST] EEPROM: wrote 0x%02X  read 0x%02X  %s\n",
               patterns[i], rb, rb == patterns[i] ? "OK" : "MISMATCH");

        if (rb != patterns[i])
            return RESULT_FAIL;
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
        /* Cannot display anything — fast-blink the onboard LED and halt */
        gpio_init(25);
        gpio_set_dir(25, GPIO_OUT);
        while (true) { gpio_put(25, 1); sleep_ms(100); gpio_put(25, 0); sleep_ms(100); }
    }

    /* --- Stage 2: EEPROM --- */
    result_t eeprom_result = test_eeprom();
    printf("[SELF-TEST] EEPROM: %s\n",
           eeprom_result == RESULT_PASS ? "PASS" :
           eeprom_result == RESULT_FAIL ? "FAIL" : "NONE (not fitted)");

    /* --- Show results and loop --- */
    draw_results(oled_result, eeprom_result);

    uint32_t loop = 0;
    while (true) {
        sleep_ms(5000);
        draw_results(oled_result, eeprom_result);
        printf("[SELF-TEST] loop=%lu  OLED:%s  EEPROM:%s\n",
               loop++,
               oled_result   == RESULT_PASS ? "PASS" : "FAIL",
               eeprom_result == RESULT_PASS ? "PASS" :
               eeprom_result == RESULT_FAIL ? "FAIL" : "NONE");
    }
}
