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
 *   FOUND — device ACKed the probe write (present on bus)
 *   NONE  — no ACK (device not fitted / address not strapped)
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"

#define I2C0_SDA_PIN    8
#define I2C0_SCL_PIN    9
#define I2C1_SDA_PIN    2
#define I2C1_SCL_PIN    3
#define I2C_BAUD        400000

#define INA219_ADDR_MIN 0x40
#define INA219_ADDR_MAX 0x4F

static void scan_bus(i2c_inst_t *bus, const char *name)
{
    printf("\n[INA219_SCAN] %s  (0x%02X – 0x%02X)\n",
           name, INA219_ADDR_MIN, INA219_ADDR_MAX);

    int found = 0;
    for (uint8_t addr = INA219_ADDR_MIN; addr <= INA219_ADDR_MAX; addr++) {
        uint8_t d = 0;
        bool ack = (i2c_write_blocking(bus, addr, &d, 1, false) >= 0);
        printf("  0x%02X  %s\n", addr, ack ? "FOUND" : "NONE");
        if (ack) found++;
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
