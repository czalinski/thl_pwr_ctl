/**
 * pwr_ctl_test.c — Single-channel power control subsystem test
 *
 * Tests MCP3428 ADC communication and toggles the PSU output enable (GP27)
 * every 5 seconds.  Live channel readings and enable state are shown on the
 * SSD1306 OLED and printed over USB serial.
 *
 * Hardware (power-control proof-of-concept board):
 *   OLED:    SSD1306 128×64  I2C1 0x3C  GP2 SDA / GP3 SCL
 *   ADC:     MCP3428 4-ch    I2C1 0x68  GP2 SDA / GP3 SCL
 *            ADR0 = ADR1 = GND  →  7-bit address 0x68  (1101 000b)
 *   Enable:  GP27 output, active-high  —  high = output stage enabled
 *
 * MCP3428 channel assignment on this board:
 *   CH1 (C1:C0 = 00)  voltage sense
 *   CH3 (C1:C0 = 10)  current sense
 *
 * ---- ADC configuration ----
 * The MCP3428 uses an internal 2.048 V reference.  In 12-bit / 240 sps mode
 * the LSB is exactly 1 mV.  Raw codes are signed 12-bit (±2048 mV full scale).
 * Actual board-level V/I scaling depends on the PoC board's signal conditioning
 * resistors and is not applied here — raw mV codes are shown for bring-up.
 *
 * Config byte format: /RDY | C1 | C0 | /OC | S1 | S0 | G1 | G0
 *   /OC  = 0  →  continuous conversion
 *   S1:S0 = 00  →  12-bit / 240 sps
 *   G1:G0 = 00  →  gain 1×
 * Channel is selected via C1:C0 in bits [6:5].
 *
 * ---- NONE vs FAIL distinction ----
 * NONE — MCP3428 does not ACK the config write: device not fitted or wiring fault.
 * FAIL — MCP3428 ACKs but /RDY bit is stuck: device present but conversion stalled.
 * PASS — MCP3428 communicates and reports ready conversions on CH1 and CH3.
 *
 * ---- I2C1 bus recovery ----
 * ssd1306_flush() sends a 1025-byte I2C write.  After this large transfer the
 * RP2040 I2C1 peripheral enters a bad state where subsequent transactions are
 * NACKed.  i2c1_reinit() is called immediately after every flush to restore
 * normal bus operation before the next ADC read.
 *
 * ---- Display layout (SSD1306 128×64, 8 pages of 8 px, 6 px/char) ----
 *   Page 0:  "PWR CTL TEST"       centred title
 *   Page 1:  "ADC: PASS"          or FAIL / NONE
 *   Page 2:  "EN:  ON "           or OFF
 *   Page 3:  "V:  +1234 mV"       CH1 raw (or "V:   ------" when invalid)
 *   Page 4:  "I:  +1234 mV"       CH3 raw (or "I:   ------" when invalid)
 *   Page 5:  "Cycle: 0"           toggle count (0 = initial state shown)
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "ssd1306.h"

/* ---------------------------------------------------------------------------
 * Constants
 * --------------------------------------------------------------------------- */

/* I2C1 — shared by OLED (0x3C) and MCP3428 (0x68) on this PoC board */
#define I2C1_SDA_PIN    2
#define I2C1_SCL_PIN    3
#define I2C1_BAUD       400000

/* MCP3428 7-bit address: fixed prefix 1101 | A2=0 | ADR1=0 | ADR0=0 = 0x68
 * Both ADR pins are pulled to GND on this board. */
#define ADDR_ADC        0x68

/* Config byte base: /RDY=1 (write: re-trigger), /OC=0 (continuous),
 * S1:S0=00 (12-bit/240sps), G1:G0=00 (gain 1×).  Channel bits set per call. */
#define ADC_CFG_BASE    0x80
#define ADC_RDY_BIT     0x80    /* /RDY in the returned config byte; 0 = ready */
#define ADC_CONV_MS     5       /* 240 sps → 4.2 ms per conversion; 5 ms gives margin */

