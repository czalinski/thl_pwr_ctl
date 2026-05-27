/**
 * dac_test.c — MCP4912T dual 10-bit DAC SPI proof-of-concept test
 *
 * Hardware:
 *   I2C0  (GP8/GP9):                     SSD1306 OLED at 0x3C
 *   SPI1  (GP10=SCK, GP11=MOSI,
 *           GP12=MISO, GP13=CS):          MCP4912T dual 10-bit DAC
 *
 * Analog signal chain:
 *   VREFA = VREFB = 2.5 V, VDD = 3.3 V
 *   DAC configured for 2x gain: V_dac = D × (2 × 2.5) / 1024 = D × 0.004883 V
 *   Op-amp (non-inverting): gain = 1 + 20 kΩ/10 kΩ = 3
 *   V_amp  = 3 × V_dac = D × 15.0 / 1024 ≈ D × 0.01465 V
 *   For V_amp = 10 V: D = round(10 × 1024 / 15) = 683
 *
 * MCP4912T SPI command word (16-bit, MSB first):
 *   [15]   A/B#   0 = channel A, 1 = channel B
 *   [14]   BUF    0 = unbuffered VREF input
 *   [13]   GA#    0 = 2x gain, 1 = 1x gain
 *   [12]   SHDN#  1 = active, 0 = shutdown
 *   [11:2] D[9:0] 10-bit code, D9 first
 *   [1:0]  X      don't care
 *   Channel A, 2x, active: 0x1000 | (code << 2)
 *   Channel B, 2x, active: 0x9000 | (code << 2)
 *
 * SPI comms check:
 *   Writes 0 V to both channels.  The MCP4912T has no MISO readback path,
 *   so "OK" means the RP2040 SPI peripheral initialised and both 16-bit
 *   transfers completed without stalling.  Confirm actual output with a
 *   multimeter on VOUTA / VOUTB (downstream of op-amp).
 *
 * Test sequence (repeats forever):
 *   Channel A: step 0–10 V in 1 V increments, 2 s/step, channel B = 0 V
 *   Channel B: step 0–10 V in 1 V increments, 2 s/step, channel A = 0 V
 *
 * ---- I2C bus recovery ----
 *   ssd1306_flush() sends a 1025-byte write that leaves the RP2040 I2C
 *   peripheral in a bad state.  bus_reinit() is called after every flush.
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/spi.h"
#include "ssd1306.h"

/* ---------------------------------------------------------------------------
 * Pin and peripheral constants
 * --------------------------------------------------------------------------- */

#define I2C0_SDA_PIN    8
#define I2C0_SCL_PIN    9
#define I2C1_SDA_PIN    2
#define I2C1_SCL_PIN    3
#define I2C_BAUD        400000u

#define SPI1_SCK_PIN    10
#define SPI1_MOSI_PIN   11
#define SPI1_MISO_PIN   12
#define SPI1_CS_PIN     13
#define SPI_BAUD        10000000u   /* 10 MHz — within MCP4912T 20 MHz max */

#define ADDR_OLED       0x3C

/* ---------------------------------------------------------------------------
 * Analog signal-chain constants
 * --------------------------------------------------------------------------- */

#define DAC_VREF_V      2.5f        /* reference voltage */
#define AMP_GAIN        3.0f        /* op-amp: 1 + 20k/10k */
#define DAC_MAX_CODE    1023u       /* 10-bit DAC, full-scale code */
/* V_amp per DAC count: 2 × VREF × AMP_GAIN / 1024 */
#define V_AMP_SCALE     (2.0f * DAC_VREF_V * AMP_GAIN / 1024.0f)  /* ~0.01465 V */

/* Test parameters */
#define NUM_STEPS       11          /* steps 0..10 → 0 V to 10 V */
#define STEP_V_AMP      1.0f        /* 1 V increment per step (amplified) */
#define STEP_MS         2000

/* ---------------------------------------------------------------------------
 * Runtime bus state — selected at boot by probing for OLED
 * --------------------------------------------------------------------------- */

static i2c_inst_t *s_bus;
static uint        s_sda, s_scl;

/* Recover I2C peripheral after ssd1306_flush() leaves it in a bad state */
static void bus_reinit(void)
{
    i2c_init(s_bus, I2C_BAUD);
    gpio_set_function(s_sda, GPIO_FUNC_I2C);
    gpio_set_function(s_scl, GPIO_FUNC_I2C);
    gpio_pull_up(s_sda);
    gpio_pull_up(s_scl);
}

