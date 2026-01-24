/**
 * UVR1611 DL-Bus Reader - MQTT & Home Assistant Integration
 *
 * Implementation of WiFi, MQTT, and Home Assistant auto-discovery
 *
 * Security: All credentials are loaded from config_store (NVS) at runtime.
 * No credentials in source code, headers, or logs.
 */

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mqtt_client.h"

#include "mqtt_ha.h"
#include "config_store.h"
#include "io_config.h"

static const char *TAG = "MQTT-HA";

// =============================================================================
// Global State
// =============================================================================
static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool mqtt_connected = false;
static system_info_t system_info = {0};

// Sensor buffers for median calculation
static sensor_buffer_t sensor_buffers[NUM_SENSORS] = {0};

// Output state tracking
static output_tracker_t output_tracker = {0};

// Speed level tracking
static speed_tracker_t speed_tracker = {0};

// Configuration from io_config.h
static const char *sensor_names[] = SENSOR_NAMES;
static const char *output_names[] = OUTPUT_NAMES;
static const char *speed_level_names[] = SPEED_LEVEL_NAMES;

// Timing
static uint32_t last_sensor_publish = 0;
static uint32_t last_output_publish = 0;
static uint32_t last_system_publish = 0;
static uint32_t last_discovery_publish = 0;

// =============================================================================
// Helper Functions
// =============================================================================

/**
 * Compare function for qsort (float comparison)
 */
static int float_compare(const void *a, const void *b)
{
    float fa = *(const float *)a;
    float fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

/**
 * Calculate median of values in buffer
 */
static float calculate_median(sensor_buffer_t *buffer)
{
    if (buffer->count == 0) {
        return 0.0f;
    }

    // Copy values to temp array for sorting
    float temp[MAX_SENSOR_SAMPLES];
    int count = buffer->count < MAX_SENSOR_SAMPLES ? buffer->count : MAX_SENSOR_SAMPLES;

    for (int i = 0; i < count; i++) {
        temp[i] = buffer->values[i];
    }

    // Sort
    qsort(temp, count, sizeof(float), float_compare);

    // Return median
    if (count % 2 == 0) {
        return (temp[count / 2 - 1] + temp[count / 2]) / 2.0f;
    } else {
        return temp[count / 2];
    }
}

/**
 * Clear sensor buffer after publishing
 */
static void clear_sensor_buffer(sensor_buffer_t *buffer)
{
    buffer->count = 0;
    buffer->index = 0;
}

/**
 * Sanitize string for MQTT topic (replace spaces with underscores, lowercase)
 */
static void sanitize_for_topic(const char *input, char *output, size_t max_len)
{
    size_t i = 0;
    while (input[i] != '\0' && i < max_len - 1) {
        char c = input[i];
        if (c == ' ' || c == '.' || c == '-') {
            output[i] = '_';
        } else if (c >= 'A' && c <= 'Z') {
            output[i] = c + 32;  // lowercase
        } else {
            output[i] = c;
        }
        i++;
    }
    output[i] = '\0';
}

/**
 * Get unique device ID from MAC address
 */
static void get_device_id(char *device_id, size_t len)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(device_id, len, "uvr1611_%02x%02x%02x", mac[3], mac[4], mac[5]);
}

// =============================================================================
// MQTT Event Handler
// =============================================================================
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT Connected");
            mqtt_connected = true;
            system_info.mqtt_connected = true;

            // Publish online status
            char topic[MQTT_TOPIC_MAX_LEN];
            snprintf(topic, sizeof(topic), "%s/status", config_get_mqtt_base_topic());
            esp_mqtt_client_publish(mqtt_client, topic, "online", 0,
                                    config_get_mqtt_qos(), config_get_mqtt_retain_status());

            // Publish discovery on connect
            mqtt_ha_publish_discovery();
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT Disconnected");
            mqtt_connected = false;
            system_info.mqtt_connected = false;
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT Error");
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                ESP_LOGE(TAG, "TCP transport error");
            }
            break;

        default:
            break;
    }
}

