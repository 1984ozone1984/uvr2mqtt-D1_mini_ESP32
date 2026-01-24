# UVR2MQTT - UVR1611 DL-Bus to MQTT Gateway

ESP32-based gateway that reads data from a Technische Alternative UVR1611 solar heating controller via DL-Bus and publishes to MQTT with Home Assistant auto-discovery.

## Hardware

- **Board**: Azdelivery D1 Mini ESP32 (ESP32-WROOM-32)
- **Connection**: DL-Bus via optocoupler to GPIO26

## Features

### DL-Bus Protocol
- GPIO interrupt-based edge capture (bypasses RMT 512-symbol limitation)
- Dedicated high-priority interrupt on Core 1 (away from WiFi on Core 0)
- Manchester-like decoding with automatic polarity detection
- SYNC detection, checksum validation
- Full 64-byte frame parsing

### Sensor Data
- 16 temperature/flow/radiation sensors with configurable names
- 13 digital outputs (pumps, valves, mixers)
- 4 speed levels for variable-speed pumps
- 2 heat meters with power (kW) and energy (kWh)

### MQTT & Home Assistant
- WiFi connection with automatic reconnection
- MQTT client with Last Will and Testament (online/offline status)
- Home Assistant auto-discovery for all entities
- Configurable publish intervals
- Median filtering for sensor values
- Immediate publish on output state changes

## Configuration

### WiFi & MQTT Settings

Copy the example configuration file:
```bash
cp src/wifi_config.h.example src/wifi_config.h
```

Edit `src/wifi_config.h` with your settings:
```c
#define WIFI_SSID               "your-wifi-ssid"
#define WIFI_PASSWORD           "your-wifi-password"
#define MQTT_BROKER_URI         "mqtt://192.168.1.100:1883"
#define MQTT_USERNAME           "mqtt-user"
#define MQTT_PASSWORD           "mqtt-password"
```

### Sensor & Output Names

Configure via ESP-IDF menuconfig:
```bash
pio run -t menuconfig
```
Navigate to: "UVR1611 I/O Configuration"

## Building

Requires PlatformIO with ESP-IDF framework.

```bash
# Build
pio run

# Upload
pio run -t upload

# Monitor serial output
pio device monitor

# Configure sensor names
pio run -t menuconfig
```

## MQTT Topics

```
uvr1611/status                          # "online" / "offline" (LWT)
uvr1611/sensor/{sensor_name}/state      # Sensor value (median)
uvr1611/switch/{output_name}/state      # "ON" / "OFF"
uvr1611/sensor/heat_meter_1_power/state # Power in kW
uvr1611/sensor/heat_meter_1_energy/state # Energy in kWh
uvr1611/system/mac                      # MAC address
uvr1611/system/ip                       # IP address
uvr1611/system/uptime                   # Uptime in seconds
```

## Home Assistant

All entities are automatically discovered via MQTT discovery. They appear under a single device "UVR1611 Gateway" with:
- Temperature sensors with proper device class
- Binary sensors for outputs (pumps, valves)
- Numeric sensors for speed levels
- Power and energy sensors for heat meters

## Protocol Details

### DL-Bus Timing
- Display clock: 488 Hz
- Bit duration: 2.048 ms (2048 us)
- Half-bit duration: 1.024 ms (1024 us)

### Frame Structure
- SYNC: 16 high bits
- Data: 64 bytes (start bit + 8 data bits + stop bit per byte)
- Checksum: Sum of bytes 0-62 mod 256

### Sensor Types
| Type | Description | Encoding |
|------|-------------|----------|
| 0x00 | Unused | - |
| 0x10 | Digital | Bit 7 of high byte |
| 0x20 | Temperature | 12-bit signed, /10 for °C |
| 0x30 | Flow rate | 12-bit × 4 l/h |
| 0x60 | Radiation | 12-bit W/m² |
| 0x70 | Room sensor | 9-bit signed, /10 for °C |

## Architecture

```
GPIO26 (Core 1) ─► Edge Buffer ─► Bit Extractor ─► Manchester Decoder ─► Frame Queue
                                    (Core 1)          (Core 1)              │
                                                                            ▼
WiFi/MQTT (Core 0) ◄────────────────────────────────────────────── Main Loop
```

- DL-Bus capture runs entirely on Core 1 to avoid WiFi interference
- WiFi power save disabled for consistent interrupt timing
- Dedicated GPIO interrupt (not shared ISR service) for lower latency

## Status

**Phase 1 & 2 Complete**:
- DL-Bus decoding: 0% error rate
- All sensor types parsed correctly
- MQTT publishing working
- Home Assistant auto-discovery working

## License

MIT
