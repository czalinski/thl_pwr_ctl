#include "config.h"

#include <string.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"

/* ---------------------------------------------------------------------------
 * W24C02 EEPROM — I2C1, address 0x50, GP2 SDA / GP3 SCL (PCB pin mapping)
 * --------------------------------------------------------------------------- */
#define EEPROM_ADDR      0x50
#define EEPROM_I2C       i2c1
#define EEPROM_SDA       2
#define EEPROM_SCL       3
#define EEPROM_BAUD      400000u
#define EEPROM_PAGE_SIZE 16          /* W24C02 page write boundary */
#define EEPROM_WR_MS     5           /* max write cycle time per page */

static bool i2c_ready = false;

static void eeprom_init(void)
{
    if (i2c_ready) return;
    i2c_init(EEPROM_I2C, EEPROM_BAUD);
    gpio_set_function(EEPROM_SDA, GPIO_FUNC_I2C);
    gpio_set_function(EEPROM_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(EEPROM_SDA);
    gpio_pull_up(EEPROM_SCL);
    i2c_ready = true;
}

static void eeprom_read(uint8_t addr, uint8_t *data, size_t len)
{
    i2c_write_blocking(EEPROM_I2C, EEPROM_ADDR, &addr, 1, true);
    i2c_read_blocking(EEPROM_I2C, EEPROM_ADDR, data, len, false);
}

static void eeprom_write(uint8_t addr, const uint8_t *data, size_t len)
{
    while (len > 0) {
        /* Respect 16-byte page boundaries */
        uint8_t page_rem = EEPROM_PAGE_SIZE - (addr % EEPROM_PAGE_SIZE);
        size_t chunk = len < page_rem ? len : page_rem;

        uint8_t buf[EEPROM_PAGE_SIZE + 1];
        buf[0] = addr;
        memcpy(buf + 1, data, chunk);
        i2c_write_blocking(EEPROM_I2C, EEPROM_ADDR, buf, chunk + 1, false);
        sleep_ms(EEPROM_WR_MS);

        addr += (uint8_t)chunk;
        data += chunk;
        len  -= chunk;
    }
}

/* ---------------------------------------------------------------------------
 * CRC-32 (standard polynomial 0xEDB88320, little-endian bit order)
 * --------------------------------------------------------------------------- */
static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
    }
    return ~crc;
}

uint32_t config_crc32(const device_config_t *cfg)
{
    return crc32_update(0, (const uint8_t *)cfg,
                        sizeof(device_config_t) - sizeof(uint32_t));
}

/* ---------------------------------------------------------------------------
 * Factory defaults
 * --------------------------------------------------------------------------- */
void config_factory_defaults(device_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->magic = CONFIG_MAGIC;

    uint8_t mac[]    = DEFAULT_MAC;
    uint8_t ip[]     = DEFAULT_IP;
    uint8_t subnet[] = DEFAULT_SUBNET;
    uint8_t gw[]     = DEFAULT_GW;
    uint8_t dns[]    = DEFAULT_DNS;

    memcpy(cfg->mac,     mac,    sizeof(cfg->mac));
    memcpy(cfg->ip,      ip,     sizeof(cfg->ip));
    memcpy(cfg->subnet,  subnet, sizeof(cfg->subnet));
    memcpy(cfg->gateway, gw,     sizeof(cfg->gateway));
    memcpy(cfg->dns,     dns,    sizeof(cfg->dns));

    cfg->crc32 = config_crc32(cfg);
}

/* ---------------------------------------------------------------------------
 * Load / Save
 * --------------------------------------------------------------------------- */
void config_load(device_config_t *cfg)
{
    eeprom_init();

    device_config_t tmp;
    eeprom_read(0, (uint8_t *)&tmp, sizeof(tmp));

    if (tmp.magic == CONFIG_MAGIC && tmp.crc32 == config_crc32(&tmp)) {
        *cfg = tmp;
    } else {
        config_factory_defaults(cfg);
    }
}

void config_save(const device_config_t *cfg)
{
    device_config_t tmp = *cfg;
    tmp.magic = CONFIG_MAGIC;
    tmp.crc32 = config_crc32(&tmp);
    eeprom_write(0, (uint8_t *)&tmp, sizeof(tmp));
}
