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
//   11  analog out: temperature change to report [C]   analog in: pressure [hPa]
//   12  analog out: report interval [min]              analog in: battery voltage [V]
//   13  analog out: low battery threshold [%]          analog in: interval in effect [min]
//   14  analog out: low battery interval [min]         analog in: awake time of the last report [ms]
//   15  analog out: longest time without a report [min] analog in: device status (STATUS_ bits)
//   16  analog out: humidity change to report [%]         analog in: stage times of the last report
//   17  analog out: pressure change to report [hPa]
//   18  analog out: settings sync (see below)
//
// The analog outputs of endpoints 11 to 17 are the settings changed from Home Assistant. The
// device sleeps almost all the time, so Zigbee2MQTT hands a changed setting over on the next
// report. To keep that cheap the settings are not sent every time: the device reports one
// checksum over all of them on endpoint 18, and Zigbee2MQTT answers by writing to the same
// endpoint: SYNC_OK when its own values give the same checksum, the settings followed by
// SYNC_OK when they do not, or SYNC_SEND when it does not know the settings yet.
//
// One wake-up cycle (everything runs in setup(), every wake-up is a fresh boot):
//   1. read the battery, go straight back to sleep if the cell is empty
//   2. read the sensors while the radio is still off
//   3. with a temperature change set: go back to sleep without the radio if nothing has
//      changed enough since the last report
//   4. start Zigbee and rejoin the network
//   5. report temperature, humidity, pressure and the settings checksum. Battery, voltage,
//      device status and interval in effect go out only when they change and once an hour,
//      the awake time once an hour
//   6. wait for Zigbee2MQTT to answer the checksum, which normally takes a fraction of a second
//   7. deep sleep for the report interval (or the low battery interval)

#ifndef ZIGBEE_MODE_ED
#error "Zigbee end device mode is not selected in Tools->Zigbee mode"
#endif

#include <Adafruit_AHTX0.h>
#include <Adafruit_BMP280.h>
#include <Preferences.h>
#include <Wire.h>
#include "Zigbee.h"
#include "driver/gpio.h"

/************************ User settings *****************************/
#define DEBUG_LOG            0     // 1 = print to USB serial (costs battery, bench use only)
#define USE_EXTERNAL_ANTENNA 1     // 1 = U.FL connector, 0 = on-board ceramic antenna

#define BATTERY_PIN           A0   // BAT+ --[R1]--+--[R2]-- GND, A0 on the middle, 100nF from A0 to GND
#define BATTERY_DIVIDER_RATIO 2.0f // (R1 + R2) / R2, 2.0 for two equal resistors
#define BATTERY_CAL           1.0f // multimeter voltage / reported voltage
#define BATTERY_CUTOFF_MV     3300 // below this the radio stays off to protect the cell
#define CUTOFF_SLEEP_MIN      360

// Defaults used until Home Assistant sets something else (kept in flash afterwards)
#define DEFAULT_INTERVAL_MIN       10
#define DEFAULT_LOW_BATT_PERCENT   25
#define DEFAULT_LOW_BATT_INTERVAL  60
#define DEFAULT_TEMP_DELTA_TENTHS   0   // 0 = report on every wake-up
#define DEFAULT_MAX_SILENT_MIN     60
#define DEFAULT_HUM_DELTA           3   // %, 0 = humidity does not trigger a report
#define DEFAULT_PRESS_DELTA_TENTHS 10   // 1.0 hPa, 0 = pressure does not trigger a report

#define INTERVAL_MIN_MIN            1
#define INTERVAL_MAX_MIN          240
#define LOW_BATT_PERCENT_MIN        5
#define LOW_BATT_PERCENT_MAX       80
#define LOW_BATT_INTERVAL_MIN_MIN   5
#define LOW_BATT_INTERVAL_MAX_MIN 720
#define LOW_BATT_HYSTERESIS         5   // % above the threshold needed to leave low battery mode
#define TEMP_DELTA_MAX_TENTHS      50   // 5.0 C
#define MAX_SILENT_MIN_MIN         10
#define MAX_SILENT_MAX_MIN       1440
#define HUM_DELTA_MAX              20   // %
#define PRESS_DELTA_MAX_TENTHS    100   // 10.0 hPa

#define REJOIN_TIMEOUT_MS   8000   // wake from sleep: network is known, rejoin is quick
#define JOIN_TIMEOUT_MS    60000   // power-on / reset: may need a full join
#define REPORT_TIMEOUT_MS    700   // wait for the coordinator to confirm the reports
#define PARENT_POLL_MS      3000   // keep-alive poll of the parent, the library default
#define SILENT_CYCLES_ALLOWED  2   // reports in a row that may get nothing back before it counts as a network failure
#define BINDING_TABLE_SIZE    32   // bindings the device can hold, 17 are in use
#define REPORT_BATCH           8   // reports sent in one go; more than the stack can queue get lost
#define BATCH_TIMEOUT_MS     300   // wait for one batch to be confirmed before the next is sent
#define CONFIG_WINDOW_MS     700   // longest wait for Zigbee2MQTT's next answer to the settings checksum
#define ANSWER_GRACE_MS       60   // after the last answer: time for the stack to send its replies
#define SLOW_REPORT_MIN       60   // battery, status, interval in effect and awake time go out this often when unchanged
#define BATTERY_REPORT_STEP    2   // % the battery level must move to be reported in between
#define PAIRING_WINDOW_MS 120000   // unpaired device, or BOOT held after reset: stay awake for interview and configure
#define FACTORY_RESET_HOLD_MS 3000 // hold BOOT during the pairing window to leave the network
/********************************************************************/

// Logging compiles to nothing unless DEBUG_LOG is set, so the release build never touches USB serial
#if DEBUG_LOG
#define LOG(...) Serial.printf(__VA_ARGS__)
#else
#define LOG(...)
#endif

