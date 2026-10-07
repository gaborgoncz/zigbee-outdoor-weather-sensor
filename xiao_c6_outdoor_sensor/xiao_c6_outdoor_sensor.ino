// XIAO ESP32-C6 battery powered outdoor sensor (Zigbee end device, deep sleep)
//   AHT20  -> temperature, humidity
//   BMP280 -> pressure
//   A0     -> battery voltage through a resistor divider
//
// Arduino IDE / arduino-cli settings (arduino-esp32 >= 3.3):
//   Board:            XIAO_ESP32C6
//   Zigbee mode:      Zigbee ED (end device)
//   Partition scheme: Zigbee 4MB with spiffs
//   Erase all flash:  Enabled on the first upload only
//
// Zigbee endpoints (matched by z2m/xiao_c6_outdoor.mjs):
//   10  temperature + humidity + battery %
//   11  analog in:  pressure [hPa]
//   12  analog out: report interval [min]          analog in: battery voltage [V]
//   13  analog out: low battery threshold [%]      analog in: interval in effect [min]
//   14  analog out: low battery interval [min]
//
// The three analog outputs are the settings changed from Home Assistant. The device sleeps
// almost all the time, so Zigbee2MQTT hands a changed setting over on the next wake-up.

#ifndef ZIGBEE_MODE_ED
#error "Zigbee end device mode is not selected in Tools->Zigbee mode"
#endif

#include <Adafruit_AHTX0.h>
#include <Adafruit_BMP280.h>
#include <Preferences.h>
#include <Wire.h>
#include "Zigbee.h"

/************************ User settings *****************************/
#define DEBUG_LOG            0     // 1 = print to USB serial (costs battery, bench use only)
#define USE_EXTERNAL_ANTENNA 1     // 1 = U.FL connector, 0 = on-board ceramic antenna

#define BATTERY_PIN           A0   // BAT+ --[R1]--+--[R2]-- GND, A0 on the middle, 100nF from A0 to GND
#define BATTERY_DIVIDER_RATIO 2.0f // (R1 + R2) / R2, 2.0 for two equal resistors
#define BATTERY_CAL           1.0f // multimeter voltage / reported voltage
#define BATTERY_CUTOFF_MV     3300 // below this the radio stays off to protect the cell
#define CUTOFF_SLEEP_MIN      360

#define ALTITUDE_M 0.0f            // > 0 reports sea level pressure instead of station pressure

// Defaults used until Home Assistant sets something else (kept in flash afterwards)
#define DEFAULT_INTERVAL_MIN       10
#define DEFAULT_LOW_BATT_PERCENT   25
#define DEFAULT_LOW_BATT_INTERVAL  60

#define INTERVAL_MIN_MIN            1
#define INTERVAL_MAX_MIN          240
#define LOW_BATT_PERCENT_MIN        5
#define LOW_BATT_PERCENT_MAX       80
#define LOW_BATT_INTERVAL_MIN_MIN   5
#define LOW_BATT_INTERVAL_MAX_MIN 720
#define LOW_BATT_HYSTERESIS         5   // % above the threshold needed to leave low battery mode

#define REJOIN_TIMEOUT_MS   8000   // wake from sleep: network is known, rejoin is quick
#define JOIN_TIMEOUT_MS    60000   // power-on / reset: may need a full join
#define REPORT_TIMEOUT_MS   1500   // wait for the coordinator to confirm the reports
#define CONFIG_WINDOW_MS    1000   // stay awake this long for setting changes from HA
#define PAIRING_WINDOW_MS 120000   // power-on / reset: stay awake for interview and configure
#define FACTORY_RESET_HOLD_MS 3000 // hold BOOT during the pairing window to leave the network
/********************************************************************/

#if DEBUG_LOG
#define LOG(...) Serial.printf(__VA_ARGS__)
#else
#define LOG(...)
#endif

#define EP_CLIMATE      10
#define EP_PRESSURE     11
#define EP_INTERVAL     12
#define EP_LOW_BATT     13
#define EP_LOW_INTERVAL 14

