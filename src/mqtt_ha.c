/**
 * UVR1611 DL-Bus Reader - MQTT & Home Assistant Integration
 *
 * Implementation of WiFi, MQTT, and Home Assistant auto-discovery
 */

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "mqtt_client.h"

#include "mqtt_ha.h"
#include "wifi_config.h"
#include "io_config.h"

static const char *TAG = "MQTT-HA";

// =============================================================================
// WiFi Event Group
// =============================================================================
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

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
// WiFi Event Handler
// =============================================================================
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    static int retry_count = 0;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        system_info.wifi_connected = false;
        if (retry_count < 10) {
            esp_wifi_connect();
            retry_count++;
            ESP_LOGI(TAG, "Retrying WiFi connection (%d/10)...", retry_count);
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "WiFi connection failed after 10 retries");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(system_info.ip_address, sizeof(system_info.ip_address),
                 IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", system_info.ip_address);
        system_info.wifi_connected = true;
        retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
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
            snprintf(topic, sizeof(topic), "%s/status", MQTT_BASE_TOPIC);
            esp_mqtt_client_publish(mqtt_client, topic, "online", 0, MQTT_QOS, MQTT_RETAIN_STATUS);

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
// WiFi Initialization
// =============================================================================
static esp_err_t wifi_init(void)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                    &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                    &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    // Disable power save mode to reduce WiFi interrupt frequency
    // This helps DL-Bus timing-critical interrupt handling
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    // Get MAC address
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(system_info.mac_address, sizeof(system_info.mac_address),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    return ESP_OK;
}

// =============================================================================
// MQTT Initialization
// =============================================================================
static esp_err_t mqtt_init(void)
{
    char lwt_topic[MQTT_TOPIC_MAX_LEN];
    snprintf(lwt_topic, sizeof(lwt_topic), "%s/status", MQTT_BASE_TOPIC);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .credentials.client_id = MQTT_CLIENT_ID,
        .session.keepalive = MQTT_KEEPALIVE_S,
        .session.last_will = {
            .topic = lwt_topic,
            .msg = "offline",
            .msg_len = 7,
            .qos = MQTT_QOS,
            .retain = MQTT_RETAIN_STATUS,
        },
    };

    // Add credentials if configured
    if (strlen(MQTT_USERNAME) > 0) {
        mqtt_cfg.credentials.username = MQTT_USERNAME;
        mqtt_cfg.credentials.authentication.password = MQTT_PASSWORD;
    }

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (mqtt_client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID,
                    mqtt_event_handler, NULL));

    return ESP_OK;
}

// =============================================================================
// Public Functions - Initialization
// =============================================================================

esp_err_t mqtt_ha_init(void)
{
    ESP_LOGI(TAG, "Initializing MQTT and Home Assistant integration");

    // Initialize NVS (required for WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize WiFi
    ESP_ERROR_CHECK(wifi_init());

    // Initialize MQTT
    ESP_ERROR_CHECK(mqtt_init());

    ESP_LOGI(TAG, "MQTT-HA initialized");
    return ESP_OK;
}

