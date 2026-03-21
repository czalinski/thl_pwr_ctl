#include "config.h"

#include <string.h>
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

/* Read-only pointer into XIP-mapped flash where the config sector lives */
#define CONFIG_XIP_PTR  ((const device_config_t *)(XIP_BASE + CONFIG_FLASH_OFFSET))

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
    /* Cover everything except the trailing crc32 field */
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
 * Load
 * --------------------------------------------------------------------------- */
void config_load(device_config_t *cfg)
{
    const device_config_t *f = CONFIG_XIP_PTR;

    if (f->magic == CONFIG_MAGIC && f->crc32 == config_crc32(f)) {
        memcpy(cfg, f, sizeof(device_config_t));
    } else {
        config_factory_defaults(cfg);
    }
}

/* ---------------------------------------------------------------------------
 * Save — must execute from RAM while flash is being written
 * --------------------------------------------------------------------------- */
static void __no_inline_not_in_flash_func(do_flash_write)(const uint8_t *data,
                                                           size_t len)
{
    uint32_t irq = save_and_disable_interrupts();
    flash_range_erase(CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(CONFIG_FLASH_OFFSET, data, len);
    restore_interrupts(irq);
}

void config_save(const device_config_t *cfg)
{
    device_config_t tmp;
    memcpy(&tmp, cfg, sizeof(tmp));
    tmp.magic = CONFIG_MAGIC;
    tmp.crc32 = config_crc32(&tmp);
    do_flash_write((const uint8_t *)&tmp, sizeof(tmp));
}
