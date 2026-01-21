# Functional Specification Document: UVR1611 DL-Bus Reader (Simple Version)

## Project Goal
ESP32 reads UVR1611 DL-Bus data and outputs parsed values to Serial Monitor for testing and debugging.

## Hardware

### ESP32 Board
- **Azdelivery D1 Mini ESP32**
- SoC ESP32-WROOM-32
- CPU Xtensa® single-dual-core 32-bit LX6
- Chipset ESP32-D0WDQ6
- Clock frequency range 80MHz / 240MHz
- RAM 512kB
- External flash memory 4MB
- I/O pins 34
- ADC channels 18
- ADC Resolution 12-bit
- DAC channels 2
- DAC Resolution 8-bit
- Communication interfaces SPI, I2C, I2S, CAN, UART
- Max. current drawn per GPIO pin 40mA
- Wi-Fi protocols 802.11 b/g/n (802.11n up to 150 Mbps)
- Wi-Fi frequency 2.4 GHz - 2.5 GHz
- Bluetooth V4.2 - BLE and Classic Bluetooth
- Wireless antena PCB
- USB to serial chip CP2104
- Dimensions 39x28x6mm (1.5x1.1x0.2in)
- Documentation: `docs/D1 Mini ESP32_EN.pdf`

### Pin Configuration
- **DL-Bus RX Pin:** GPIO26
- **GND:** GND pin
- **Power:** 5V via USB-C or 3.3V from pin

### UVR1611 Connection
```
UVR1611 DL-Bus → Optocoupler Output → Azdelivery D1 Mini ESP32 GPIO26
                 Optocoupler GND    → Azdelivery D1 Mini ESP32 GND
```

## UVR1611 Protocol Specification

### Reference Documents
- **Protocol Specification**: `docs/Datenleitung.PDF`
- Complete DL-Bus interface documentation

### Protocol Basics
- **Display clock:** 488 Hz
- **Encoding Type:** Manchester
- **Polarity/Convention:** rising edge = 1 high
- **Sync:** 16 high bits Byte 0
- **Bit duration:** 2.048 ms
- **Frame duration:** ~1.35 seconds
- **Frame size:** 65 bytes total
- **Device ID:** 0x80 (UVR1611)
- **Device ID inverted:** 0x7F
- **Checksum:** Data Byte 64 (sum of bytes 1-63 mod 256)

sor type:**
```
x000 xxxx - Unused
x001 xxxx - Digital (Bit 7: 1=ON, 0=OFF)
x010 xxxx - Temperature (1/10 °C)
x011 xxxx - Flow rate (4 l/h)
x110 xxxx - Radiation (1 W/m²)
x111 xxxx - Room sensor (1/10 °C)
```

**Temperature Reconstruction:**
```c
if (highByte & 0x80) {  // Negative
    value = (lowByte + 256 * highByte - 65536) / 10.0;
} else {  // Positive
    value = (lowByte + 256 * highByte) / 10.0;
}
```

### Output States Decoding
**Byte 41 (Outputs A1-A8):**
```
Bit 0: Output A1
Bit 1: Output A2
Bit 2: Output A3
Bit 3: Output A4
Bit 4: Output A5
Bit 5: Output A6
Bit 6: Output A7
Bit 7: Output A8
```

**Byte 42 (Outputs A9-A13):**
```
Bit 0: Output A9
Bit 1: Output A10
Bit 2: Output A11
Bit 3: Output A12
Bit 4: Output A13
```

### Speed Levels Decoding
**Bytes 43, 44, 45, 46 (Speed levels A1, A2, A6, A7):**
```
Bits 0-4: Speed value (0-30)
Bit 5: 0 = speed control active, 1 = inactive
```

### Heat Meter Decoding
**Heat Meter Register (Byte 47):**
```
Bit 0: Heat meter 1 active
Bit 1: Heat meter 2 active
```

