/**
 * UVR1611 DL-Bus Reader - Configuration Storage Implementation
 *
 * NVS-backed configuration with secure credential handling.
 *
 * Storage layout:
 * - Namespace "uvr_config": Non-secret settings
 * - Namespace "uvr_secret": WiFi/MQTT credentials (never logged)
 *
 * Security:
 * - Secrets are NEVER logged, printed, or returned as pointers
 * - All secret getters copy to caller-provided buffers
 * - Kconfig has NO defaults for secrets
 */

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "config_store.h"
#include "io_config.h"

static const char *TAG = "CONFIG";

// =============================================================================
// NVS Namespaces and Keys
// =============================================================================

#define NVS_NAMESPACE_CONFIG    "uvr_config"
#define NVS_NAMESPACE_SECRET    "uvr_secret"

// Non-secret keys
#define KEY_VERSION             "version"
#define KEY_HOSTNAME            "hostname"
#define KEY_MQTT_URI            "mqtt_uri"
#define KEY_MQTT_CLIENT_ID      "mqtt_client"
#define KEY_MQTT_BASE_TOPIC     "mqtt_topic"
#define KEY_HA_DISC_PREFIX      "ha_prefix"
#define KEY_INTERVAL_SENSORS    "int_sensors"
#define KEY_INTERVAL_OUTPUTS    "int_outputs"
#define KEY_INTERVAL_SYSTEM     "int_system"
#define KEY_INTERVAL_DISCOVERY  "int_disc"
#define KEY_WIFI_TIMEOUT        "wifi_timeout"
#define KEY_DLBUS_GPIO          "dlbus_gpio"
#define KEY_MQTT_KEEPALIVE      "mqtt_keep"
#define KEY_MQTT_QOS            "mqtt_qos"
#define KEY_MQTT_RETAIN_SENS    "mqtt_ret_s"
#define KEY_MQTT_RETAIN_STAT    "mqtt_ret_st"

// Secret keys (stored in separate namespace)
#define KEY_WIFI_SSID           "wifi_ssid"
#define KEY_WIFI_PASSWORD       "wifi_pass"
#define KEY_MQTT_USERNAME       "mqtt_user"
#define KEY_MQTT_PASSWORD       "mqtt_pass"

// I/O name key prefixes (sensor_0 through sensor_15, output_0 through output_12)
#define KEY_SENSOR_PREFIX       "sensor_"
#define KEY_OUTPUT_PREFIX       "output_"

// =============================================================================
// Runtime Configuration Storage
// =============================================================================

// Non-secret configuration (loaded at init, cached in RAM)
typedef struct {
    char hostname[CONFIG_HOSTNAME_MAX_LEN];
    char mqtt_broker_uri[CONFIG_MQTT_URI_MAX_LEN];
    char mqtt_client_id[CONFIG_MQTT_CLIENT_ID_MAX_LEN];
    char mqtt_base_topic[CONFIG_MQTT_TOPIC_MAX_LEN];
    char ha_discovery_prefix[CONFIG_MQTT_TOPIC_MAX_LEN];
    char sensor_names[CONFIG_NUM_SENSORS][CONFIG_IO_NAME_MAX_LEN];
    char output_names[CONFIG_NUM_OUTPUTS][CONFIG_IO_NAME_MAX_LEN];
    uint16_t interval_sensors;
    uint16_t interval_outputs;
    uint16_t interval_system;
    uint16_t interval_discovery;
    uint16_t wifi_timeout;
    uint16_t mqtt_keepalive;
    uint8_t dlbus_gpio;
    uint8_t mqtt_qos;
    bool mqtt_retain_sensors;
    bool mqtt_retain_status;
    uint8_t version;
} config_data_t;

static config_data_t s_config;
static bool s_initialized = false;
static SemaphoreHandle_t s_mutex = NULL;

// =============================================================================
// Internal Helpers
// =============================================================================

static void lock(void)
{
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_mutex) {
        xSemaphoreGive(s_mutex);
    }
}

/**
 * Load string from NVS, or use default if not found
 */
static void load_string(nvs_handle_t handle, const char *key,
                        char *dest, size_t dest_len, const char *default_val)
{
    size_t len = dest_len;
    esp_err_t err = nvs_get_str(handle, key, dest, &len);
    if (err != ESP_OK) {
        strncpy(dest, default_val, dest_len - 1);
        dest[dest_len - 1] = '\0';
    }
}

