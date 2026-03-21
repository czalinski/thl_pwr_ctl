#pragma once

#include <stdbool.h>
#include "config.h"

/**
 * Prepare the HTTP server.  The listening socket is opened on the first call
 * to http_server_poll(), so this function is a no-op placeholder that exists
 * to allow future initialisation work without changing main().
 *
 * Returns 0 on success (always, currently).
 */
int http_server_init(void);

/**
 * Drive one iteration of the HTTP server state machine.
 * Must be called repeatedly from the main loop.
 *
 * @param cfg  Pointer to the live configuration.  May be updated and saved to
 *             flash if the operator submits a new configuration via the browser.
 *
 * @return true  A configuration save was performed and the device is about to
 *               reboot (caller should not rely on any further state after this).
 *         false Normal poll; no reboot pending.
 */
bool http_server_poll(device_config_t *cfg);