**Heat Meter 1 (Bytes 48-55):**
```
Bytes 48-51: Instantaneous power (1/100 kW)
  Special encoding for low_low byte
Bytes 52-53: kWh (1/10 kWh)
Bytes 54-55: MWh (1 MWh)
```

**Heat Meter 2 (Bytes 56-63):**
```
Bytes 56-59: Instantaneous power (1/100 kW)
Bytes 60-61: kWh (1/10 kWh)
Bytes 62-63: MWh (1 MWh)
```

## My UVR1611 Sensor Configuration

### Reference Documents
- **Sensor Config Page 1**: `docs/sensoren1.pdf`
- **Sensor Config Page 2**: `docs/Sensoren 2.pdf`

### Actual Sensor Mapping

**Sensors (from sensoren1.pdf & Sensoren 2.pdf):**
- **Sensor 1:** T.Heizkr.VL (Heizkreis Vorlauf) - Temperature
- **Sensor 2:** T.Heizkr.RL (Heizkreis Rücklauf) - Temperature
- **Sensor 3:** T.Heizkr.VL1 Radiatoren - Temperature
- **Sensor 4:** T.Heizkr.VL2 FB (Fußbodenheizung) - Temperature
- **Sensor 5:** Temp.Raum (Raumtemperatur) - Temperature
- **Sensor 6:** Temp.Raum1 - Temperature
- **Sensor 7:** T.Boiler - Temperature
- **Sensor 8:** T.Boiler.u (Boiler unten) - Temperature
- **Sensor 9:** T.Kollektor (Solar) - Temperature
- **Sensor 10:** --- (Unused)
- **Sensor 11:** --- (Unused)
- **Sensor 12:** --- (Unused)
- **Sensor 13:** --- (Unused)
- **Sensor 14:** --- (Unused)### Data Frame Structure
```
Byte 0:     SYNC (16 High-Bits without Start/Stop)
Byte 1:     Device ID (0x80)
Byte 2:     Device ID inverted (0x7F)
Byte 3:     Reserved (don't care)
Byte 4:     Timestamp - Minute
Byte 5:     Timestamp - Hour (Bit 5 = DST flag)
Byte 6:     Timestamp - Day
Byte 7:     Timestamp - Month
Byte 8:     Timestamp - Year (Year - 2000)
Byte 9-40:  16 Sensors (2 bytes each: Low, High)
Byte 41-42: Output states (13 outputs total)
Byte 43:    Speed level A1
Byte 44:    Speed level A2
Byte 45:    Speed level A6
Byte 46:    Speed level A7
Byte 47:    Heat meter register
Byte 48-55: Heat meter 1 data
Byte 56-63: Heat meter 2 data
Byte 64:    Checksum (sum mod 256)
```

### Sensor Data Encoding
**High-Byte bits 4-6 determine sen
- **Sensor 15:** Durchfl.HKR l/h (Durchfluss Heizkreis) - Flow rate
- **Sensor 16:** --- (Unused)

**Outputs (from sensoren1.pdf):**
- **Ausgang 1:** Pump-Hzkr1 Radiatoren (Pumpe Heizkreis 1)
- **Ausgang 2:** Pumpe-Hzkr2 FB (Pumpe Fußbodenheizung)
- **Ausgang 3:** Misch Hzkr1 Radiatore (Mischer Heizkreis 1)
- **Ausgang 4:** Zu (Mischer Zu)
- **Ausgang 5:** (Not labeled)
- **Ausgang 6:** Ventil
- **Ausgang 7:** Pumpe-Solar
- **Ausgang 8:** Misch Hzkr2 FB Auf (Mischer FB Auf)
- **Ausgang 9:** Misch FB Zu (Mischer FB Zu)
- **Ausgang 10:** LadePWW (Warmwasser Ladung)
- **Ausgang 11:** (Not labeled)
- **Ausgang 12:** (Not labeled)
- **Ausgang 13:** (Not labeled)

