/*
 * hello_world.c — minimal HTTP server for initial W5500 hardware validation.
 *
 * Hardcoded to 172.22.77.77/16.  No flash config, no watchdog — just proves
 * the W5500 SPI link is alive and HTTP works.  Browse to http://172.22.77.77/
 * after flashing.  USB serial (115200) prints status messages.
 *
 * Replace with the full thl_pwr_ctl target once hardware is verified.
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "socket.h"
#include "w5500_port.h"

#define HTTP_SOCK   0
#define HTTP_PORT   80

static const uint8_t MAC[]    = {0x00, 0x08, 0xDC, 0x77, 0x77, 0x01};
static const uint8_t IP[]     = {172, 22, 77, 77};
static const uint8_t SUBNET[] = {255, 255, 0, 0};
static const uint8_t GW[]     = {172, 22, 0, 1};
static const uint8_t DNS[]    = {8, 8, 8, 8};

static const char HTTP_RESPONSE[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html; charset=UTF-8\r\n"
    "Connection: close\r\n"
    "\r\n"
    "<!DOCTYPE html>"
    "<html><head>"
    "<meta charset=\"UTF-8\">"
    "<title>THL Power Control</title>"
    "<style>"
    "body{font-family:sans-serif;display:flex;justify-content:center;"
    "align-items:center;height:100vh;margin:0;background:#f0f4f8}"
    ".card{background:#fff;padding:40px 48px;border-radius:10px;"
    "box-shadow:0 4px 16px rgba(0,0,0,.12);text-align:center}"
    "h1{margin:0 0 8px;color:#1a73e8;font-size:1.8em}"
    "p{margin:4px 0;color:#555}"
    ".badge{display:inline-block;margin-top:16px;padding:6px 16px;"
    "background:#e8f5e9;color:#256029;border-radius:20px;font-size:.85em}"
    "</style>"
    "</head><body>"
    "<div class=\"card\">"
    "<h1>THL Power Control</h1>"
    "<p>W5500 Ethernet &mdash; Hello World</p>"
    "<p><strong>172.22.77.77</strong> &mdash; port 80</p>"
    "<div class=\"badge\">&#x2714; Network stack OK</div>"
    "</div>"
    "</body></html>";

int main(void)
{
    stdio_init_all();
    sleep_ms(2000);   /* wait for USB serial to enumerate */

    printf("THL Power Control — hello_world\n");
    printf("Initialising W5500...\n");

    w5500_port_init(MAC, IP, SUBNET, GW, DNS);

    printf("W5500 ready.  Browse to http://%d.%d.%d.%d/\n",
           IP[0], IP[1], IP[2], IP[3]);

    uint32_t req_count = 0;

    while (1) {
        switch (getSn_SR(HTTP_SOCK)) {

        case SOCK_CLOSED:
            socket(HTTP_SOCK, Sn_MR_TCP, HTTP_PORT, 0);
            break;

        case SOCK_INIT:
            listen(HTTP_SOCK);
            printf("Listening on port %d...\n", HTTP_PORT);
            break;

        case SOCK_LISTEN:
            break;

        case SOCK_ESTABLISHED: {
            uint8_t buf[256];
            int32_t n = recv(HTTP_SOCK, buf, sizeof(buf) - 1);
            if (n > 0) {
                req_count++;
                printf("Request #%lu received (%ld bytes)\n",
                       (unsigned long)req_count, (long)n);
                send(HTTP_SOCK,
                     (uint8_t *)HTTP_RESPONSE,
                     sizeof(HTTP_RESPONSE) - 1);   /* -1: exclude null terminator */
                disconnect(HTTP_SOCK);
            }
            break;
        }

        case SOCK_CLOSE_WAIT:
            disconnect(HTTP_SOCK);
            break;

        default:
            break;
        }
    }
}