ZigbeeTempSensor zbClimate = ZigbeeTempSensor(EP_CLIMATE);
ZigbeeAnalog zbPressure = ZigbeeAnalog(EP_PRESSURE);
ZigbeeAnalog zbInterval = ZigbeeAnalog(EP_INTERVAL);
ZigbeeAnalog zbLowBatt = ZigbeeAnalog(EP_LOW_BATT);
ZigbeeAnalog zbLowInterval = ZigbeeAnalog(EP_LOW_INTERVAL);

Adafruit_AHTX0 aht;
Adafruit_BMP280 bmp;
Preferences prefs;

// Survive deep sleep, cleared on power-on
RTC_DATA_ATTR bool lowBatteryMode = false;
RTC_DATA_ATTR bool pairingDone = false;
RTC_DATA_ATTR uint8_t networkFailures = 0;

struct Settings {
  uint16_t intervalMin;
  uint16_t lowBattPercent;
  uint16_t lowBattIntervalMin;
};
Settings settings;

volatile bool settingsChanged = false;
volatile uint32_t lastSettingWriteMs = 0;
volatile uint8_t reportsConfirmed = 0;

uint16_t batteryMv = 0;
uint8_t batteryPct = 0;
bool batteryValid = false;

/************************ Settings *****************************/
void loadSettings() {
  prefs.begin("cfg", true);
  settings.intervalMin = prefs.getUShort("interval", DEFAULT_INTERVAL_MIN);
  settings.lowBattPercent = prefs.getUShort("lowpct", DEFAULT_LOW_BATT_PERCENT);
  settings.lowBattIntervalMin = prefs.getUShort("lowint", DEFAULT_LOW_BATT_INTERVAL);
  prefs.end();
}

void saveSettings() {
  prefs.begin("cfg", false);
  prefs.putUShort("interval", settings.intervalMin);
  prefs.putUShort("lowpct", settings.lowBattPercent);
  prefs.putUShort("lowint", settings.lowBattIntervalMin);
  prefs.end();
}

// Called from the Zigbee task when Zigbee2MQTT writes an analog output (and with an unchanged
// value when the sketch publishes its own setting), so only real changes are flagged.
void applySetting(uint16_t &target, float value, uint16_t minValue, uint16_t maxValue) {
  uint16_t v = (uint16_t)constrain(lroundf(value), (long)minValue, (long)maxValue);
  if (v != target) {
    target = v;
    settingsChanged = true;
    lastSettingWriteMs = millis();
  }
}

void onIntervalChange(float value) {
  applySetting(settings.intervalMin, value, INTERVAL_MIN_MIN, INTERVAL_MAX_MIN);
}

void onLowBattChange(float value) {
  applySetting(settings.lowBattPercent, value, LOW_BATT_PERCENT_MIN, LOW_BATT_PERCENT_MAX);
}

void onLowIntervalChange(float value) {
  applySetting(settings.lowBattIntervalMin, value, LOW_BATT_INTERVAL_MIN_MIN, LOW_BATT_INTERVAL_MAX_MIN);
}

/************************ Battery *****************************/
uint16_t readBatteryMillivolts() {
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) {
    sum += analogReadMilliVolts(BATTERY_PIN);
  }
  return (uint16_t)((sum / 8.0f) * BATTERY_DIVIDER_RATIO * BATTERY_CAL);
}

// Resting voltage of a single Li-ion / LiPo cell
uint8_t batteryPercent(uint16_t mv) {
  static const uint16_t curve[][2] = {{4150, 100}, {4050, 90}, {3970, 80}, {3900, 70}, {3840, 60}, {3790, 50},
                                      {3750, 40},  {3710, 30}, {3670, 20}, {3600, 10}, {3450, 5},  {3300, 0}};
  const int n = sizeof(curve) / sizeof(curve[0]);
  if (mv >= curve[0][0]) {
    return 100;
  }
  for (int i = 1; i < n; i++) {
    if (mv >= curve[i][0]) {
      return curve[i][1] + (uint32_t)(mv - curve[i][0]) * (curve[i - 1][1] - curve[i][1]) / (curve[i - 1][0] - curve[i][0]);
    }
  }
  return 0;
}