/**
 * Load uint16 from NVS, or use default if not found
 */
static uint16_t load_u16(nvs_handle_t handle, const char *key, uint16_t default_val)
{
    uint16_t value;
    if (nvs_get_u16(handle, key, &value) != ESP_OK) {
        return default_val;
    }
    return value;
}

/**
 * Load uint8 from NVS, or use default if not found
 */
static uint8_t load_u8(nvs_handle_t handle, const char *key, uint8_t default_val)
{
    uint8_t value;
    if (nvs_get_u8(handle, key, &value) != ESP_OK) {
        return default_val;
    }
    return value;
}

/**
 * Save string to NVS
 */
static esp_err_t save_string(const char *key, const char *value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_CONFIG, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

/**
 * Save uint16 to NVS
 */
static esp_err_t save_u16(const char *key, uint16_t value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_CONFIG, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u16(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

/**
 * Save uint8 to NVS
 */
static esp_err_t save_u8(const char *key, uint8_t value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_CONFIG, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

/**
 * Load defaults from Kconfig
 */
static void load_kconfig_defaults(void)
{
    // Device settings
    strncpy(s_config.hostname, CONFIG_UVR_HOSTNAME, sizeof(s_config.hostname) - 1);
    s_config.dlbus_gpio = CONFIG_UVR_DLBUS_GPIO;
    s_config.wifi_timeout = CONFIG_UVR_WIFI_TIMEOUT;

    // MQTT settings (non-secret)
    strncpy(s_config.mqtt_broker_uri, CONFIG_UVR_MQTT_BROKER_URI, sizeof(s_config.mqtt_broker_uri) - 1);
    strncpy(s_config.mqtt_client_id, CONFIG_UVR_MQTT_CLIENT_ID, sizeof(s_config.mqtt_client_id) - 1);
    strncpy(s_config.mqtt_base_topic, CONFIG_UVR_MQTT_BASE_TOPIC, sizeof(s_config.mqtt_base_topic) - 1);
    strncpy(s_config.ha_discovery_prefix, CONFIG_UVR_HA_DISCOVERY_PREFIX, sizeof(s_config.ha_discovery_prefix) - 1);
    s_config.mqtt_keepalive = CONFIG_UVR_MQTT_KEEPALIVE;
    s_config.mqtt_qos = CONFIG_UVR_MQTT_QOS;
#ifdef CONFIG_UVR_MQTT_RETAIN_SENSORS
    s_config.mqtt_retain_sensors = 1;
#else
    s_config.mqtt_retain_sensors = 0;
#endif
#ifdef CONFIG_UVR_MQTT_RETAIN_STATUS
    s_config.mqtt_retain_status = 1;
#else
    s_config.mqtt_retain_status = 0;
#endif

    // Publish intervals
    s_config.interval_sensors = CONFIG_UVR_PUBLISH_INTERVAL_SENSORS;
    s_config.interval_outputs = CONFIG_UVR_PUBLISH_INTERVAL_OUTPUTS;
    s_config.interval_system = CONFIG_UVR_PUBLISH_INTERVAL_SYSTEM;
    s_config.interval_discovery = CONFIG_UVR_HA_DISCOVERY_INTERVAL;

    // I/O names from Kconfig
    const char *kconfig_sensor_names[] = SENSOR_NAMES;
    const char *kconfig_output_names[] = OUTPUT_NAMES;

    for (int i = 0; i < CONFIG_NUM_SENSORS; i++) {
        strncpy(s_config.sensor_names[i], kconfig_sensor_names[i], CONFIG_IO_NAME_MAX_LEN - 1);
        s_config.sensor_names[i][CONFIG_IO_NAME_MAX_LEN - 1] = '\0';
    }

    for (int i = 0; i < CONFIG_NUM_OUTPUTS; i++) {
        strncpy(s_config.output_names[i], kconfig_output_names[i], CONFIG_IO_NAME_MAX_LEN - 1);
        s_config.output_names[i][CONFIG_IO_NAME_MAX_LEN - 1] = '\0';
    }

    s_config.version = CONFIG_VERSION;
}

/**
 * Load NVS overrides for non-secret config
 */
static void load_nvs_overrides(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_CONFIG, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No NVS config found, using Kconfig defaults");
        return;
    }

    // Check version
    uint8_t stored_version = load_u8(handle, KEY_VERSION, 0);
    if (stored_version != CONFIG_VERSION && stored_version != 0) {
        ESP_LOGW(TAG, "Config version mismatch (stored=%d, current=%d), using defaults",
                 stored_version, CONFIG_VERSION);
        nvs_close(handle);
        return;
    }

    // Load overrides (Kconfig defaults already set)
    load_string(handle, KEY_HOSTNAME, s_config.hostname, sizeof(s_config.hostname), s_config.hostname);
    load_string(handle, KEY_MQTT_URI, s_config.mqtt_broker_uri, sizeof(s_config.mqtt_broker_uri), s_config.mqtt_broker_uri);
    load_string(handle, KEY_MQTT_CLIENT_ID, s_config.mqtt_client_id, sizeof(s_config.mqtt_client_id), s_config.mqtt_client_id);
    load_string(handle, KEY_MQTT_BASE_TOPIC, s_config.mqtt_base_topic, sizeof(s_config.mqtt_base_topic), s_config.mqtt_base_topic);
    load_string(handle, KEY_HA_DISC_PREFIX, s_config.ha_discovery_prefix, sizeof(s_config.ha_discovery_prefix), s_config.ha_discovery_prefix);

    s_config.interval_sensors = load_u16(handle, KEY_INTERVAL_SENSORS, s_config.interval_sensors);
    s_config.interval_outputs = load_u16(handle, KEY_INTERVAL_OUTPUTS, s_config.interval_outputs);
    s_config.interval_system = load_u16(handle, KEY_INTERVAL_SYSTEM, s_config.interval_system);
    s_config.interval_discovery = load_u16(handle, KEY_INTERVAL_DISCOVERY, s_config.interval_discovery);
    s_config.wifi_timeout = load_u16(handle, KEY_WIFI_TIMEOUT, s_config.wifi_timeout);
    s_config.mqtt_keepalive = load_u16(handle, KEY_MQTT_KEEPALIVE, s_config.mqtt_keepalive);

    s_config.dlbus_gpio = load_u8(handle, KEY_DLBUS_GPIO, s_config.dlbus_gpio);
    s_config.mqtt_qos = load_u8(handle, KEY_MQTT_QOS, s_config.mqtt_qos);
    s_config.mqtt_retain_sensors = load_u8(handle, KEY_MQTT_RETAIN_SENS, s_config.mqtt_retain_sensors);
    s_config.mqtt_retain_status = load_u8(handle, KEY_MQTT_RETAIN_STAT, s_config.mqtt_retain_status);

    // Load I/O names from NVS (if stored)
    char key[16];
    for (int i = 0; i < CONFIG_NUM_SENSORS; i++) {
        snprintf(key, sizeof(key), "%s%d", KEY_SENSOR_PREFIX, i);
        load_string(handle, key, s_config.sensor_names[i], CONFIG_IO_NAME_MAX_LEN, s_config.sensor_names[i]);
    }
    for (int i = 0; i < CONFIG_NUM_OUTPUTS; i++) {
        snprintf(key, sizeof(key), "%s%d", KEY_OUTPUT_PREFIX, i);
        load_string(handle, key, s_config.output_names[i], CONFIG_IO_NAME_MAX_LEN, s_config.output_names[i]);
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "Loaded NVS config overrides");
}

// =============================================================================
// Secret Storage Helpers (NVS only, never logged)
// =============================================================================

/**
 * Get secret string from NVS
 * Copies to caller buffer, never returns internal pointer
 */
static esp_err_t get_secret(const char *key, char *buffer, size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_SECRET, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        buffer[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }

    size_t len = buffer_len;
    err = nvs_get_str(handle, key, buffer, &len);
    nvs_close(handle);

    if (err != ESP_OK) {
        buffer[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_OK;
}

/**
 * Set secret string in NVS
 * Never logs the value
 */
static esp_err_t set_secret(const char *key, const char *value)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_SECRET, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

/**
 * Check if secret exists in NVS
 */
static bool has_secret(const char *key)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_SECRET, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }

    size_t len = 0;
    err = nvs_get_str(handle, key, NULL, &len);
    nvs_close(handle);

    return (err == ESP_OK && len > 1);  // len includes null terminator
}

/**
 * Erase secret from NVS
 */
static esp_err_t erase_secret(const char *key)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_SECRET, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_key(handle, key);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_commit(handle);
        err = ESP_OK;
    }

    nvs_close(handle);
    return err;
}

// =============================================================================
// Public API - Initialization
// =============================================================================

esp_err_t config_store_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing configuration store");

    // Initialize NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition was truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return err;
    }

    // Create mutex for thread safety
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    // Initialize config structure
    memset(&s_config, 0, sizeof(s_config));

    // Load Kconfig defaults first
    load_kconfig_defaults();

    // Apply NVS overrides
    load_nvs_overrides();

    // Log non-secret config (NEVER log secrets)
    ESP_LOGI(TAG, "Config loaded: hostname=%s, gpio=%d, mqtt_uri=%s",
             s_config.hostname, s_config.dlbus_gpio, s_config.mqtt_broker_uri);
    ESP_LOGI(TAG, "Intervals: sensors=%ds, outputs=%ds, system=%ds, discovery=%ds",
             s_config.interval_sensors, s_config.interval_outputs,
             s_config.interval_system, s_config.interval_discovery);

    // Log credential status without revealing content
    if (config_has_wifi_credentials()) {
        ESP_LOGI(TAG, "WiFi credentials: configured");
    } else {
        ESP_LOGW(TAG, "WiFi credentials: NOT configured");
    }

    if (config_has_mqtt_credentials()) {
        ESP_LOGI(TAG, "MQTT credentials: configured");
    } else {
        ESP_LOGI(TAG, "MQTT credentials: not configured (anonymous)");
    }

    s_initialized = true;
    return ESP_OK;
}

