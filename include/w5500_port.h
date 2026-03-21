#pragma once

#include <stdint.h>

/**
 * Initialize SPI0 and the W5500, then apply the supplied network settings.
 *
 * Pin assignments (W5500-EVB-Pico hardware):
 *   MISO  GPIO 16
 *   CS    GPIO 17  (active-low, software-controlled)
 *   SCK   GPIO 18
 *   MOSI  GPIO 19
 *   INT   GPIO 21  (unused by this driver; reserved for future IRQ use)
 *   RST   GPIO 20
 */
void w5500_port_init(const uint8_t mac[6],
                     const uint8_t ip[4],
                     const uint8_t subnet[4],
                     const uint8_t gateway[4],
                     const uint8_t dns[4]);
