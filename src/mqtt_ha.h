/**
 * UVR1611 DL-Bus Reader - MQTT & Home Assistant Integration
 *
 * Features:
 * - WiFi connection management
 * - MQTT client with Last Will and Testament (LWT) for status
 * - Home Assistant auto-discovery
 * - Sensor value buffering with median calculation
 * - Output state change detection and immediate publish
 */

#ifndef MQTT_HA_H
#define MQTT_HA_H

#include <stdbool.h>
#include <stdint.h>
#include "io_config.h"

// =============================================================================
// Constants
// =============================================================================
#define MAX_SENSOR_SAMPLES      64   // Max samples to buffer for median calculation
#define MQTT_TOPIC_MAX_LEN      128
#define MQTT_PAYLOAD_MAX_LEN    512

// =============================================================================
// Data Types
// =============================================================================

// Sensor value sample for median calculation
typedef struct {
    float values[MAX_SENSOR_SAMPLES];
    int count;
    int index;
} sensor_buffer_t;

// Output state tracking for change detection
typedef struct {
    bool states[NUM_OUTPUTS];
    bool initialized;
} output_tracker_t;

// Speed level tracking
typedef struct {
    int values[NUM_SPEED_LEVELS];
    bool active[NUM_SPEED_LEVELS];
    bool initialized;
} speed_tracker_t;

// System information
typedef struct {
    char mac_address[18];    // "AA:BB:CC:DD:EE:FF"
    char ip_address[16];     // "192.168.1.100"
    uint32_t uptime_seconds;
    bool wifi_connected;
    bool mqtt_connected;
} system_info_t;

// =============================================================================
// Initialization Functions
// =============================================================================

/**
 * Initialize WiFi and MQTT subsystems
 * Must be called once at startup after app_main() begins
 * @return ESP_OK on success
 */
esp_err_t mqtt_ha_init(void);

/**
 * Start WiFi connection and MQTT client
 * @return ESP_OK on success
 */
esp_err_t mqtt_ha_start(void);

// =============================================================================
// Sensor Value Functions
// =============================================================================

/**
 * Add a sensor value sample to the buffer for median calculation
 * @param sensor_index Sensor index (0-15 for S1-S16)
 * @param value Sensor value (temperature, flow rate, etc.)
 */
void mqtt_ha_add_sensor_sample(int sensor_index, float value);

/**
 * Publish all sensor values (median of buffered samples)
 * Called periodically by main loop
 */
void mqtt_ha_publish_sensors(void);

// =============================================================================
// Output State Functions
// =============================================================================

/**
 * Update output state and publish if changed
 * @param output_index Output index (0-12 for A1-A13)
 * @param is_on Output state (true = ON, false = OFF)
 */
void mqtt_ha_update_output(int output_index, bool is_on);

/**
 * Publish all output states
 * Called periodically by main loop
 */
void mqtt_ha_publish_outputs(void);

// =============================================================================
// Speed Level Functions
// =============================================================================

/**
 * Update speed level and publish if changed
 * @param speed_index Speed index (0-3 for A1, A2, A6, A7)
 * @param value Speed value (0-30)
 * @param active Speed control active flag
 */
void mqtt_ha_update_speed(int speed_index, int value, bool active);

/**
 * Publish all speed levels
 */
void mqtt_ha_publish_speeds(void);

// =============================================================================
// Heat Meter Functions
// =============================================================================

/**
 * Add heat meter sample to buffer for interval-based publishing
 * @param meter_index Heat meter index (0 or 1)
 * @param power_kw Instantaneous power in kW
 * @param energy_kwh Total energy in kWh
 * @param active Heat meter active flag
 */
void mqtt_ha_add_heat_meter_sample(int meter_index, float power_kw, float energy_kwh, bool active);

/**
 * Publish heat meter data (called on interval by mqtt_ha_loop)
 */
void mqtt_ha_publish_heat_meters(void);

// =============================================================================
// System Functions
// =============================================================================

/**
 * Publish system information (MAC, IP, uptime)
 */
void mqtt_ha_publish_system_info(void);

/**
 * Get current system information
 * @return Pointer to system_info_t structure
 */
const system_info_t* mqtt_ha_get_system_info(void);

// =============================================================================
// Home Assistant Discovery
// =============================================================================

/**
 * Publish Home Assistant auto-discovery configuration for all entities
 */
void mqtt_ha_publish_discovery(void);

// =============================================================================
// Data Export Functions (for webserver)
// =============================================================================

/**
 * Get current sensor values for display
 * @param values Output array of NUM_SENSORS floats
 * @param valid Output array of NUM_SENSORS bools indicating valid data
 */
void mqtt_ha_get_sensor_values(float values[NUM_SENSORS], bool valid[NUM_SENSORS]);

/**
 * Get current output states for display
 * @param states Output array of NUM_OUTPUTS bools
 */
void mqtt_ha_get_output_states(bool states[NUM_OUTPUTS]);

// =============================================================================
// Status Functions
// =============================================================================

/**
 * Check if MQTT is connected
 * @return true if connected
 */
bool mqtt_ha_is_connected(void);

/**
 * Main MQTT task loop - call periodically from main loop
 * Handles publishing based on intervals
 */
void mqtt_ha_loop(void);

#endif /* MQTT_HA_H */