void updateLowBatteryMode() {
  if (!batteryValid) {
    lowBatteryMode = false;
  } else if (batteryPct <= settings.lowBattPercent) {
    lowBatteryMode = true;
  } else if (batteryPct >= settings.lowBattPercent + LOW_BATT_HYSTERESIS) {
    lowBatteryMode = false;
  }
}

/************************ Sleep *****************************/
uint16_t activeIntervalMin() {
  if (lowBatteryMode) {
    return max(settings.intervalMin, settings.lowBattIntervalMin);
  }
  return settings.intervalMin;
}

void deepSleepMinutes(uint32_t minutes) {
  LOG("Awake for %lu ms, sleeping %lu min\r\n", millis(), minutes);
#if DEBUG_LOG
  Serial.flush();
#endif
  Wire.end();
  digitalWrite(LED_BUILTIN, HIGH);
  esp_sleep_enable_timer_wakeup((uint64_t)minutes * 60ULL * 1000000ULL);
  esp_deep_sleep_start();
}

// Coordinator or parent unreachable: sleep longer after every failure (up to 16x the interval)
// so a network that is down does not drain the battery.
void sleepAfterNetworkFailure() {
  if (networkFailures < 4) {
    networkFailures++;
  }
  LOG("No network (failure %u)\r\n", networkFailures);
  deepSleepMinutes(min((uint32_t)activeIntervalMin() << networkFailures, (uint32_t)LOW_BATT_INTERVAL_MAX_MIN));
}

/************************ Zigbee *****************************/
void onGlobalResponse(zb_cmd_type_t command, esp_zb_zcl_status_t status, uint8_t endpoint, uint16_t cluster) {
  if (command == ZB_CMD_REPORT_ATTRIBUTE && status == ESP_ZB_ZCL_STATUS_SUCCESS) {
    reportsConfirmed = reportsConfirmed + 1;
  }
}

void waitForReports(uint8_t expected) {
  uint32_t start = millis();
  while (reportsConfirmed < expected && millis() - start < REPORT_TIMEOUT_MS) {
    delay(20);
  }
  LOG("%u/%u reports confirmed\r\n", reportsConfirmed, expected);
}

uint8_t reportSettings() {
  zbInterval.setAnalogOutput(settings.intervalMin);
  zbLowBatt.setAnalogOutput(settings.lowBattPercent);
  zbLowInterval.setAnalogOutput(settings.lowBattIntervalMin);
  zbLowBatt.setAnalogInput(activeIntervalMin());
  return zbInterval.reportAnalogOutput() + zbLowBatt.reportAnalogOutput() + zbLowInterval.reportAnalogOutput() + zbLowBatt.reportAnalogInput();
}

// Settings written by Zigbee2MQTT: store them and report back what is now in effect
void commitSettings() {
  if (!settingsChanged) {
    return;
  }
  settingsChanged = false;
  saveSettings();
  updateLowBatteryMode();
  LOG("New settings: interval %u min, low battery %u %% -> %u min\r\n", settings.intervalMin, settings.lowBattPercent, settings.lowBattIntervalMin);
  reportsConfirmed = 0;
  waitForReports(reportSettings());
}

// After power-on or reset (or a first join that only succeeded later): stay awake so Zigbee2MQTT can interview and configure the device.
// Holding BOOT leaves the network so the device can be paired again.
void pairingWindow() {
  LOG("Pairing window open for %d s\r\n", PAIRING_WINDOW_MS / 1000);
  pinMode(BOOT_PIN, INPUT_PULLUP);
  uint32_t start = millis();
  uint32_t lastReport = 0;
  while (millis() - start < PAIRING_WINDOW_MS) {
    digitalWrite(LED_BUILTIN, (millis() / 500) % 2);
    if (digitalRead(BOOT_PIN) == LOW) {
      uint32_t pressed = millis();
      while (digitalRead(BOOT_PIN) == LOW && millis() - pressed < FACTORY_RESET_HOLD_MS) {
        delay(20);
      }
      if (millis() - pressed >= FACTORY_RESET_HOLD_MS) {
        LOG("Factory reset\r\n");
        Zigbee.factoryReset(true);
      }
    }
    // Reports sent before Zigbee2MQTT bound the clusters went nowhere, so repeat them
    if (millis() - lastReport > 15000) {
      lastReport = millis();
      zbClimate.report();
      zbClimate.reportBatteryPercentage();
      zbPressure.reportAnalogInput();
      zbInterval.reportAnalogInput();
      reportSettings();
    }
    commitSettings();
    delay(50);
  }
  pairingDone = true;
}