bool config_store_is_initialized(void)
{
    return s_initialized;
}

// =============================================================================
// Public API - Non-Secret Getters
// =============================================================================

const char *config_get_hostname(void)
{
    return s_config.hostname;
}

const char *config_get_mqtt_broker_uri(void)
{
    return s_config.mqtt_broker_uri;
}

const char *config_get_mqtt_client_id(void)
{
    return s_config.mqtt_client_id;
}

const char *config_get_mqtt_base_topic(void)
{
    return s_config.mqtt_base_topic;
}

const char *config_get_ha_discovery_prefix(void)
{
    return s_config.ha_discovery_prefix;
}

uint16_t config_get_publish_interval_sensors(void)
{
    return s_config.interval_sensors;
}

uint16_t config_get_publish_interval_outputs(void)
{
    return s_config.interval_outputs;
}

uint16_t config_get_publish_interval_system(void)
{
    return s_config.interval_system;
}

uint16_t config_get_ha_discovery_interval(void)
{
    return s_config.interval_discovery;
}

uint16_t config_get_wifi_timeout(void)
{
    return s_config.wifi_timeout;
}

uint8_t config_get_dlbus_gpio(void)
{
    return s_config.dlbus_gpio;
}

uint16_t config_get_mqtt_keepalive(void)
{
    return s_config.mqtt_keepalive;
}

