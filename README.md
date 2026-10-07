# Zigbee outdoor weather sensor

[![Compile](https://github.com/gaborgoncz/zigbee-outdoor-weather-sensor/actions/workflows/compile.yml/badge.svg)](https://github.com/gaborgoncz/zigbee-outdoor-weather-sensor/actions/workflows/compile.yml)
[![Buy Me a Coffee](https://img.shields.io/badge/Buy%20me%20a%20coffee-support-FFDD00?logo=buymeacoffee&logoColor=black)](https://buymeacoffee.com/gaborgoncz)

A battery powered outdoor sensor built on the **Seeed Studio XIAO ESP32-C6**. It measures temperature, humidity and air pressure, reports them over **Zigbee** to Home Assistant through Zigbee2MQTT, and spends the rest of its life in deep sleep.

How often it wakes up, and how it behaves on a low battery, is set from Home Assistant with sliders. No reflashing needed.

> **Status:** the firmware compiles for the XIAO ESP32-C6 (arduino-esp32 3.3.11, checked on every push). It has not yet been tested on hardware, and the Zigbee2MQTT converter has not yet been run. Treat this as a first version.

## Features

- Temperature and humidity from an **AHT20**, pressure from a **BMP280** (0.1 hPa resolution)
- Battery level and battery voltage
- Deep sleep between measurements, radio on for only a few seconds per wake-up
- **External antenna** on the U.FL connector (switchable in the sketch)
- Three settings exposed to Home Assistant as number entities
- Automatic longer sleep when the battery runs low
- Radio stays off entirely when the cell is nearly empty
- Backs off when the Zigbee network is unreachable instead of draining the battery

## What shows up in Home Assistant

| Entity | Kind | Notes |
|---|---|---|
| Temperature | sensor | °C |
| Humidity | sensor | % |
| Pressure | sensor | hPa, station pressure by default |
| Battery | sensor | % |
| Voltage | sensor | mV |
| Active interval | diagnostic | interval the device is using right now |
| Report interval | setting | 1–240 min, default 10 |
| Low battery threshold | setting | 5–80 %, default 25 |
| Low battery interval | setting | 5–720 min, default 60 |

The device is asleep almost all the time, so a changed setting is **applied on its next wake-up**. Zigbee2MQTT holds the new value until then. Pressing the reset button on the board applies it straight away.

## Parts

- Seeed Studio XIAO ESP32-C6
- AHT20 + BMP280 combo module (I2C)
- Single Li-ion or LiPo cell (1S, 3.7 V nominal) **with protection circuit**
- 2.4 GHz antenna with U.FL connector
- 2 × 1 MΩ resistor and 1 × 100 nF capacitor for battery sensing

## Wiring

![Wiring diagram](docs/wiring.svg)

| XIAO ESP32-C6 | Connects to |
|---|---|
| BAT+ (pad on the underside) | Cell positive, top of R1 |
| BAT− (pad on the underside) | Cell negative, bottom of R2, capacitor |
| A0 (D0) | Middle of the R1/R2 divider, capacitor |
| 3V3 | Sensor VCC |
| GND | Sensor GND |
| D4 (SDA) | Sensor SDA |
| D5 (SCL) | Sensor SCL |
| U.FL | External antenna |

Notes:

- The diagram is a schematic. Pin positions do not match the physical board.
- BAT− and GND are the same net.
- The XIAO has no built-in battery measurement, which is what the divider is for. Without it the device reports 0 % and skips all battery logic.
- One cell only. Cells in parallel are fine, two in series will destroy the board.
- The firmware cutoff only stops the radio. It does not disconnect the cell, so use a protected cell or a small BMS board.

## Flashing

Requires the Arduino IDE or `arduino-cli` with the **esp32** board package 3.3 or newer, plus the libraries *Adafruit AHTX0* and *Adafruit BMP280*.

Board settings:

| Setting | Value |
|---|---|
| Board | XIAO_ESP32C6 |
| Zigbee mode | Zigbee ED (end device) |
| Partition scheme | Zigbee 4MB with spiffs |
| Erase all flash before upload | Enabled, first upload only |

With `arduino-cli`:

```bash
arduino-cli compile --fqbn "esp32:esp32:XIAO_ESP32C6:PartitionScheme=zigbee,ZigbeeMode=ed" xiao_c6_outdoor_sensor
```

```bash
arduino-cli upload --fqbn "esp32:esp32:XIAO_ESP32C6:PartitionScheme=zigbee,ZigbeeMode=ed" -p <port> xiao_c6_outdoor_sensor
```

Once the device deep sleeps, its USB port disappears. To flash again, hold **BOOT** while plugging in the cable.

## Zigbee2MQTT setup

1. In the Zigbee2MQTT UI open **Settings → Dev console → External converters**.
2. Create `xiao_c6_outdoor.mjs` and paste the contents of [`z2m/xiao_c6_outdoor.mjs`](z2m/xiao_c6_outdoor.mjs).
3. Save and restart Zigbee2MQTT.
4. Enable **Permit join**, then power the board.

After power-on or reset the device stays awake for **2 minutes** with the LED blinking, so Zigbee2MQTT can interview and configure it. Holding **BOOT** for 3 seconds during that window makes it leave the network so it can be paired again.

## Configuration in the sketch

The values at the top of [`xiao_c6_outdoor_sensor.ino`](xiao_c6_outdoor_sensor/xiao_c6_outdoor_sensor.ino):

| Define | Default | Purpose |
|---|---|---|
| `DEBUG_LOG` | `0` | Set to `1` for USB serial logging on the bench |
| `USE_EXTERNAL_ANTENNA` | `1` | `0` uses the on-board ceramic antenna |
| `BATTERY_DIVIDER_RATIO` | `2.0` | (R1 + R2) / R2 |
| `BATTERY_CAL` | `1.0` | Multimeter voltage divided by reported voltage |
| `BATTERY_CUTOFF_MV` | `3300` | Below this the radio stays off |
| `ALTITUDE_M` | `0` | Set your altitude to report sea level pressure |
| `CONFIG_WINDOW_MS` | `1000` | Longest wait for Zigbee2MQTT to answer the settings report |

## How it works

Every wake-up is a fresh boot that runs once and ends in deep sleep:

1. Read the battery. If the cell is nearly empty, sleep for 6 hours without starting the radio.
2. Read the sensors while the radio is still off.
3. Start Zigbee and rejoin the network.
4. Report the measurements and the current settings.
5. Wait for Zigbee2MQTT to answer. It writes all three settings back, changed or not, and the device goes on as soon as the third answer is in, normally after a fraction of a second. A changed setting is stored in flash. If no answer comes, it gives up after one second.
6. Deep sleep for the report interval, or for the low battery interval while the battery is at or below the threshold.

Low battery mode ends once the battery is 5 % above the threshold again. If nothing answers on the network, the sleep time doubles after every failed attempt, up to 16 times the interval.

### Zigbee endpoints

| Endpoint | Content |
|---|---|
| 10 | Temperature, humidity, battery percentage |
| 11 | Pressure (analog input) |
| 12 | Report interval (analog output), battery voltage (analog input) |
| 13 | Low battery threshold (analog output), interval in effect (analog input) |
| 14 | Low battery interval (analog output) |

## Things to know

- **Cold weather:** a Li-ion cell's voltage sags below 0 °C, so the battery percentage dips on cold nights. Lower the threshold if low battery mode kicks in too early.
- **Charging:** the XIAO charges the cell whenever USB is connected. Li-ion should not be charged below 0 °C.
- **Calibration:** compare the reported voltage with a multimeter and adjust `BATTERY_CAL`.
- **Firmware and converter belong together:** the device sleeps as soon as the converter has answered its settings report. With an older converter that only writes changed settings, it still works but waits the full `CONFIG_WINDOW_MS` on every wake-up.
- **Settings arrive late:** if a setting regularly needs two wake-ups to apply, raise `CONFIG_WINDOW_MS`.

## Support

If this project is useful to you, you can [buy me a coffee](https://buymeacoffee.com/gaborgoncz).