/********************* Arduino functions **************************/
void setup() {
  bool coldBoot = esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER;

#if DEBUG_LOG
  Serial.begin(115200);
  delay(coldBoot ? 2000 : 0);  // time to open the serial monitor
#endif
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);  // LED is active low

  loadSettings();

  // Battery first: an empty cell goes straight back to sleep without powering the radio.
  // Below 2.5 V there is no cell on the divider (USB only), so no battery logic applies.
  batteryMv = readBatteryMillivolts();
  batteryValid = batteryMv > 2500;
  batteryPct = batteryValid ? batteryPercent(batteryMv) : 0;
  LOG("Battery %u mV, %u %%\r\n", batteryMv, batteryPct);
  if (batteryValid && batteryMv < BATTERY_CUTOFF_MV && !coldBoot) {
    deepSleepMinutes(CUTOFF_SLEEP_MIN);
  }
  updateLowBatteryMode();

  // Sensors are read before the radio starts to keep the radio-on time short
  Wire.begin();
  bool climateOk = false, pressureOk = false;
  float temperature = 0, humidity = 0, pressure = 0;
  if (aht.begin()) {
    sensors_event_t humEvent, tempEvent;
    climateOk = aht.getEvent(&humEvent, &tempEvent);
    temperature = tempEvent.temperature;
    humidity = humEvent.relative_humidity;
  }
  if (bmp.begin(0x77) || bmp.begin(0x76)) {
    // Forced mode: one conversion, then the BMP280 drops back to its sleep mode
    bmp.setSampling(Adafruit_BMP280::MODE_FORCED, Adafruit_BMP280::SAMPLING_X1, Adafruit_BMP280::SAMPLING_X4, Adafruit_BMP280::FILTER_OFF);
    if (bmp.takeForcedMeasurement()) {
      pressure = bmp.readPressure() / 100.0f;
      if (ALTITUDE_M > 0) {
        pressure = pressure / powf(1.0f - ALTITUDE_M / 44330.0f, 5.255f);
      }
      pressureOk = true;
    }
  }
  LOG("T %.2f C, RH %.1f %%, P %.1f hPa (aht %d, bmp %d)\r\n", temperature, humidity, pressure, climateOk, pressureOk);

  // RF switch: initVariant() already enabled it and selected the on-board antenna
#if USE_EXTERNAL_ANTENNA
  digitalWrite(WIFI_ANT_CONFIG, HIGH);
