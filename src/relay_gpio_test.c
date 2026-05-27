/**
 * relay_gpio_test.c — Relay driver and GPIO subsystem proof-of-concept test
 *
 * Hardware (relay/GPIO PoC board — same RP2040 MCU):
 *   I2C bus:   selected at boot by probing for OLED on I2C0 (GP8/GP9) first,
 *              then I2C1 (GP2/GP3).  Physical jumpers connect all I2C devices
 *              to one bus or the other.
 *   OLED:      SSD1306 128×64       I2C 0x3C  — display
 *   GPIO exp:  TCAL9539PWR          I2C 0x74  (A0=A1=GND)
 *   LED drv:   TLC59108IPWR         I2C 0x40  (A0-A3=GND) [verify at boot scan]
 *   GP22:      direct relay drive, bypasses GPIO expander
 *
 * GPIO expander pin assignment:
 *   P0_0   output  — drives ULN2803 input (relay via extender)
 *   P0_4   output  — direct GPIO out
 *   P1_0   input   — external signal input
 *
 * LED driver output assignment:
 *   OUT0   GP22 relay asserted
 *   OUT2   P0_0 (extender relay) asserted
 *   OUT4   mirrors P1_0 input (updated every 50 ms)
 *   OUT6   P0_4 output asserted
 *
 * Test sequence (round robin, 1 s per step):
 *   Step 0: GP22 = ON,  P0_0 = OFF, P0_4 = OFF
 *   Step 1: GP22 = OFF, P0_0 = ON,  P0_4 = OFF
 *   Step 2: GP22 = OFF, P0_0 = OFF, P0_4 = ON
 * P1_0 → OUT4 is sampled every 50 ms throughout all steps.
 *
 * ---- I2C bus recovery ----
 * ssd1306_flush() sends a 1025-byte write that leaves the RP2040 I2C
 * peripheral in a bad state.  bus_reinit() is called after every flush.
 *
 * ---- Device addresses ----
 * TCAL9539: 7-bit 0x74  (fixed prefix 11101, A1=0, A0=0)
 * TLC59108: 7-bit 0x40  — CONFIRM via boot I2C scan; update ADDR_LED_DRV
 *           if the scan shows a different address.
 *
 * ---- TCAL9539 registers (same map as TCA9539) ----
 *   0x00  Input  Port 0    0x01  Input  Port 1
 *   0x02  Output Port 0    0x03  Output Port 1
 *   0x06  Config Port 0    0x07  Config Port 1  (1=input, 0=output)
 *
 * ---- TLC59108 LEDOUT register encoding (2 bits per channel) ----
 *   00 = off   01 = fully on   10 = PWM   11 = group PWM
 *   LEDOUT0 (0x0C): OUT3[7:6] OUT2[5:4] OUT1[3:2] OUT0[1:0]
 *   LEDOUT1 (0x0D): OUT7[7:6] OUT6[5:4] OUT5[3:2] OUT4[1:0]
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "ssd1306.h"

/* ---------------------------------------------------------------------------
 * Pin and address constants
 * --------------------------------------------------------------------------- */

#define I2C0_SDA_PIN   8
#define I2C0_SCL_PIN   9
#define I2C1_SDA_PIN   2
#define I2C1_SCL_PIN   3
#define I2C_BAUD       400000

#define ADDR_OLED      0x3C
#define ADDR_GPIO_EXP  0x74   /* TCAL9539 A0=A1=GND */
#define ADDR_LED_DRV   0x40   /* TLC59108 A0-A3=GND — verify via boot scan */

#define GP22_PIN       22     /* direct relay drive */

/* TCAL9539 registers */
#define TCA_INPUT0     0x00
#define TCA_INPUT1     0x01
#define TCA_OUTPUT0    0x02
#define TCA_OUTPUT1    0x03
#define TCA_CONFIG0    0x06
#define TCA_CONFIG1    0x07

