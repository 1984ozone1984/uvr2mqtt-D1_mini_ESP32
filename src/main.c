/**
 * UVR1611 DL-Bus Reader - Phase 1: Pipeline Architecture
 *
 * Architecture:
 *   GPIO Interrupt -> Edge Circular Buffer -> Bit Extractor Task
 *                  -> Manchester Decoder Task -> Frame Queue
 *
 * Using GPIO interrupts instead of RMT to avoid buffer size limitations.
 * ESP32 RMT without DMA is limited to 512 symbols, but we need ~656.
 *
 * DL-Bus Protocol (UVR1611):
 * - Display clock: 488 Hz
 * - Bit duration: 2.048 ms (2048 us)
 * - Half-bit duration: 1.024 ms (1024 us)
 * - Data is XOR'd with 488 Hz clock, then inverted by output transistor
 * - SYNC: 16 high bits without start/stop bit
 * - Byte format: 1 start bit (0), 8 data bits (LSB first), 1 stop bit (1)
 * - Frame: SYNC + 64 bytes (device ID, data, checksum)
 *
 * Hardware: Azdelivery D1 Mini ESP32, GPIO26 for DL-Bus input
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "io_config.h"
#include "mqtt_ha.h"

static const char *TAG = "DL-BUS";

// =============================================================================
// I/O Configuration Arrays (from Kconfig)
// =============================================================================
static const char *sensor_names[NUM_SENSORS] = SENSOR_NAMES;
static const char *output_names[NUM_OUTPUTS] = OUTPUT_NAMES;
static const char *speed_level_names[NUM_SPEED_LEVELS] = SPEED_LEVEL_NAMES;
static const char *sensor_units[NUM_SENSOR_TYPES] = SENSOR_UNITS;

// =============================================================================
// Frame Byte Offsets (UVR1611 Protocol)
// =============================================================================
#define FRAME_OFF_DEVICE_ID     0
#define FRAME_OFF_DEVICE_ID_INV 1
#define FRAME_OFF_RESERVED      2
#define FRAME_OFF_MINUTE        3
#define FRAME_OFF_HOUR          4
#define FRAME_OFF_DAY           5
#define FRAME_OFF_MONTH         6
#define FRAME_OFF_YEAR          7
#define FRAME_OFF_SENSORS       8   // 16 sensors × 2 bytes = bytes 8-39
#define FRAME_OFF_OUTPUTS_1     40  // Outputs A1-A8
#define FRAME_OFF_OUTPUTS_2     41  // Outputs A9-A13
#define FRAME_OFF_SPEED_A1      42
#define FRAME_OFF_SPEED_A2      43
#define FRAME_OFF_SPEED_A6      44
#define FRAME_OFF_SPEED_A7      45
#define FRAME_OFF_HEAT_REG      46  // Heat meter register
#define FRAME_OFF_HEAT1         47  // Heat meter 1 (8 bytes)
#define FRAME_OFF_HEAT2         55  // Heat meter 2 (8 bytes)
#define FRAME_OFF_CHECKSUM      63

// Sensor type codes (upper nibble of high byte)
#define SENSOR_TYPE_UNUSED      0x00
#define SENSOR_TYPE_DIGITAL     0x10
#define SENSOR_TYPE_TEMP        0x20
#define SENSOR_TYPE_FLOW        0x30
#define SENSOR_TYPE_RADIATION   0x60
#define SENSOR_TYPE_ROOM        0x70

// =============================================================================
// Hardware Configuration
// =============================================================================
#define DL_BUS_GPIO         GPIO_NUM_26

// =============================================================================
// DL-Bus Timing Constants (in microseconds)
// =============================================================================
#define BIT_DURATION_US     2048        // One bit = 2.048 ms
#define HALF_BIT_US         1024        // Half bit = 1.024 ms
#define TOLERANCE_US        200         // Timing tolerance

// Pulse classification thresholds
#define SHORT_PULSE_MIN     (HALF_BIT_US - TOLERANCE_US)    // ~824 us
#define SHORT_PULSE_MAX     (HALF_BIT_US + TOLERANCE_US)    // ~1224 us
#define LONG_PULSE_MIN      (BIT_DURATION_US - TOLERANCE_US) // ~1848 us
#define LONG_PULSE_MAX      (BIT_DURATION_US + TOLERANCE_US) // ~2248 us

// =============================================================================
// Frame Configuration
// =============================================================================
#define SYNC_BITS           16          // SYNC = 16 high bits
#define SYNC_HALF_BITS      32          // = 32 half-bit periods
#define FRAME_BYTES         64          // 64 data bytes after SYNC
#define BITS_PER_BYTE       10          // 1 start + 8 data + 1 stop

// =============================================================================
// Buffer Sizes
// =============================================================================
// Frame = SYNC(16 bits) + 64 bytes × 10 bits = 656 bits × ~2 pulses/bit = ~1312 pulses
// 1.5 frames = ~2000 pulses, use 2048 for circular buffer
#define EDGE_BUFFER_SIZE    2048

// Bit queue: 64 bytes x 10 bits = 640 bits/frame, use 1024 for margin
#define BIT_QUEUE_SIZE      1024

// Frame queue: hold 2-3 complete frames
#define FRAME_QUEUE_SIZE    3

// =============================================================================
// Data Types
// =============================================================================

// Pulse classification
typedef enum {
    PULSE_SHORT,        // Half-bit pulse (~1024 us)
    PULSE_LONG,         // Full-bit pulse (~2048 us)
    PULSE_INVALID       // Out of expected range
} pulse_type_t;

// Edge entry in circular buffer
typedef struct {
    uint16_t duration;  // Pulse duration in us
    uint8_t level;      // 0 = low, 1 = high
} edge_t;

// Circular buffer for edges
typedef struct {
    edge_t buffer[EDGE_BUFFER_SIZE];
    volatile uint32_t head;         // Write position (producer)
    volatile uint32_t tail;         // Read position (consumer)
    SemaphoreHandle_t mutex;        // Protect concurrent access
    SemaphoreHandle_t data_ready;   // Signal data available
} edge_buffer_t;

// Bit entry for queue
typedef struct {
    uint8_t value;      // 0 or 1
    uint8_t is_sync;    // 1 if this bit is part of SYNC marker
} bit_entry_t;

// Complete decoded frame
typedef struct {
    uint8_t data[FRAME_BYTES];
    uint8_t length;
    uint8_t valid;      // Checksum OK
    uint32_t timestamp; // Tick count when received
} dlbus_frame_t;

// =============================================================================
// Global Variables
// =============================================================================

// GPIO interrupt capture state
static volatile int64_t last_edge_time = 0;
static volatile int last_level = -1;

// Pipeline buffers/queues
static edge_buffer_t edge_buffer;
static QueueHandle_t bit_queue;
static QueueHandle_t frame_queue;

// Task handles
static TaskHandle_t bit_extractor_task_handle = NULL;
static TaskHandle_t manchester_decoder_task_handle = NULL;

// Statistics
static volatile uint32_t stat_edges_received = 0;
static volatile uint32_t stat_bits_extracted = 0;
static volatile uint32_t stat_syncs_detected = 0;
static volatile uint32_t stat_frames_decoded = 0;
static volatile uint32_t stat_frames_valid = 0;
static volatile uint32_t stat_errors = 0;
static volatile uint32_t stat_gaps_detected = 0;
static volatile uint32_t stat_short_pulses = 0;
static volatile uint32_t stat_long_pulses = 0;
static volatile uint32_t stat_invalid_pulses = 0;

// Debug: last frame bit count
static volatile uint32_t debug_last_frame_bits = 0;
static volatile uint32_t debug_bits_since_sync = 0;

// =============================================================================
// Edge Circular Buffer Functions
// =============================================================================

static void edge_buffer_init(edge_buffer_t *eb)
{
    memset(eb->buffer, 0, sizeof(eb->buffer));
    eb->head = 0;
    eb->tail = 0;
    eb->mutex = xSemaphoreCreateMutex();
    eb->data_ready = xSemaphoreCreateBinary();
}

static inline uint32_t edge_buffer_count(edge_buffer_t *eb)
{
    uint32_t head = eb->head;
    uint32_t tail = eb->tail;
    if (head >= tail) {
        return head - tail;
    }
    return EDGE_BUFFER_SIZE - tail + head;
}

static inline bool edge_buffer_is_full(edge_buffer_t *eb)
{
    return edge_buffer_count(eb) >= (EDGE_BUFFER_SIZE - 1);
}

static inline bool edge_buffer_is_empty(edge_buffer_t *eb)
{
    return eb->head == eb->tail;
}

// Add edge to buffer (producer side - ISR safe with mutex)
static bool edge_buffer_put(edge_buffer_t *eb, uint16_t duration, uint8_t level)
{
    if (edge_buffer_is_full(eb)) {
        return false;  // Buffer overflow
    }

    uint32_t next_head = (eb->head + 1) % EDGE_BUFFER_SIZE;
    eb->buffer[eb->head].duration = duration;
    eb->buffer[eb->head].level = level;
    eb->head = next_head;

    return true;
}

// Get edge from buffer (consumer side)
static bool edge_buffer_get(edge_buffer_t *eb, edge_t *edge)
{
    if (edge_buffer_is_empty(eb)) {
        return false;
    }

    *edge = eb->buffer[eb->tail];
    eb->tail = (eb->tail + 1) % EDGE_BUFFER_SIZE;

    return true;
}


// =============================================================================
// Pulse Classification
// =============================================================================

static pulse_type_t classify_pulse(uint32_t duration_us)
{
    if (duration_us >= SHORT_PULSE_MIN && duration_us <= SHORT_PULSE_MAX) {
        return PULSE_SHORT;
    } else if (duration_us >= LONG_PULSE_MIN && duration_us <= LONG_PULSE_MAX) {
        return PULSE_LONG;
    }
    return PULSE_INVALID;
}

// =============================================================================
// GPIO Interrupt Handler
// =============================================================================

/**
 * GPIO ISR - captures edge timestamps for DL-Bus decoding
 *
 * Optimized for minimal latency:
 * - No semaphore signaling (task polls instead)
 * - Inline buffer write
 * - Minimal branching
 */