// Zigbee endpoint numbers, see the table at the top of the file
#define EP_CLIMATE      10
#define EP_PRESSURE     11
#define EP_INTERVAL     12
#define EP_LOW_BATT     13
#define EP_LOW_INTERVAL 14
#define EP_STATUS       15
#define EP_HUM_DELTA    16
#define EP_PRESS_DELTA  17
#define EP_SYNC         18

// The library allows one analog input and one analog output per endpoint, which is why the
// settings and the extra readings are spread over several endpoints.
ZigbeeTempSensor zbClimate = ZigbeeTempSensor(EP_CLIMATE);
ZigbeeAnalog zbPressure = ZigbeeAnalog(EP_PRESSURE);
ZigbeeAnalog zbInterval = ZigbeeAnalog(EP_INTERVAL);
ZigbeeAnalog zbLowBatt = ZigbeeAnalog(EP_LOW_BATT);
ZigbeeAnalog zbLowInterval = ZigbeeAnalog(EP_LOW_INTERVAL);
ZigbeeAnalog zbStatus = ZigbeeAnalog(EP_STATUS);
ZigbeeAnalog zbHumDelta = ZigbeeAnalog(EP_HUM_DELTA);
ZigbeeAnalog zbPressDelta = ZigbeeAnalog(EP_PRESS_DELTA);
ZigbeeAnalog zbSync = ZigbeeAnalog(EP_SYNC);

Adafruit_AHTX0 aht;
Adafruit_BMP280 bmp;
Preferences prefs;

// Kept in RTC memory: these survive deep sleep and are cleared by power-on or the reset button
RTC_DATA_ATTR bool lowBatteryMode = false;    // currently using the low battery interval
RTC_DATA_ATTR uint8_t networkFailures = 0;    // consecutive wake-ups without contact, drives the back-off
RTC_DATA_ATTR uint8_t silentCycles = 0;       // consecutive reports that got nothing back from the coordinator
RTC_DATA_ATTR uint32_t lastAwakeMs = 0;       // how long the last wake-up that used the radio took
// The last report, to decide whether the next measurement is worth sending
RTC_DATA_ATTR bool lastReportValid = false;
RTC_DATA_ATTR float lastTemperature = 0;
RTC_DATA_ATTR float lastHumidity = 0;
RTC_DATA_ATTR float lastPressure = 0;
RTC_DATA_ATTR uint8_t lastStatus = 0;
RTC_DATA_ATTR uint16_t minutesSinceReport = 0;
// The slow values as last reported, and how long ago all of them went out
RTC_DATA_ATTR uint8_t lastBatteryPct = 0;
RTC_DATA_ATTR uint16_t lastActiveMin = 0;
RTC_DATA_ATTR uint32_t lastAwakeSent = 0;
// Where the time of that wake-up went, sent along with the awake time: three bytes holding the
// join, the reports and the wait for the settings answer, each in units of 10 ms
RTC_DATA_ATTR uint32_t lastStages = 0;
RTC_DATA_ATTR uint16_t minutesSinceSlow = 0;

// Settings changeable from Home Assistant, stored in flash (NVS) so they survive a battery swap
struct Settings {
  uint16_t intervalMin;
  uint16_t lowBattPercent;
  uint16_t lowBattIntervalMin;
  uint16_t tempDeltaTenths;                   // 0.1 C steps, 0 = report on every wake-up
  uint16_t maxSilentMin;
  uint16_t humDelta;                          // %, 0 = not watched
  uint16_t pressDeltaTenths;                  // 0.1 hPa steps, 0 = not watched
};
Settings settings;
bool pairingDone = false;                     // the pairing window has been completed once, stored in flash as well

// Shared with the Zigbee task (callbacks below run there), hence volatile
volatile bool settingsChanged = false;        // a setting was written over Zigbee and is not saved yet
volatile uint8_t syncAnswer = 0;              // SYNC_ answer of Zigbee2MQTT to the settings checksum, 0 = none yet
volatile uint32_t lastWriteMs = 0;            // when Zigbee2MQTT last wrote a setting
volatile uint32_t reportsConfirmed = 0;       // one REPORT_ bit per report acknowledged by the coordinator

// Battery reading of this wake-up
uint16_t batteryMv = 0;
uint8_t batteryPct = 0;
bool batteryValid = false;                    // false when no cell is on the divider (USB power only)

// Bits of the device status reported on endpoint 15
#define STATUS_AHT_FAULT   0x01
#define STATUS_BMP_FAULT   0x02
#define STATUS_LOW_BATTERY 0x04

uint8_t sensorFaults = 0;                     // STATUS_ fault bits of the sensors that failed to read
bool radioUsed = false;                       // the radio was started during this wake-up

/************************ Settings *****************************/
// Read the settings from flash, falling back to the defaults on a fresh device
void loadSettings() {
  prefs.begin("cfg", true);
  settings.intervalMin = prefs.getUShort("interval", DEFAULT_INTERVAL_MIN);
  settings.lowBattPercent = prefs.getUShort("lowpct", DEFAULT_LOW_BATT_PERCENT);
  settings.lowBattIntervalMin = prefs.getUShort("lowint", DEFAULT_LOW_BATT_INTERVAL);
  settings.tempDeltaTenths = prefs.getUShort("tdelta", DEFAULT_TEMP_DELTA_TENTHS);
  settings.maxSilentMin = prefs.getUShort("maxsil", DEFAULT_MAX_SILENT_MIN);
  settings.humDelta = prefs.getUShort("hdelta", DEFAULT_HUM_DELTA);
  settings.pressDeltaTenths = prefs.getUShort("pdelta", DEFAULT_PRESS_DELTA_TENTHS);
  pairingDone = prefs.getBool("paired", false);
  prefs.end();
}

// Remember across resets and battery swaps whether the device has been paired
void savePairingDone(bool done) {
  pairingDone = done;
  prefs.begin("cfg", false);
  prefs.putBool("paired", done);
  prefs.end();
}