#endif

  // Endpoints
  zbClimate.setManufacturerAndModel("CustomDIY", "XIAO_C6_Outdoor");
  zbClimate.setMinMaxValue(-40, 85);
  zbClimate.setTolerance(0.3);
  zbClimate.addHumiditySensor(0, 100, 2, 0);
  zbClimate.setPowerSource(ZB_POWER_SOURCE_BATTERY, batteryPct, batteryMv / 100);

  zbPressure.addAnalogInput();
  zbPressure.setAnalogInputDescription("Pressure (hPa)");
  zbPressure.setAnalogInputResolution(0.1);

  zbInterval.addAnalogOutput();
  zbInterval.setAnalogOutputDescription("Report interval (min)");
  zbInterval.setAnalogOutputResolution(1);
  zbInterval.setAnalogOutputMinMax(INTERVAL_MIN_MIN, INTERVAL_MAX_MIN);
  zbInterval.onAnalogOutputChange(onIntervalChange);
  zbInterval.addAnalogInput();
  zbInterval.setAnalogInputDescription("Battery voltage (V)");
  zbInterval.setAnalogInputResolution(0.001);

  zbLowBatt.addAnalogOutput();
  zbLowBatt.setAnalogOutputDescription("Low battery threshold (%)");
  zbLowBatt.setAnalogOutputResolution(1);
  zbLowBatt.setAnalogOutputMinMax(LOW_BATT_PERCENT_MIN, LOW_BATT_PERCENT_MAX);
  zbLowBatt.onAnalogOutputChange(onLowBattChange);
  zbLowBatt.addAnalogInput();
  zbLowBatt.setAnalogInputDescription("Interval in effect (min)");
  zbLowBatt.setAnalogInputResolution(1);

  zbLowInterval.addAnalogOutput();
  zbLowInterval.setAnalogOutputDescription("Low battery interval (min)");
  zbLowInterval.setAnalogOutputResolution(1);
  zbLowInterval.setAnalogOutputMinMax(LOW_BATT_INTERVAL_MIN_MIN, LOW_BATT_INTERVAL_MAX_MIN);
  zbLowInterval.onAnalogOutputChange(onLowIntervalChange);

  Zigbee.onGlobalDefaultResponse(onGlobalResponse);
  Zigbee.addEndpoint(&zbClimate);
  Zigbee.addEndpoint(&zbPressure);
  Zigbee.addEndpoint(&zbInterval);
  Zigbee.addEndpoint(&zbLowBatt);
  Zigbee.addEndpoint(&zbLowInterval);

  // Join / rejoin. The radio stays in receive while awake (rx on when idle, the library
  // default), so a setting written by Zigbee2MQTT arrives without waiting for a parent poll.
  // The parent must not forget this child between two wake-ups, hence the long aging timeout.
  uint32_t joinTimeout = pairingDone ? REJOIN_TIMEOUT_MS : JOIN_TIMEOUT_MS;
  esp_zb_cfg_t zigbeeConfig = ZIGBEE_DEFAULT_ED_CONFIG();
  zigbeeConfig.nwk_cfg.zed_cfg.ed_timeout = ESP_ZB_ED_AGING_TIMEOUT_16384MIN;
  zigbeeConfig.nwk_cfg.zed_cfg.keep_alive = 10000;
  Zigbee.setTimeout(joinTimeout);
  bool started = Zigbee.begin(&zigbeeConfig, false);
  uint32_t joinStart = millis();
  while (started && !Zigbee.connected() && millis() - joinStart < joinTimeout) {
    delay(50);
  }
  if (!started || !Zigbee.connected()) {
    sleepAfterNetworkFailure();
  }
  LOG("Connected after %lu ms\r\n", millis());

  // Report
  reportsConfirmed = 0;
  uint8_t sent = 0;
  if (climateOk) {
    zbClimate.setTemperature(temperature);
    zbClimate.setHumidity(humidity);
    sent += zbClimate.reportTemperature() + zbClimate.reportHumidity();
  }
  if (pressureOk) {
    zbPressure.setAnalogInput(pressure);
    sent += zbPressure.reportAnalogInput();
  }
  zbClimate.setBatteryPercentage(batteryPct);
  zbClimate.setBatteryVoltage(batteryMv / 100);
  zbInterval.setAnalogInput(batteryMv / 1000.0f);
  sent += zbClimate.reportBatteryPercentage() + zbInterval.reportAnalogInput();
  sent += reportSettings();
  waitForReports(sent);

  if (!pairingDone) {
    pairingWindow();
  } else if (reportsConfirmed == 0) {
    // A device that was joined before counts as connected right after boot, so a missing
    // coordinator only shows up here: nothing was confirmed.
    sleepAfterNetworkFailure();
  } else {
    // Zigbee2MQTT answers the settings report with any value changed in HA meanwhile.
    // Every received write keeps the window open a little longer.
    uint32_t windowStart = millis();
    while (millis() - windowStart < CONFIG_WINDOW_MS || (settingsChanged && millis() - lastSettingWriteMs < 400)) {
      delay(20);
    }
    commitSettings();
  }

  networkFailures = 0;
  deepSleepMinutes(activeIntervalMin());
}

void loop() {}
