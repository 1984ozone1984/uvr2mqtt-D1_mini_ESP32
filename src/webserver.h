/**
 * UVR1611 DL-Bus Reader - Web Configuration Server
 *
 * Provides HTTP interface for:
 * - Status page with sensor/output data
 * - Configuration forms for WiFi, MQTT, and device settings
 * - REST API for status and configuration
 */

#ifndef WEBSERVER_H
#define WEBSERVER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Initialization
// =============================================================================

/**
 * Start the HTTP web server
 *
 * Starts HTTP server on port 80 with all endpoints registered.
 * Works in both STA and AP modes.
 *
 * @return ESP_OK on success
 */
esp_err_t webserver_start(void);

/**
 * Stop the HTTP web server
 *
 * @return ESP_OK on success
 */
esp_err_t webserver_stop(void);

/**
 * Check if webserver is running
 * @return true if running
 */
bool webserver_is_running(void);

#ifdef __cplusplus
}
#endif

#endif /* WEBSERVER_H */
