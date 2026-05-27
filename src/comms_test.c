/**
 * comms_test.c — I2C and SPI device communication test
 *
 * Probes each device for basic communication. Results are displayed on the
 * SSD1306 OLED and printed over USB serial every 5 seconds.
 *
 * RESULT_NONE — no ACK / no SPI response: device not fitted or not wired
 * RESULT_FAIL — device responds but data is wrong: fitted but faulty
 * RESULT_PASS — device communicates correctly
 *
 * Stage 1 — SSD1306 OLED       I2C1  0x3C  GP2/GP3    init + display
 * Stage 2 — W24C02 EEPROM      I2C1  0x50  GP2/GP3    write + read verify
 * Stage 3 — TCA9555 GPIO exp   I2C1  0x20  GP2/GP3    I2C ACK probe
 * Stage 4 — MCP3428 PSU ADC    I2C0  0x36  GP8/GP9    I2C ACK probe
 * Stage 5 — MCP3428 Ext ADC    I2C0  0x37  GP8/GP9    I2C ACK probe
 * Stage 6 — MCP48FVB02T DAC    SPI1        GP10-13    write + readback
 *
 * ---- I2C1 bus recovery note ----
 * ssd1306_flush() sends a 1025-byte framebuffer write. After this large
 * transfer the RP2040 I2C1 peripheral enters a bad state where all
 * subsequent transactions are NACKed. Calling i2c_init() after ssd1306_init()
 * resets the peripheral and restores normal operation.
 *
 * ---- EEPROM scratch area ----
 * Write/read verification uses address 0xFE (near end of 256-byte device)
 * to stay clear of any application data stored at the start.
 *
 * ---- SPI DAC probe ----
 * The MCP48FVB02T supports 24-bit SPI readback (cmd = 0b11). The probe
 * writes 0xA5 to DAC channel A, then reads it back. MISO pulled high
 * (no device) returns 0xFF; a matching readback confirms the device.
 * SPI frame: bits[23:19]=addr, bits[18:17]=cmd, bit[16]=err, bits[15:0]=data.
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/spi.h"
#include "ssd1306.h"

/* ---------------------------------------------------------------------------
 * Pin and address constants
 * --------------------------------------------------------------------------- */

/* I2C1 — OLED, EEPROM, GPIO expander */
#define I2C1_SDA_PIN   2
#define I2C1_SCL_PIN   3
#define I2C1_BAUD      400000

/* I2C0 — ADC pair */
#define I2C0_SDA_PIN   8
#define I2C0_SCL_PIN   9
#define I2C0_BAUD      400000

/* Device addresses (7-bit) */
#define ADDR_OLED      0x3C
#define ADDR_EEPROM    0x50
#define ADDR_GPIO_EXP  0x20
#define ADDR_ADC_PSU   0x36
#define ADDR_ADC_EXT   0x37

/* EEPROM scratch byte — near end of 256-byte address space */
#define EEPROM_SCRATCH 0xFE
#define EEPROM_WR_MS   5

/* SPI1 DAC */
#define DAC_SCK_PIN    10
#define DAC_MOSI_PIN   11
#define DAC_MISO_PIN   12
#define DAC_CS_PIN     13
#define DAC_SPI_BAUD   1000000
#define DAC_TEST_VAL   0xA5   /* arbitrary non-zero, non-0xFF test pattern */

/* ---------------------------------------------------------------------------
 * Result store and display — identical pattern to self_test
 * --------------------------------------------------------------------------- */
#define MAX_RESULTS 8

typedef enum { RESULT_PASS, RESULT_FAIL, RESULT_NONE } result_t;

static const char *result_label[MAX_RESULTS];
static result_t    result_value[MAX_RESULTS];
static int         result_count = 0;

static void record(const char *label, result_t r)
{
    if (result_count < MAX_RESULTS) {
        result_label[result_count] = label;
        result_value[result_count] = r;
        result_count++;
    }
}

static uint8_t center_col(const char *s)
{
    uint16_t px = (uint16_t)(strlen(s) * SSD1306_CHAR_W);
    return (uint8_t)(px >= SSD1306_WIDTH ? 0 : (SSD1306_WIDTH - px) / 2);
}