static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    int64_t now = esp_timer_get_time();
    int level = gpio_get_level(DL_BUS_GPIO);
    int64_t prev = last_edge_time;

    last_edge_time = now;

    if (prev > 0) {
        uint32_t duration = (uint32_t)(now - prev);

        // Filter: valid pulses are 500-10000us (more tolerant range)
        if (duration >= 500 && duration <= 10000) {
            // Inline buffer write for speed
            uint32_t head = edge_buffer.head;
            uint32_t next = (head + 1) % EDGE_BUFFER_SIZE;
            if (next != edge_buffer.tail) {  // Not full
                edge_buffer.buffer[head].duration = (uint16_t)duration;
                edge_buffer.buffer[head].level = !level;  // Level that just ended
                edge_buffer.head = next;
                stat_edges_received++;
            }
        }
    }
}

/**
 * Initialize GPIO interrupt for DL-Bus capture
 * ISR is pinned to Core 1 to avoid WiFi interference (WiFi runs on Core 0)
 */
static esp_err_t init_gpio_capture(void)
{
    ESP_LOGI(TAG, "Initializing GPIO interrupt capture on GPIO%d (Core 1)", DL_BUS_GPIO);

    // Configure GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << DL_BUS_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,  // Trigger on both rising and falling edges
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    // Install GPIO ISR service with high priority, pinned to Core 1
    // ESP_INTR_FLAG_IRAM: ISR in IRAM for fast execution
    // ESP_INTR_FLAG_LEVEL3: High priority interrupt
    int intr_flags = ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL3;
    ESP_ERROR_CHECK(gpio_install_isr_service(intr_flags));

    // Add handler for our GPIO
    ESP_ERROR_CHECK(gpio_isr_handler_add(DL_BUS_GPIO, gpio_isr_handler, NULL));

    // Initialize timing
    last_edge_time = 0;
    last_level = gpio_get_level(DL_BUS_GPIO);

    ESP_LOGI(TAG, "GPIO interrupt capture initialized");
    return ESP_OK;
}

