/**
 * UVR1611 DL-Bus Reader - Configuration Storage Abstraction
 *
 * Provides secure, NVS-backed configuration storage with:
 * - Non-secret config: Kconfig defaults with NVS override
 * - Secret config: NVS only (WiFi/MQTT credentials)
 *
 * Security guarantees:
 * - Secrets NEVER appear in source code, headers, or logs
 * - Secrets exist only in NVS at runtime
 * - All secret getters copy to caller-provided buffers
 */

#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Constants
// =============================================================================

// Maximum lengths for configuration values (including null terminator)
#define CONFIG_HOSTNAME_MAX_LEN         32
#define CONFIG_WIFI_SSID_MAX_LEN        33   // 802.11 max SSID = 32 bytes
#define CONFIG_WIFI_PASSWORD_MAX_LEN    64
#define CONFIG_MQTT_URI_MAX_LEN         128
#define CONFIG_MQTT_USERNAME_MAX_LEN    64
#define CONFIG_MQTT_PASSWORD_MAX_LEN    64
#define CONFIG_MQTT_CLIENT_ID_MAX_LEN   32
#define CONFIG_MQTT_TOPIC_MAX_LEN       64

// Current configuration version (increment on schema changes)
#define CONFIG_VERSION                  1

// =============================================================================
// Initialization
// =============================================================================

/**
 * Initialize the configuration store
 *
 * Must be called once at startup before any other config functions.
 * Initializes NVS, loads defaults from Kconfig, applies NVS overrides.
 *
 * @return ESP_OK on success, error code on failure
 */
esp_err_t config_store_init(void);

/**
 * Check if configuration store is initialized
 *
 * @return true if initialized, false otherwise
 */
bool config_store_is_initialized(void);

// =============================================================================
// Non-Secret Configuration (Kconfig defaults with NVS override)
// =============================================================================

// --- Device Settings ---

/**
 * Get device hostname
 * @return Hostname string (valid until next set or reboot)
 */
const char *config_get_hostname(void);

/**
 * Set device hostname
 * @param hostname New hostname (max CONFIG_HOSTNAME_MAX_LEN-1 chars)
 * @return ESP_OK on success
 */
esp_err_t config_set_hostname(const char *hostname);

// --- MQTT Settings (non-secret) ---

/**
 * Get MQTT broker URI
 * @return URI string (e.g., "mqtt://192.168.1.100:1883")
 */
const char *config_get_mqtt_broker_uri(void);

/**
 * Set MQTT broker URI
 * @param uri New broker URI
 * @return ESP_OK on success
 */
esp_err_t config_set_mqtt_broker_uri(const char *uri);

/**
 * Get MQTT client ID
 * @return Client ID string
 */
const char *config_get_mqtt_client_id(void);

/**
 * Set MQTT client ID
 * @param client_id New client ID
 * @return ESP_OK on success
 */
esp_err_t config_set_mqtt_client_id(const char *client_id);

/**
 * Get MQTT base topic
 * @return Base topic string (e.g., "uvr1611")
 */
const char *config_get_mqtt_base_topic(void);

/**
 * Set MQTT base topic
 * @param topic New base topic
 * @return ESP_OK on success
 */
esp_err_t config_set_mqtt_base_topic(const char *topic);

/**
 * Get Home Assistant discovery prefix
 * @return Discovery prefix string (e.g., "homeassistant")
 */
const char *config_get_ha_discovery_prefix(void);

/**
 * Set Home Assistant discovery prefix
 * @param prefix New discovery prefix
 * @return ESP_OK on success
 */
esp_err_t config_set_ha_discovery_prefix(const char *prefix);

// --- Timing Settings ---

/**
 * Get sensor publish interval in seconds
 * @return Interval in seconds
 */
uint16_t config_get_publish_interval_sensors(void);

/**
 * Set sensor publish interval
 * @param seconds Interval in seconds (1-3600)
 * @return ESP_OK on success
 */
esp_err_t config_set_publish_interval_sensors(uint16_t seconds);

/**
 * Get output publish interval in seconds
 * @return Interval in seconds
 */
uint16_t config_get_publish_interval_outputs(void);

/**
 * Set output publish interval
 * @param seconds Interval in seconds (1-3600)
 * @return ESP_OK on success
 */
esp_err_t config_set_publish_interval_outputs(uint16_t seconds);

/**
 * Get system info publish interval in seconds
 * @return Interval in seconds
 */
uint16_t config_get_publish_interval_system(void);

/**
 * Set system info publish interval
 * @param seconds Interval in seconds (1-3600)
 * @return ESP_OK on success
 */
esp_err_t config_set_publish_interval_system(uint16_t seconds);

/**
 * Get HA discovery republish interval in seconds
 * @return Interval in seconds (0 = only on startup)
 */
uint16_t config_get_ha_discovery_interval(void);

/**
 * Set HA discovery republish interval
 * @param seconds Interval in seconds (0 = only on startup)
 * @return ESP_OK on success
 */
