/**
 * UVR1611 DL-Bus Reader - WiFi Manager
 *
 * Manages WiFi connectivity with STA/AP mode switching:
 * - Tries STA mode with stored credentials on boot
 * - Falls back to AP mode if no credentials or connection fails
 * - Provides mDNS for hostname.local access
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include "esp_err.h"
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Constants
// =============================================================================

#define WIFI_AP_MAX_CONNECTIONS     4
#define WIFI_AP_CHANNEL             1
#define WIFI_AP_SSID_MAX_LEN        32
#define WIFI_AP_IP_ADDR             "192.168.4.1"

// =============================================================================
// Initialization
// =============================================================================

/**
 * Initialize WiFi Manager
 *
 * Initializes WiFi stack, event loop, and netif.
 * Must be called once before wifi_manager_start().
 *
 * @return ESP_OK on success
 */
esp_err_t wifi_manager_init(void);

/**
 * Start WiFi connection
 *
 * Attempts STA mode connection with stored credentials.
 * Falls back to AP mode if:
 * - No WiFi credentials configured
 * - Connection fails after timeout
 *
 * @return ESP_OK on success (either STA connected or AP started)
 */
esp_err_t wifi_manager_start(void);

// =============================================================================
// Status Functions
// =============================================================================

/**
 * Check if connected to WiFi (STA mode)
 * @return true if connected in STA mode
 */
bool wifi_manager_is_connected(void);

/**
 * Check if in AP mode
 * @return true if in AP mode
 */
bool wifi_manager_is_ap_mode(void);

/**
 * Get current WiFi mode
 * @return WIFI_MODE_STA, WIFI_MODE_AP, or WIFI_MODE_NULL
 */
wifi_mode_t wifi_manager_get_mode(void);

/**
 * Get current IP address
 * @param buf Output buffer (at least 16 bytes)
 * @param len Buffer length
 */
void wifi_manager_get_ip(char *buf, size_t len);

/**
 * Get AP mode SSID
 * @param buf Output buffer (at least WIFI_AP_SSID_MAX_LEN bytes)
 * @param len Buffer length
 */
void wifi_manager_get_ap_ssid(char *buf, size_t len);

/**
 * Get MAC address as string
 * @param buf Output buffer (at least 18 bytes for "AA:BB:CC:DD:EE:FF")
 * @param len Buffer length
 */
void wifi_manager_get_mac(char *buf, size_t len);

// =============================================================================
// Control Functions
// =============================================================================

/**
 * Trigger reconnection attempt
 *
 * If in AP mode with valid credentials, attempts STA connection.
 * Use after saving new WiFi credentials.
 *
 * @return ESP_OK on success
 */
esp_err_t wifi_manager_reconnect(void);

/**
 * Force switch to AP mode
 *
 * Disconnects from STA and starts AP mode.
 *
 * @return ESP_OK on success
 */
esp_err_t wifi_manager_start_ap(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_MANAGER_H */