// =============================================================================
// MQTT Initialization
// =============================================================================
static esp_err_t mqtt_init(void)
{
    char lwt_topic[MQTT_TOPIC_MAX_LEN];
    snprintf(lwt_topic, sizeof(lwt_topic), "%s/status", config_get_mqtt_base_topic());

    // Build MQTT config from config store
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = config_get_mqtt_broker_uri(),
        .credentials.client_id = config_get_mqtt_client_id(),
        .session.keepalive = config_get_mqtt_keepalive(),
        .session.last_will = {
            .topic = lwt_topic,
            .msg = "offline",
            .msg_len = 7,
            .qos = config_get_mqtt_qos(),
            .retain = config_get_mqtt_retain_status(),
        },
    };

    // Add credentials if configured (from NVS, never logged)
    char mqtt_user[CONFIG_MQTT_USERNAME_MAX_LEN];
    char mqtt_pass[CONFIG_MQTT_PASSWORD_MAX_LEN];

    if (config_has_mqtt_credentials()) {
        if (config_get_mqtt_username(mqtt_user, sizeof(mqtt_user)) == ESP_OK) {
            mqtt_cfg.credentials.username = mqtt_user;

            if (config_get_mqtt_password(mqtt_pass, sizeof(mqtt_pass)) == ESP_OK) {
                mqtt_cfg.credentials.authentication.password = mqtt_pass;
            }
        }
    }

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);

    // Clear sensitive data
    memset(mqtt_user, 0, sizeof(mqtt_user));
    memset(mqtt_pass, 0, sizeof(mqtt_pass));

    if (mqtt_client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID,
                    mqtt_event_handler, NULL));

    ESP_LOGI(TAG, "MQTT initialized (broker: %s)", config_get_mqtt_broker_uri());
    return ESP_OK;
}

// =============================================================================
// Public Functions - Initialization
// =============================================================================

