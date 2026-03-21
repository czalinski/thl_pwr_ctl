#include <stdio.h>
#include "pico/stdlib.h"
#include "config.h"
#include "w5500_port.h"
#include "http_server.h"

int main(void)
{
    stdio_init_all();

    /* Brief delay to allow USB serial to enumerate before first printf */
    sleep_ms(2000);

    printf("THL Power Control — starting\n");

    /* Load network settings from flash (falls back to factory defaults) */
    device_config_t cfg;
    config_load(&cfg);

    printf("Network config: %d.%d.%d.%d / %d.%d.%d.%d  gw %d.%d.%d.%d\n",
           cfg.ip[0], cfg.ip[1], cfg.ip[2], cfg.ip[3],
           cfg.subnet[0], cfg.subnet[1], cfg.subnet[2], cfg.subnet[3],
           cfg.gateway[0], cfg.gateway[1], cfg.gateway[2], cfg.gateway[3]);

    /* Bring up W5500 with stored (or default) settings */
    w5500_port_init(cfg.mac, cfg.ip, cfg.subnet, cfg.gateway, cfg.dns);

    printf("W5500 initialised\n");

    http_server_init();

    printf("HTTP server listening on port %d\n", 80);

    while (1) {
        http_server_poll(&cfg);
        /* http_server_poll() does not return after a config save — it reboots. */
    }
}