/* P0 config: bit0=P0_0 output, bit4=P0_4 output, rest inputs */
#define TCA_CFG0_VAL   0xEE   /* 1110 1110 */
#define TCA_CFG1_VAL   0xFF   /* all inputs */

/* TLC59108 registers */
#define TLC_MODE1      0x00
#define TLC_LEDOUT0    0x0C   /* OUT0-OUT3 */
#define TLC_LEDOUT1    0x0D   /* OUT4-OUT7 */

/* LEDOUT bit positions for our outputs (01 = fully on in 2-bit field) */
#define OUT0_SHIFT     0      /* LEDOUT0 bits [1:0] */
#define OUT2_SHIFT     4      /* LEDOUT0 bits [5:4] */
#define OUT4_SHIFT     0      /* LEDOUT1 bits [1:0] */
#define OUT6_SHIFT     4      /* LEDOUT1 bits [5:4] */

#define POLL_MS        50     /* P1_0 poll interval */
#define STEP_MS        1000   /* round-robin step duration */
#define POLLS_PER_STEP (STEP_MS / POLL_MS)

/* ---------------------------------------------------------------------------
 * Runtime bus state
 * --------------------------------------------------------------------------- */

static i2c_inst_t *s_bus;
static uint        s_sda, s_scl;

static void bus_reinit(void)
{
    i2c_init(s_bus, I2C_BAUD);
    gpio_set_function(s_sda, GPIO_FUNC_I2C);
    gpio_set_function(s_scl, GPIO_FUNC_I2C);
    gpio_pull_up(s_sda);
    gpio_pull_up(s_scl);
}

/* ---------------------------------------------------------------------------
 * I2C helpers
 * --------------------------------------------------------------------------- */

static bool i2c_write_reg(uint8_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_write_blocking(s_bus, dev, buf, 2, false) == 2;
}

static bool i2c_read_reg(uint8_t dev, uint8_t reg, uint8_t *val)
{
    if (i2c_write_blocking(s_bus, dev, &reg, 1, true) != 1) return false;
    return i2c_read_blocking(s_bus, dev, val, 1, false) == 1;
}

/* Probe: returns true if device ACKs a 1-byte write */
static bool i2c_probe(uint8_t addr)
{
    uint8_t d = 0;
    return i2c_write_blocking(s_bus, addr, &d, 1, false) >= 0;
}

/* Scan 0x08-0x77, print found addresses to serial */
static void i2c_scan(void)
{
    printf("[RELAY] I2C scan on %s:\n", s_bus == i2c0 ? "I2C0" : "I2C1");
    int found = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
        uint8_t d = 0;
        if (i2c_write_blocking(s_bus, a, &d, 1, false) >= 0) {
            printf("  found 0x%02X\n", a);
            found++;
        }
    }
    if (!found) printf("  (none)\n");
}

/* ---------------------------------------------------------------------------
 * TCAL9539 GPIO expander
 * --------------------------------------------------------------------------- */

static bool     s_gpio_exp_ok = false;
static uint8_t  s_out0 = 0x00;   /* tracked output port 0 shadow */

static bool gpio_exp_init(void)
{
    if (!i2c_write_reg(ADDR_GPIO_EXP, TCA_CONFIG0, TCA_CFG0_VAL)) return false;
    if (!i2c_write_reg(ADDR_GPIO_EXP, TCA_CONFIG1, TCA_CFG1_VAL)) return false;
    /* Start with all outputs low */
    s_out0 = 0x00;
    i2c_write_reg(ADDR_GPIO_EXP, TCA_OUTPUT0, s_out0);
    return true;
}

/* Set or clear a single bit in the output port 0 shadow and write it */
static void gpio_exp_set_p0(uint8_t bit, bool on)
{
    if (on) s_out0 |=  (1u << bit);
    else    s_out0 &= ~(1u << bit);
    i2c_write_reg(ADDR_GPIO_EXP, TCA_OUTPUT0, s_out0);
}