uint8_t config_get_mqtt_qos(void)
{
    return s_config.mqtt_qos;
}

bool config_get_mqtt_retain_sensors(void)
{
    return s_config.mqtt_retain_sensors;
}

bool config_get_mqtt_retain_status(void)
{
    return s_config.mqtt_retain_status;
}

// =============================================================================
// Public API - Non-Secret Setters
// =============================================================================

esp_err_t config_set_hostname(const char *hostname)
{
    if (hostname == NULL || strlen(hostname) >= CONFIG_HOSTNAME_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    lock();
    strncpy(s_config.hostname, hostname, sizeof(s_config.hostname) - 1);
    s_config.hostname[sizeof(s_config.hostname) - 1] = '\0';
    unlock();

    esp_err_t err = save_string(KEY_HOSTNAME, hostname);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Hostname updated: %s", hostname);
    }
    return err;
}

esp_err_t config_set_mqtt_broker_uri(const char *uri)
{
    if (uri == NULL || strlen(uri) >= CONFIG_MQTT_URI_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    lock();
    strncpy(s_config.mqtt_broker_uri, uri, sizeof(s_config.mqtt_broker_uri) - 1);
    s_config.mqtt_broker_uri[sizeof(s_config.mqtt_broker_uri) - 1] = '\0';
    unlock();

    esp_err_t err = save_string(KEY_MQTT_URI, uri);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "MQTT broker URI updated: %s", uri);
    }
    return err;
}

