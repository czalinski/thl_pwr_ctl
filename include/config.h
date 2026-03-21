#pragma once

#include <stdint.h>
#include <stdbool.h>

#define CONFIG_MAGIC  0xC0DEBEEF

/* ---------------------------------------------------------------------------
 * Factory-default network settings (REQ-001)
 * --------------------------------------------------------------------------- */
#define DEFAULT_IP      {172,  22,  77,  77}
#define DEFAULT_SUBNET  {255, 255,   0,   0}
#define DEFAULT_GW      {172,  22,   0,   1}
#define DEFAULT_DNS     {  8,   8,   8,   8}
/* WizNet OUI 00:08:DC — last three bytes are device-specific */
#define DEFAULT_MAC     {0x00, 0x08, 0xDC, 0x77, 0x77, 0x01}

/* ---------------------------------------------------------------------------
 * Persistent configuration structure
 *
 * Stored in the W24C02 EEPROM at I2C address 0x50 (I2C1, GP2/GP3).
 * The crc32 field covers all preceding bytes.
 * --------------------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint32_t magic;       /*  4 — CONFIG_MAGIC when valid */
    uint8_t  mac[6];      /*  6 — Ethernet MAC address    */
    uint8_t  ip[4];       /*  4 — IPv4 address            */
    uint8_t  subnet[4];   /*  4 — subnet mask             */
    uint8_t  gateway[4];  /*  4 — default gateway         */
    uint8_t  dns[4];      /*  4 — DNS server              */
    uint32_t crc32;       /*  4 — CRC-32 of above bytes   */
                          /* total: 30 bytes               */
} device_config_t;

/* ---------------------------------------------------------------------------
 * API
 * --------------------------------------------------------------------------- */

/**
 * Initialise I2C1 and load configuration from the W24C02 EEPROM into *cfg.
 * Falls back to factory defaults if the EEPROM contains no valid config
 * (bad magic or CRC mismatch).
 */
void config_load(device_config_t *cfg);

/**
 * Persist *cfg to the W24C02 EEPROM.
 * Recomputes and stores the CRC before writing.
 */
void config_save(const device_config_t *cfg);

/** Fill *cfg with factory defaults.  Does NOT write to EEPROM. */
void config_factory_defaults(device_config_t *cfg);

/** CRC-32 over the config body (all bytes except the crc32 field itself). */
uint32_t config_crc32(const device_config_t *cfg);