// Write the settings to flash. Only called after a real change, to spare flash wear.
void saveSettings() {
  prefs.begin("cfg", false);
  prefs.putUShort("interval", settings.intervalMin);
  prefs.putUShort("lowpct", settings.lowBattPercent);
  prefs.putUShort("lowint", settings.lowBattIntervalMin);
  prefs.putUShort("tdelta", settings.tempDeltaTenths);
  prefs.putUShort("maxsil", settings.maxSilentMin);
  prefs.putUShort("hdelta", settings.humDelta);
  prefs.putUShort("pdelta", settings.pressDeltaTenths);
  prefs.end();
}

// What Zigbee2MQTT writes to the sync endpoint. Both lie above any checksum.
#define SYNC_OK   70000   // nothing (more) to change
#define SYNC_SEND 70001   // settings not known there: report all of them

// The library calls the analog output callbacks in two cases: from the Zigbee task when
// Zigbee2MQTT writes a value, and from the main task when the sketch publishes its own value.
// Only the first is an answer from Zigbee2MQTT, so the calling task tells them apart.
TaskHandle_t mainTask = NULL;

void applySetting(uint16_t &target, float value, uint16_t minValue, uint16_t maxValue) {
  if (xTaskGetCurrentTaskHandle() == mainTask) {
    return;
  }
  uint16_t v = (uint16_t)constrain(lroundf(value), (long)minValue, (long)maxValue);
  if (v != target) {
    target = v;
    settingsChanged = true;
  }
  lastWriteMs = millis();
}

// Zigbee2MQTT's answer to the settings checksum
void onSyncAnswer(float value) {
  if (xTaskGetCurrentTaskHandle() == mainTask) {
    return;
  }
  long v = lroundf(value);
  if (v == SYNC_OK || v == SYNC_SEND) {
    syncAnswer = v == SYNC_OK ? 1 : 2;
  }
}

// One number that changes whenever a setting does. xiao_c6_outdoor.mjs calculates the same
// from the values Home Assistant wants, in the same order.
uint16_t settingsChecksum() {
  const uint16_t values[] = {settings.intervalMin, settings.lowBattPercent, settings.lowBattIntervalMin, settings.tempDeltaTenths,
                             settings.maxSilentMin, settings.humDelta,       settings.pressDeltaTenths};
  uint32_t sum = 0;
  for (uint16_t v : values) {
    sum = (sum * 31 + v) % 65521;
  }
  return (uint16_t)sum;
}

// One callback per setting: the library passes only the value, not which endpoint it came from
void onIntervalChange(float value) {
  applySetting(settings.intervalMin, value, INTERVAL_MIN_MIN, INTERVAL_MAX_MIN);
}

void onLowBattChange(float value) {
  applySetting(settings.lowBattPercent, value, LOW_BATT_PERCENT_MIN, LOW_BATT_PERCENT_MAX);
}

void onLowIntervalChange(float value) {
  applySetting(settings.lowBattIntervalMin, value, LOW_BATT_INTERVAL_MIN_MIN, LOW_BATT_INTERVAL_MAX_MIN);
}

// Sent in degrees, kept in tenths of a degree
void onTempDeltaChange(float value) {
  applySetting(settings.tempDeltaTenths, value * 10.0f, 0, TEMP_DELTA_MAX_TENTHS);
}

void onHumDeltaChange(float value) {
  applySetting(settings.humDelta, value, 0, HUM_DELTA_MAX);
}

// Sent in hPa, kept in tenths of a hPa
void onPressDeltaChange(float value) {
  applySetting(settings.pressDeltaTenths, value * 10.0f, 0, PRESS_DELTA_MAX_TENTHS);
}

void onMaxSilentChange(float value) {
  applySetting(settings.maxSilentMin, value, MAX_SILENT_MIN_MIN, MAX_SILENT_MAX_MIN);
}

/************************ Battery *****************************/
// Average of 8 ADC samples, scaled back up by the divider ratio to the real cell voltage
uint16_t readBatteryMillivolts() {
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) {
    sum += analogReadMilliVolts(BATTERY_PIN);
  }
  return (uint16_t)((sum / 8.0f) * BATTERY_DIVIDER_RATIO * BATTERY_CAL);
}

// Voltage to charge level for a single Li-ion / LiPo cell at rest: {millivolts, percent}
// points, linearly interpolated in between.
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

// Enter low battery mode at the threshold, leave it only LOW_BATT_HYSTERESIS % above it,
// so a cell hovering around the threshold does not flip back and forth.
void updateLowBatteryMode() {
  if (!batteryValid) {
    lowBatteryMode = false;
  } else if (batteryPct <= settings.lowBattPercent) {
    lowBatteryMode = true;
  } else if (batteryPct >= settings.lowBattPercent + LOW_BATT_HYSTERESIS) {
    lowBatteryMode = false;
  }
}

// Sensor faults and low battery mode in one value, so Home Assistant sees a failed sensor
uint8_t deviceStatus() {
  return sensorFaults | (lowBatteryMode ? STATUS_LOW_BATTERY : 0);
}

/************************ Report on change *****************************/
// True when this measurement is not worth starting the radio for: a temperature change is
// set, and since the last report nothing moved by its own change setting, the status is the
// same and the longest time without a report has not passed. A humidity or pressure change
// of 0 means that reading is not watched. A sensor that failed to read is not compared.
bool nothingToReport(float temperature, float humidity, float pressure) {
  if (settings.tempDeltaTenths == 0 || !lastReportValid) {
    return false;
  }
  if (minutesSinceReport >= settings.maxSilentMin || deviceStatus() != lastStatus) {
    return false;
  }
  if (!(sensorFaults & STATUS_AHT_FAULT)) {
    if (fabsf(temperature - lastTemperature) >= settings.tempDeltaTenths / 10.0f) {
      return false;
    }
    if (settings.humDelta > 0 && fabsf(humidity - lastHumidity) >= settings.humDelta) {
      return false;
    }
  }
  if (!(sensorFaults & STATUS_BMP_FAULT) && settings.pressDeltaTenths > 0 && fabsf(pressure - lastPressure) >= settings.pressDeltaTenths / 10.0f) {
    return false;
  }
  return true;
}