static void draw_screen(void)
{
    static const char *tag[] = { "PASS", "FAIL", "NONE" };
    char line[22];
    ssd1306_clear();
    ssd1306_draw_string(center_col("THL PWR CTL"), 0, "THL PWR CTL");
    for (int i = 0; i < result_count && i < 7; i++) {
        snprintf(line, sizeof(line), "%s%s",
                 result_label[i], tag[result_value[i]]);
        ssd1306_draw_string(4, 1 + i, line);
    }
    ssd1306_flush();
}

/* ---------------------------------------------------------------------------
 * I2C ACK probe — write 1 byte to an address; returns true if ACKed.
 * Used for devices where a bare 1-byte write is safe (GPIO exp, ADC).
 * --------------------------------------------------------------------------- */
static bool i2c_probe(i2c_inst_t *bus, uint8_t addr)
{
    uint8_t d = 0;
    return i2c_write_blocking(bus, addr, &d, 1, false) >= 0;
}

/* ---------------------------------------------------------------------------
 * Stage 2 — W24C02 EEPROM (I2C1)
 *
 * Writes three alternating patterns to scratch address 0xFE then reads each
 * back. NONE if the device does not ACK; FAIL on data mismatch.
 * --------------------------------------------------------------------------- */
static result_t test_eeprom(void)
{
    static const uint8_t pats[] = { 0xAA, 0x55, 0xA5 };

    /* Probe */
    uint8_t probe[2] = { EEPROM_SCRATCH, 0x00 };
    if (i2c_write_blocking(i2c1, ADDR_EEPROM, probe, 2, false) != 2) {
        printf("[COMMS] EEPROM: no ACK at 0x%02X\n", ADDR_EEPROM);
        return RESULT_NONE;
    }
    sleep_ms(EEPROM_WR_MS);

    for (size_t i = 0; i < sizeof(pats); i++) {
        uint8_t wr[2] = { EEPROM_SCRATCH, pats[i] };
        if (i2c_write_blocking(i2c1, ADDR_EEPROM, wr, 2, false) != 2)
            return RESULT_FAIL;
        sleep_ms(EEPROM_WR_MS);

        uint8_t addr = EEPROM_SCRATCH, rb = 0;
        if (i2c_write_blocking(i2c1, ADDR_EEPROM, &addr, 1, true)  != 1 ||
            i2c_read_blocking (i2c1, ADDR_EEPROM, &rb,   1, false) != 1)
            return RESULT_FAIL;

        printf("[COMMS] EEPROM: wrote 0x%02X  read 0x%02X  %s\n",
               pats[i], rb, rb == pats[i] ? "OK" : "MISMATCH");
        if (rb != pats[i]) return RESULT_FAIL;
    }
    return RESULT_PASS;
}

/* ---------------------------------------------------------------------------
 * Stage 6 — MCP48FVB02T DAC (SPI1)
 *
 * Writes 0xA5 to DAC channel A then reads it back. MISO pulled high with no
 * device returns 0xFF bytes; a matching readback confirms the device is present
 * and the SPI bus is wired correctly.
 *
 * SPI frame (24-bit, MSB first):
 *   Write: byte0 = (addr<<3)|(cmd<<1)|err  byte1 = 0x00  byte2 = value
 *          addr=0x00 (DAC A), cmd=0b00 (write), err=0
 *   Read:  byte0 = (addr<<3)|(0b11<<1)|0   byte1 = 0x00  byte2 = 0x00
 *          MISO returns [don't-care, high-byte, value]
 * --------------------------------------------------------------------------- */