// =============================================================================
// Bit Extractor Task
// =============================================================================

/**
 * Bit Extractor Task
 *
 * Reads edges from circular buffer, classifies pulses, and extracts bits
 * using Manchester-like decoding based on pulse durations.
 *
 * DL-Bus encoding (after XOR with clock and transistor inversion):
 * - SHORT pulse (~1024us) = half-bit, same bit value continues
 * - LONG pulse (~2048us) = full-bit boundary crossing
 *
 * We track accumulated time and extract bits at bit boundaries.
 */
static void bit_extractor_task(void *arg)
{
    ESP_LOGI(TAG, "Bit extractor task started");

    int current_level = 1;  // Assume starting HIGH (idle)
    int half_bit_count = 0;
    uint32_t bits_this_frame = 0;

    while (1) {
        // Poll for data (more efficient than semaphore for continuous stream)
        if (edge_buffer_is_empty(&edge_buffer)) {
            vTaskDelay(1);  // Minimal delay when idle
            continue;
        }

        // Process all available edges
        edge_t edge;
        while (edge_buffer_get(&edge_buffer, &edge)) {
            pulse_type_t ptype = classify_pulse(edge.duration);

            // Count half-bits based on pulse duration
            int half_bits = 0;
            if (ptype == PULSE_SHORT) {
                half_bits = 1;
                stat_short_pulses++;
            } else if (ptype == PULSE_LONG) {
                half_bits = 2;
                stat_long_pulses++;
            } else if (edge.duration > LONG_PULSE_MAX && edge.duration < 5000) {
                // Slightly longer pulse - estimate half-bits
                half_bits = (edge.duration + HALF_BIT_US / 2) / HALF_BIT_US;
                stat_long_pulses++;
            } else if (edge.duration >= 5000) {
                // Gap detected - always log for debugging
                stat_gaps_detected++;
                ESP_LOGW(TAG, "GAP: %lu us, bits_so_far=%lu, half_bits_pending=%d, level=%d",
                         (unsigned long)edge.duration, (unsigned long)bits_this_frame,
                         half_bit_count, edge.level);
                if (bits_this_frame > 0) {
                    debug_last_frame_bits = bits_this_frame;
                }
                half_bit_count = 0;
                bits_this_frame = 0;
                continue;
            } else {
                // Too short - noise, skip
                stat_invalid_pulses++;
                continue;
            }

            half_bit_count += half_bits;
            current_level = edge.level;

            // Extract bits at each 2 half-bit boundary
            while (half_bit_count >= 2) {
                half_bit_count -= 2;

                bit_entry_t bit;
                bit.value = current_level ? 1 : 0;
                bit.is_sync = 0;

                // Send to queue (non-blocking, drop if full)
                if (xQueueSend(bit_queue, &bit, 0) == pdTRUE) {
                    stat_bits_extracted++;
                    bits_this_frame++;
                    debug_bits_since_sync = bits_this_frame;
                }
            }
        }

        // Small yield
        taskYIELD();
    }
}

// =============================================================================
// Manchester Decoder Task
// =============================================================================

/**
 * Manchester Decoder Task
 *
 * Reads bits from queue, detects SYNC pattern, and assembles bytes.
 *
 * SYNC: 16 consecutive 1-bits (or 0-bits if inverted)
 * Byte: start(0) + D0 + D1 + D2 + D3 + D4 + D5 + D6 + D7 + stop(1)
 */
typedef enum {
    DECODER_STATE_HUNTING_SYNC,
    DECODER_STATE_IN_BYTE,
} decoder_state_t;