/* Read P1_0; returns -1 on I2C error */
static int gpio_exp_read_p1_0(void)
{
    uint8_t val;
    if (!i2c_read_reg(ADDR_GPIO_EXP, TCA_INPUT1, &val)) return -1;
    return (val >> 0) & 1;
}

/* ---------------------------------------------------------------------------
 * TLC59108 LED driver
 * --------------------------------------------------------------------------- */

static bool    s_led_ok = false;
static uint8_t s_ledout0 = 0x00;
static uint8_t s_ledout1 = 0x00;

static bool led_drv_init(void)
{
    /* Wake from sleep (clear bit 4), disable ALLCALL (bit 0) */
    if (!i2c_write_reg(ADDR_LED_DRV, TLC_MODE1, 0x00)) return false;
    sleep_ms(1);   /* oscillator settle after waking */
    s_ledout0 = 0x00;
    s_ledout1 = 0x00;
    i2c_write_reg(ADDR_LED_DRV, TLC_LEDOUT0, s_ledout0);
    i2c_write_reg(ADDR_LED_DRV, TLC_LEDOUT1, s_ledout1);
    return true;
}

/* Set one 2-bit LED field in a LEDOUT byte: on=01, off=00 */
static uint8_t ledout_set(uint8_t reg_val, uint8_t shift, bool on)
{
    reg_val &= ~(0x03u << shift);
    if (on) reg_val |= (0x01u << shift);
    return reg_val;
}

static void led_set_out0(bool on)
{
    s_ledout0 = ledout_set(s_ledout0, OUT0_SHIFT, on);
    i2c_write_reg(ADDR_LED_DRV, TLC_LEDOUT0, s_ledout0);
}

static void led_set_out2(bool on)
{
    s_ledout0 = ledout_set(s_ledout0, OUT2_SHIFT, on);
    i2c_write_reg(ADDR_LED_DRV, TLC_LEDOUT0, s_ledout0);
}

static void led_set_out4(bool on)
{
    s_ledout1 = ledout_set(s_ledout1, OUT4_SHIFT, on);
    i2c_write_reg(ADDR_LED_DRV, TLC_LEDOUT1, s_ledout1);
}

static void led_set_out6(bool on)
{
    s_ledout1 = ledout_set(s_ledout1, OUT6_SHIFT, on);
    i2c_write_reg(ADDR_LED_DRV, TLC_LEDOUT1, s_ledout1);
}

/* ---------------------------------------------------------------------------
 * Display
 * --------------------------------------------------------------------------- */

static const char *STEP_NAMES[] = { "GP22 relay", "P0_0 relay", "P0_4 out  " };

static void draw_screen(uint8_t step, bool gp22, bool p0_0, bool p0_4,
                        int p1_0, bool gpio_exp_ok, bool led_ok,
                        const char *bus_name, uint32_t cycle)
{
    char line[22];
    ssd1306_clear();

    ssd1306_draw_string(16, 0, "RELAY GPIO TEST");

    snprintf(line, sizeof(line), "Bus:  %s", bus_name);
    ssd1306_draw_string(4, 1, line);

    snprintf(line, sizeof(line), "Step: %u %s", step, STEP_NAMES[step]);
    ssd1306_draw_string(4, 2, line);

    snprintf(line, sizeof(line), "GP22: %s  P0_0: %s",
             gp22 ? "ON " : "OFF", p0_0 ? "ON " : "OFF");
    ssd1306_draw_string(4, 3, line);

    snprintf(line, sizeof(line), "P0_4: %s  P1_0: %s",
             p0_4 ? "ON " : "OFF",
             p1_0 < 0 ? "ERR" : (p1_0 ? "HI " : "LO "));
    ssd1306_draw_string(4, 4, line);

    snprintf(line, sizeof(line), "EXP:%s LED:%s  #%lu",
             gpio_exp_ok ? "OK " : "NON",
             led_ok      ? "OK " : "NON",
             (unsigned long)cycle);
    ssd1306_draw_string(4, 5, line);

    ssd1306_flush();
}