/************************ Antenna *****************************/
// The XIAO ESP32-C6 has an RF switch between the radio and its two antennas:
//   WIFI_ENABLE (GPIO3)      LOW  = switch powered
//   WIFI_ANT_CONFIG (GPIO14) HIGH = external U.FL antenna, LOW = on-board antenna
// The switch is only powered while the radio is in use.
void antennaOn() {
  pinMode(WIFI_ANT_CONFIG, OUTPUT);
  digitalWrite(WIFI_ANT_CONFIG, USE_EXTERNAL_ANTENNA ? HIGH : LOW);
  digitalWrite(WIFI_ENABLE, LOW);
  radioUsed = true;
}

// Switch off, and hold the pin so it stays off through deep sleep (pins float otherwise)
void antennaOff() {
  digitalWrite(WIFI_ENABLE, HIGH);
  gpio_hold_en((gpio_num_t)WIFI_ENABLE);
}

/************************ Sleep *****************************/
// Interval to sleep for right now. Low battery mode can only make it longer, never shorter.
uint16_t activeIntervalMin() {
  if (lowBatteryMode) {
    return max(settings.intervalMin, settings.lowBattIntervalMin);
  }
  return settings.intervalMin;
}

// Power everything down and deep sleep. Never returns: the timer wake-up restarts setup().
void deepSleepMinutes(uint32_t minutes) {
  LOG("Awake for %lu ms, sleeping %lu min\r\n", millis(), minutes);
  if (radioUsed) {
    lastAwakeMs = millis();
  }
  minutesSinceReport = (uint16_t)min((uint32_t)minutesSinceReport + minutes, (uint32_t)UINT16_MAX);
  minutesSinceSlow = (uint16_t)min((uint32_t)minutesSinceSlow + minutes, (uint32_t)UINT16_MAX);
#if DEBUG_LOG
  Serial.flush();
#endif
  Wire.end();
  digitalWrite(LED_BUILTIN, HIGH);
  antennaOff();
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
  lastReportValid = false;  // the next wake-up tries again whatever the measurement is
  deepSleepMinutes(min((uint32_t)activeIntervalMin() << networkFailures, (uint32_t)LOW_BATT_INTERVAL_MAX_MIN));
}

/************************ Zigbee *****************************/
// One bit per report the sketch sends
#define REPORT_TEMPERATURE  0x001
#define REPORT_HUMIDITY     0x002
#define REPORT_BATTERY      0x004
#define REPORT_PRESSURE     0x008
#define REPORT_VOLTAGE      0x010
#define REPORT_ACTIVE       0x020
#define REPORT_INTERVAL     0x040
#define REPORT_LOW_BATT     0x080
#define REPORT_LOW_INTERVAL 0x100
#define REPORT_TEMP_DELTA   0x200
#define REPORT_MAX_SILENT   0x400
#define REPORT_AWAKE        0x800
#define REPORT_STATUS       0x1000
#define REPORT_HUM_DELTA    0x2000
#define REPORT_PRESS_DELTA  0x4000
#define REPORT_SYNC         0x8000
#define REPORT_STAGES       0x10000

// The REPORT_ bit of the report sent from this endpoint and cluster
uint32_t reportBit(uint8_t endpoint, uint16_t cluster) {
  switch (cluster) {
    case ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT:         return REPORT_TEMPERATURE;
    case ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT: return REPORT_HUMIDITY;
    case ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG:             return REPORT_BATTERY;
    case ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT:
      switch (endpoint) {
        case EP_PRESSURE:     return REPORT_PRESSURE;
        case EP_INTERVAL:     return REPORT_VOLTAGE;
        case EP_LOW_BATT:     return REPORT_ACTIVE;
        case EP_LOW_INTERVAL: return REPORT_AWAKE;
        case EP_STATUS:       return REPORT_STATUS;
        case EP_HUM_DELTA:    return REPORT_STAGES;
      }
      return 0;
    case ESP_ZB_ZCL_CLUSTER_ID_ANALOG_OUTPUT:
      switch (endpoint) {
        case EP_PRESSURE:     return REPORT_TEMP_DELTA;
        case EP_INTERVAL:     return REPORT_INTERVAL;
        case EP_LOW_BATT:     return REPORT_LOW_BATT;
        case EP_LOW_INTERVAL: return REPORT_LOW_INTERVAL;
        case EP_STATUS:       return REPORT_MAX_SILENT;
        case EP_HUM_DELTA:    return REPORT_HUM_DELTA;
        case EP_PRESS_DELTA:  return REPORT_PRESS_DELTA;
        case EP_SYNC:         return REPORT_SYNC;
      }
      return 0;
  }
  return 0;
}

// The coordinator answers every attribute report with a default response. Tracking them
// tells when everything has been delivered, so the device can sleep as early as possible.
// They are tracked per report, not counted: the Zigbee stack also sends a report of its own
// after boot (seen for the low battery interval), and its answer must not stand in for a
// report of the sketch that is still on its way.
void onGlobalResponse(zb_cmd_type_t command, esp_zb_zcl_status_t status, uint8_t endpoint, uint16_t cluster) {
  if (command == ZB_CMD_REPORT_ATTRIBUTE && status == ESP_ZB_ZCL_STATUS_SUCCESS) {
    reportsConfirmed = reportsConfirmed | reportBit(endpoint, cluster);
  }
}

// Block until every expected report is confirmed, or the timeout has passed
void waitForReports(uint32_t expected, uint32_t timeoutMs) {
  uint32_t start = millis();
  while ((reportsConfirmed & expected) != expected && millis() - start < timeoutMs) {
    delay(10);
  }
}