static void manchester_decoder_task(void *arg)
{
    ESP_LOGI(TAG, "Manchester decoder task started");

    decoder_state_t state = DECODER_STATE_HUNTING_SYNC;
    int consecutive_ones = 0;
    int consecutive_zeros = 0;
    int bit_index = 0;          // Bit position within byte (0-9)
    int byte_index = 0;         // Byte position within frame
    uint8_t current_byte = 0;
    bool invert_data = false;   // If SYNC was zeros, invert subsequent data
    dlbus_frame_t frame;
    uint32_t total_bits_in_frame = 0;  // Debug: count all bits since SYNC

    bit_entry_t bit;

    while (1) {
        // Get next bit (block with timeout)
        if (xQueueReceive(bit_queue, &bit, pdMS_TO_TICKS(100)) != pdTRUE) {
            // Timeout - if we were in a frame, it's probably incomplete
            if (state == DECODER_STATE_IN_BYTE && byte_index > 0) {
                ESP_LOGW(TAG, "Frame timeout at byte %d, bit %d (total bits: %lu, need %d)",
                         byte_index, bit_index, (unsigned long)total_bits_in_frame, FRAME_BYTES * 10);
                // Print partial frame for debugging
                if (byte_index >= 2) {
                    ESP_LOGW(TAG, "Partial frame: ID=0x%02X, inv=0x%02X, last_byte[%d]=0x%02X",
                             frame.data[0], frame.data[1], byte_index-1, frame.data[byte_index-1]);
                }
                state = DECODER_STATE_HUNTING_SYNC;
                consecutive_ones = 0;
                consecutive_zeros = 0;
                total_bits_in_frame = 0;
            }
            continue;
        }

        uint8_t bit_val = bit.value;

        switch (state) {
            case DECODER_STATE_HUNTING_SYNC:
                // Look for 16+ consecutive same bits
                if (bit_val == 1) {
                    consecutive_ones++;
                    consecutive_zeros = 0;
                } else {
                    consecutive_zeros++;
                    consecutive_ones = 0;
                }

                // SYNC detected?
                if (consecutive_ones >= SYNC_BITS) {
                    ESP_LOGI(TAG, "SYNC detected (%d ones)", consecutive_ones);
                    stat_syncs_detected++;
                    state = DECODER_STATE_IN_BYTE;
                    bit_index = 0;
                    byte_index = 0;
                    current_byte = 0;
                    invert_data = false;
                    total_bits_in_frame = 0;
                    memset(&frame, 0, sizeof(frame));
                    consecutive_ones = 0;
                    consecutive_zeros = 0;
                } else if (consecutive_zeros >= SYNC_BITS) {
                    ESP_LOGI(TAG, "SYNC detected (inverted, %d zeros)", consecutive_zeros);
                    stat_syncs_detected++;
                    state = DECODER_STATE_IN_BYTE;
                    bit_index = 0;
                    byte_index = 0;
                    current_byte = 0;
                    invert_data = true;  // Invert all subsequent bits
                    total_bits_in_frame = 0;
                    memset(&frame, 0, sizeof(frame));
                    consecutive_ones = 0;
                    consecutive_zeros = 0;
                }
                break;

            case DECODER_STATE_IN_BYTE:
                total_bits_in_frame++;

                // Apply inversion if needed
                if (invert_data) {
                    bit_val = !bit_val;
                }

                if (bit_index == 0) {
                    // Start bit - should be 0
                    if (bit_val != 0) {
                        // Not a valid start bit - might be still in SYNC
                        // or frame alignment lost
                        if (bit_val == 1) {
                            consecutive_ones++;
                            if (consecutive_ones < SYNC_BITS) {
                                // Still possibly trailing SYNC
                                continue;
                            }
                        }
                        // Lost sync
                        ESP_LOGW(TAG, "Bad start bit at byte %d, resync", byte_index);
                        state = DECODER_STATE_HUNTING_SYNC;
                        consecutive_ones = 0;
                        consecutive_zeros = 0;
                        continue;
                    }
                    consecutive_ones = 0;
                    bit_index++;
                } else if (bit_index <= 8) {
                    // Data bits (LSB first)
                    if (bit_val) {
                        current_byte |= (1 << (bit_index - 1));
                    }
                    bit_index++;
                } else {
                    // Stop bit (bit_index == 9) - should be 1
                    if (bit_val != 1) {
                        ESP_LOGW(TAG, "Bad stop bit at byte %d (0x%02X)", byte_index, current_byte);
                        // Continue anyway, might be noise
                    }

                    // Byte complete
                    if (byte_index < FRAME_BYTES) {
                        frame.data[byte_index] = current_byte;
                        // Debug: print first few bytes and periodically
                        if (byte_index < 3 || byte_index == 59 || byte_index == 63) {
                            ESP_LOGI(TAG, "  Byte[%d] = 0x%02X", byte_index, current_byte);
                        }
                        byte_index++;
                    }

                    // Reset for next byte
                    bit_index = 0;
                    current_byte = 0;

                    // Frame complete?
                    if (byte_index >= FRAME_BYTES) {
                        frame.length = byte_index;
                        frame.timestamp = xTaskGetTickCount();

                        // Validate checksum (sum of bytes 0-62 mod 256 == byte 63)
                        uint8_t checksum = 0;
                        for (int i = 0; i < 63; i++) {
                            checksum += frame.data[i];
                        }
                        frame.valid = (checksum == frame.data[63]);

                        if (frame.valid) {
                            stat_frames_valid++;
                        }
                        stat_frames_decoded++;

                        // Send to frame queue
                        if (xQueueSend(frame_queue, &frame, 0) != pdTRUE) {
                            ESP_LOGW(TAG, "Frame queue full, dropping frame");
                        }

                        // Go back to hunting SYNC
                        state = DECODER_STATE_HUNTING_SYNC;
                    }
                }
                break;
        }
    }
}