/* ---------------------------------------------------------------------------
 * Main
 * --------------------------------------------------------------------------- */

int main(void)
{
    stdio_init_all();
    sleep_ms(2000);
    printf("[RELAY] Starting\n");

    /* GP22: direct relay drive, start de-asserted */
    gpio_init(GP22_PIN);
    gpio_set_dir(GP22_PIN, GPIO_OUT);
    gpio_put(GP22_PIN, 0);

    /* ---- Bus detection ----
     * Probe OLED on I2C0 first; fall back to I2C1.
     * All I2C devices (OLED, GPIO exp, LED driver) share the jumper-selected bus. */
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

    printf("[RELAY] Bus: %s\n", bus_name);

    if (!s_bus) {
        /* No bus found — blink the LED and stall; nothing else can proceed */
        printf("[RELAY] ERROR: OLED not found on either bus — check jumpers\n");
        gpio_init(25); gpio_set_dir(25, GPIO_OUT);
        while (true) {
            gpio_put(25, 1); sleep_ms(200);
            gpio_put(25, 0); sleep_ms(200);
        }
    }

    /* ---- OLED init ---- */
    ssd1306_set_bus(s_bus, s_sda, s_scl);
    bool oled_ok = ssd1306_init();
    printf("[RELAY] OLED: %s\n", oled_ok ? "PASS" : "FAIL");
    bus_reinit();   /* recover from 1025-byte flush */

    /* ---- I2C scan (useful for verifying LED driver address) ---- */
    i2c_scan();

    /* ---- TCAL9539 init ---- */
    s_gpio_exp_ok = gpio_exp_init();
    printf("[RELAY] GPIO-EXP (0x%02X): %s\n",
           ADDR_GPIO_EXP, s_gpio_exp_ok ? "PASS" : "NONE");

    /* ---- TLC59108 init ---- */
    s_led_ok = led_drv_init();
    printf("[RELAY] LED-DRV (0x%02X): %s\n",
           ADDR_LED_DRV, s_led_ok ? "PASS" : "NONE");

    /* ---- Round-robin test loop ---- */
    uint8_t  step  = 0;
    uint32_t cycle = 0;
    bool     gp22  = false, p0_0 = false, p0_4 = false;
    int      p1_0  = -1;

    while (true) {
        /* Apply outputs for this step */
        gp22 = (step == 0);
        p0_0 = (step == 1);
        p0_4 = (step == 2);

        gpio_put(GP22_PIN, gp22 ? 1 : 0);

        if (s_gpio_exp_ok) {
            gpio_exp_set_p0(0, p0_0);
            gpio_exp_set_p0(4, p0_4);
        }

        if (s_led_ok) {
            led_set_out0(gp22);
            led_set_out2(p0_0);
            led_set_out6(p0_4);
        }

        printf("[RELAY] step=%u  GP22=%d P0_0=%d P0_4=%d\n",
               step, gp22, p0_0, p0_4);

        /* Update display once at step start */
        if (oled_ok) {
            draw_screen(step, gp22, p0_0, p0_4, p1_0,
                        s_gpio_exp_ok, s_led_ok, bus_name, cycle);
            bus_reinit();
        }

        /* Poll P1_0 → OUT4 every POLL_MS for STEP_MS total */
        for (int t = 0; t < POLLS_PER_STEP; t++) {
            sleep_ms(POLL_MS);

            p1_0 = gpio_exp_read_p1_0();
            bool p1_0_on = (p1_0 == 1);

            if (s_led_ok)
                led_set_out4(p1_0_on);

            /* Print P1_0 change on transitions for visibility */
            static int last_p1_0 = -2;
            if (p1_0 != last_p1_0) {
                printf("[RELAY]   P1_0=%s\n",
                       p1_0 < 0 ? "ERR" : (p1_0 ? "HI" : "LO"));
                last_p1_0 = p1_0;
            }
        }

        step = (step + 1) % 3;
        cycle++;
    }
}