esp_err_t config_set_mqtt_client_id(const char *client_id)
{
    if (client_id == NULL || strlen(client_id) >= CONFIG_MQTT_CLIENT_ID_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    lock();
    strncpy(s_config.mqtt_client_id, client_id, sizeof(s_config.mqtt_client_id) - 1);
    s_config.mqtt_client_id[sizeof(s_config.mqtt_client_id) - 1] = '\0';
    unlock();

    esp_err_t err = save_string(KEY_MQTT_CLIENT_ID, client_id);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "MQTT client ID updated");
    }
    return err;
}

esp_err_t config_set_mqtt_base_topic(const char *topic)
{
    if (topic == NULL || strlen(topic) >= CONFIG_MQTT_TOPIC_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    lock();
    strncpy(s_config.mqtt_base_topic, topic, sizeof(s_config.mqtt_base_topic) - 1);
    s_config.mqtt_base_topic[sizeof(s_config.mqtt_base_topic) - 1] = '\0';
    unlock();

    esp_err_t err = save_string(KEY_MQTT_BASE_TOPIC, topic);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "MQTT base topic updated: %s", topic);
    }
    return err;
}

esp_err_t config_set_ha_discovery_prefix(const char *prefix)
{
    if (prefix == NULL || strlen(prefix) >= CONFIG_MQTT_TOPIC_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    lock();
    strncpy(s_config.ha_discovery_prefix, prefix, sizeof(s_config.ha_discovery_prefix) - 1);
    s_config.ha_discovery_prefix[sizeof(s_config.ha_discovery_prefix) - 1] = '\0';
    unlock();

    esp_err_t err = save_string(KEY_HA_DISC_PREFIX, prefix);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HA discovery prefix updated: %s", prefix);
    }
    return err;
}

esp_err_t config_set_publish_interval_sensors(uint16_t seconds)
{
    if (seconds < 1 || seconds > 3600) {
        return ESP_ERR_INVALID_ARG;
    }

    lock();
    s_config.interval_sensors = seconds;
    unlock();

    esp_err_t err = save_u16(KEY_INTERVAL_SENSORS, seconds);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Sensor publish interval updated: %ds", seconds);
    }
    return err;
}

esp_err_t config_set_publish_interval_outputs(uint16_t seconds)
{
    if (seconds < 1 || seconds > 3600) {
        return ESP_ERR_INVALID_ARG;
    }

    lock();
    s_config.interval_outputs = seconds;
    unlock();

    esp_err_t err = save_u16(KEY_INTERVAL_OUTPUTS, seconds);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Output publish interval updated: %ds", seconds);
    }
    return err;
}

esp_err_t config_set_publish_interval_system(uint16_t seconds)
{
    if (seconds < 1 || seconds > 3600) {
        return ESP_ERR_INVALID_ARG;
    }

    lock();
    s_config.interval_system = seconds;
    unlock();

    esp_err_t err = save_u16(KEY_INTERVAL_SYSTEM, seconds);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "System publish interval updated: %ds", seconds);
    }
    return err;
}

esp_err_t config_set_ha_discovery_interval(uint16_t seconds)
{
    if (seconds > 3600) {
        return ESP_ERR_INVALID_ARG;
    }

    lock();
    s_config.interval_discovery = seconds;
    unlock();

    esp_err_t err = save_u16(KEY_INTERVAL_DISCOVERY, seconds);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HA discovery interval updated: %ds", seconds);
    }
    return err;
}

esp_err_t config_set_wifi_timeout(uint16_t seconds)
{
    if (seconds < 5 || seconds > 120) {
        return ESP_ERR_INVALID_ARG;
    }

    lock();
    s_config.wifi_timeout = seconds;
    unlock();

    esp_err_t err = save_u16(KEY_WIFI_TIMEOUT, seconds);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "WiFi timeout updated: %ds", seconds);
    }
    return err;
}