// =============================================================================
// Sensor/Output Parsing Functions
// =============================================================================

/**
 * Get sensor type from high byte
 * Type is encoded in bits 4-6 of the high byte
 */
static uint8_t get_sensor_type(uint8_t high_byte)
{
    return high_byte & 0x70;  // Mask bits 4-6
}

/**
 * Get unit string for sensor type
 */
static const char* get_sensor_unit(uint8_t sensor_type)
{
    uint8_t idx = (sensor_type >> 4) & 0x07;
    if (idx < NUM_SENSOR_TYPES) {
        return sensor_units[idx];
    }
    return "";
}

/**
 * Decode sensor value based on type
 * Returns the value as a float, sets *valid to indicate if sensor is active
 *
 * Encoding per type:
 * - Temperature (0x20): 12-bit signed value in low byte + bits 0-3 of high byte
 * - Room sensor (0x70): 8-bit value in low byte only (bits 0-3 of high byte = mode)
 * - Flow (0x30): 12-bit unsigned value × 4 l/h
 * - Radiation (0x60): 12-bit unsigned value in W/m²
 */
static float decode_sensor_value(uint8_t low_byte, uint8_t high_byte, bool *valid)
{
    uint8_t sensor_type = get_sensor_type(high_byte);
    *valid = (sensor_type != SENSOR_TYPE_UNUSED);

    if (!*valid) {
        return 0.0f;
    }

    switch (sensor_type) {
        case SENSOR_TYPE_DIGITAL:
            // Digital: bit 7 of high byte = ON/OFF
            return (high_byte & 0x80) ? 1.0f : 0.0f;

        case SENSOR_TYPE_TEMP: {
            // Temperature: 12-bit signed value (low byte + bits 0-3 of high byte)
            int16_t raw_value = low_byte | ((high_byte & 0x0F) << 8);
            // Check sign bit (bit 7 of high byte indicates negative)
            if (high_byte & 0x80) {
                raw_value = raw_value - 4096;  // 12-bit two's complement
            }
            return raw_value / 10.0f;
        }

        case SENSOR_TYPE_ROOM: {
            // Room sensor: 8-bit value in low byte only
            // Bits 0-3 of high byte contain operating mode, not temperature data
            // Bit 7 of high byte is sign bit
            int16_t raw_value = low_byte;
            if (high_byte & 0x80) {
                raw_value = raw_value - 256;  // 8-bit two's complement
            }
            return raw_value / 10.0f;
        }

        case SENSOR_TYPE_FLOW:
            // Flow rate: 12-bit unsigned × 4 l/h
            return (low_byte | ((high_byte & 0x0F) << 8)) * 4.0f;

        case SENSOR_TYPE_RADIATION:
            // Radiation: 12-bit unsigned in W/m²
            return (float)(low_byte | ((high_byte & 0x0F) << 8));

        default:
            return 0.0f;
    }
}

/**
 * Check if output is ON (from output state bytes)
 */
static bool is_output_on(const uint8_t *frame_data, int output_num)
{
    if (output_num < 1 || output_num > NUM_OUTPUTS) {
        return false;
    }

    if (output_num <= 8) {
        // Outputs A1-A8 in byte 40
        return (frame_data[FRAME_OFF_OUTPUTS_1] >> (output_num - 1)) & 0x01;
    } else {
        // Outputs A9-A13 in byte 41
        return (frame_data[FRAME_OFF_OUTPUTS_2] >> (output_num - 9)) & 0x01;
    }
}

/**
 * Decode speed level value
 * Returns speed value (0-30), sets *active to indicate if speed control is active
 */
static int decode_speed_level(uint8_t speed_byte, bool *active)
{
    // Bit 5: 0 = active, 1 = inactive
    *active = !(speed_byte & 0x20);
    // Bits 0-4: speed value (0-30)
    return speed_byte & 0x1F;
}

/**
 * Decode heat meter power (instantaneous)
 * Returns power in kW
 */
static float decode_heat_power(const uint8_t *heat_data)
{
    // Bytes 0-3: power in 1/100 kW (special encoding for byte 0)
    uint32_t power_raw = heat_data[0] | (heat_data[1] << 8) |
                         (heat_data[2] << 16) | (heat_data[3] << 24);
    return power_raw / 100.0f;
}

/**
 * Decode heat meter energy
 * Returns total energy in kWh
 */
static float decode_heat_energy(const uint8_t *heat_data)
{
    // Bytes 4-5: kWh (1/10 kWh)
    uint16_t kwh_raw = heat_data[4] | (heat_data[5] << 8);
    // Bytes 6-7: MWh
    uint16_t mwh = heat_data[6] | (heat_data[7] << 8);

    return (mwh * 1000.0f) + (kwh_raw / 10.0f);
}

// =============================================================================
// Frame Processing (Main Task)
// =============================================================================

