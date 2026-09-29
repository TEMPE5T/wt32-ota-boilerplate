#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

/** Set once a DHCP lease or the failsafe static address is installed. */
#define WT32_IP_READY_BIT BIT0
/** Set after the EMAC driver and the Ethernet netif have started. */
#define WT32_ETH_READY_BIT BIT1
/** Set while the PHY reports link up. Cleared on disconnect. */
#define WT32_LINK_UP_BIT BIT2

/**
 * Enable the LAN8720 oscillator, install the ESP32 EMAC, and assign an address.
 *
 * DHCP runs first when WT32_USE_DHCP is set. The static address from
 * menuconfig is applied when DHCP is off or when the lease wait expires.
 * An unparseable static address is logged and is not installed; the
 * function still returns ESP_OK if the driver itself started, so a bad
 * failsafe does not reboot-loop a factory image.
 */
esp_err_t wt32_eth_start(void);

/** Event group carrying WT32_IP_READY_BIT, WT32_ETH_READY_BIT, WT32_LINK_UP_BIT. */
EventGroupHandle_t wt32_eth_events(void);

bool wt32_eth_driver_ready(void);
bool wt32_eth_link_is_up(void);
bool wt32_eth_has_ip(void);

/** "dhcp", "static", or "none". */
const char *wt32_eth_addr_mode(void);

/** Writes the current IPv4 address, or "0.0.0.0" when none is installed. */
esp_err_t wt32_eth_get_ip_str(char *buf, size_t len);