esp_err_t mqtt_ha_start(void)
{
    ESP_LOGI(TAG, "Starting WiFi...");
    ESP_ERROR_CHECK(esp_wifi_start());

    // Wait for connection
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_S * 1000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected, starting MQTT...");
        ESP_ERROR_CHECK(esp_mqtt_client_start(mqtt_client));
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "WiFi connection failed");
        return ESP_FAIL;
    }
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
        snprintf(topic, sizeof(topic), "%s/sensor/%s/state", MQTT_BASE_TOPIC, sanitized_name);

        // Create payload
        snprintf(payload, sizeof(payload), "%.1f", median);

        esp_mqtt_client_publish(mqtt_client, topic, payload, 0, MQTT_QOS, MQTT_RETAIN_SENSORS);

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
        snprintf(topic, sizeof(topic), "%s/switch/%s/state", MQTT_BASE_TOPIC, sanitized_name);

        esp_mqtt_client_publish(mqtt_client, topic, is_on ? "ON" : "OFF", 0, MQTT_QOS, MQTT_RETAIN_SENSORS);

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

    for (int i = 0; i < NUM_OUTPUTS; i++) {
        sanitize_for_topic(output_names[i], sanitized_name, sizeof(sanitized_name));
        snprintf(topic, sizeof(topic), "%s/switch/%s/state", MQTT_BASE_TOPIC, sanitized_name);

        esp_mqtt_client_publish(mqtt_client, topic,
                                output_tracker.states[i] ? "ON" : "OFF",
                                0, MQTT_QOS, MQTT_RETAIN_SENSORS);
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
        snprintf(topic, sizeof(topic), "%s/sensor/%s/state", MQTT_BASE_TOPIC, sanitized_name);

        if (active) {
            snprintf(payload, sizeof(payload), "%d", value);
        } else {
            snprintf(payload, sizeof(payload), "inactive");
        }

        esp_mqtt_client_publish(mqtt_client, topic, payload, 0, MQTT_QOS, MQTT_RETAIN_SENSORS);

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

    for (int i = 0; i < NUM_SPEED_LEVELS; i++) {
        sanitize_for_topic(speed_level_names[i], sanitized_name, sizeof(sanitized_name));
        snprintf(topic, sizeof(topic), "%s/sensor/%s/state", MQTT_BASE_TOPIC, sanitized_name);

        if (speed_tracker.active[i]) {
            snprintf(payload, sizeof(payload), "%d", speed_tracker.values[i]);
        } else {
            snprintf(payload, sizeof(payload), "inactive");
        }

        esp_mqtt_client_publish(mqtt_client, topic, payload, 0, MQTT_QOS, MQTT_RETAIN_SENSORS);
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

    if (active) {
        // Publish power
        snprintf(topic, sizeof(topic), "%s/sensor/heat_meter_%d_power/state", MQTT_BASE_TOPIC, meter_index + 1);
        snprintf(payload, sizeof(payload), "%.2f", power_kw);
        esp_mqtt_client_publish(mqtt_client, topic, payload, 0, MQTT_QOS, MQTT_RETAIN_SENSORS);

        // Publish energy
        snprintf(topic, sizeof(topic), "%s/sensor/heat_meter_%d_energy/state", MQTT_BASE_TOPIC, meter_index + 1);
        snprintf(payload, sizeof(payload), "%.1f", energy_kwh);
        esp_mqtt_client_publish(mqtt_client, topic, payload, 0, MQTT_QOS, MQTT_RETAIN_SENSORS);
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

    // Update uptime
    system_info.uptime_seconds = xTaskGetTickCount() / configTICK_RATE_HZ;

    // Publish MAC address
    snprintf(topic, sizeof(topic), "%s/system/mac", MQTT_BASE_TOPIC);
    esp_mqtt_client_publish(mqtt_client, topic, system_info.mac_address, 0, MQTT_QOS, true);

    // Publish IP address
    snprintf(topic, sizeof(topic), "%s/system/ip", MQTT_BASE_TOPIC);
    esp_mqtt_client_publish(mqtt_client, topic, system_info.ip_address, 0, MQTT_QOS, true);

    // Publish uptime
    snprintf(topic, sizeof(topic), "%s/system/uptime", MQTT_BASE_TOPIC);
    snprintf(payload, sizeof(payload), "%lu", (unsigned long)system_info.uptime_seconds);
    esp_mqtt_client_publish(mqtt_client, topic, payload, 0, MQTT_QOS, false);

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

    sanitize_for_topic(unique_id, sanitized_id, sizeof(sanitized_id));

    snprintf(topic, sizeof(topic), "%s/sensor/%s/%s/config",
             HA_DISCOVERY_PREFIX, device_id, sanitized_id);

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
        ",\"availability_topic\":\"%s/status\"", MQTT_BASE_TOPIC);

    snprintf(payload + len, sizeof(payload) - len, "}");

    esp_mqtt_client_publish(mqtt_client, topic, payload, 0, MQTT_QOS, true);
}

static void publish_ha_binary_sensor_discovery(const char *name, const char *device_class,
                                               const char *state_topic, const char *unique_id,
                                               const char *device_id)
{
    char topic[MQTT_TOPIC_MAX_LEN];
    char payload[MQTT_PAYLOAD_MAX_LEN];
    char sanitized_id[32];

    sanitize_for_topic(unique_id, sanitized_id, sizeof(sanitized_id));

    snprintf(topic, sizeof(topic), "%s/binary_sensor/%s/%s/config",
             HA_DISCOVERY_PREFIX, device_id, sanitized_id);

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
        MQTT_BASE_TOPIC, device_id);

    esp_mqtt_client_publish(mqtt_client, topic, payload, 0, MQTT_QOS, true);
}

void mqtt_ha_publish_discovery(void)
{
    if (!mqtt_connected) {
        return;
    }

    char device_id[32];
    char state_topic[MQTT_TOPIC_MAX_LEN];
    char sanitized_name[32];

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
                 MQTT_BASE_TOPIC, sanitized_name);

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
                 MQTT_BASE_TOPIC, sanitized_name);

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
                 MQTT_BASE_TOPIC, sanitized_name);

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
                 MQTT_BASE_TOPIC, i + 1);
        snprintf(sanitized_name, sizeof(sanitized_name), "heat_meter_%d_power", i + 1);
        publish_ha_sensor_discovery(name, "power", "kW", state_topic, sanitized_name, device_id);

        // Energy sensor
        snprintf(name, sizeof(name), "Heat Meter %d Energy", i + 1);
        snprintf(state_topic, sizeof(state_topic), "%s/sensor/heat_meter_%d_energy/state",
                 MQTT_BASE_TOPIC, i + 1);
        snprintf(sanitized_name, sizeof(sanitized_name), "heat_meter_%d_energy", i + 1);
        publish_ha_sensor_discovery(name, "energy", "kWh", state_topic, sanitized_name, device_id);
    }

    // ==========================================================================
    // System Sensors
    // ==========================================================================
    snprintf(state_topic, sizeof(state_topic), "%s/system/uptime", MQTT_BASE_TOPIC);
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

void mqtt_ha_loop(void)
{
    uint32_t now = xTaskGetTickCount() / configTICK_RATE_HZ;

    // Publish sensors on interval
    if (now - last_sensor_publish >= PUBLISH_INTERVAL_SENSORS_S) {
        mqtt_ha_publish_sensors();
        last_sensor_publish = now;
    }

    // Publish outputs on interval (in addition to immediate on change)
    if (now - last_output_publish >= PUBLISH_INTERVAL_OUTPUTS_S) {
        mqtt_ha_publish_outputs();
        mqtt_ha_publish_speeds();
        last_output_publish = now;
    }

    // Publish system info on interval
    if (now - last_system_publish >= PUBLISH_INTERVAL_SYSTEM_S) {
        mqtt_ha_publish_system_info();
        last_system_publish = now;
    }

    // Re-publish discovery on interval (if configured)
    #if HA_DISCOVERY_INTERVAL_S > 0
    if (now - last_discovery_publish >= HA_DISCOVERY_INTERVAL_S) {
        mqtt_ha_publish_discovery();
        last_discovery_publish = now;
    }
    #endif
}