static void print_frame(const dlbus_frame_t *frame)
{
    printf("\n==================== UVR1611 Frame #%lu ====================\n",
           (unsigned long)stat_frames_valid);

    // Timestamp
    int minute = frame->data[FRAME_OFF_MINUTE];
    int hour = frame->data[FRAME_OFF_HOUR] & 0x1F;
    int dst = (frame->data[FRAME_OFF_HOUR] >> 5) & 1;
    int day = frame->data[FRAME_OFF_DAY];
    int month = frame->data[FRAME_OFF_MONTH];
    int year = 2000 + frame->data[FRAME_OFF_YEAR];
    printf("Timestamp: %04d-%02d-%02d %02d:%02d (DST: %s)\n",
           year, month, day, hour, minute, dst ? "Yes" : "No");

    // Device info
    printf("Device ID: 0x%02X (%s)\n", frame->data[FRAME_OFF_DEVICE_ID],
           frame->data[FRAME_OFF_DEVICE_ID] == 0x80 ? "Valid" : "Invalid");
    printf("Checksum:  %s\n", frame->valid ? "OK" : "INVALID");

    // ==========================================================================
    // SENSORS
    // ==========================================================================
    printf("\nSENSORS:\n");
    printf("--------\n");

    for (int i = 0; i < NUM_SENSORS; i++) {
        int byte_offset = FRAME_OFF_SENSORS + (i * 2);
        uint8_t low_byte = frame->data[byte_offset];
        uint8_t high_byte = frame->data[byte_offset + 1];

        bool valid;
        float value = decode_sensor_value(low_byte, high_byte, &valid);
        uint8_t sensor_type = get_sensor_type(high_byte);
        const char *unit = get_sensor_unit(sensor_type);

        // Skip unused sensors (marked with "---")
        if (strcmp(sensor_names[i], "---") == 0) {
            continue;
        }

        printf("  S%-2d %-16s: ", i + 1, sensor_names[i]);

        if (!valid || sensor_type == SENSOR_TYPE_UNUSED) {
            printf("(unused)\n");
        } else if (sensor_type == SENSOR_TYPE_DIGITAL) {
            printf("%s\n", value > 0 ? "ON" : "OFF");
        } else if (sensor_type == SENSOR_TYPE_TEMP || sensor_type == SENSOR_TYPE_ROOM) {
            printf("%6.1f%s\n", value, unit);
        } else if (sensor_type == SENSOR_TYPE_FLOW) {
            printf("%6.0f%s\n", value, unit);
        } else if (sensor_type == SENSOR_TYPE_RADIATION) {
            printf("%6.0f%s\n", value, unit);
        } else {
            printf("%6.1f%s\n", value, unit);
        }
    }

    // ==========================================================================
    // OUTPUTS
    // ==========================================================================
    printf("\nOUTPUTS:\n");
    printf("--------\n");

    for (int i = 0; i < NUM_OUTPUTS; i++) {
        bool is_on = is_output_on(frame->data, i + 1);
        printf("  A%-2d %-16s: %s\n", i + 1, output_names[i], is_on ? "EIN" : "AUS");
    }

    // ==========================================================================
    // SPEED LEVELS
    // ==========================================================================
    printf("\nSPEED LEVELS:\n");
    printf("-------------\n");

    // Speed level byte offsets: A1=42, A2=43, A6=44, A7=45
    static const int speed_offsets[NUM_SPEED_LEVELS] = {
        FRAME_OFF_SPEED_A1, FRAME_OFF_SPEED_A2, FRAME_OFF_SPEED_A6, FRAME_OFF_SPEED_A7
    };

    for (int i = 0; i < NUM_SPEED_LEVELS; i++) {
        bool active;
        int speed = decode_speed_level(frame->data[speed_offsets[i]], &active);
        printf("  %-16s: ", speed_level_names[i]);
        if (active) {
            printf("%d\n", speed);
        } else {
            printf("--- (inactive)\n");
        }
    }

    // ==========================================================================
    // HEAT METERS
    // ==========================================================================
    printf("\nHEAT METERS:\n");
    printf("------------\n");

    uint8_t heat_reg = frame->data[FRAME_OFF_HEAT_REG];

    // Heat meter 1
    if (heat_reg & 0x01) {
        float power1 = decode_heat_power(&frame->data[FRAME_OFF_HEAT1]);
        float energy1 = decode_heat_energy(&frame->data[FRAME_OFF_HEAT1]);
        printf("  Heat Meter 1:\n");
        printf("    Power:  %7.2f kW\n", power1);
        printf("    Energy: %9.1f kWh (%.1f MWh)\n", energy1, energy1 / 1000.0f);
    } else {
        printf("  Heat Meter 1: (inactive)\n");
    }

    // Heat meter 2
    if (heat_reg & 0x02) {
        float power2 = decode_heat_power(&frame->data[FRAME_OFF_HEAT2]);
        float energy2 = decode_heat_energy(&frame->data[FRAME_OFF_HEAT2]);
        printf("  Heat Meter 2:\n");
        printf("    Power:  %7.2f kW\n", power2);
        printf("    Energy: %9.1f kWh (%.1f MWh)\n", energy2, energy2 / 1000.0f);
    } else {
        printf("  Heat Meter 2: (inactive)\n");
    }

    // ==========================================================================
    // STATISTICS
    // ==========================================================================
    printf("\nSTATISTICS:\n");
    printf("-----------\n");
    printf("  Good Frames: %lu\n", (unsigned long)stat_frames_valid);
    printf("  Bad Frames:  %lu\n", (unsigned long)(stat_frames_decoded - stat_frames_valid));
    if (stat_frames_decoded > 0) {
        printf("  Error Rate:  %.2f%%\n",
               100.0f * (stat_frames_decoded - stat_frames_valid) / stat_frames_decoded);
    }

    printf("============================================================\n\n");
}

