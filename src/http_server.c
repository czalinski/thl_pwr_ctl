#include "http_server.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "socket.h"
#include "hardware/watchdog.h"

#define HTTP_SOCKET     0
#define HTTP_PORT       80

#define RECV_BUF_SIZE   2048
#define SEND_BUF_SIZE   2048

static uint8_t rx_buf[RECV_BUF_SIZE];
static uint8_t tx_buf[SEND_BUF_SIZE];

/* ---------------------------------------------------------------------------
 * Embedded HTML configuration page
 *
 * Format specifiers (in order):
 *   %s  — status banner HTML (empty string when no message)
 *   %s  — current IP address string
 *   %s  — current subnet mask string
 *   %s  — current gateway string
 *   %s  — current DNS string
 *
 * Note: %% in C format strings produces a literal '%' in output (used for CSS).
 * --------------------------------------------------------------------------- */
static const char HTML_FMT[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>THL Power Control</title>"
    "<style>"
    "body{font-family:sans-serif;max-width:420px;margin:40px auto;padding:16px}"
    "h1{font-size:1.2em;margin-bottom:4px}"
    "h2{font-size:.9em;color:#666;margin-top:0;font-weight:normal}"
    "label{display:block;margin:10px 0 3px;font-weight:bold;font-size:.9em}"
    "input[type=text]{width:100%%;padding:7px;border:1px solid #ccc;"
    "border-radius:3px;box-sizing:border-box;font-size:1em}"
    ".sb{margin-top:14px;padding:9px 22px;border:none;border-radius:3px;"
    "cursor:pointer;font-size:1em}"
    ".save{background:#1a73e8;color:#fff}"
    ".rst{background:#d93025;color:#fff}"
    ".ok{background:#e8f5e9;color:#256029;padding:10px;border-radius:3px;"
    "margin-bottom:14px;font-size:.9em}"
    "hr{border:0;border-top:1px solid #eee;margin:18px 0}"
    "</style></head><body>"
    "<h1>THL Power Control</h1>"
    "<h2>Network Configuration</h2>"
    "%s"
    "<form method=\"POST\" action=\"/config\">"
    "<label>IP Address</label>"
    "<input type=\"text\" name=\"ip\" value=\"%s\">"
    "<label>Subnet Mask</label>"
    "<input type=\"text\" name=\"subnet\" value=\"%s\">"
    "<label>Default Gateway</label>"
    "<input type=\"text\" name=\"gw\" value=\"%s\">"
    "<label>DNS Server</label>"
    "<input type=\"text\" name=\"dns\" value=\"%s\">"
    "<br><input type=\"submit\" class=\"sb save\" value=\"Save &amp; Reboot\">"
    "</form>"
    "<hr>"
    "<form method=\"POST\" action=\"/reset\">"
    "<input type=\"submit\" class=\"sb rst\" value=\"Factory Reset\""
    " onclick=\"return confirm('Reset all settings to factory defaults?')\">"
    "</form>"
    "</body></html>";

/* ---------------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------------- */

static void ip_to_str(const uint8_t ip[4], char *buf, size_t len)
{
    snprintf(buf, len, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
}

/** Extract the value for 'key' from a URL-encoded form body into out[outlen]. */
static bool parse_form_value(const char *body, const char *key,
                              char *out, size_t outlen)
{
    char search[32];
    snprintf(search, sizeof(search), "%s=", key);
    const char *p = strstr(body, search);
    if (!p) return false;
    p += strlen(search);
    size_t n = 0;
    while (*p && *p != '&' && n < outlen - 1)
        out[n++] = *p++;
    out[n] = '\0';
    return n > 0;
}

/** Parse a dotted-decimal string like "192.168.1.1" into ip[4]. */
static bool parse_ip_str(const char *str, uint8_t ip[4])
{
    int a, b, c, d;
    if (sscanf(str, "%d.%d.%d.%d", &a, &b, &c, &d) != 4)
        return false;
    if ((unsigned)a > 255 || (unsigned)b > 255 ||
        (unsigned)c > 255 || (unsigned)d > 255)
        return false;
    ip[0] = (uint8_t)a; ip[1] = (uint8_t)b;
    ip[2] = (uint8_t)c; ip[3] = (uint8_t)d;
    return true;
}

/** Reboot the device via the watchdog after a short delay. */
static void reboot_device(void)
{
    watchdog_enable(100, 1);   /* fire in 100 ms */
    while (1) tight_loop_contents();
}

/* ---------------------------------------------------------------------------
 * HTTP response builders
 * --------------------------------------------------------------------------- */

static int32_t send_redirect(uint8_t sn, const char *location)
{
    int n = snprintf((char *)tx_buf, SEND_BUF_SIZE,
        "HTTP/1.1 303 See Other\r\n"
        "Location: %s\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n",
        location);
    return send(sn, tx_buf, (uint16_t)n);
}

static int32_t send_html_page(uint8_t sn, const device_config_t *cfg,
                               const char *status_html)
{
    char ip_s[16], sn_s[16], gw_s[16], dns_s[16];
    ip_to_str(cfg->ip,      ip_s,  sizeof(ip_s));
    ip_to_str(cfg->subnet,  sn_s,  sizeof(sn_s));
    ip_to_str(cfg->gateway, gw_s,  sizeof(gw_s));
    ip_to_str(cfg->dns,     dns_s, sizeof(dns_s));

    char body[SEND_BUF_SIZE];
    int body_len = snprintf(body, sizeof(body), HTML_FMT,
                            status_html, ip_s, sn_s, gw_s, dns_s);

    int hdr_len = snprintf((char *)tx_buf, SEND_BUF_SIZE,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=UTF-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        body_len);

    /* Send headers then body in two calls to avoid a large intermediate copy */
    int32_t r = send(sn, tx_buf, (uint16_t)hdr_len);
    if (r < 0) return r;
    return send(sn, (uint8_t *)body, (uint16_t)body_len);
}

static int32_t send_not_found(uint8_t sn)
{
    int n = snprintf((char *)tx_buf, SEND_BUF_SIZE,
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 9\r\n"
        "Connection: close\r\n"
        "\r\n"
        "Not Found");
    return send(sn, tx_buf, (uint16_t)n);
}

/* ---------------------------------------------------------------------------
 * Request handler — returns true if a reboot is needed
 * --------------------------------------------------------------------------- */
static bool handle_request(const char *req, int32_t req_len, device_config_t *cfg)
{
    (void)req_len;

    /* --- Parse method and path from request line --- */
    char method[8] = {0};
    char path[64]  = {0};
    sscanf(req, "%7s %63s", method, path);

    /* Strip query string from path (e.g. "/?saved=1" → "/") */
    char *q = strchr(path, '?');
    const char *query = q ? q + 1 : "";
    if (q) *q = '\0';

    bool is_get  = (strcmp(method, "GET")  == 0);
    bool is_post = (strcmp(method, "POST") == 0);

    /* --- Locate POST body (after \r\n\r\n) --- */
    const char *body = "";
    const char *end_of_headers = strstr(req, "\r\n\r\n");
    if (end_of_headers)
        body = end_of_headers + 4;

    /* ---- Route ---- */

    if (is_get && strcmp(path, "/") == 0) {
        const char *status = "";
        char ok_buf[80] = {0};
        if (strstr(query, "saved=1")) {
            snprintf(ok_buf, sizeof(ok_buf),
                     "<div class=\"ok\">Settings saved. Rebooting&hellip;</div>");
            status = ok_buf;
        } else if (strstr(query, "reset=1")) {
            snprintf(ok_buf, sizeof(ok_buf),
                     "<div class=\"ok\">Factory reset complete.</div>");
            status = ok_buf;
        }
        send_html_page(HTTP_SOCKET, cfg, status);
        disconnect(HTTP_SOCKET);
        return false;
    }

    if (is_post && strcmp(path, "/config") == 0) {
        char ip_s[20], sn_s[20], gw_s[20], dns_s[20];
        bool ok =
            parse_form_value(body, "ip",     ip_s,  sizeof(ip_s))  &&
            parse_form_value(body, "subnet", sn_s,  sizeof(sn_s))  &&
            parse_form_value(body, "gw",     gw_s,  sizeof(gw_s))  &&
            parse_form_value(body, "dns",    dns_s, sizeof(dns_s));

        device_config_t new_cfg;
        memcpy(&new_cfg, cfg, sizeof(new_cfg));
        ok = ok &&
             parse_ip_str(ip_s,  new_cfg.ip)      &&
             parse_ip_str(sn_s,  new_cfg.subnet)  &&
             parse_ip_str(gw_s,  new_cfg.gateway) &&
             parse_ip_str(dns_s, new_cfg.dns);

        if (ok) {
            config_save(&new_cfg);
            memcpy(cfg, &new_cfg, sizeof(*cfg));
            send_redirect(HTTP_SOCKET, "/?saved=1");
            disconnect(HTTP_SOCKET);
            return true;   /* caller should reboot */
        } else {
            /* Bad form data — redisplay the form */
            send_html_page(HTTP_SOCKET, cfg,
                "<div class=\"ok\" style=\"background:#fce8e6;color:#c62828\">"
                "Invalid input. Please use dotted-decimal notation (e.g. 198.168.77.77)."
                "</div>");
            disconnect(HTTP_SOCKET);
            return false;
        }
    }

    if (is_post && strcmp(path, "/reset") == 0) {
        config_factory_defaults(cfg);
        config_save(cfg);
        send_redirect(HTTP_SOCKET, "/?reset=1");
        disconnect(HTTP_SOCKET);
        return true;   /* caller should reboot */
    }

    send_not_found(HTTP_SOCKET);
    disconnect(HTTP_SOCKET);
    return false;
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

int http_server_init(void)
{
    return 0;   /* socket lifecycle is managed in http_server_poll() */
}

bool http_server_poll(device_config_t *cfg)
{
    bool reboot_needed = false;

    switch (getSn_SR(HTTP_SOCKET)) {

    case SOCK_CLOSED:
        socket(HTTP_SOCKET, Sn_MR_TCP, HTTP_PORT, 0);
        break;

    case SOCK_INIT:
        listen(HTTP_SOCKET);
        break;

    case SOCK_LISTEN:
        break;   /* waiting for a client */

    case SOCK_ESTABLISHED: {
        /*
         * Accumulate the incoming request.  For a browser on the same LAN
         * the full request (headers + small POST body) typically arrives in
         * a single TCP segment, but we loop briefly to handle any
         * fragmentation.
         */
        if (getSn_RX_RSR(HTTP_SOCKET) == 0)
            break;   /* no data yet */

        int32_t total = 0;
        int32_t n;
        while (total < (int32_t)(RECV_BUF_SIZE - 1)) {
            n = recv(HTTP_SOCKET, rx_buf + total,
                     (uint16_t)(RECV_BUF_SIZE - 1 - total));
            if (n > 0) {
                total += n;
                rx_buf[total] = '\0';
                if (strstr((char *)rx_buf, "\r\n\r\n"))
                    break;   /* end of HTTP headers reached */
            } else {
                break;
            }
        }

        if (total > 0)
            reboot_needed = handle_request((char *)rx_buf, total, cfg);
        break;
    }

    case SOCK_CLOSE_WAIT:
        disconnect(HTTP_SOCKET);
        break;

    default:
        break;
    }

    /* If a save or reset was performed, wait for the socket to close then reboot */
    if (reboot_needed) {
        uint32_t deadline = to_ms_since_boot(get_absolute_time()) + 500;
        while (getSn_SR(HTTP_SOCKET) != SOCK_CLOSED) {
            if (to_ms_since_boot(get_absolute_time()) > deadline) break;
        }
        reboot_device();
    }

    return reboot_needed;
}