// Every report the sketch sends, in the order they go out
struct Report {
  uint32_t bit;
  bool (*send)();
};
const Report REPORTS[] = {
  {REPORT_TEMPERATURE, [] { return zbClimate.reportTemperature(); }},
  {REPORT_HUMIDITY, [] { return zbClimate.reportHumidity(); }},
  {REPORT_PRESSURE, [] { return zbPressure.reportAnalogInput(); }},
  {REPORT_BATTERY, [] { return zbClimate.reportBatteryPercentage(); }},
  {REPORT_VOLTAGE, [] { return zbInterval.reportAnalogInput(); }},
  {REPORT_STATUS, [] { return zbStatus.reportAnalogInput(); }},
  {REPORT_AWAKE, [] { return zbLowInterval.reportAnalogInput(); }},
  {REPORT_STAGES, [] { return zbHumDelta.reportAnalogInput(); }},
  {REPORT_ACTIVE, [] { return zbLowBatt.reportAnalogInput(); }},
  {REPORT_SYNC, [] { return zbSync.reportAnalogOutput(); }},
  {REPORT_INTERVAL, [] { return zbInterval.reportAnalogOutput(); }},
  {REPORT_LOW_BATT, [] { return zbLowBatt.reportAnalogOutput(); }},
  {REPORT_LOW_INTERVAL, [] { return zbLowInterval.reportAnalogOutput(); }},
  {REPORT_TEMP_DELTA, [] { return zbPressure.reportAnalogOutput(); }},
  {REPORT_MAX_SILENT, [] { return zbStatus.reportAnalogOutput(); }},
  {REPORT_HUM_DELTA, [] { return zbHumDelta.reportAnalogOutput(); }},
  {REPORT_PRESS_DELTA, [] { return zbPressDelta.reportAnalogOutput(); }},
};
// The seven settings. Only sent when Zigbee2MQTT asks for them, and while pairing.
#define REPORT_SETTINGS (REPORT_INTERVAL | REPORT_LOW_BATT | REPORT_LOW_INTERVAL | REPORT_TEMP_DELTA | REPORT_MAX_SILENT | REPORT_HUM_DELTA | REPORT_PRESS_DELTA)
// The values that rarely change
#define REPORT_SLOW (REPORT_BATTERY | REPORT_VOLTAGE | REPORT_STATUS | REPORT_ACTIVE | REPORT_AWAKE | REPORT_STAGES)

// Temperature, humidity and pressure. A sensor that failed to read is left out rather than
// reported with a bogus value.
uint32_t measurementReports() {
  return ((sensorFaults & STATUS_AHT_FAULT) ? 0 : REPORT_TEMPERATURE | REPORT_HUMIDITY) | ((sensorFaults & STATUS_BMP_FAULT) ? 0 : REPORT_PRESSURE);
}

// The slow values worth sending now: all of them every SLOW_REPORT_MIN, in between only the
// ones that changed. The awake time is a little different every time, so in between it only
// goes out when it has moved by more than a fifth.
uint32_t slowReports() {
  if (!lastReportValid || minutesSinceSlow >= SLOW_REPORT_MIN) {
    return REPORT_SLOW;
  }
  uint32_t due = 0;
  if (abs((int)batteryPct - (int)lastBatteryPct) >= BATTERY_REPORT_STEP) {
    due |= REPORT_BATTERY | REPORT_VOLTAGE;
  }
  if (deviceStatus() != lastStatus) {
    due |= REPORT_STATUS;
  }
  if (activeIntervalMin() != lastActiveMin) {
    due |= REPORT_ACTIVE;
  }
  uint32_t awakeChange = lastAwakeMs > lastAwakeSent ? lastAwakeMs - lastAwakeSent : lastAwakeSent - lastAwakeMs;
  if (awakeChange > lastAwakeSent / 5) {
    due |= REPORT_AWAKE | REPORT_STAGES;
  }
  return due;
}

// Remember what was sent, for slowReports() on the next wake-ups
void rememberSlowReports(uint32_t sent) {
  if (sent & REPORT_BATTERY) {
    lastBatteryPct = batteryPct;
  }
  if (sent & REPORT_STATUS) {
    lastStatus = deviceStatus();
  }
  if (sent & REPORT_ACTIVE) {
    lastActiveMin = activeIntervalMin();
  }
  if (sent & REPORT_AWAKE) {
    lastAwakeSent = lastAwakeMs;
  }
  if ((sent & REPORT_SLOW) == REPORT_SLOW) {
    minutesSinceSlow = 0;
  }
}

// Send the chosen reports in batches of REPORT_BATCH, waiting for each batch to be confirmed.
// The stack queues only a few outgoing messages: fifteen reports fired in one go lost about a
// third of them on the way. Returns the REPORT_ bits of the reports sent.
uint32_t sendReports(uint32_t which) {
  uint32_t sent = 0;
  uint8_t inBatch = 0;
  for (const Report &report : REPORTS) {
    if (!(which & report.bit)) {
      continue;
    }
    if (report.send()) {
      sent |= report.bit;
    }
    if (++inBatch >= REPORT_BATCH) {
      inBatch = 0;
      waitForReports(sent, BATCH_TIMEOUT_MS);
    }
  }
  return sent;
}

// One round of reports that must arrive, used for the settings: send them, wait until they
// are confirmed, and send the ones that were not confirmed a second time. With no answer at
// all the coordinator is not there, and trying again would only cost battery. Returns the
// REPORT_ bits of the reports sent.
uint32_t report(uint32_t which) {
  reportsConfirmed = 0;
  uint32_t sent = sendReports(which);
  waitForReports(sent, REPORT_TIMEOUT_MS);
  uint32_t missing = sent & ~reportsConfirmed;
  if (missing && reportsConfirmed) {
    waitForReports(sendReports(missing), REPORT_TIMEOUT_MS);
  }
  LOG("%d/%d reports confirmed (sent 0x%05lx, repeated 0x%05lx, confirmed 0x%05lx)\r\n", __builtin_popcount(reportsConfirmed & sent),
      __builtin_popcount(sent), (unsigned long)sent, (unsigned long)(reportsConfirmed ? missing : 0), (unsigned long)reportsConfirmed);
  return sent;
}