// =============================================================================
// MQTT Frame Processing
// =============================================================================

/**
 * Process frame data for MQTT publishing
 * Extracts all values and sends to MQTT subsystem
 */
static void process_frame_for_mqtt(const dlbus_frame_t *frame)
{
    // Only process valid frames
    if (!frame->valid) {
        return;
    }

    // ==========================================================================
    // Process Sensor Values (add samples for median calculation)
    // ==========================================================================
    for (int i = 0; i < NUM_SENSORS; i++) {
        // Skip unused sensors
        if (strcmp(sensor_names[i], "---") == 0) {
            continue;
        }

        int byte_offset = FRAME_OFF_SENSORS + (i * 2);
        uint8_t low_byte = frame->data[byte_offset];
        uint8_t high_byte = frame->data[byte_offset + 1];

        bool valid;
        float value = decode_sensor_value(low_byte, high_byte, &valid);

        if (valid) {
            mqtt_ha_add_sensor_sample(i, value);
        }
    }

    // ==========================================================================
    // Process Output States (publish immediately on change)
    // ==========================================================================
    for (int i = 0; i < NUM_OUTPUTS; i++) {
        bool is_on = is_output_on(frame->data, i + 1);
        mqtt_ha_update_output(i, is_on);
    }

    // ==========================================================================
    // Process Speed Levels (publish immediately on change)
    // ==========================================================================
    static const int speed_offsets[NUM_SPEED_LEVELS] = {
        FRAME_OFF_SPEED_A1, FRAME_OFF_SPEED_A2, FRAME_OFF_SPEED_A6, FRAME_OFF_SPEED_A7
    };

    for (int i = 0; i < NUM_SPEED_LEVELS; i++) {
        bool active;
        int speed = decode_speed_level(frame->data[speed_offsets[i]], &active);
        mqtt_ha_update_speed(i, speed, active);
    }

    // ==========================================================================
    // Process Heat Meters
    // ==========================================================================
    uint8_t heat_reg = frame->data[FRAME_OFF_HEAT_REG];

    // Heat meter 1
    bool heat1_active = (heat_reg & 0x01) != 0;
    if (heat1_active) {
        float power1 = decode_heat_power(&frame->data[FRAME_OFF_HEAT1]);
        float energy1 = decode_heat_energy(&frame->data[FRAME_OFF_HEAT1]);
        mqtt_ha_publish_heat_meter(0, power1, energy1, true);
    }

    // Heat meter 2
    bool heat2_active = (heat_reg & 0x02) != 0;
    if (heat2_active) {
        float power2 = decode_heat_power(&frame->data[FRAME_OFF_HEAT2]);
        float energy2 = decode_heat_energy(&frame->data[FRAME_OFF_HEAT2]);
        mqtt_ha_publish_heat_meter(1, power2, energy2, true);
    }
}

// =============================================================================
// GPIO Debug Functions
// =============================================================================

static int read_gpio_level(void)
{
    return gpio_get_level(DL_BUS_GPIO);
}

static void debug_gpio_activity(int duration_ms)
{
    printf("\n=== GPIO%d ACTIVITY MONITOR (%d ms) ===\n", DL_BUS_GPIO, duration_ms);

    int last_level = read_gpio_level();
    int transitions = 0;
    int high_count = 0;
    int low_count = 0;
    int samples = 0;

    TickType_t start = xTaskGetTickCount();
    TickType_t end = start + pdMS_TO_TICKS(duration_ms);

    while (xTaskGetTickCount() < end) {
        int level = read_gpio_level();
        samples++;

        if (level) {
            high_count++;
        } else {
            low_count++;
        }

        if (level != last_level) {
            transitions++;
            last_level = level;
        }

        esp_rom_delay_us(10);
    }

    printf("  Samples:     %d\n", samples);
    printf("  Transitions: %d\n", transitions);
    printf("  High count:  %d (%.1f%%)\n", high_count, 100.0 * high_count / samples);
    printf("  Low count:   %d (%.1f%%)\n", low_count, 100.0 * low_count / samples);
    printf("  Final level: %d\n", read_gpio_level());

    if (transitions == 0) {
        printf("  WARNING: No signal transitions detected!\n");
    } else if (transitions < 10) {
        printf("  WARNING: Very few transitions\n");
    } else {
        printf("  Signal activity detected OK\n");
    }
    printf("=====================================\n\n");
}

static void init_gpio_for_debug(void)
{
    // Temporarily configure GPIO for activity monitoring (before interrupt setup)
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << DL_BUS_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
}


// =============================================================================
// Main Application
// =============================================================================

