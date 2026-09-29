#pragma once

#include <stdbool.h>

/**
 * Create application FreeRTOS tasks.
 *
 * Called once, after Ethernet is up and after a pending OTA image has
 * been confirmed. Add tasks here. Leave the Ethernet and OTA components
 * alone unless the board wiring changes.
 */
void user_app_start(void);

/**
 * Extra check for the first boot of an OTA image.
 *
 * Return false to roll back to the previous application. The boilerplate
 * already requires the Ethernet driver to have started, and an address
 * when the link is up. This hook is for product-specific checks. The
 * sample returns true.
 */
bool user_app_diagnostics(void);