// Hand the settings, their checksum and the values that follow from them to the stack
void publishSettings() {
  zbInterval.setAnalogOutput(settings.intervalMin);
  zbLowBatt.setAnalogOutput(settings.lowBattPercent);
  zbLowInterval.setAnalogOutput(settings.lowBattIntervalMin);
  zbPressure.setAnalogOutput(settings.tempDeltaTenths / 10.0f);
  zbStatus.setAnalogOutput(settings.maxSilentMin);
  zbHumDelta.setAnalogOutput(settings.humDelta);
  zbPressDelta.setAnalogOutput(settings.pressDeltaTenths / 10.0f);
  zbSync.setAnalogOutput(settingsChecksum());
  zbLowBatt.setAnalogInput(activeIntervalMin());
  zbStatus.setAnalogInput(deviceStatus());
}

// Settings written by Zigbee2MQTT: store them, and report what follows from them if that changed
void commitSettings() {
  if (!settingsChanged) {
    return;
  }
  settingsChanged = false;
  saveSettings();
  updateLowBatteryMode();
  LOG("New settings: interval %u min, low battery %u %% -> %u min, change %u x 0.1 C / %u %% / %u x 0.1 hPa, silent up to %u min\r\n",
      settings.intervalMin, settings.lowBattPercent, settings.lowBattIntervalMin, settings.tempDeltaTenths, settings.humDelta,
      settings.pressDeltaTenths, settings.maxSilentMin);
  publishSettings();
  uint32_t due = slowReports() & (REPORT_STATUS | REPORT_ACTIVE);
  if (due) {
    rememberSlowReports(report(due));
  }
}

// Zigbee2MQTT does not know the settings yet and asked for them
void answerSyncRequest() {
  if (syncAnswer == 2) {
    syncAnswer = 0;
    report(REPORT_SETTINGS);
  }
}

// A device that has not been paired yet, or BOOT held after a reset: stay awake so
// Zigbee2MQTT can interview and configure the device. The LED blinks while the window is open.
// Holding BOOT leaves the network so the device can be paired again.
void pairingWindow() {
  LOG("Pairing window open for %d s\r\n", PAIRING_WINDOW_MS / 1000);
  uint32_t start = millis();
  uint32_t lastReport = 0;
  // The press that opened the window must be released before BOOT can leave the network
  bool bootArmed = false;
  while (millis() - start < PAIRING_WINDOW_MS) {
    digitalWrite(LED_BUILTIN, (millis() / 500) % 2);
    // BOOT held for FACTORY_RESET_HOLD_MS: erase the Zigbee network data and restart
    if (digitalRead(BOOT_PIN) == HIGH) {
      bootArmed = true;
    } else if (bootArmed) {
      uint32_t pressed = millis();
      while (digitalRead(BOOT_PIN) == LOW && millis() - pressed < FACTORY_RESET_HOLD_MS) {
        delay(20);
      }
      if (millis() - pressed >= FACTORY_RESET_HOLD_MS) {
        LOG("Factory reset\r\n");
        savePairingDone(false);
        Zigbee.factoryReset(true);
      }
    }
    // Reports sent before Zigbee2MQTT bound the clusters went nowhere, so repeat them
    if (millis() - lastReport > 15000) {
      lastReport = millis();
      publishSettings();
      sendReports(measurementReports() | REPORT_SLOW | REPORT_SYNC);
    }
    // Settings written during the window are applied right away
    answerSyncRequest();
    commitSettings();
    delay(50);
  }
}