/* ---------------------------------------------------------------------------
 * MCP4912T DAC driver
 * --------------------------------------------------------------------------- */

/* Command header nibbles for 2x gain, unbuffered VREF, active output */
#define CMD_CHA   0x1000u   /* A/B#=0, BUF=0, GA#=0(2x), SHDN#=1 */
#define CMD_CHB   0x9000u   /* A/B#=1, BUF=0, GA#=0(2x), SHDN#=1 */

/* Write a 10-bit code to one DAC channel.  ch: 0 = A, 1 = B */
static void dac_write(uint8_t ch, uint16_t code)
{
    uint16_t cmd = (ch ? CMD_CHB : CMD_CHA) | ((code & 0x3FFu) << 2);
    uint8_t  buf[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF) };
    gpio_put(SPI1_CS_PIN, 0);
    spi_write_blocking(spi1, buf, 2);
    gpio_put(SPI1_CS_PIN, 1);
}

/* Convert a target amplified voltage to the nearest 10-bit DAC code */
static uint16_t voltage_to_code(float v_amp)
{
    float c = v_amp / V_AMP_SCALE;
    if (c < 0.0f)             return 0;
    if (c > (float)DAC_MAX_CODE) return DAC_MAX_CODE;
    return (uint16_t)(c + 0.5f);
}

/* ---------------------------------------------------------------------------
 * Display
 * --------------------------------------------------------------------------- */

static void draw_screen(bool spi_ok, uint8_t active_ch, uint8_t step,
                        uint16_t code, uint32_t cycle, const char *bus_name)
{
    char     line[22];
    float    v_amp = code * V_AMP_SCALE;
    float    v_dac = v_amp / AMP_GAIN;

    ssd1306_clear();

    ssd1306_draw_string(10, 0, "ANALOG OUT TEST");

    snprintf(line, sizeof(line), "Bus:%-12s", bus_name);
    ssd1306_draw_string(0, 1, line);

    snprintf(line, sizeof(line), "SPI:%-4s  Cyc:%5lu",
             spi_ok ? "OK" : "FAIL", (unsigned long)cycle);
    ssd1306_draw_string(0, 2, line);

    snprintf(line, sizeof(line), "Chan: %c   Step:%2u/10",
             active_ch ? 'B' : 'A', step);
    ssd1306_draw_string(0, 3, line);

    snprintf(line, sizeof(line), "Target: %2u.0V amp", step);
    ssd1306_draw_string(0, 4, line);

    snprintf(line, sizeof(line), "DAC code:   %4u", code);
    ssd1306_draw_string(0, 5, line);

    snprintf(line, sizeof(line), "V_dac:  %6.3fV", v_dac);
    ssd1306_draw_string(0, 6, line);

    snprintf(line, sizeof(line), "V_amp:  %6.3fV", v_amp);
    ssd1306_draw_string(0, 7, line);

    ssd1306_flush();
}

/* ---------------------------------------------------------------------------
 * Main
 * --------------------------------------------------------------------------- */