**Heat Meters (from Sensoren 2.pdf):**
- **Drehz A1:** --- (Speed level A1)
- **Drehz A2:** --- (Speed level A2)
- **Drehz A6:** --- (Speed level A6)
- **Drehz A7:** --- (Speed level A7)
- **Leistung 1:** 0.00 kW (Heat meter 1 power)
- **Energie 1:** 5,112.0 kWh (Heat meter 1 total energy)
- **Leistung 2:** --- (Heat meter 2 inactive)
- **Energie 2:** --- (Heat meter 2 inactive)

## Software Requirements

### Development Environment
- **PlatformIO** on local laptop (Mac)
- **Framework: ESP-IDF** (Espressif IoT Development Framework)
- **VS Code** with PlatformIO extension
- Direct USB-C connection to board
- Serial Monitor @ 115200 baud
- Code access via Samba: `smb://claude-code.local/projects/uvr2mqtt-d1-rmt`

### Firmware Features
1. Read DL-Bus on GPIO26 with RMT 
2. Custom baud rate for 488 Hz display clock
3. SYNC detection (16 High-Bits)
4. Frame reception (65 bytes)
5. Device ID validation (0x80, 0x7F)
6. Checksum validation
7. Timestamp parsing with DST flag
8. Sensor value decoding with type detection
9. Output state parsing (13 outputs)
10. Speed level parsing (A1, A2, A6, A7)
11. Heat meter 1 parsing (power, kWh, MWh)
12. Serial output with actual sensor names
13. Frame statistics and error counting

### Serial Monitor Output Format
```
==================== UVR1611 Frame #123 ====================
Timestamp: 2026-01-13 17:30:45 (DST: No)
Device ID: 0x80 (Valid)
Checksum: OK (0xAB)
Frame Time: 1347ms | Rate: 0.74 fps

TEMPERATURES:
  S1  T.Heizkr.VL:              61.0°C
  S2  T.Heizkr.RL:              54.4°C
  S3  T.Heizkr.VL1 Radiatoren:  34.9°C
  S4  T.Heizkr.VL2 FB:          39.0°C
  S5  Temp.Raum:                28.8°C
  S6  Temp.Raum1:               23.5°C
  S7  T.Boiler:                 51.6°C
  S8  T.Boiler.u:               26.0°C
  S9  T.Kollektor:              14.9°C
  S10-S14: (Unused)

FLOW RATE:
  S15 Durchfl.HKR:              0 l/h
  S16: (Unused)

OUTPUTS:
  A1  Pump-Hzkr1 Radiatoren:    AUS
  A2  Pumpe-Hzkr2 FB:           AUS
  A3  Misch Hzkr1 Radiatore:    AUS
  A4  Zu:                       AUS
  A5:                           AUS
  A6  Ventil:                   AUS
  A7  Pumpe-Solar:              AUS
  A8  Misch Hzkr2 FB Auf:       AUS
  A9  Misch FB Zu:              AUS
  A10 LadePWW:                  AUS
  A11-A13:                      AUS

SPEED LEVELS:
  A1:  --- (inactive)
  A2:  --- (inactive)
  A6:  --- (inactive)
  A7:  --- (inactive)

HEAT METER 1:
  Power:    0.00 kW
  Energy:   5112.0 kWh / 5.1 MWh

HEAT METER 2:
  (Inactive)

STATISTICS:
  Good Frames:  123
  Bad Frames:   0
  Error Rate:   0.00%
  Uptime:       2m 45s
============================================================
```

### Debug Mode Output
When debug mode is enabled, also display:
```
RAW FRAME DATA (Hex):
80 7F 00 2D 11 0D 01 1A 02 60 00 20 02 5D 01 84 00 ...
```

## Project Structure
```
uvr1611-simple/
├── uvr1611-simple_fsd.md           # This file
├── platformio.ini               # PlatformIO configuration (ESP-IDF framework)
├── main/
│   └── main.c                   # Main ESP-IDF code (C, not .cpp)
├── include/
│   ├── uvr1611_protocol.h       # Protocol definitions
│   └── sensor_names.h           # Sensor name mappings
├── docs/
│   ├── DL-Bus-Schnittstelle Datenleitung.PDF         # DL-Bus protocol spec
│   ├── D1 Mini ESP32_EN.pdf         # Hardware documentation
├── .gitignore
└── README.md
```