/********************* Arduino functions **************************/
void setup() {
  mainTask = xTaskGetCurrentTaskHandle();

  // Anything but a timer wake-up is a power-on or a press of the reset button
  bool coldBoot = esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER;

#if DEBUG_LOG
  Serial.begin(115200);
  delay(coldBoot ? 2000 : 0);  // time to open the serial monitor
#endif
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);  // LED is active low
  pinMode(BOOT_PIN, INPUT_PULLUP);

  // The board package powers the RF switch at boot. Keep it off until the radio is needed,
  // then release the hold that kept it off during deep sleep.
  pinMode(WIFI_ENABLE, OUTPUT);
  digitalWrite(WIFI_ENABLE, HIGH);
  gpio_hold_dis((gpio_num_t)WIFI_ENABLE);

  loadSettings();
  // After power-on or reset everything is reported, whatever was sent before
  if (coldBoot) {
    lastReportValid = false;
  }

  // Battery first: an empty cell goes straight back to sleep without powering the radio.
  // Below 2.5 V there is no cell on the divider (USB only), so no battery logic applies.
  batteryMv = readBatteryMillivolts();
  batteryValid = batteryMv > 2500;
  batteryPct = batteryValid ? batteryPercent(batteryMv) : 0;
  LOG("%s (reset reason %d). Battery %u mV, %u %%\r\n", coldBoot ? "Power-on or reset" : "Timer wake-up", (int)esp_reset_reason(), batteryMv, batteryPct);
  // A manual reset skips the cutoff once, so the device can still be reached on a weak cell.
  if (batteryValid && batteryMv < BATTERY_CUTOFF_MV && !coldBoot) {
    deepSleepMinutes(CUTOFF_SLEEP_MIN);
  }
  updateLowBatteryMode();

  // Sensors are read before the radio starts to keep the radio-on time short
  Wire.begin();
  bool climateOk = false, pressureOk = false;
  float temperature = 0, humidity = 0, pressure = 0;
  // AHT20: one measurement takes about 80 ms, the sensor idles by itself afterwards
  if (aht.begin()) {
    sensors_event_t humEvent, tempEvent;
    climateOk = aht.getEvent(&humEvent, &tempEvent);
    temperature = tempEvent.temperature;
    humidity = humEvent.relative_humidity;
  }
  // BMP280: the I2C address depends on the module, so both are tried
  if (bmp.begin(0x77) || bmp.begin(0x76)) {
    // Forced mode: one conversion, then the BMP280 drops back to its sleep mode
    bmp.setSampling(Adafruit_BMP280::MODE_FORCED, Adafruit_BMP280::SAMPLING_X1, Adafruit_BMP280::SAMPLING_X4, Adafruit_BMP280::FILTER_OFF);
    if (bmp.takeForcedMeasurement()) {
      // Station pressure. The Zigbee2MQTT converter turns it into sea level pressure.
      pressure = bmp.readPressure() / 100.0f;
      pressureOk = true;
    }
  }
  sensorFaults = (climateOk ? 0 : STATUS_AHT_FAULT) | (pressureOk ? 0 : STATUS_BMP_FAULT);
  LOG("T %.2f C, RH %.1f %%, P %.1f hPa (aht %d, bmp %d)\r\n", temperature, humidity, pressure, climateOk, pressureOk);

  // Nothing new to tell: skip the radio, which is most of the cost of a wake-up. Power-on
  // and the reset button always report.
  if (!coldBoot && nothingToReport(temperature, humidity, pressure)) {
    LOG("No change, %u min since the last report\r\n", minutesSinceReport);
    deepSleepMinutes(activeIntervalMin());
  }

  // Endpoint 10: temperature, humidity and battery. The manufacturer and model strings are
  // what Zigbee2MQTT uses to pick the external converter.
  zbClimate.setManufacturerAndModel("CustomDIY", "XIAO_C6_Outdoor");
  zbClimate.setMinMaxValue(-40, 85);
  zbClimate.setTolerance(0.3);
  zbClimate.addHumiditySensor(0, 100, 2, 0);
  zbClimate.setPowerSource(ZB_POWER_SOURCE_BATTERY, batteryPct, batteryMv / 100);

  // Endpoint 11: temperature change setting, plus pressure as a float for 0.1 hPa resolution
  zbPressure.addAnalogOutput();
  zbPressure.setAnalogOutputDescription("Temperature change to report (C)");
  zbPressure.setAnalogOutputResolution(0.1);
  zbPressure.setAnalogOutputMinMax(0, TEMP_DELTA_MAX_TENTHS / 10.0f);
  zbPressure.onAnalogOutputChange(onTempDeltaChange);
  zbPressure.addAnalogInput();
  zbPressure.setAnalogInputDescription("Pressure (hPa)");
  zbPressure.setAnalogInputResolution(0.1);

  // Endpoint 12: report interval setting, plus the battery voltage reading
  zbInterval.addAnalogOutput();
  zbInterval.setAnalogOutputDescription("Report interval (min)");
  zbInterval.setAnalogOutputResolution(1);
  zbInterval.setAnalogOutputMinMax(INTERVAL_MIN_MIN, INTERVAL_MAX_MIN);
  zbInterval.onAnalogOutputChange(onIntervalChange);
  zbInterval.addAnalogInput();
  zbInterval.setAnalogInputDescription("Battery voltage (V)");
  zbInterval.setAnalogInputResolution(0.001);

  // Endpoint 13: low battery threshold setting, plus the interval currently in effect
  zbLowBatt.addAnalogOutput();
  zbLowBatt.setAnalogOutputDescription("Low battery threshold (%)");
  zbLowBatt.setAnalogOutputResolution(1);
  zbLowBatt.setAnalogOutputMinMax(LOW_BATT_PERCENT_MIN, LOW_BATT_PERCENT_MAX);
  zbLowBatt.onAnalogOutputChange(onLowBattChange);
  zbLowBatt.addAnalogInput();
  zbLowBatt.setAnalogInputDescription("Interval in effect (min)");
  zbLowBatt.setAnalogInputResolution(1);

  // Endpoint 14: low battery interval setting, plus the awake time of the last report
  zbLowInterval.addAnalogOutput();
  zbLowInterval.setAnalogOutputDescription("Low battery interval (min)");
  zbLowInterval.setAnalogOutputResolution(1);
  zbLowInterval.setAnalogOutputMinMax(LOW_BATT_INTERVAL_MIN_MIN, LOW_BATT_INTERVAL_MAX_MIN);
  zbLowInterval.onAnalogOutputChange(onLowIntervalChange);
  zbLowInterval.addAnalogInput();
  zbLowInterval.setAnalogInputDescription("Awake time (ms)");
  zbLowInterval.setAnalogInputResolution(1);

  // Endpoint 15: longest time without a report setting, plus the device status
  zbStatus.addAnalogOutput();
  zbStatus.setAnalogOutputDescription("Longest time without a report (min)");
  zbStatus.setAnalogOutputResolution(1);
  zbStatus.setAnalogOutputMinMax(MAX_SILENT_MIN_MIN, MAX_SILENT_MAX_MIN);
  zbStatus.onAnalogOutputChange(onMaxSilentChange);
  zbStatus.addAnalogInput();
  zbStatus.setAnalogInputDescription("Device status");
  zbStatus.setAnalogInputResolution(1);

  // Endpoints 16 and 17: humidity and pressure change settings, plus the stage times on 16
  zbHumDelta.addAnalogOutput();
  zbHumDelta.setAnalogOutputDescription("Humidity change to report (%)");
  zbHumDelta.setAnalogOutputResolution(1);
  zbHumDelta.setAnalogOutputMinMax(0, HUM_DELTA_MAX);
  zbHumDelta.onAnalogOutputChange(onHumDeltaChange);
  zbHumDelta.addAnalogInput();
  zbHumDelta.setAnalogInputDescription("Stage times");
  zbHumDelta.setAnalogInputResolution(1);
  zbPressDelta.addAnalogOutput();
  zbPressDelta.setAnalogOutputDescription("Pressure change to report (hPa)");
  zbPressDelta.setAnalogOutputResolution(0.1);
  zbPressDelta.setAnalogOutputMinMax(0, PRESS_DELTA_MAX_TENTHS / 10.0f);
  zbPressDelta.onAnalogOutputChange(onPressDeltaChange);

  // Endpoint 18: settings checksum out, Zigbee2MQTT's answer in
  zbSync.addAnalogOutput();
  zbSync.setAnalogOutputDescription("Settings sync");
  zbSync.setAnalogOutputResolution(1);
  zbSync.setAnalogOutputMinMax(0, 100000);
  zbSync.onAnalogOutputChange(onSyncAnswer);

  Zigbee.onGlobalDefaultResponse(onGlobalResponse);
  Zigbee.addEndpoint(&zbClimate);
  Zigbee.addEndpoint(&zbPressure);
  Zigbee.addEndpoint(&zbInterval);
  Zigbee.addEndpoint(&zbLowBatt);
  Zigbee.addEndpoint(&zbLowInterval);
  Zigbee.addEndpoint(&zbStatus);
  Zigbee.addEndpoint(&zbHumDelta);
  Zigbee.addEndpoint(&zbPressDelta);
  Zigbee.addEndpoint(&zbSync);

  // Join / rejoin. The parent must not forget this child between two wake-ups, hence the long
  // aging timeout. The device is awake for about a second, so the keep-alive poll never
  // comes round; answers of the coordinator arrive directly.
  uint32_t joinTimeout = pairingDone ? REJOIN_TIMEOUT_MS : JOIN_TIMEOUT_MS;
  esp_zb_cfg_t zigbeeConfig = ZIGBEE_DEFAULT_ED_CONFIG();
  zigbeeConfig.nwk_cfg.zed_cfg.ed_timeout = ESP_ZB_ED_AGING_TIMEOUT_16384MIN;
  zigbeeConfig.nwk_cfg.zed_cfg.keep_alive = PARENT_POLL_MS;
  // Every report needs its own binding to the coordinator, and the stack keeps 16 by default
  esp_zb_aps_src_binding_table_size_set(BINDING_TABLE_SIZE);
  esp_zb_aps_dst_binding_table_size_set(BINDING_TABLE_SIZE);
  Zigbee.setTimeout(joinTimeout);
  antennaOn();  // sensors are done, the radio starts now
  uint32_t joinStart = millis();
  bool started = Zigbee.begin(&zigbeeConfig, false);
  while (started && !Zigbee.connected() && millis() - joinStart < joinTimeout) {
    delay(10);
  }
  uint32_t joinMs = millis() - joinStart;
  // BOOT held at this point after a reset asks for the pairing window on a paired device.
  // (BOOT cannot wake the device from deep sleep, and held during the reset itself it would
  // start the bootloader, so it is pressed right after reset and held until the LED blinks.)
  bool windowRequested = coldBoot && digitalRead(BOOT_PIN) == LOW;
  // No network found in time (typically an unpaired device with permit join off)
  if (!started || !Zigbee.connected()) {
    // A paired device whose network is gone can still be made to leave it
    if (started && windowRequested) {
      pairingWindow();
    }
    sleepAfterNetworkFailure();
  }
  LOG("Connected after %lu ms\r\n", millis());

  // Hand the values to the stack, then report them
  syncAnswer = 0;
  if (climateOk) {
    zbClimate.setTemperature(temperature);
    zbClimate.setHumidity(humidity);
  }
  if (pressureOk) {
    zbPressure.setAnalogInput(pressure);
  }
  zbClimate.setBatteryPercentage(batteryPct);
  zbClimate.setBatteryVoltage(batteryMv / 100);
  zbInterval.setAnalogInput(batteryMv / 1000.0f);
  zbLowInterval.setAnalogInput(lastAwakeMs);
  zbHumDelta.setAnalogInput(lastStages);
  publishSettings();
  // The measurements are sent without waiting for each confirmation: the confirmations of
  // the coordinator are what goes missing now and then, not the reports, and waiting for
  // them more than doubled the awake time of such a wake-up. The answer to the settings
  // checksum, which comes right after, is the one sign needed that the coordinator is there.
  reportsConfirmed = 0;
  uint32_t reportStart = millis();
  uint32_t sent = sendReports(measurementReports() | slowReports() | REPORT_SYNC);
  uint32_t reportMs = millis() - reportStart;
  uint32_t syncMs = 0;

  if (!pairingDone || windowRequested) {
    pairingWindow();
    if (!pairingDone) {
      savePairingDone(true);
    }
    silentCycles = 0;
    networkFailures = 0;
  } else {
    // Sleep as soon as Zigbee2MQTT has answered the checksum. Before its SYNC_OK it may write
    // settings, each of which restarts the wait. CONFIG_WINDOW_MS is only the fallback for a
    // lost answer; a changed setting missed that way is written again on the next report.
    uint32_t windowStart = millis();
    lastWriteMs = windowStart;
    while (syncAnswer == 0 && millis() - lastWriteMs < CONFIG_WINDOW_MS) {
      delay(10);
    }
    syncMs = millis() - windowStart;
    LOG("Sync answer %u after %lu ms, confirmed 0x%05lx of 0x%05lx\r\n", syncAnswer, syncMs, (unsigned long)reportsConfirmed, (unsigned long)sent);
    if (syncAnswer == 0 && reportsConfirmed == 0) {
      // A device that was joined before counts as connected right after boot, so a missing
      // coordinator only shows up here: nothing came back at all. That also happens now and
      // then although the reports did arrive (seen often behind a range extender), so the
      // first SILENT_CYCLES_ALLOWED times in a row are taken as delivered. After that it
      // counts as a network failure.
      if (silentCycles >= SILENT_CYCLES_ALLOWED) {
        sleepAfterNetworkFailure();
      }
      silentCycles++;
    } else {
      silentCycles = 0;
      networkFailures = 0;
      delay(ANSWER_GRACE_MS);
      answerSyncRequest();
      commitSettings();
    }
  }

  // What Home Assistant now knows, for the next wake-up to compare against
  lastReportValid = true;
  if (climateOk) {
    lastTemperature = temperature;
    lastHumidity = humidity;
  }
  if (pressureOk) {
    lastPressure = pressure;
  }
  rememberSlowReports(sent);
  minutesSinceReport = 0;
  lastStages = (min(joinMs / 10, (uint32_t)255) << 16) | (min(reportMs / 10, (uint32_t)255) << 8) | min(syncMs / 10, (uint32_t)255);
  deepSleepMinutes(activeIntervalMin());
}

// Never reached: setup() always ends in deep sleep
void loop() {}
