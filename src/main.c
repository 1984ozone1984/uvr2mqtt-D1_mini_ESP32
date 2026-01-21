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

static const char *TAG = "DL-BUS";

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
 */
static esp_err_t init_gpio_capture(void)
{
    ESP_LOGI(TAG, "Initializing GPIO interrupt capture on GPIO%d", DL_BUS_GPIO);

    // Configure GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << DL_BUS_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,  // Trigger on both rising and falling edges
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    // Install GPIO ISR service with high priority
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL3));

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
// Frame Processing (Main Task)
// =============================================================================

static void print_frame(const dlbus_frame_t *frame)
{
    printf("\n========================================\n");
    printf("DL-BUS FRAME (tick=%lu)\n", (unsigned long)frame->timestamp);
    printf("========================================\n");

    // Print raw hex
    printf("RAW (%d bytes):\n", frame->length);
    for (int i = 0; i < frame->length; i++) {
        printf("%02X ", frame->data[i]);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    if (frame->length % 16 != 0) printf("\n");

    // Key fields
    printf("\nKey Fields:\n");
    printf("  Device ID:     0x%02X %s\n", frame->data[0],
           frame->data[0] == 0x80 ? "(UVR1611)" : "");
    printf("  ID Inverted:   0x%02X %s\n", frame->data[1],
           frame->data[1] == 0x7F ? "(OK)" : "(unexpected)");
    printf("  Checksum:      %s\n", frame->valid ? "VALID" : "INVALID");

    // Timestamp (if present)
    if (frame->length >= 9) {
        int minute = frame->data[3];
        int hour = frame->data[4] & 0x1F;
        int dst = (frame->data[4] >> 5) & 1;
        int day = frame->data[5];
        int month = frame->data[6];
        int year = 2000 + frame->data[7];
        printf("  Time:          %04d-%02d-%02d %02d:%02d (DST:%d)\n",
               year, month, day, hour, minute, dst);
    }

    printf("========================================\n\n");
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
    printf("  UVR1611 DL-Bus Reader - GPIO Interrupt Capture\n");
    printf("================================================\n");
    printf("GPIO:         %d\n", DL_BUS_GPIO);
    printf("Bit duration: %d us (488 Hz clock)\n", BIT_DURATION_US);
    printf("Frame size:   %d bytes\n", FRAME_BYTES);
    printf("================================================\n\n");

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

    // Initialize GPIO interrupt capture (replaces RMT)
    ESP_ERROR_CHECK(init_gpio_capture());

    // Create tasks
    ESP_LOGI(TAG, "Starting pipeline tasks...");

    // Bit extractor task
    xTaskCreate(bit_extractor_task, "bit_extract", 4096, NULL, 8, &bit_extractor_task_handle);

    // Manchester decoder task
    xTaskCreate(manchester_decoder_task, "manchester", 4096, NULL, 6, &manchester_decoder_task_handle);

    ESP_LOGI(TAG, "Pipeline started with GPIO interrupt capture!");
    printf("\n--- Waiting for frames ---\n\n");

    // Main loop - process decoded frames and print statistics
    dlbus_frame_t frame;
    TickType_t last_stats_time = xTaskGetTickCount();

    while (1) {
        // Check for decoded frames
        if (xQueueReceive(frame_queue, &frame, pdMS_TO_TICKS(1000)) == pdTRUE) {
            print_frame(&frame);
        }

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
            printf("--------------------------------------------\n\n");
            last_stats_time = now;
        }
    }
}
