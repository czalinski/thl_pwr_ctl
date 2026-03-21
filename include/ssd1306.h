#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "hardware/i2c.h"

// ---------------------------------------------------------------------------
// SSD1306 128×64 I2C driver
// Default bus / pins match the PCB pin mapping:
//   GP2 = I2C1 SDA, GP3 = I2C1 SCL
// Override via CMake target_compile_definitions if needed.
// ---------------------------------------------------------------------------
#ifndef SSD1306_I2C_INST
#define SSD1306_I2C_INST  i2c1
#endif
#ifndef SSD1306_SDA_PIN
#define SSD1306_SDA_PIN   2
#endif
#ifndef SSD1306_SCL_PIN
#define SSD1306_SCL_PIN   3
#endif
#ifndef SSD1306_I2C_BAUD
#define SSD1306_I2C_BAUD  400000u   /* 400 kHz fast-mode */
#endif

#define SSD1306_ADDR    0x3C
#define SSD1306_WIDTH   128
#define SSD1306_HEIGHT  64
#define SSD1306_PAGES   (SSD1306_HEIGHT / 8)  /* 8 */

// Font metrics (5-wide glyph + 1-pixel gap = 6 px per character)
#define SSD1306_FONT_W  5
#define SSD1306_FONT_H  8   /* one page tall */
#define SSD1306_CHAR_W  6   /* advance width including gap */

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/**
 * Initialise I2C bus and send the SSD1306 startup sequence.
 * Must be called once before any other ssd1306_* function.
 * Returns true on success, false if the display did not ACK.
 */
bool ssd1306_init(void);

/** Fill framebuffer with 0 (all pixels off). Does NOT push to display. */
void ssd1306_clear(void);

/**
 * Draw an ASCII string into the framebuffer at the given pixel position.
 * @param col  Starting pixel column  [0, SSD1306_WIDTH)
 * @param page Starting page row      [0, SSD1306_PAGES)   (1 page = 8 px tall)
 * @param str  Null-terminated ASCII string
 * Characters outside the display bounds are clipped.
 */
void ssd1306_draw_string(uint8_t col, uint8_t page, const char *str);

/** Push the framebuffer to the display over I2C. */
void ssd1306_flush(void);