/* 0-based channel indices.  Maps to C1:C0 in bits [6:5] of the config byte.
 * CH1: C1:C0=00 → ch=0 → cfg=0x80   (voltage sense)
 * CH3: C1:C0=10 → ch=2 → cfg=0xC0   (current sense) */
#define ADC_CH_VOLT     0
#define ADC_CH_CURR     2

/* PSU output enable */
#define EN_PIN          27      /* GP27: active-high; starts LOW (output disabled) */

#define TOGGLE_MS       5000    /* EN toggle period */
#define ADC_BURST_N     8       /* back-to-back samples per noise check (~40 ms/ch) */

typedef enum { RESULT_PASS, RESULT_FAIL, RESULT_NONE } result_t;

/* ---------------------------------------------------------------------------
 * I2C1 re-initialisation
 *
 * Called after every ssd1306_flush() to recover the RP2040 I2C1 peripheral
 * from the bad state caused by the 1025-byte framebuffer write.
 * --------------------------------------------------------------------------- */
static void i2c1_reinit(void)
{
    i2c_init(i2c1, I2C1_BAUD);
    gpio_set_function(I2C1_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C1_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C1_SDA_PIN);
    gpio_pull_up(I2C1_SCL_PIN);
}

/* ---------------------------------------------------------------------------
 * MCP3428 — single channel read
 *
 * Writes a config byte to select channel ch (0-based: 0=CH1 … 3=CH4) in
 * continuous mode, waits one conversion period, then reads 3 bytes.
 *
 * Returns:
 *   RESULT_NONE  — config write NACKed: device not present or wiring fault
 *   RESULT_FAIL  — I2C read error, or /RDY bit set (conversion stalled)
 *   RESULT_PASS  — conversion ready; *out_mv holds the signed mV result
 * --------------------------------------------------------------------------- */
static result_t adc_read_ch(uint8_t ch, int16_t *out_mv)
{
    uint8_t cfg = (uint8_t)(ADC_CFG_BASE | (ch << 5));
    if (i2c_write_blocking(i2c1, ADDR_ADC, &cfg, 1, false) != 1)
        return RESULT_NONE;

    sleep_ms(ADC_CONV_MS);

    uint8_t buf[3];
    if (i2c_read_blocking(i2c1, ADDR_ADC, buf, 3, false) != 3)
        return RESULT_FAIL;

    if (buf[2] & ADC_RDY_BIT)
        return RESULT_FAIL;   /* /RDY still set — conversion not yet complete */

    /* The MCP3428 sign-extends the 12-bit result into the upper 4 bits of
     * buf[0], so casting the two data bytes directly to int16_t gives the
     * correct signed value (range −2048 … +2047 = −2048 mV … +2047 mV). */
    *out_mv = (int16_t)(((uint16_t)buf[0] << 8) | buf[1]);
    return RESULT_PASS;
}

/* ---------------------------------------------------------------------------
 * MCP3428 — burst read
 *
 * Takes ADC_BURST_N back-to-back samples of channel ch and returns the
 * min, max, and last value.  Stops early and returns the error code if any
 * individual read fails.
 * --------------------------------------------------------------------------- */
static result_t adc_burst(uint8_t ch,
                           int16_t *out_min, int16_t *out_max, int16_t *out_last)
{
    int16_t mn =  32767, mx = -32768, last = 0;
    for (uint8_t i = 0; i < ADC_BURST_N; i++) {
        int16_t v;
        result_t r = adc_read_ch(ch, &v);
        if (r != RESULT_PASS) return r;
        if (v < mn) mn = v;
        if (v > mx) mx = v;
        last = v;
    }
    *out_min  = mn;
    *out_max  = mx;
    *out_last = last;
    return RESULT_PASS;
}

/* ---------------------------------------------------------------------------
 * Display
 * --------------------------------------------------------------------------- */
static uint8_t center_col(const char *s)
{
    uint16_t px = (uint16_t)(strlen(s) * SSD1306_CHAR_W);
    return (uint8_t)(px >= SSD1306_WIDTH ? 0 : (SSD1306_WIDTH - px) / 2);
}