## Implementation Phases

### Phase 1: Basic DL-Bus reading with RMT functionality ✓
**Goal:** Receive raw data from DL-Bus
- Configure RMT on GPIO26
- Set appropriate baud rate for 488 Hz timing
- DL-Bus is a manchester code
- Receive raw bytes
- Display hex dump to Serial Monitor
- Verify data is being received

**Success:** Raw bytes visible in Serial Monitor

### Phase 2: SYNC Detection ✓
**Goal:** Identify frame boundaries
- Detect SYNC pattern (16 High-Bits)
- Mark start of frame
- Collect 65-byte frames
- Display frame boundaries

**Success:** Can identify complete frames

### Phase 3: Frame Validation ✓
**Goal:** Validate received frames
- Check Device ID (0x80)
- Check Device ID inverted (0x7F)
- Calculate checksum (sum of bytes 1-63 mod 256)
- Validate checksum against byte 64
- Count good vs bad frames
- Measure frame timing

**Success:** 100% checksum validation rate

### Phase 4: Timestamp Parsing ✓
**Goal:** Decode timestamp
- Parse minute, hour, day, month, year
- Extract DST flag from hour byte (bit 5)
- Format as readable date/time
- Display in output

**Success:** Correct date/time displayed

### Phase 5: Temperature Sensors ✓
**Goal:** Decode all 9 temperature sensors
- Parse sensor bytes (9-40, 2 bytes per sensor)
- Extract type bits (4-6 of high byte)
- Identify temperature sensors (type 0x20)
- Apply temperature reconstruction formula
- Handle positive and negative values
- Map to actual sensor names
- Display formatted values

**Success:** All 9 temps match UVR1611 display

### Phase 6: Flow Rate Sensor ✓
**Goal:** Decode flow rate sensor
- Identify flow rate type (0x30)
- Apply flow rate formula (value * 4 l/h)
- Display with sensor name

**Success:** Flow rate matches display

### Phase 7: Output States ✓
**Goal:** Decode all 13 outputs
- Parse output bytes 41-42
- Extract individual bits
- Map to output names
- Display ON/AUS states

**Success:** Output states match UVR1611

### Phase 8: Speed Levels ✓
**Goal:** Decode speed levels
- Parse speed bytes 43-46
- Extract value (bits 0-4)
- Check active flag (bit 5)
- Display values or "inactive"

**Success:** Speed levels displayed correctly

### Phase 9: Heat Meter ✓
**Goal:** Decode heat meter 1
- Check heat meter register (byte 47)
- Parse instantaneous power (bytes 48-51)
- Parse kWh (bytes 52-53)
- Parse MWh (bytes 54-55)
- Apply formulas
- Display formatted values

**Success:** Heat meter values match UVR1611

### Phase 10: Polish & Optimize ✓
**Goal:** Production-ready code
- Clean up code structure
- Add comments
- Optimize performance
- Add debug mode option
- Format output beautifully
- Add statistics
- Test stability

**Success:** Stable operation 1+ hour

## Testing Plan

### Hardware Connection Test
**Steps:**
1. ✅ Connect board to Mac via USB-C
2. ✅ Verify recognition in PlatformIO
3. ✅ Upload blink sketch
4. ✅ Verify LED blinks
5. ✅ Open Serial Monitor @ 115200
6. ✅ Verify communication

**Expected:** Basic communication works

### DL-Bus Connection Test
**Steps:**
1. ✅ Power off UVR1611
2. ✅ Connect optocoupler output to board GPIO26
3. ✅ Connect optocoupler GND to board GPIO26 GND
4. ✅ Power on UVR1611
5. ✅ Upload initial UART test code
6. ✅ Observe raw bytes in Serial Monitor

**Expected:** Receive data every ~1.35 seconds

