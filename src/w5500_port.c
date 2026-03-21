#include "w5500_port.h"

#include <string.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "wizchip_conf.h"

/* SPI bus and pin assignments for the W5500-EVB-Pico */
#define W5500_SPI        spi0
#define W5500_SPI_HZ     (33u * 1000u * 1000u)   /* 33 MHz (W5500 max: 80 MHz) */

#define W5500_PIN_MISO   16
#define W5500_PIN_CS     17
#define W5500_PIN_SCK    18
#define W5500_PIN_MOSI   19
#define W5500_PIN_RST    20
#define W5500_PIN_INT    21   /* reserved for future interrupt-driven use */

/* ---------------------------------------------------------------------------
 * ioLibrary_Driver SPI callbacks
 * --------------------------------------------------------------------------- */
static void cs_select(void)   { gpio_put(W5500_PIN_CS, 0); }
static void cs_deselect(void) { gpio_put(W5500_PIN_CS, 1); }

static uint8_t spi_read_byte(void)
{
    uint8_t rx;
    spi_read_blocking(W5500_SPI, 0xFF, &rx, 1);
    return rx;
}

static void spi_write_byte(uint8_t tx)
{
    spi_write_blocking(W5500_SPI, &tx, 1);
}

static void spi_read_burst(uint8_t *buf, uint16_t len)
{
    spi_read_blocking(W5500_SPI, 0xFF, buf, len);
}

static void spi_write_burst(uint8_t *buf, uint16_t len)
{
    spi_write_blocking(W5500_SPI, buf, len);
}

/* ---------------------------------------------------------------------------
 * Public init
 * --------------------------------------------------------------------------- */
void w5500_port_init(const uint8_t mac[6],
                     const uint8_t ip[4],
                     const uint8_t subnet[4],
                     const uint8_t gateway[4],
                     const uint8_t dns[4])
{
    /* --- SPI hardware init --- */
    spi_init(W5500_SPI, W5500_SPI_HZ);
    gpio_set_function(W5500_PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(W5500_PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(W5500_PIN_MOSI, GPIO_FUNC_SPI);

    gpio_init(W5500_PIN_CS);
    gpio_set_dir(W5500_PIN_CS, GPIO_OUT);
    gpio_put(W5500_PIN_CS, 1);   /* deselect */

    /* --- Hardware reset of W5500 --- */
    gpio_init(W5500_PIN_RST);
    gpio_set_dir(W5500_PIN_RST, GPIO_OUT);
    gpio_put(W5500_PIN_RST, 0);
    sleep_ms(10);
    gpio_put(W5500_PIN_RST, 1);
    sleep_ms(100);   /* W5500 PLL lock time */

    /* --- Register ioLibrary callbacks --- */
    reg_wizchip_cris_cbfunc(NULL, NULL);   /* no critical-section wrapper needed */
    reg_wizchip_cs_cbfunc(cs_select, cs_deselect);
    reg_wizchip_spi_cbfunc(spi_read_byte, spi_write_byte);
    reg_wizchip_spiburst_cbfunc(spi_read_burst, spi_write_burst);

    /* --- Initialise W5500 socket buffers (2 KB each × 8 sockets = 16 KB RX+TX) --- */
    uint8_t rx_sizes[8] = {2, 2, 2, 2, 2, 2, 2, 2};
    uint8_t tx_sizes[8] = {2, 2, 2, 2, 2, 2, 2, 2};
    wizchip_init(tx_sizes, rx_sizes);

    /* --- Apply network configuration --- */
    wiz_NetInfo net;
    memset(&net, 0, sizeof(net));
    memcpy(net.mac, mac,     6);
    memcpy(net.ip,  ip,      4);
    memcpy(net.sn,  subnet,  4);
    memcpy(net.gw,  gateway, 4);
    memcpy(net.dns, dns,     4);
    net.dhcp = NETINFO_STATIC;
    wizchip_setnetinfo(&net);
}
