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
| `src/wifi_manager.c` | WiFi STA/AP mode management with auto-fallback |
| `src/wifi_manager.h` | WiFi manager public API |
| `src/webserver.c` | HTTP server for web-based configuration UI |
| `src/webserver.h` | Webserver public API |
| `src/mqtt_ha.c` | MQTT client, Home Assistant discovery |
| `src/mqtt_ha.h` | MQTT module public API |
| `src/io_config.h` | Sensor/output name defaults from Kconfig |
| `src/Kconfig.projbuild` | ESP-IDF menuconfig definitions |
| `partitions.csv` | OTA-compatible partition table |

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

// I/O names (editable via web UI)
const char *config_get_sensor_name(int index);  // 0-15 for S1-S16
esp_err_t config_set_sensor_name(int index, const char *name);
const char *config_get_output_name(int index);  // 0-12 for A1-A13
esp_err_t config_set_output_name(int index, const char *name);
const char *config_get_speed_name(int index);   // 0-3 for Drehzahlstufen
esp_err_t config_set_speed_name(int index, const char *name);
const char *config_get_heat_meter_power_name(int index);  // 0-1 for WMZ power
esp_err_t config_set_heat_meter_power_name(int index, const char *name);
const char *config_get_heat_meter_energy_name(int index); // 0-1 for WMZ energy
esp_err_t config_set_heat_meter_energy_name(int index, const char *name);

// Name validation (for MQTT filtering)
bool config_is_name_enabled(const char *name);  // true if has alphanumeric chars

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
2. Device starts in AP mode (no credentials configured)
3. Connect to WiFi network "UVR1611-XXXXXX" (XXXXXX = last 6 chars of MAC)
4. Open http://192.168.4.1 in browser
5. Configure WiFi credentials and MQTT settings via web UI
6. Device saves settings to NVS and reboots
7. Device connects to configured WiFi in STA mode
8. Access web UI via http://hostname.local or IP address

## WiFi Manager

The device supports two WiFi modes with automatic fallback:

**STA Mode (Station)**:
- Connects to configured WiFi network
- Provides mDNS at hostname.local
- MQTT publishing active
- Web UI accessible on local network

**AP Mode (Access Point)**:
- Activates when no credentials or connection fails
- SSID: "UVR1611-XXXXXX" (XXXXXX = MAC suffix)
- IP: 192.168.4.1
- Web UI for initial configuration
- DL-Bus reading continues (local only)

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
- `UVR_PUBLISH_INTERVAL_SENSORS` - Sensors, heat meters, system info (default: 30s)
- `UVR_PUBLISH_INTERVAL_OUTPUTS` - Output states (default: 30s)

**I/O Names (Kconfig defaults, editable via web UI):**
- Sensor names S1-S16
- Output names A1-A13
- Speed level names (Drehzahlstufen A1, A2, A6, A7)
- Heat meter names (WMZ1/WMZ2 power and energy)

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

## Home Assistant Auto-Discovery

Entities are automatically discovered via MQTT with proper device grouping.
**Only entities with configured names are published** - use `---` or empty name to disable.

**Name Filtering:**
- Names must contain at least one alphanumeric character (letter or number)
- Names with only `-` characters (like `---`) are disabled
- Empty names are disabled
- Disabled entities are not published to MQTT and not registered in HA

**Sensor Entities:**
- Temperature/flow sensors (S1-S16) with proper device_class
- Speed level sensors (Drehzahlstufen A1, A2, A6, A7)
- Heat meter power (kW) and energy (kWh)
- System uptime

**Binary Sensor Entities:**
- Output states (A1-A13) for pumps, valves, mixers

**Diagnostic Entities:**
- IP Address (entity_category: diagnostic)
- MAC Address (entity_category: diagnostic)
- Connectivity Status (binary_sensor with device_class: connectivity)

## Web Configuration Interface

The device provides a web-based configuration UI:

**Endpoints**:
- `GET /` - Main status page with sensor/output data
- `GET /config` - Configuration page
- `GET /api/status` - System status JSON
- `POST /config/wifi` - Update WiFi settings
- `POST /config/mqtt` - Update MQTT settings
- `POST /config/io` - Update sensor/output names
- `POST /ota` - Upload firmware update
- `POST /reboot` - Restart device

**Features**:
- Real-time sensor and output display
- WiFi and MQTT configuration forms
- Sensor and output name editing (S1-S16, A1-A13)
- Speed level name editing (Drehzahlstufen A1, A2, A6, A7)
- Heat meter name editing (WMZ1/WMZ2 power and energy)
- OTA firmware update with progress indicator
- Current firmware version display
- Settings saved to NVS (persist across reboots)
- Works in both STA and AP modes

## OTA Firmware Update

The device supports over-the-air firmware updates:

- Custom partition table with two OTA partitions (1.75MB each)
- Upload via web interface at `/config` page
- Validates firmware header before flashing
- Auto-reboot after successful update
- First flash requires USB; subsequent updates via OTA

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