esp_err_t mqtt_ha_init(void)
{
    ESP_LOGI(TAG, "Initializing MQTT and Home Assistant integration");

    // config_store_init() must be called before this function
    // It handles NVS initialization
    if (!config_store_is_initialized()) {
        ESP_LOGE(TAG, "Config store not initialized! Call config_store_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    // Get MAC address for system info
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(system_info.mac_address, sizeof(system_info.mac_address),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // Initialize MQTT client
    esp_err_t err = mqtt_init();
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "MQTT-HA initialized");
    return ESP_OK;
}

esp_err_t mqtt_ha_start(void)
{
    if (mqtt_client == NULL) {
        ESP_LOGE(TAG, "MQTT client not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Starting MQTT client...");
    esp_err_t err = esp_mqtt_client_start(mqtt_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
        return err;
    }

    system_info.wifi_connected = true;  // Assume WiFi is managed externally
    return ESP_OK;
}

// =============================================================================
// Public Functions - Sensor Values
// =============================================================================

void mqtt_ha_add_sensor_sample(int sensor_index, float value)
{
    if (sensor_index < 0 || sensor_index >= NUM_SENSORS) {
        return;
    }

    sensor_buffer_t *buffer = &sensor_buffers[sensor_index];

    // Add to circular buffer
    buffer->values[buffer->index] = value;
    buffer->index = (buffer->index + 1) % MAX_SENSOR_SAMPLES;
    if (buffer->count < MAX_SENSOR_SAMPLES) {
        buffer->count++;
    }
}

void mqtt_ha_publish_sensors(void)
{
    if (!mqtt_connected) {
        return;
    }

    char topic[MQTT_TOPIC_MAX_LEN];
    char payload[32];
    char sanitized_name[32];
    const char *base_topic = config_get_mqtt_base_topic();
    uint8_t qos = config_get_mqtt_qos();
    bool retain = config_get_mqtt_retain_sensors();

    for (int i = 0; i < NUM_SENSORS; i++) {
        // Skip unused sensors
        if (strcmp(sensor_names[i], "---") == 0) {
            continue;
        }

        sensor_buffer_t *buffer = &sensor_buffers[i];
        if (buffer->count == 0) {
            continue;
        }

        float median = calculate_median(buffer);

        // Create topic
        sanitize_for_topic(sensor_names[i], sanitized_name, sizeof(sanitized_name));
        snprintf(topic, sizeof(topic), "%s/sensor/%s/state", base_topic, sanitized_name);

        // Create payload
        snprintf(payload, sizeof(payload), "%.1f", median);

        esp_mqtt_client_publish(mqtt_client, topic, payload, 0, qos, retain);

        // Clear buffer after publishing
        clear_sensor_buffer(buffer);
    }

    ESP_LOGI(TAG, "Published sensor values");
}

// =============================================================================
// Public Functions - Output States
// =============================================================================

void mqtt_ha_update_output(int output_index, bool is_on)
{
    if (output_index < 0 || output_index >= NUM_OUTPUTS) {
        return;
    }

    // Check if state changed
    if (output_tracker.initialized && output_tracker.states[output_index] == is_on) {
        return;  // No change
    }

    output_tracker.states[output_index] = is_on;

    // Publish immediately on change
    if (mqtt_connected && output_tracker.initialized) {
        char topic[MQTT_TOPIC_MAX_LEN];
        char sanitized_name[32];

        sanitize_for_topic(output_names[output_index], sanitized_name, sizeof(sanitized_name));
        snprintf(topic, sizeof(topic), "%s/switch/%s/state",
                 config_get_mqtt_base_topic(), sanitized_name);

        esp_mqtt_client_publish(mqtt_client, topic, is_on ? "ON" : "OFF", 0,
                                config_get_mqtt_qos(), config_get_mqtt_retain_sensors());

        ESP_LOGI(TAG, "Output %s changed to %s", output_names[output_index], is_on ? "ON" : "OFF");
    }

    output_tracker.initialized = true;
}

void mqtt_ha_publish_outputs(void)
{
    if (!mqtt_connected) {
        return;
    }

    char topic[MQTT_TOPIC_MAX_LEN];
    char sanitized_name[32];
    const char *base_topic = config_get_mqtt_base_topic();
    uint8_t qos = config_get_mqtt_qos();
    bool retain = config_get_mqtt_retain_sensors();

    for (int i = 0; i < NUM_OUTPUTS; i++) {
        sanitize_for_topic(output_names[i], sanitized_name, sizeof(sanitized_name));
        snprintf(topic, sizeof(topic), "%s/switch/%s/state", base_topic, sanitized_name);

        esp_mqtt_client_publish(mqtt_client, topic,
                                output_tracker.states[i] ? "ON" : "OFF",
                                0, qos, retain);
    }

    ESP_LOGI(TAG, "Published output states");
}

// =============================================================================
// Public Functions - Speed Levels
// =============================================================================

void mqtt_ha_update_speed(int speed_index, int value, bool active)
{
    if (speed_index < 0 || speed_index >= NUM_SPEED_LEVELS) {
        return;
    }

    // Check if changed
    if (speed_tracker.initialized &&
        speed_tracker.values[speed_index] == value &&
        speed_tracker.active[speed_index] == active) {
        return;
    }

    speed_tracker.values[speed_index] = value;
    speed_tracker.active[speed_index] = active;

    // Publish immediately on change
    if (mqtt_connected && speed_tracker.initialized) {
        char topic[MQTT_TOPIC_MAX_LEN];
        char payload[16];
        char sanitized_name[32];

        sanitize_for_topic(speed_level_names[speed_index], sanitized_name, sizeof(sanitized_name));
        snprintf(topic, sizeof(topic), "%s/sensor/%s/state",
                 config_get_mqtt_base_topic(), sanitized_name);

        if (active) {
            snprintf(payload, sizeof(payload), "%d", value);
        } else {
            snprintf(payload, sizeof(payload), "inactive");
        }

        esp_mqtt_client_publish(mqtt_client, topic, payload, 0,
                                config_get_mqtt_qos(), config_get_mqtt_retain_sensors());

        ESP_LOGI(TAG, "Speed %s changed to %s", speed_level_names[speed_index], payload);
    }

    speed_tracker.initialized = true;
}

void mqtt_ha_publish_speeds(void)
{
    if (!mqtt_connected) {
        return;
    }

    char topic[MQTT_TOPIC_MAX_LEN];
    char payload[16];
    char sanitized_name[32];
    const char *base_topic = config_get_mqtt_base_topic();
    uint8_t qos = config_get_mqtt_qos();
    bool retain = config_get_mqtt_retain_sensors();

    for (int i = 0; i < NUM_SPEED_LEVELS; i++) {
        sanitize_for_topic(speed_level_names[i], sanitized_name, sizeof(sanitized_name));
        snprintf(topic, sizeof(topic), "%s/sensor/%s/state", base_topic, sanitized_name);

        if (speed_tracker.active[i]) {
            snprintf(payload, sizeof(payload), "%d", speed_tracker.values[i]);
        } else {
            snprintf(payload, sizeof(payload), "inactive");
        }

        esp_mqtt_client_publish(mqtt_client, topic, payload, 0, qos, retain);
    }
}

// =============================================================================
// Public Functions - Heat Meters
// =============================================================================

void mqtt_ha_publish_heat_meter(int meter_index, float power_kw, float energy_kwh, bool active)
{
    if (!mqtt_connected || meter_index < 0 || meter_index > 1) {
        return;
    }

    char topic[MQTT_TOPIC_MAX_LEN];
    char payload[32];
    const char *base_topic = config_get_mqtt_base_topic();
    uint8_t qos = config_get_mqtt_qos();
    bool retain = config_get_mqtt_retain_sensors();

    if (active) {
        // Publish power
        snprintf(topic, sizeof(topic), "%s/sensor/heat_meter_%d_power/state", base_topic, meter_index + 1);
        snprintf(payload, sizeof(payload), "%.2f", power_kw);
        esp_mqtt_client_publish(mqtt_client, topic, payload, 0, qos, retain);

        // Publish energy
        snprintf(topic, sizeof(topic), "%s/sensor/heat_meter_%d_energy/state", base_topic, meter_index + 1);
        snprintf(payload, sizeof(payload), "%.1f", energy_kwh);
        esp_mqtt_client_publish(mqtt_client, topic, payload, 0, qos, retain);
    }
}

// =============================================================================
// Public Functions - System Info
// =============================================================================

void mqtt_ha_publish_system_info(void)
{
    if (!mqtt_connected) {
        return;
    }

    char topic[MQTT_TOPIC_MAX_LEN];
    char payload[64];
    const char *base_topic = config_get_mqtt_base_topic();
    uint8_t qos = config_get_mqtt_qos();

    // Update uptime
    system_info.uptime_seconds = xTaskGetTickCount() / configTICK_RATE_HZ;

    // Publish MAC address
    snprintf(topic, sizeof(topic), "%s/system/mac", base_topic);
    esp_mqtt_client_publish(mqtt_client, topic, system_info.mac_address, 0, qos, true);

    // Publish IP address
    snprintf(topic, sizeof(topic), "%s/system/ip", base_topic);
    esp_mqtt_client_publish(mqtt_client, topic, system_info.ip_address, 0, qos, true);

    // Publish uptime
    snprintf(topic, sizeof(topic), "%s/system/uptime", base_topic);
    snprintf(payload, sizeof(payload), "%lu", (unsigned long)system_info.uptime_seconds);
    esp_mqtt_client_publish(mqtt_client, topic, payload, 0, qos, false);

    ESP_LOGI(TAG, "Published system info (uptime: %lus)", (unsigned long)system_info.uptime_seconds);
}

const system_info_t* mqtt_ha_get_system_info(void)
{
    system_info.uptime_seconds = xTaskGetTickCount() / configTICK_RATE_HZ;
    return &system_info;
}

// =============================================================================
// Home Assistant Discovery
// =============================================================================

static void publish_ha_sensor_discovery(const char *name, const char *device_class,
                                        const char *unit, const char *state_topic,
                                        const char *unique_id, const char *device_id)
{
    char topic[MQTT_TOPIC_MAX_LEN];
    char payload[MQTT_PAYLOAD_MAX_LEN];
    char sanitized_id[32];
    const char *ha_prefix = config_get_ha_discovery_prefix();
    const char *base_topic = config_get_mqtt_base_topic();
    uint8_t qos = config_get_mqtt_qos();

    sanitize_for_topic(unique_id, sanitized_id, sizeof(sanitized_id));

    snprintf(topic, sizeof(topic), "%s/sensor/%s/%s/config",
             ha_prefix, device_id, sanitized_id);

    int len = snprintf(payload, sizeof(payload),
        "{"
        "\"name\":\"%s\","
        "\"state_topic\":\"%s\","
        "\"unique_id\":\"%s_%s\","
        "\"device\":{"
            "\"identifiers\":[\"%s\"],"
            "\"name\":\"UVR1611 Gateway\","
            "\"model\":\"UVR1611\","
            "\"manufacturer\":\"Technische Alternative\""
        "}",
        name, state_topic, device_id, sanitized_id, device_id);

    if (device_class != NULL && strlen(device_class) > 0) {
        len += snprintf(payload + len, sizeof(payload) - len,
            ",\"device_class\":\"%s\"", device_class);
    }

    if (unit != NULL && strlen(unit) > 0) {
        len += snprintf(payload + len, sizeof(payload) - len,
            ",\"unit_of_measurement\":\"%s\"", unit);
    }

    // Add availability topic
    len += snprintf(payload + len, sizeof(payload) - len,
        ",\"availability_topic\":\"%s/status\"", base_topic);

    snprintf(payload + len, sizeof(payload) - len, "}");

    esp_mqtt_client_publish(mqtt_client, topic, payload, 0, qos, true);
}

static void publish_ha_binary_sensor_discovery(const char *name, const char *device_class,
                                               const char *state_topic, const char *unique_id,
                                               const char *device_id)
{
    char topic[MQTT_TOPIC_MAX_LEN];
    char payload[MQTT_PAYLOAD_MAX_LEN];
    char sanitized_id[32];
    const char *ha_prefix = config_get_ha_discovery_prefix();
    const char *base_topic = config_get_mqtt_base_topic();
    uint8_t qos = config_get_mqtt_qos();

    sanitize_for_topic(unique_id, sanitized_id, sizeof(sanitized_id));

    snprintf(topic, sizeof(topic), "%s/binary_sensor/%s/%s/config",
             ha_prefix, device_id, sanitized_id);

    snprintf(payload, sizeof(payload),
        "{"
        "\"name\":\"%s\","
        "\"state_topic\":\"%s\","
        "\"unique_id\":\"%s_%s\","
        "\"device_class\":\"%s\","
        "\"payload_on\":\"ON\","
        "\"payload_off\":\"OFF\","
        "\"availability_topic\":\"%s/status\","
        "\"device\":{"
            "\"identifiers\":[\"%s\"],"
            "\"name\":\"UVR1611 Gateway\","
            "\"model\":\"UVR1611\","
            "\"manufacturer\":\"Technische Alternative\""
        "}"
        "}",
        name, state_topic, device_id, sanitized_id, device_class,
        base_topic, device_id);

    esp_mqtt_client_publish(mqtt_client, topic, payload, 0, qos, true);
}

void mqtt_ha_publish_discovery(void)
{
    if (!mqtt_connected) {
        return;
    }

    char device_id[32];
    char state_topic[MQTT_TOPIC_MAX_LEN];
    char sanitized_name[32];
    const char *base_topic = config_get_mqtt_base_topic();

    get_device_id(device_id, sizeof(device_id));

    ESP_LOGI(TAG, "Publishing Home Assistant discovery (device_id: %s)", device_id);

    // ==========================================================================
    // Temperature Sensors
    // ==========================================================================
    for (int i = 0; i < NUM_SENSORS; i++) {
        if (strcmp(sensor_names[i], "---") == 0) {
            continue;
        }

        sanitize_for_topic(sensor_names[i], sanitized_name, sizeof(sanitized_name));
        snprintf(state_topic, sizeof(state_topic), "%s/sensor/%s/state",
                 base_topic, sanitized_name);

        // Determine device class and unit based on sensor name
        const char *device_class = "temperature";
        const char *unit = "°C";

        if (strstr(sensor_names[i], "Durchfl") != NULL) {
            device_class = "";  // No device class for flow
            unit = "l/h";
        }

        publish_ha_sensor_discovery(sensor_names[i], device_class, unit,
                                    state_topic, sanitized_name, device_id);
    }

    // ==========================================================================
    // Output Binary Sensors (Pumps, Valves, etc.)
    // ==========================================================================
    for (int i = 0; i < NUM_OUTPUTS; i++) {
        sanitize_for_topic(output_names[i], sanitized_name, sizeof(sanitized_name));
        snprintf(state_topic, sizeof(state_topic), "%s/switch/%s/state",
                 base_topic, sanitized_name);

        // Determine device class based on output name
        const char *device_class = "running";  // Default for pumps
        if (strstr(output_names[i], "Ventil") != NULL ||
            strstr(output_names[i], "Misch") != NULL) {
            device_class = "opening";
        }

        publish_ha_binary_sensor_discovery(output_names[i], device_class,
                                           state_topic, sanitized_name, device_id);
    }

    // ==========================================================================
    // Speed Level Sensors
    // ==========================================================================
    for (int i = 0; i < NUM_SPEED_LEVELS; i++) {
        sanitize_for_topic(speed_level_names[i], sanitized_name, sizeof(sanitized_name));
        snprintf(state_topic, sizeof(state_topic), "%s/sensor/%s/state",
                 base_topic, sanitized_name);

        publish_ha_sensor_discovery(speed_level_names[i], "", "%",
                                    state_topic, sanitized_name, device_id);
    }

    // ==========================================================================
    // Heat Meter Sensors
    // ==========================================================================
    for (int i = 0; i < 2; i++) {
        char name[32];

        // Power sensor
        snprintf(name, sizeof(name), "Heat Meter %d Power", i + 1);
        snprintf(state_topic, sizeof(state_topic), "%s/sensor/heat_meter_%d_power/state",
                 base_topic, i + 1);
        snprintf(sanitized_name, sizeof(sanitized_name), "heat_meter_%d_power", i + 1);
        publish_ha_sensor_discovery(name, "power", "kW", state_topic, sanitized_name, device_id);

        // Energy sensor
        snprintf(name, sizeof(name), "Heat Meter %d Energy", i + 1);
        snprintf(state_topic, sizeof(state_topic), "%s/sensor/heat_meter_%d_energy/state",
                 base_topic, i + 1);
        snprintf(sanitized_name, sizeof(sanitized_name), "heat_meter_%d_energy", i + 1);
        publish_ha_sensor_discovery(name, "energy", "kWh", state_topic, sanitized_name, device_id);
    }

    // ==========================================================================
    // System Sensors
    // ==========================================================================
    snprintf(state_topic, sizeof(state_topic), "%s/system/uptime", base_topic);
    publish_ha_sensor_discovery("UVR1611 Uptime", "duration", "s",
                                state_topic, "system_uptime", device_id);

    ESP_LOGI(TAG, "Home Assistant discovery published");
}

// =============================================================================
// Status Functions
// =============================================================================

bool mqtt_ha_is_connected(void)
{
    return mqtt_connected;
}

// =============================================================================
// Data Export Functions (for webserver)
// =============================================================================

void mqtt_ha_get_sensor_values(float values[NUM_SENSORS], bool valid[NUM_SENSORS])
{
    for (int i = 0; i < NUM_SENSORS; i++) {
        sensor_buffer_t *buffer = &sensor_buffers[i];
        if (buffer->count > 0) {
            values[i] = calculate_median(buffer);
            valid[i] = true;
        } else {
            values[i] = 0.0f;
            valid[i] = false;
        }
    }
}

void mqtt_ha_get_output_states(bool states[NUM_OUTPUTS])
{
    for (int i = 0; i < NUM_OUTPUTS; i++) {
        states[i] = output_tracker.states[i];
    }
}

void mqtt_ha_loop(void)
{
    uint32_t now = xTaskGetTickCount() / configTICK_RATE_HZ;

    // Publish sensors on interval (from config store)
    uint16_t sensor_interval = config_get_publish_interval_sensors();
    if (now - last_sensor_publish >= sensor_interval) {
        mqtt_ha_publish_sensors();
        last_sensor_publish = now;
    }

    // Publish outputs on interval (in addition to immediate on change)
    uint16_t output_interval = config_get_publish_interval_outputs();
    if (now - last_output_publish >= output_interval) {
        mqtt_ha_publish_outputs();
        mqtt_ha_publish_speeds();
        last_output_publish = now;
    }

    // Publish system info on interval
    uint16_t system_interval = config_get_publish_interval_system();
    if (now - last_system_publish >= system_interval) {
        mqtt_ha_publish_system_info();
        last_system_publish = now;
    }

    // Re-publish discovery on interval (if configured > 0)
    uint16_t discovery_interval = config_get_ha_discovery_interval();
    if (discovery_interval > 0 && now - last_discovery_publish >= discovery_interval) {
        mqtt_ha_publish_discovery();
        last_discovery_publish = now;
    }
}
