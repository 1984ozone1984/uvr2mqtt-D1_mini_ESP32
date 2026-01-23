# CLAUDE.md - Project Context for Claude Code

## Project Overview

**UVR2MQTT** - ESP32-based gateway that reads UVR1611 solar heating controller data via DL-Bus protocol and publishes to MQTT with Home Assistant auto-discovery.

## Hardware

- **Board**: Azdelivery D1 Mini ESP32 (WROOM-32)
- **DL-Bus Input**: GPIO26
- **Protocol**: 488 Hz Manchester-like encoding, 64-byte frames

## Architecture

### Core Allocation
- **Core 0**: WiFi stack, MQTT client, main task
- **Core 1**: GPIO ISR, Bit Extractor, Manchester Decoder (timing-critical)

### Data Pipeline
```
GPIO26 ISR → Edge Buffer (2048) → Bit Extractor Task → Bit Queue (1024)
          → Manchester Decoder Task → Frame Queue (3) → Main Task → MQTT
```

## Key Files

| File | Description |
|------|-------------|
| `src/main.c` | DL-Bus capture, decode, frame parsing |
| `src/mqtt_ha.c` | WiFi, MQTT, Home Assistant discovery |
| `src/mqtt_ha.h` | MQTT module public API |
| `src/io_config.h` | Sensor/output name arrays from Kconfig |
| `src/wifi_config.h` | WiFi/MQTT credentials (git-ignored) |
| `src/wifi_config.h.example` | Template for wifi_config.h |
| `src/Kconfig.projbuild` | ESP-IDF menuconfig definitions |

## Build System

- **Framework**: ESP-IDF via PlatformIO
- **Config**: `pio run -t menuconfig` for sensor/output names
- **Build**: External Mac with VSCode + PlatformIO (not built by Claude)

## Configuration

### wifi_config.h (create from .example)
- `WIFI_SSID`, `WIFI_PASSWORD`
- `MQTT_BROKER_URI`, `MQTT_USERNAME`, `MQTT_PASSWORD`
- `PUBLISH_INTERVAL_*` settings

### Kconfig.projbuild (via menuconfig)
- Sensor names S1-S16
- Output names A1-A13
- Speed level names
- Sensor units

## DL-Bus Protocol Details

- **Bit duration**: 2048 µs
- **Half-bit**: 1024 µs
- **SYNC**: 16 consecutive high bits
- **Frame**: 64 bytes (device ID, timestamp, 16 sensors, 13 outputs, speeds, heat meters, checksum)
- **Byte format**: 1 start bit (0) + 8 data LSB first + 1 stop bit (1)

## MQTT Topics

```
uvr1611/status              → online/offline (LWT)
uvr1611/sensor/{name}/state → Sensor values (median)
uvr1611/switch/{name}/state → Output states (ON/OFF)
uvr1611/system/mac          → MAC address
uvr1611/system/ip           → IP address
uvr1611/system/uptime       → Uptime seconds
```

## Current Branch: feature/webserver-config

Adding web-based configuration interface:
- HTTP server for settings UI
- NVS-based runtime configuration
- REST API for config read/write
- Live sensor/output data display

## Development Notes

- WiFi causes timing interference; DL-Bus tasks pinned to Core 1
- GPIO ISR uses direct register access (IRAM-safe)
- Timing tolerance increased to 350µs for WiFi jitter compensation
- Sensor values published as median of samples between intervals
- Output states published immediately on change

## Testing

User builds and uploads via external Mac. Claude does not build.
Changes are verified by user on actual hardware.