static void draw_screen(result_t adc_status,
                        bool en_on,
                        bool volt_valid, int16_t volt_mv,
                        int16_t volt_min, int16_t volt_max,
                        bool curr_valid, int16_t curr_mv,
                        int16_t curr_min, int16_t curr_max,
                        uint32_t cycle)
{
    static const char *adc_tag[] = { "PASS", "FAIL", "NONE" };
    char line[22];

    ssd1306_clear();

    /* Page 0 — centred title */
    ssd1306_draw_string(center_col("PWR CTL TEST"), 0, "PWR CTL TEST");

    /* Page 1 — MCP3428 communication status */
    snprintf(line, sizeof(line), "ADC: %s", adc_tag[adc_status]);
    ssd1306_draw_string(4, 1, line);

    /* Page 2 — output enable state */
    snprintf(line, sizeof(line), "EN:  %s", en_on ? "ON " : "OFF");
    ssd1306_draw_string(4, 2, line);

    /* Page 3 — voltage channel (CH1), raw mV
     * %+5d: sign-forced, minimum 5 chars wide; covers ±2047 cleanly. */
    if (volt_valid)
        snprintf(line, sizeof(line), "V: %+5d mV", volt_mv);
    else
        snprintf(line, sizeof(line), "V:   ------");
    ssd1306_draw_string(4, 3, line);

    /* Page 4 — current channel (CH3), raw mV */
    if (curr_valid)
        snprintf(line, sizeof(line), "I: %+5d mV", curr_mv);
    else
        snprintf(line, sizeof(line), "I:   ------");
    ssd1306_draw_string(4, 4, line);

    /* Page 5 — enable toggle count */
    snprintf(line, sizeof(line), "Cycle: %lu", (unsigned long)cycle);
    ssd1306_draw_string(4, 5, line);

    /* Page 6 — voltage burst span (max−min), 0 = no variation detected */
    if (volt_valid)
        snprintf(line, sizeof(line), "Vspn:%+d..%+d", volt_min, volt_max);
    else
        snprintf(line, sizeof(line), "Vspn: ------");
    ssd1306_draw_string(4, 6, line);

    /* Page 7 — current burst span */
    if (curr_valid)
        snprintf(line, sizeof(line), "Ispn:%+d..%+d", curr_min, curr_max);
    else
        snprintf(line, sizeof(line), "Ispn: ------");
    ssd1306_draw_string(4, 7, line);

    ssd1306_flush();
}

/* ---------------------------------------------------------------------------
 * Main
 * --------------------------------------------------------------------------- */
