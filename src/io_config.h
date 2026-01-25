/**
 * UVR1611 DL-Bus Reader - I/O Configuration Header
 *
 * This file provides access to the Kconfig-defined sensor and output names.
 * Configure via: idf.py menuconfig -> "UVR1611 I/O Configuration"
 */

#ifndef IO_CONFIG_H
#define IO_CONFIG_H

#include "sdkconfig.h"

// =============================================================================
// Constants
// =============================================================================
#define NUM_SENSORS       16
#define NUM_OUTPUTS       13
#define NUM_SPEED_LEVELS  4
#define NUM_HEAT_METERS   2
#define NUM_SENSOR_TYPES  8

// =============================================================================
// Sensor Names Array (S1-S16)
// =============================================================================
#define SENSOR_NAMES { \
    CONFIG_SENSOR_S1_NAME, \
    CONFIG_SENSOR_S2_NAME, \
    CONFIG_SENSOR_S3_NAME, \
    CONFIG_SENSOR_S4_NAME, \
    CONFIG_SENSOR_S5_NAME, \
    CONFIG_SENSOR_S6_NAME, \
    CONFIG_SENSOR_S7_NAME, \
    CONFIG_SENSOR_S8_NAME, \
    CONFIG_SENSOR_S9_NAME, \
    CONFIG_SENSOR_S10_NAME, \
    CONFIG_SENSOR_S11_NAME, \
    CONFIG_SENSOR_S12_NAME, \
    CONFIG_SENSOR_S13_NAME, \
    CONFIG_SENSOR_S14_NAME, \
    CONFIG_SENSOR_S15_NAME, \
    CONFIG_SENSOR_S16_NAME \
}

// =============================================================================
// Output Names Array (A1-A13)
// =============================================================================
#define OUTPUT_NAMES { \
    CONFIG_OUTPUT_A1_NAME, \
    CONFIG_OUTPUT_A2_NAME, \
    CONFIG_OUTPUT_A3_NAME, \
    CONFIG_OUTPUT_A4_NAME, \
    CONFIG_OUTPUT_A5_NAME, \
    CONFIG_OUTPUT_A6_NAME, \
    CONFIG_OUTPUT_A7_NAME, \
    CONFIG_OUTPUT_A8_NAME, \
    CONFIG_OUTPUT_A9_NAME, \
    CONFIG_OUTPUT_A10_NAME, \
    CONFIG_OUTPUT_A11_NAME, \
    CONFIG_OUTPUT_A12_NAME, \
    CONFIG_OUTPUT_A13_NAME \
}

// =============================================================================
// Speed Level Names Array
// =============================================================================
#define SPEED_LEVEL_NAMES { \
    CONFIG_SPEED_A1_NAME, \
    CONFIG_SPEED_A2_NAME, \
    CONFIG_SPEED_A6_NAME, \
    CONFIG_SPEED_A7_NAME \
}

// =============================================================================
// Heat Meter Power Names Array
// =============================================================================
#define HEAT_METER_POWER_NAMES { \
    CONFIG_HEAT_METER_1_POWER_NAME, \
    CONFIG_HEAT_METER_2_POWER_NAME \
}

// =============================================================================
// Heat Meter Energy Names Array
// =============================================================================
#define HEAT_METER_ENERGY_NAMES { \
    CONFIG_HEAT_METER_1_ENERGY_NAME, \
    CONFIG_HEAT_METER_2_ENERGY_NAME \
}

// =============================================================================
// Sensor Unit Strings Array
// Index: (type_byte >> 4)
// =============================================================================
#define SENSOR_UNITS { \
    CONFIG_UNIT_UNUSED, \
    CONFIG_UNIT_DIGITAL, \
    CONFIG_UNIT_TEMPERATURE, \
    CONFIG_UNIT_FLOW_RATE, \
    CONFIG_UNIT_RESERVED_40, \
    CONFIG_UNIT_RESERVED_50, \
    CONFIG_UNIT_RADIATION, \
    CONFIG_UNIT_ROOM_SENSOR \
}

#endif /* IO_CONFIG_H */
