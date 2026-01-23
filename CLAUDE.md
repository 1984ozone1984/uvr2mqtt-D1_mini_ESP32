# CLAUDE.md - Project Context for Claude Code

## Project Overview

**UVR2MQTT** - ESP32-based gateway that reads UVR1611 solar heating controller data via DL-Bus protocol and publishes to MQTT with Home Assistant auto-discovery.

## Hardware

- **Board**: Azdelivery D1 Mini ESP32 (WROOM-32)
- **DL-Bus Input**: GPIO26 (configurable via Kconfig)
- **Protocol**: 488 Hz Manchester-like encoding, 64-byte frames

## Architecture

### Core Allocation
- **Core 0**: WiFi stack, MQTT client, main task
- **Core 1**: GPIO ISR, Bit Extractor, Manchester Decoder (timing-critical)

### Data Pipeline
```
GPIO26 ISR -> Edge Buffer (2048) -> Bit Extractor Task -> Bit Queue (1024)
          -> Manchester Decoder Task -> Frame Queue (3) -> Main Task -> MQTT
```

## Key Files

| File | Description |
|------|-------------|
| `src/main.c` | DL-Bus capture, decode, frame parsing |
| `src/config_store.c` | NVS-backed configuration storage (secrets + settings) |
| `src/config_store.h` | Configuration API (thread-safe, secure) |
| `src/mqtt_ha.c` | WiFi, MQTT, Home Assistant discovery |
| `src/mqtt_ha.h` | MQTT module public API |
| `src/io_config.h` | Sensor/output name arrays from Kconfig |
| `src/Kconfig.projbuild` | ESP-IDF menuconfig definitions |

## Configuration System

### Security Model
- **Secrets (WiFi/MQTT credentials)**: Stored ONLY in NVS at runtime
  - Never in source code, headers, Kconfig, logs, or commits
  - Retrieved via copy-to-buffer API (never direct pointers)
- **Non-secrets**: Kconfig defaults with NVS override
  - Hostname, MQTT broker URI, publish intervals, GPIO pin

### Configuration Precedence
1. Check NVS for stored value
2. Fall back to Kconfig compile-time default
3. Secrets have NO defaults (must be provisioned)

### Key APIs
```c
// Non-secrets (safe to log)
const char *config_get_hostname(void);
const char *config_get_mqtt_broker_uri(void);
uint16_t config_get_publish_interval_sensors(void);

// Secrets (never logged, copy to caller buffer)
bool config_has_wifi_credentials(void);
esp_err_t config_get_wifi_ssid(char *buf, size_t len);
esp_err_t config_set_wifi_credentials(const char *ssid, const char *pass);
```

### NVS Namespaces
- `uvr_config`: Non-secret settings (hostname, intervals, etc.)
- `uvr_secret`: Credentials (WiFi SSID/password, MQTT user/password)

## Build System

- **Framework**: ESP-IDF via PlatformIO
- **Config**: `pio run -t menuconfig` for non-secret settings
- **Build**: External Mac with VSCode + PlatformIO (not built by Claude)

## Initial Setup (First Boot)

Since credentials are not in source code, they must be provisioned:

1. Build and flash firmware
2. Device starts but WiFi/MQTT fail (no credentials)
3. Use one of these methods to set credentials:
   - Web interface (when implemented)
   - Serial console provisioning tool
   - Direct NVS write via esptool

## Kconfig Options (Non-Secret)

Located in `src/Kconfig.projbuild`:

**Device Settings:**
- `UVR_HOSTNAME` - mDNS hostname (default: uvr1611-gateway)
- `UVR_DLBUS_GPIO` - DL-Bus input pin (default: 26)
- `UVR_WIFI_TIMEOUT` - Connection timeout (default: 30s)

**MQTT Settings:**
- `UVR_MQTT_BROKER_URI` - Broker address (default: mqtt://192.168.1.100:1883)
- `UVR_MQTT_CLIENT_ID` - Client identifier
- `UVR_MQTT_BASE_TOPIC` - Base topic (default: uvr1611)
- `UVR_HA_DISCOVERY_PREFIX` - HA discovery prefix (default: homeassistant)

**Publish Intervals:**
- `UVR_PUBLISH_INTERVAL_SENSORS` - Sensor values (default: 30s)
- `UVR_PUBLISH_INTERVAL_OUTPUTS` - Output states (default: 30s)
- `UVR_PUBLISH_INTERVAL_SYSTEM` - System info (default: 60s)

**I/O Names:**
- Sensor names S1-S16
- Output names A1-A13
- Speed level names

## DL-Bus Protocol Details

- **Bit duration**: 2048 us
- **Half-bit**: 1024 us
- **SYNC**: 16 consecutive high bits
- **Frame**: 64 bytes (device ID, timestamp, 16 sensors, 13 outputs, speeds, heat meters, checksum)
- **Byte format**: 1 start bit (0) + 8 data LSB first + 1 stop bit (1)

## MQTT Topics

```
uvr1611/status              -> online/offline (LWT)
uvr1611/sensor/{name}/state -> Sensor values (median)
uvr1611/switch/{name}/state -> Output states (ON/OFF)
uvr1611/system/mac          -> MAC address
uvr1611/system/ip           -> IP address
uvr1611/system/uptime       -> Uptime seconds
```

## Current Branch: feature/webserver-config

Adding web-based configuration interface:
- HTTP server for settings UI
- NVS-based runtime configuration (DONE)
- REST API for config read/write
- Live sensor/output data display

## Development Notes

- WiFi causes timing interference; DL-Bus tasks pinned to Core 1
- GPIO ISR uses direct register access (IRAM-safe)
- Timing tolerance increased to 350us for WiFi jitter compensation
- Sensor values published as median of samples between intervals
- Output states published immediately on change
- Credentials cleared from memory after use (security)

## Testing

User builds and uploads via external Mac. Claude does not build.
Changes are verified by user on actual hardware.
