/**
 * ina219_scan.c — I2C address scan for INA219 current/voltage sensors
 *
 * Scans both I2C buses for devices in the INA219 address range 0x40–0x4F.
 * Both buses are probed so the result is bus-independent.
 *
 * INA219 address encoding (A1 / A0 pin strapping):
 *   A1  A0   Addr     A1   A0   Addr
 *   GND GND  0x40     VS+  GND  0x44
 *   GND VS+  0x41     VS+  VS+  0x45
 *   GND SDA  0x42     VS+  SDA  0x46
 *   GND SCL  0x43     VS+  SCL  0x47
 *   SCL GND  0x48     SDA  GND  0x4C
 *   SCL VS+  0x49     SDA  VS+  0x4D
 *   SCL SDA  0x4A     SDA  SDA  0x4E
 *   SCL SCL  0x4B     SDA  SCL  0x4F
 *
 * Output key:
 *   FOUND  — device ACKed; config register read and checked against default
 *   NONE   — no ACK (device not fitted / address not strapped)
 *
 * INA219 config register (0x00) power-on default: 0x399F
 *   [15:13] BRNG/PG = 0b001  (32V bus range, ±320 mV shunt)
 *   [12:11] BADC   = 0b11    (12-bit, 532 µs)
 *   [7:3]   SADC   = 0b11    (12-bit, 532 µs)
 *   [2:0]   MODE   = 0b111   (continuous shunt+bus)
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"

#define I2C0_SDA_PIN    8
#define I2C0_SCL_PIN    9
#define I2C1_SDA_PIN    2
#define I2C1_SCL_PIN    3
#define I2C_BAUD        400000

#define INA219_ADDR_MIN   0x40
#define INA219_ADDR_MAX   0x4F
#define INA219_REG_CONFIG 0x00
#define INA219_CONFIG_DEFAULT 0x399F

/* Read a 16-bit big-endian register; returns false on I2C error */
static bool ina219_read_reg(i2c_inst_t *bus, uint8_t addr,
                             uint8_t reg, uint16_t *val)
{
    if (i2c_write_blocking(bus, addr, &reg, 1, true) != 1) return false;
    uint8_t buf[2];
    if (i2c_read_blocking(bus, addr, buf, 2, false) != 2) return false;
    *val = ((uint16_t)buf[0] << 8) | buf[1];
    return true;
}

static void scan_bus(i2c_inst_t *bus, const char *name)
{
    printf("\n[INA219_SCAN] %s  (0x%02X – 0x%02X)\n",
           name, INA219_ADDR_MIN, INA219_ADDR_MAX);

    int found = 0;
    for (uint8_t addr = INA219_ADDR_MIN; addr <= INA219_ADDR_MAX; addr++) {
        uint8_t d = 0;
        bool ack = (i2c_write_blocking(bus, addr, &d, 1, false) >= 0);
        if (!ack) {
            printf("  0x%02X  NONE\n", addr);
            continue;
        }
        found++;
        uint16_t cfg;
        if (!ina219_read_reg(bus, addr, INA219_REG_CONFIG, &cfg)) {
            printf("  0x%02X  FOUND  config=READ_ERR\n", addr);
        } else {
            printf("  0x%02X  FOUND  config=0x%04X  %s\n", addr, cfg,
                   cfg == INA219_CONFIG_DEFAULT ? "(default — OK)" : "(non-default)");
        }
    }

    printf("[INA219_SCAN] %s: %d device(s) found in range\n", name, found);
}

int main(void)
{
    stdio_init_all();
    sleep_ms(2000);   /* wait for USB CDC to enumerate */

    printf("[INA219_SCAN] Starting — scanning both I2C buses\n");

    /* I2C0: GP8 SDA, GP9 SCL */
    i2c_init(i2c0, I2C_BAUD);
    gpio_set_function(I2C0_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C0_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C0_SDA_PIN);
    gpio_pull_up(I2C0_SCL_PIN);
    sleep_ms(5);
    scan_bus(i2c0, "I2C0 (GP8/GP9)");

    /* I2C1: GP2 SDA, GP3 SCL */
    i2c_init(i2c1, I2C_BAUD);
    gpio_set_function(I2C1_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C1_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C1_SDA_PIN);
    gpio_pull_up(I2C1_SCL_PIN);
    sleep_ms(5);
    scan_bus(i2c1, "I2C1 (GP2/GP3)");

    printf("\n[INA219_SCAN] Done.\n");

    /* Keep USB CDC alive so output is readable after the scan completes */
    while (true) {
        sleep_ms(5000);
    }
}