static result_t test_dac(void)
{
    spi_init(spi1, DAC_SPI_BAUD);
    spi_set_format(spi1, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(DAC_SCK_PIN,  GPIO_FUNC_SPI);
    gpio_set_function(DAC_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_set_function(DAC_MISO_PIN, GPIO_FUNC_SPI);
    gpio_pull_up(DAC_MISO_PIN);   /* pull high so absent device reads 0xFF */
    gpio_init(DAC_CS_PIN);
    gpio_set_dir(DAC_CS_PIN, GPIO_OUT);
    gpio_put(DAC_CS_PIN, 1);
    sleep_us(100);

    /* Write 0xA5 to DAC channel A */
    uint8_t wr[3] = { 0x00, 0x00, DAC_TEST_VAL };
    gpio_put(DAC_CS_PIN, 0);
    spi_write_blocking(spi1, wr, 3);
    gpio_put(DAC_CS_PIN, 1);
    sleep_us(10);

    /* Read back DAC channel A */
    uint8_t rd_tx[3] = { 0x06, 0x00, 0x00 };   /* (0x00<<3)|(0b11<<1) = 0x06 */
    uint8_t rd_rx[3] = { 0 };
    gpio_put(DAC_CS_PIN, 0);
    spi_write_read_blocking(spi1, rd_tx, rd_rx, 3);
    gpio_put(DAC_CS_PIN, 1);

    uint8_t got = rd_rx[2];
    printf("[COMMS] DAC: wrote 0x%02X  read 0x%02X\n", DAC_TEST_VAL, got);

    if (got == 0xFF)
        return RESULT_NONE;   /* MISO floating high — not fitted */
    if (got == DAC_TEST_VAL)
        return RESULT_PASS;
    return RESULT_FAIL;
}

/* ---------------------------------------------------------------------------
 * Main
 * --------------------------------------------------------------------------- */
int main(void)
{
    stdio_init_all();
    sleep_ms(3000);

    printf("[COMMS] Starting\n");

    /* ---- Stage 1: OLED ---- */
    result_t r;
    bool oled_ok = ssd1306_init();
    if (oled_ok) {
        r = RESULT_PASS;
        printf("[COMMS] OLED: PASS\n");
    } else {
        r = RESULT_FAIL;
        printf("[COMMS] OLED: FAIL (no ACK at 0x%02X)\n", ADDR_OLED);
    }
    record("OLED:   ", r);

    /* Reset I2C1 after ssd1306_init() — the 1025-byte framebuffer flush
     * leaves the RP2040 I2C1 peripheral in a bad state. Re-initialising
     * here restores normal operation for all subsequent I2C1 tests. */
    i2c_init(i2c1, I2C1_BAUD);
    gpio_set_function(I2C1_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C1_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C1_SDA_PIN);
    gpio_pull_up(I2C1_SCL_PIN);

    /* ---- Stage 2: EEPROM ---- */
    r = test_eeprom();
    printf("[COMMS] EEPROM: %s\n",
           r == RESULT_PASS ? "PASS" : r == RESULT_FAIL ? "FAIL" : "NONE");
    record("EEPROM: ", r);

    /* ---- Stage 3: TCA9555 GPIO expander (I2C1) ---- */
    r = i2c_probe(i2c1, ADDR_GPIO_EXP) ? RESULT_PASS : RESULT_NONE;
    printf("[COMMS] GPIO-EXP: %s\n", r == RESULT_PASS ? "PASS" : "NONE");
    record("GPIO:   ", r);

    /* ---- Stages 4 & 5: MCP3428 ADCs (I2C0) ---- */
    i2c_init(i2c0, I2C0_BAUD);
    gpio_set_function(I2C0_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C0_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C0_SDA_PIN);
    gpio_pull_up(I2C0_SCL_PIN);

    r = i2c_probe(i2c0, ADDR_ADC_PSU) ? RESULT_PASS : RESULT_NONE;
    printf("[COMMS] ADC-PSU: %s\n", r == RESULT_PASS ? "PASS" : "NONE");
    record("ADC-PSU:", r);

    r = i2c_probe(i2c0, ADDR_ADC_EXT) ? RESULT_PASS : RESULT_NONE;
    printf("[COMMS] ADC-EXT: %s\n", r == RESULT_PASS ? "PASS" : "NONE");
    record("ADC-EXT:", r);

    /* ---- Stage 6: MCP48FVB02T DAC (SPI1) ---- */
    r = test_dac();
    printf("[COMMS] DAC: %s\n",
           r == RESULT_PASS ? "PASS" : r == RESULT_FAIL ? "FAIL" : "NONE");
    record("DAC:    ", r);

    /* ---- Display and loop ---- */
    if (oled_ok) draw_screen();

    uint32_t loop = 0;
    while (true) {
        sleep_ms(5000);
        if (oled_ok) draw_screen();

        printf("[COMMS] loop=%lu", loop++);
        for (int i = 0; i < result_count; i++) {
            printf("  %s%s", result_label[i],
                   result_value[i] == RESULT_PASS ? "PASS" :
                   result_value[i] == RESULT_FAIL ? "FAIL" : "NONE");
        }
        printf("\n");
    }
}
