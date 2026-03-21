#pragma once

#include <stdint.h>
#include <stdbool.h>

/*
 * Configuration is stored in the last 4 KB flash sector (one FLASH_SECTOR_SIZE
 * erase block).  On the W5500-EVB-Pico this is the RP2040's onboard 2 MB QSPI
 * flash.  If your custom PCB adds a separate SPI NOR flash chip, replace the
 * flash_write_config() implementation in config.c with one that targets that
 * device; the rest of the API stays the same.
 */
#include "hardware/flash.h"

#define CONFIG_FLASH_OFFSET  (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define CONFIG_MAGIC         0xC0DEBEEF

/* ---------------------------------------------------------------------------
 * Factory-default network settings (REQ-001)
 *
 * 172.22.0.0/16 is a valid RFC-1918 private range (172.16.0.0/12 block).
 * It is uncommon as a default: 172.16–17.x are the "obvious" picks,
 * 172.17.0.0/16 is Docker's default bridge, and 172.31.0.0/16 is the AWS
 * default VPC — so 172.22.x.x sees very little accidental overlap.
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
 * Total size is 512 bytes (2 × FLASH_PAGE_SIZE) so flash_range_program()
 * can write it in one call.  The crc32 field covers all preceding bytes.
 * --------------------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint32_t magic;       /*  4 — CONFIG_MAGIC when valid          */
    uint8_t  mac[6];      /*  6 — Ethernet MAC address             */
    uint8_t  ip[4];       /*  4 — IPv4 address                     */
    uint8_t  subnet[4];   /*  4 — subnet mask                      */
    uint8_t  gateway[4];  /*  4 — default gateway                  */
    uint8_t  dns[4];      /*  4 — DNS server                       */
    uint8_t  _pad[482];   /* 482 — reserved for future settings    */
    uint32_t crc32;       /*  4 — CRC-32 of all preceding bytes    */
                          /* total: 512 bytes                      */
} device_config_t;

/* Verify size at compile time — requires C11 */
_Static_assert(sizeof(device_config_t) == 512, "device_config_t must be 512 bytes");

/* ---------------------------------------------------------------------------
 * API
 * --------------------------------------------------------------------------- */

/**
 * Load configuration from flash into *cfg.
 * Falls back to factory defaults if flash contains no valid config (bad magic
 * or CRC mismatch).
 */
void config_load(device_config_t *cfg);

/**
 * Persist *cfg to flash (erase sector, then program).
 * Recomputes and stores the CRC before writing.
 * The inner write runs from RAM so XIP flash stalls are safe.
 */
void config_save(const device_config_t *cfg);

/** Fill *cfg with factory defaults.  Does NOT write to flash. */
void config_factory_defaults(device_config_t *cfg);

/** CRC-32 over the config body (all bytes except the crc32 field itself). */
uint32_t config_crc32(const device_config_t *cfg);