int main(void)
{
    stdio_init_all();
    sleep_ms(2000);
    printf("[DAC] Starting\n");

    /* ---- Bus detection ----
     * Probe OLED on I2C0 first; fall back to I2C1.
     * Physical jumpers on the board select which bus all I2C devices share. */
    s_bus = NULL;
    const char *bus_name = "NONE";

    i2c_init(i2c0, I2C_BAUD);
    gpio_set_function(I2C0_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C0_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C0_SDA_PIN);
    gpio_pull_up(I2C0_SCL_PIN);
    sleep_ms(5);
    {
        uint8_t d = 0;
        if (i2c_write_blocking(i2c0, ADDR_OLED, &d, 1, false) >= 0) {
            s_bus = i2c0; s_sda = I2C0_SDA_PIN; s_scl = I2C0_SCL_PIN;
            bus_name = "I2C0 (GP8/9)";
        }
    }

    if (!s_bus) {
        i2c_init(i2c1, I2C_BAUD);
        gpio_set_function(I2C1_SDA_PIN, GPIO_FUNC_I2C);
        gpio_set_function(I2C1_SCL_PIN, GPIO_FUNC_I2C);
        gpio_pull_up(I2C1_SDA_PIN);
        gpio_pull_up(I2C1_SCL_PIN);
        sleep_ms(5);
        uint8_t d = 0;
        if (i2c_write_blocking(i2c1, ADDR_OLED, &d, 1, false) >= 0) {
            s_bus = i2c1; s_sda = I2C1_SDA_PIN; s_scl = I2C1_SCL_PIN;
            bus_name = "I2C1 (GP2/3)";
        }
    }

    printf("[DAC] Bus: %s\n", bus_name);

    if (!s_bus) {
        printf("[DAC] ERROR: OLED not found on either bus — check jumpers\n");
        gpio_init(25); gpio_set_dir(25, GPIO_OUT);
        while (true) { gpio_put(25, 1); sleep_ms(200); gpio_put(25, 0); sleep_ms(200); }
    }

    /* ---- I2C scan on detected bus ---- */
    printf("[DAC] I2C scan on %s:\n", bus_name);
    for (uint8_t a = 0x08; a < 0x78; a++) {
        uint8_t d = 0;
        if (i2c_write_blocking(s_bus, a, &d, 1, false) >= 0)
            printf("  found 0x%02X\n", a);
    }

    /* ---- OLED init ---- */
    ssd1306_set_bus(s_bus, s_sda, s_scl);
    bool oled_ok = ssd1306_init();
    bus_reinit();
    printf("[DAC] OLED: %s\n", oled_ok ? "PASS" : "FAIL");

    /* ---- SPI1 for MCP4912T ---- */
    spi_init(spi1, SPI_BAUD);
    spi_set_format(spi1, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(SPI1_SCK_PIN,  GPIO_FUNC_SPI);
    gpio_set_function(SPI1_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_set_function(SPI1_MISO_PIN, GPIO_FUNC_SPI);
    gpio_init(SPI1_CS_PIN);
    gpio_set_dir(SPI1_CS_PIN, GPIO_OUT);
    gpio_put(SPI1_CS_PIN, 1);   /* CS idle high */

    /* ---- SPI comms check ----
     * Write 0 V to both channels.  MCP4912T is write-only (no MISO readback);
     * "OK" means both 16-bit transfers completed.  Verify with a DMM on
     * VOUTA/VOUTB after the op-amp. */
    dac_write(0, 0);
    dac_write(1, 0);
    bool spi_ok = true;
    printf("[DAC] SPI: OK (write-only; measure VOUTA/VOUTB to confirm)\n");

    /* ---- Test loop ---- */
    uint32_t cycle = 0;

    while (true) {
        /* Channel A sweep, B held at 0 V */
        dac_write(1, 0);
        printf("[DAC] --- Chan A sweep (B=0V) ---\n");

        for (uint8_t step = 0; step < NUM_STEPS; step++) {
            uint16_t code = voltage_to_code(step * STEP_V_AMP);
            dac_write(0, code);

            printf("[DAC] A: step=%2u  target=%4.1fV  code=%4u"
                   "  V_dac=%5.3fV  V_amp=%6.3fV\n",
                   step, step * STEP_V_AMP, code,
                   code * V_AMP_SCALE / AMP_GAIN,
                   code * V_AMP_SCALE);

            if (oled_ok) {
                draw_screen(spi_ok, 0, step, code, cycle, bus_name);
                bus_reinit();
            }

            sleep_ms(STEP_MS);
        }

        /* Channel B sweep, A held at 0 V */
        dac_write(0, 0);
        printf("[DAC] --- Chan B sweep (A=0V) ---\n");

        for (uint8_t step = 0; step < NUM_STEPS; step++) {
            uint16_t code = voltage_to_code(step * STEP_V_AMP);
            dac_write(1, code);

            printf("[DAC] B: step=%2u  target=%4.1fV  code=%4u"
                   "  V_dac=%5.3fV  V_amp=%6.3fV\n",
                   step, step * STEP_V_AMP, code,
                   code * V_AMP_SCALE / AMP_GAIN,
                   code * V_AMP_SCALE);

            if (oled_ok) {
                draw_screen(spi_ok, 1, step, code, cycle, bus_name);
                bus_reinit();
            }

            sleep_ms(STEP_MS);
        }

        cycle++;
    }
}