int main(void)
{
    stdio_init_all();
    sleep_ms(3000);   /* wait for USB CDC to enumerate on the host */

    printf("[PWRTEST] Starting\n");

    /* --- GP27: PSU output enable, start LOW (output disabled) --- */
    gpio_init(EN_PIN);
    gpio_set_dir(EN_PIN, GPIO_OUT);
    gpio_put(EN_PIN, 0);
    bool en_on = false;

    /* --- Stage 1: OLED (also initialises I2C1 internally) --- */
    bool oled_ok = ssd1306_init();
    printf("[PWRTEST] OLED: %s\n", oled_ok ? "PASS" : "FAIL (no ACK at 0x3C)");

    /* Re-init I2C1 after ssd1306_init()'s 1025-byte framebuffer flush.
     * The flush leaves the RP2040 I2C1 peripheral in a bad state; re-init
     * restores it before we attempt to talk to the MCP3428. */
    i2c1_reinit();

    /* --- Stage 2: MCP3428 initial communication check ---
     * Bare 3-byte read first to verify the device ACKs its address (NONE if
     * not). Then read CH1 and CH3; FAIL if either conversion stalls. */
    result_t adc_status;
    int16_t  volt_mv = 0, volt_min = 0, volt_max = 0;
    int16_t  curr_mv = 0, curr_min = 0, curr_max = 0;
    bool     volt_valid = false;
    bool     curr_valid = false;

    {
        uint8_t probe[3];
        if (i2c_read_blocking(i2c1, ADDR_ADC, probe, 3, false) != 3) {
            adc_status = RESULT_NONE;
            printf("[PWRTEST] ADC: NONE (no ACK at 0x%02X)\n", ADDR_ADC);
        } else {
            result_t rv = adc_burst(ADC_CH_VOLT, &volt_min, &volt_max, &volt_mv);
            result_t ri = adc_burst(ADC_CH_CURR, &curr_min, &curr_max, &curr_mv);
            volt_valid = (rv == RESULT_PASS);
            curr_valid = (ri == RESULT_PASS);

            if (rv == RESULT_PASS && ri == RESULT_PASS) {
                adc_status = RESULT_PASS;
                printf("[PWRTEST] ADC: PASS"
                       "  CH1=%+d mV [%+d..%+d span=%d]"
                       "  CH3=%+d mV [%+d..%+d span=%d]\n",
                       volt_mv, volt_min, volt_max, volt_max - volt_min,
                       curr_mv, curr_min, curr_max, curr_max - curr_min);
            } else {
                adc_status = RESULT_FAIL;
                printf("[PWRTEST] ADC: FAIL  CH1=%s  CH3=%s\n",
                       rv == RESULT_PASS ? "PASS" :
                       rv == RESULT_NONE ? "NONE" : "FAIL",
                       ri == RESULT_PASS ? "PASS" :
                       ri == RESULT_NONE ? "NONE" : "FAIL");
            }
        }
    }

    /* --- Initial display (cycle=0 = pre-toggle state) --- */
    if (oled_ok) {
        draw_screen(adc_status, en_on,
                    volt_valid, volt_mv, volt_min, volt_max,
                    curr_valid, curr_mv, curr_min, curr_max,
                    0);
        i2c1_reinit();
    }

    /* --- Main loop: toggle EN every 5 s, read ADC, update display ---
     *
     * Sequence each iteration:
     *   1. Sleep 5 s
     *   2. Toggle GP27
     *   3. Read CH1 and CH3 from MCP3428 (I2C1 is clean after last re-init)
     *   4. Flush OLED with new data
     *   5. Re-init I2C1 to recover from the flush (ready for next iteration)
     */
    uint32_t cycle = 0;
    while (true) {
        sleep_ms(TOGGLE_MS);
        cycle++;

        /* Toggle output enable */
        en_on = !en_on;
        gpio_put(EN_PIN, en_on ? 1 : 0);
        printf("[PWRTEST] cycle=%lu  EN=%s\n",
               (unsigned long)cycle, en_on ? "ON" : "OFF");

        /* Read ADC channels — burst of ADC_BURST_N samples each */
        result_t rv = adc_burst(ADC_CH_VOLT, &volt_min, &volt_max, &volt_mv);
        result_t ri = adc_burst(ADC_CH_CURR, &curr_min, &curr_max, &curr_mv);
        volt_valid = (rv == RESULT_PASS);
        curr_valid = (ri == RESULT_PASS);

        if (rv == RESULT_NONE) {
            adc_status = RESULT_NONE;
        } else if (rv == RESULT_PASS && ri == RESULT_PASS) {
            adc_status = RESULT_PASS;
        } else {
            adc_status = RESULT_FAIL;
        }

        printf("[PWRTEST] ADC: %s"
               "  V=%+d [%+d..%+d spn=%d]"
               "  I=%+d [%+d..%+d spn=%d]\n",
               adc_status == RESULT_PASS ? "PASS" :
               adc_status == RESULT_FAIL ? "FAIL" : "NONE",
               volt_valid ? (int)volt_mv  : 0,
               volt_valid ? (int)volt_min : 0,
               volt_valid ? (int)volt_max : 0,
               volt_valid ? volt_max - volt_min : 0,
               curr_valid ? (int)curr_mv  : 0,
               curr_valid ? (int)curr_min : 0,
               curr_valid ? (int)curr_max : 0,
               curr_valid ? curr_max - curr_min : 0);

        if (oled_ok) {
            draw_screen(adc_status, en_on,
                        volt_valid, volt_mv, volt_min, volt_max,
                        curr_valid, curr_mv, curr_min, curr_max,
                        cycle);
            i2c1_reinit();
        }
    }
}