void app_main(void)
{
    printf("\n\n");
    printf("================================================\n");
    printf("  UVR1611 DL-Bus Reader - MQTT Gateway\n");
    printf("================================================\n");
    printf("GPIO:         %d\n", DL_BUS_GPIO);
    printf("Bit duration: %d us (488 Hz clock)\n", BIT_DURATION_US);
    printf("Frame size:   %d bytes\n", FRAME_BYTES);
    printf("================================================\n\n");

    // ==========================================================================
    // Initialize DL-Bus Capture FIRST (before WiFi to avoid timing interference)
    // ==========================================================================

    // Check signal activity before setting up interrupts
    init_gpio_for_debug();
    printf("Checking signal activity...\n");
    debug_gpio_activity(500);

    // Initialize circular buffer
    ESP_LOGI(TAG, "Initializing edge buffer (size=%d)", EDGE_BUFFER_SIZE);
    edge_buffer_init(&edge_buffer);

    // Create queues
    ESP_LOGI(TAG, "Creating bit queue (size=%d)", BIT_QUEUE_SIZE);
    bit_queue = xQueueCreate(BIT_QUEUE_SIZE, sizeof(bit_entry_t));
    if (bit_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create bit queue");
        return;
    }

    ESP_LOGI(TAG, "Creating frame queue (size=%d)", FRAME_QUEUE_SIZE);
    frame_queue = xQueueCreate(FRAME_QUEUE_SIZE, sizeof(dlbus_frame_t));
    if (frame_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create frame queue");
        return;
    }

    // Initialize GPIO interrupt capture
    ESP_ERROR_CHECK(init_gpio_capture());

    // Create DL-Bus tasks PINNED TO CORE 1 (WiFi runs on Core 0)
    // This prevents WiFi interrupts from interfering with timing-critical DL-Bus decoding
    ESP_LOGI(TAG, "Starting pipeline tasks on Core 1...");

    // Bit extractor task - high priority, pinned to Core 1
    xTaskCreatePinnedToCore(bit_extractor_task, "bit_extract", 4096, NULL,
                            configMAX_PRIORITIES - 2, &bit_extractor_task_handle, 1);

    // Manchester decoder task - medium-high priority, pinned to Core 1
    xTaskCreatePinnedToCore(manchester_decoder_task, "manchester", 4096, NULL,
                            configMAX_PRIORITIES - 3, &manchester_decoder_task_handle, 1);

    ESP_LOGI(TAG, "DL-Bus pipeline started on Core 1!");

    // Wait for DL-Bus pipeline to stabilize before starting WiFi
    // WiFi initialization causes significant interrupt activity
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "DL-Bus pipeline stabilized");

    // ==========================================================================
    // Initialize MQTT and WiFi (runs on Core 0)
    // ==========================================================================
    ESP_LOGI(TAG, "Initializing MQTT...");
    if (mqtt_ha_init() != ESP_OK) {
        ESP_LOGE(TAG, "MQTT initialization failed!");
        // Continue without MQTT - DL-Bus reading will still work
    } else {
        ESP_LOGI(TAG, "Starting WiFi and MQTT...");
        if (mqtt_ha_start() != ESP_OK) {
            ESP_LOGW(TAG, "MQTT start failed - continuing without MQTT");
        } else {
            const system_info_t *sys_info = mqtt_ha_get_system_info();
            ESP_LOGI(TAG, "WiFi connected: IP=%s, MAC=%s", sys_info->ip_address, sys_info->mac_address);
        }
    }

    printf("\n--- Waiting for frames ---\n\n");

    // Main loop - process decoded frames, MQTT, and print statistics
    dlbus_frame_t frame;
    TickType_t last_stats_time = xTaskGetTickCount();

    while (1) {
        // Check for decoded frames
        if (xQueueReceive(frame_queue, &frame, pdMS_TO_TICKS(100)) == pdTRUE) {
            // Print frame to serial
            print_frame(&frame);

            // Process frame for MQTT publishing
            process_frame_for_mqtt(&frame);
        }

        // Run MQTT loop (handles publish intervals)
        mqtt_ha_loop();

        // Print statistics every 10 seconds
        TickType_t now = xTaskGetTickCount();
        if ((now - last_stats_time) >= pdMS_TO_TICKS(10000)) {
            printf("\n--- Pipeline Statistics (GPIO Interrupt) ---\n");
            printf("  Edges captured:  %lu\n", (unsigned long)stat_edges_received);
            printf("  Pulses: short=%lu, long=%lu, invalid=%lu\n",
                   (unsigned long)stat_short_pulses, (unsigned long)stat_long_pulses,
                   (unsigned long)stat_invalid_pulses);
            printf("  Gaps detected:   %lu\n", (unsigned long)stat_gaps_detected);
            printf("  Bits extracted:  %lu\n", (unsigned long)stat_bits_extracted);
            printf("  Last frame bits: %lu (need %d)\n", (unsigned long)debug_last_frame_bits, FRAME_BYTES * 10);
            printf("  SYNCs detected:  %lu\n", (unsigned long)stat_syncs_detected);
            printf("  Frames decoded:  %lu\n", (unsigned long)stat_frames_decoded);
            printf("  Frames valid:    %lu\n", (unsigned long)stat_frames_valid);
            printf("  Edge buffer:     %lu/%d\n", (unsigned long)edge_buffer_count(&edge_buffer), EDGE_BUFFER_SIZE);
            printf("  Bit queue:       %lu/%d\n", (unsigned long)uxQueueMessagesWaiting(bit_queue), BIT_QUEUE_SIZE);
            printf("  MQTT connected:  %s\n", mqtt_ha_is_connected() ? "Yes" : "No");
            printf("--------------------------------------------\n\n");
            last_stats_time = now;
        }
    }
}