esp_err_t config_set_dlbus_gpio(uint8_t gpio)
{
    if (gpio > 39) {
        return ESP_ERR_INVALID_ARG;
    }

    lock();
    s_config.dlbus_gpio = gpio;
    unlock();

    esp_err_t err = save_u8(KEY_DLBUS_GPIO, gpio);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "DL-Bus GPIO updated: %d", gpio);
    }
    return err;
}

// =============================================================================
// Public API - I/O Names
// =============================================================================

const char *config_get_sensor_name(int index)
{
    if (index < 0 || index >= CONFIG_NUM_SENSORS) {
        return "---";
    }
    return s_config.sensor_names[index];
}

esp_err_t config_set_sensor_name(int index, const char *name)
{
    if (index < 0 || index >= CONFIG_NUM_SENSORS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (name == NULL || strlen(name) >= CONFIG_IO_NAME_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    lock();
    strncpy(s_config.sensor_names[index], name, CONFIG_IO_NAME_MAX_LEN - 1);
    s_config.sensor_names[index][CONFIG_IO_NAME_MAX_LEN - 1] = '\0';
    unlock();

    // Save to NVS
    char key[16];
    snprintf(key, sizeof(key), "%s%d", KEY_SENSOR_PREFIX, index);
    esp_err_t err = save_string(key, name);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Sensor S%d name updated: %s", index + 1, name);
    }
    return err;
}

const char *config_get_output_name(int index)
{
    if (index < 0 || index >= CONFIG_NUM_OUTPUTS) {
        return "---";
    }
    return s_config.output_names[index];
}

esp_err_t config_set_output_name(int index, const char *name)
{
    if (index < 0 || index >= CONFIG_NUM_OUTPUTS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (name == NULL || strlen(name) >= CONFIG_IO_NAME_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    lock();
    strncpy(s_config.output_names[index], name, CONFIG_IO_NAME_MAX_LEN - 1);
    s_config.output_names[index][CONFIG_IO_NAME_MAX_LEN - 1] = '\0';
    unlock();

    // Save to NVS
    char key[16];
    snprintf(key, sizeof(key), "%s%d", KEY_OUTPUT_PREFIX, index);
    esp_err_t err = save_string(key, name);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Output A%d name updated: %s", index + 1, name);
    }
    return err;
}

esp_err_t config_reset_io_names(void)
{
    // Reload Kconfig defaults for I/O names
    const char *kconfig_sensor_names[] = SENSOR_NAMES;
    const char *kconfig_output_names[] = OUTPUT_NAMES;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_CONFIG, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    char key[16];

    lock();
    for (int i = 0; i < CONFIG_NUM_SENSORS; i++) {
        strncpy(s_config.sensor_names[i], kconfig_sensor_names[i], CONFIG_IO_NAME_MAX_LEN - 1);
        s_config.sensor_names[i][CONFIG_IO_NAME_MAX_LEN - 1] = '\0';
        // Erase from NVS to use defaults
        snprintf(key, sizeof(key), "%s%d", KEY_SENSOR_PREFIX, i);
        nvs_erase_key(handle, key);
    }

    for (int i = 0; i < CONFIG_NUM_OUTPUTS; i++) {
        strncpy(s_config.output_names[i], kconfig_output_names[i], CONFIG_IO_NAME_MAX_LEN - 1);
        s_config.output_names[i][CONFIG_IO_NAME_MAX_LEN - 1] = '\0';
        // Erase from NVS to use defaults
        snprintf(key, sizeof(key), "%s%d", KEY_OUTPUT_PREFIX, i);
        nvs_erase_key(handle, key);
    }
    unlock();

    nvs_commit(handle);
    nvs_close(handle);

    ESP_LOGI(TAG, "I/O names reset to Kconfig defaults");
    return ESP_OK;
}

// =============================================================================
// Public API - Secret Getters (NVS only, never logged)
// =============================================================================

bool config_has_wifi_credentials(void)
{
    return has_secret(KEY_WIFI_SSID);
}

esp_err_t config_get_wifi_ssid(char *buffer, size_t buffer_len)
{
    if (buffer == NULL || buffer_len < CONFIG_WIFI_SSID_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    return get_secret(KEY_WIFI_SSID, buffer, buffer_len);
}

esp_err_t config_get_wifi_password(char *buffer, size_t buffer_len)
{
    if (buffer == NULL || buffer_len < CONFIG_WIFI_PASSWORD_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    return get_secret(KEY_WIFI_PASSWORD, buffer, buffer_len);
}

esp_err_t config_set_wifi_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL || strlen(ssid) == 0 || strlen(ssid) >= CONFIG_WIFI_SSID_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (password != NULL && strlen(password) >= CONFIG_WIFI_PASSWORD_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = set_secret(KEY_WIFI_SSID, ssid);
    if (err != ESP_OK) {
        return err;
    }

    err = set_secret(KEY_WIFI_PASSWORD, password ? password : "");
    if (err != ESP_OK) {
        return err;
    }

    // Log without revealing credentials
    ESP_LOGI(TAG, "WiFi credentials stored");
    return ESP_OK;
}

esp_err_t config_clear_wifi_credentials(void)
{
    erase_secret(KEY_WIFI_SSID);
    erase_secret(KEY_WIFI_PASSWORD);
    ESP_LOGI(TAG, "WiFi credentials cleared");
    return ESP_OK;
}

bool config_has_mqtt_credentials(void)
{
    return has_secret(KEY_MQTT_USERNAME);
}

esp_err_t config_get_mqtt_username(char *buffer, size_t buffer_len)
{
    if (buffer == NULL || buffer_len < CONFIG_MQTT_USERNAME_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    return get_secret(KEY_MQTT_USERNAME, buffer, buffer_len);
}

esp_err_t config_get_mqtt_password(char *buffer, size_t buffer_len)
{
    if (buffer == NULL || buffer_len < CONFIG_MQTT_PASSWORD_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    return get_secret(KEY_MQTT_PASSWORD, buffer, buffer_len);
}

esp_err_t config_set_mqtt_credentials(const char *username, const char *password)
{
    if (username == NULL || strlen(username) == 0 || strlen(username) >= CONFIG_MQTT_USERNAME_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (password != NULL && strlen(password) >= CONFIG_MQTT_PASSWORD_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = set_secret(KEY_MQTT_USERNAME, username);
    if (err != ESP_OK) {
        return err;
    }

    err = set_secret(KEY_MQTT_PASSWORD, password ? password : "");
    if (err != ESP_OK) {
        return err;
    }

    // Log without revealing credentials
    ESP_LOGI(TAG, "MQTT credentials stored");
    return ESP_OK;
}

esp_err_t config_clear_mqtt_credentials(void)
{
    erase_secret(KEY_MQTT_USERNAME);
    erase_secret(KEY_MQTT_PASSWORD);
    ESP_LOGI(TAG, "MQTT credentials cleared");
    return ESP_OK;
}

// =============================================================================
// Public API - Factory Reset
// =============================================================================

esp_err_t config_factory_reset(void)
{
    ESP_LOGW(TAG, "Factory reset initiated");

    // Erase both namespaces
    nvs_handle_t handle;

    // Erase config namespace
    if (nvs_open(NVS_NAMESPACE_CONFIG, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }

    // Erase secret namespace
    if (nvs_open(NVS_NAMESPACE_SECRET, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }

    // Reload Kconfig defaults
    lock();
    memset(&s_config, 0, sizeof(s_config));
    load_kconfig_defaults();
    unlock();

    ESP_LOGI(TAG, "Factory reset complete - reboot recommended");
    return ESP_OK;
}

esp_err_t config_reset_nonsecrets(void)
{
    ESP_LOGI(TAG, "Resetting non-secret configuration to defaults");

    // Erase config namespace only (keep secrets)
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE_CONFIG, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }

    // Reload Kconfig defaults
    lock();
    load_kconfig_defaults();
    unlock();

    ESP_LOGI(TAG, "Non-secret configuration reset complete");
    return ESP_OK;
}

// =============================================================================
// Public API - Versioning
// =============================================================================

uint8_t config_get_version(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE_CONFIG, NVS_READONLY, &handle) != ESP_OK) {
        return 0;
    }

    uint8_t version = load_u8(handle, KEY_VERSION, 0);
    nvs_close(handle);
    return version;
}
