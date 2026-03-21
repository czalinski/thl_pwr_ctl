/**
 * self_test.c — THL Power Control PCB self-test firmware
 *
 * Iteration 1: OLED basic check
 *   - Writes "OLED Working" to the SSD1306 display.
 *   - Prints matching debug string over USB serial.
 *   - Loops every 1 second.
 *
 * OLED: SSD1306 128×64, I2C address 0x3C
 * I2C pins: SDA = GP2, SCL = GP3  (I2C1, 400 kHz) — per PCB pin mapping
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "ssd1306.h"

// "OLED Working" is 12 chars × 6 px = 72 px wide.
// Centre horizontally: (128 - 72) / 2 = 28.
// Centre vertically: middle of 8 pages → page 3 (rows 24-31).
#define TEXT_COL  28
#define TEXT_PAGE  3

int main(void)
{
    stdio_init_all();

    // Brief pause so USB CDC enumerates before the first printf
    sleep_ms(2000);

    printf("[SELF-TEST] Starting — iteration 1: OLED check\n");

    if (!ssd1306_init()) {
        // Display did not ACK — report over serial and blink onboard LED
        printf("[SELF-TEST] ERROR: SSD1306 not found at I2C 0x%02X "
               "(SDA=GP%d SCL=GP%d)\n",
               SSD1306_ADDR, SSD1306_SDA_PIN, SSD1306_SCL_PIN);

        const uint led = 25;   /* onboard LED on W5500-EVB-Pico */
        gpio_init(led);
        gpio_set_dir(led, GPIO_OUT);
        while (true) {
            gpio_put(led, 1); sleep_ms(100);
            gpio_put(led, 0); sleep_ms(100);
        }
    }

    printf("[SELF-TEST] SSD1306 OK — entering display loop\n");

    uint32_t loop = 0;
    while (true) {
        ssd1306_clear();
        ssd1306_draw_string(TEXT_COL, TEXT_PAGE, "OLED Working");
        ssd1306_flush();

        printf("[SELF-TEST] loop=%lu  OLED Working\n", loop++);
        sleep_ms(1000);
    }
}
