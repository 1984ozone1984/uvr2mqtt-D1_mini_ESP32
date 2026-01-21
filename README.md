# UVR2MQTT-D1-RMT

ESP32-based DL-Bus reader for UVR1611 solar heating controller.

## Hardware

- **Board**: Azdelivery D1 Mini ESP32 (ESP32-WROOM-32)
- **Connection**: DL-Bus via optocoupler to GPIO26

## Features

- Reads UVR1611 DL-Bus data frames (64 bytes)
- GPIO interrupt-based edge capture (no RMT buffer limitations)
- Manchester-like decoding with automatic polarity detection
- SYNC detection, checksum validation
- Full frame parsing with timestamp

## Protocol

- Display clock: 488 Hz
- Bit duration: 2.048 ms
- Frame: 64 bytes + checksum
- Device ID: 0x80 (UVR1611)

## Building

Requires PlatformIO with ESP-IDF framework.

```bash
pio run
pio run -t upload
pio device monitor
```

## Status

**Phase 1 Complete**: Raw DL-Bus decoding working with valid checksums.

## License

MIT