esp_err_t config_set_ha_discovery_interval(uint16_t seconds);

/**
 * Get WiFi connection timeout in seconds
 * @return Timeout in seconds
 */
uint16_t config_get_wifi_timeout(void);

/**
 * Set WiFi connection timeout
 * @param seconds Timeout in seconds (5-120)
 * @return ESP_OK on success
 */
esp_err_t config_set_wifi_timeout(uint16_t seconds);

// --- Hardware Settings ---

/**
 * Get DL-Bus GPIO pin number
 * @return GPIO number
 */
uint8_t config_get_dlbus_gpio(void);

/**
 * Set DL-Bus GPIO pin number
 * @param gpio GPIO number
 * @return ESP_OK on success
 */
esp_err_t config_set_dlbus_gpio(uint8_t gpio);

// --- MQTT Advanced Settings ---

/**
 * Get MQTT keepalive interval in seconds
 * @return Keepalive interval
 */
uint16_t config_get_mqtt_keepalive(void);

/**
 * Get MQTT QoS level
 * @return QoS level (0, 1, or 2)
 */
uint8_t config_get_mqtt_qos(void);

/**
 * Get MQTT retain flag for sensors
 * @return true if retain enabled
 */
bool config_get_mqtt_retain_sensors(void);

/**
 * Get MQTT retain flag for status
 * @return true if retain enabled
 */
bool config_get_mqtt_retain_status(void);

// =============================================================================
// Secret Configuration (NVS only - NEVER in source code or logs)
// =============================================================================

/**
 * Check if WiFi credentials are configured
 * @return true if SSID is set (password may be empty for open networks)
 */
bool config_has_wifi_credentials(void);

/**
 * Get WiFi SSID
 *
 * Copies SSID to caller-provided buffer. Never returns internal pointer.
 *
 * @param buffer Output buffer (must be at least CONFIG_WIFI_SSID_MAX_LEN bytes)
 * @param buffer_len Size of output buffer
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not configured
 */
esp_err_t config_get_wifi_ssid(char *buffer, size_t buffer_len);

/**
 * Get WiFi password
 *
 * Copies password to caller-provided buffer. Never returns internal pointer.
 *
 * @param buffer Output buffer (must be at least CONFIG_WIFI_PASSWORD_MAX_LEN bytes)
 * @param buffer_len Size of output buffer
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not configured
 */
esp_err_t config_get_wifi_password(char *buffer, size_t buffer_len);

/**
 * Set WiFi credentials
 *
 * Stores SSID and password in NVS. Password may be empty for open networks.
 *
 * @param ssid WiFi SSID (required, max 32 chars)
 * @param password WiFi password (may be empty, max 63 chars)
 * @return ESP_OK on success
 */
esp_err_t config_set_wifi_credentials(const char *ssid, const char *password);

/**
 * Clear WiFi credentials from NVS
 * @return ESP_OK on success
 */
esp_err_t config_clear_wifi_credentials(void);

/**
 * Check if MQTT credentials are configured
 * @return true if username is set
 */
bool config_has_mqtt_credentials(void);

/**
 * Get MQTT username
 *
 * Copies username to caller-provided buffer.
 *
 * @param buffer Output buffer (must be at least CONFIG_MQTT_USERNAME_MAX_LEN bytes)
 * @param buffer_len Size of output buffer
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not configured
 */
esp_err_t config_get_mqtt_username(char *buffer, size_t buffer_len);

/**
 * Get MQTT password
 *
 * Copies password to caller-provided buffer.
 *
 * @param buffer Output buffer (must be at least CONFIG_MQTT_PASSWORD_MAX_LEN bytes)
 * @param buffer_len Size of output buffer
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not configured
 */
esp_err_t config_get_mqtt_password(char *buffer, size_t buffer_len);

/**
 * Set MQTT credentials
 *
 * Stores username and password in NVS.
 *
 * @param username MQTT username (required)
 * @param password MQTT password (may be empty)
 * @return ESP_OK on success
 */
esp_err_t config_set_mqtt_credentials(const char *username, const char *password);

/**
 * Clear MQTT credentials from NVS
 * @return ESP_OK on success
 */
esp_err_t config_clear_mqtt_credentials(void);

// =============================================================================
// Factory Reset
// =============================================================================

/**
 * Reset all configuration to defaults
 *
 * - Non-secrets: Reset to Kconfig defaults
 * - Secrets: Cleared from NVS
 *
 * @return ESP_OK on success
 */
esp_err_t config_factory_reset(void);

/**
 * Reset only non-secret configuration to Kconfig defaults
 * @return ESP_OK on success
 */
esp_err_t config_reset_nonsecrets(void);

// =============================================================================
// Versioning
// =============================================================================

/**
 * Get stored configuration version
 * @return Version number (0 if never saved)
 */
uint8_t config_get_version(void);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_STORE_H */