### SYNC Detection Test
**Steps:**
1. ✅ Implement SYNC detection
2. ✅ Count detected frames
3. ✅ Verify frame rate ~0.74 fps

**Expected:** Consistent frame detection

### Checksum Validation Test
**Steps:**
1. ✅ Implement checksum calculation
2. ✅ Compare with byte 64
3. ✅ Count valid/invalid frames
4. ✅ Run for 100+ frames

**Expected:** 100% valid checksums

### Temperature Accuracy Test
**Steps:**
1. ✅ Parse all temperature sensors
2. ✅ Compare with UVR1611 display
3. ✅ Check T.Heizkr.VL (should be ~60°C)
4. ✅ Check T.Boiler (should be ~50°C)
5. ✅ Check T.Kollektor (Solar, varies)

**Expected:** Values within ±0.1°C

### Output State Test
**Steps:**
1. ✅ Parse output states
2. ✅ Compare with UVR1611 display
3. ✅ Toggle an output manually
4. ✅ Verify state change detected

**Expected:** States match exactly

### Stability Test
**Steps:**
1. ✅ Run continuously for 1 hour
2. ✅ Monitor error rate
3. ✅ Check for memory leaks
4. ✅ Verify no frame drops

**Expected:** 0% error rate, stable operation

### Load Test
**Steps:**
1. ✅ Run for 24 hours
2. ✅ Log all frames
3. ✅ Analyze statistics

**Expected:** Continuous stable operation

## Success Criteria

### Must Have (P0)
1. ✅ Receive complete DL-Bus frames
2. ✅ 100% checksum validation
3. ✅ Parse all 9 temperature sensors correctly
4. ✅ Parse flow rate sensor
5. ✅ Parse all 13 output states
6. ✅ Parse heat meter 1 data
7. ✅ Display with actual sensor names from my UVR1611
8. ✅ Values match UVR1611 display (±0.1°C)
9. ✅ Stable operation 1+ hour without errors
10. ✅ Frame rate ~0.74 fps (1.35s period)

### Should Have (P1)
1. ✅ Timestamp parsing with DST flag
2. ✅ Speed level parsing
3. ✅ Frame statistics (good/bad/error rate)
4. ✅ Formatted, readable serial output
5. ✅ Frame timing measurement

### Nice to Have (P2)
1. ⬜ Debug mode with raw hex dump
2. ⬜ Configurable output detail level
3. ⬜ LED status indicator
4. ⬜ Uptime display
5. ⬜ Average frame time calculation

## Known Sensors Summary

**Active Sensors: 10/16**
- 9 Temperature sensors (S1-S9)
- 1 Flow rate sensor (S15)
- 6 Unused sensors (S10-S14, S16)

**Active Outputs: 10/13**
- 10 Named outputs (A1-A10)
- 3 Unlabeled outputs (A11-A13)

**Heat Meters:**
- Heat meter 1: ACTIVE (5112 kWh total)
- Heat meter 2: INACTIVE

**Speed Levels:**
- A1, A2, A6, A7 available
- Currently all inactive

## Future Enhancements (Out of Scope)

### Phase 2: MQTT Publishing
- Add WiFi connection
- Publish to MQTT broker
- Home Assistant integration
- Auto-discovery

### Phase 3: OTA Programming
- ESP32 programming over Wifi



## Notes

**DL-Bus Signal Characteristics:**
- XOR with 488 Hz clock
- Inverted by output transistor
- Need to sync with display clock
- Read during second half-period of each bit

**Critical Timing:**
- Bit duration: 2.048 ms
- Baud rate calculation: 1 / 2.048ms ≈ 488 baud (approx)
- May need custom UART timing

**Checksum Calculation:**
- Sum bytes 1 through 63 (not byte 0 SYNC, not byte 64 checksum)
- Take modulo 256 (lowest 8 bits)
- Compare with byte 64

**Temperature Encoding:**
- Always check type bits (4-6) of high byte
- For negative: subtract 65536 before dividing
- Room sensors have special encoding with operating mode