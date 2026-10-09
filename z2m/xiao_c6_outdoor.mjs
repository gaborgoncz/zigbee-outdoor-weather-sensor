// Zigbee2MQTT external converter for the XIAO ESP32-C6 outdoor sensor
// (firmware: ../xiao_c6_outdoor_sensor/xiao_c6_outdoor_sensor.ino)
//
// What it does:
//   - turns the device's reports into temperature, humidity, pressure, battery and voltage
//   - works out sea level pressure, dew point and absolute humidity from them
//   - decodes the device status into a sensor status and a low battery mode flag
//   - exposes seven settings as number entities in Home Assistant
//   - holds a changed setting until the sleeping device reports, then writes it
//   - answers the device's settings checksum, so it can go back to sleep without waiting
//
// Install: Zigbee2MQTT UI -> Settings -> Dev console -> External converters -> create
// "xiao_c6_outdoor.mjs", paste this file, save, restart Zigbee2MQTT.

import * as exposes from 'zigbee-herdsman-converters/lib/exposes';

const e = exposes.presets;
const ea = exposes.access;

// Settings live in the analog output cluster of these endpoints.
// The ranges must match the limits in the firmware.
const SETTINGS = {
    report_interval: {endpoint: 12, min: 1, max: 240, step: 1, unit: 'min', description: 'Time between two measurements'},
    low_battery_threshold: {endpoint: 13, min: 5, max: 80, step: 1, unit: '%', description: 'Battery level at or below which the low battery interval is used'},
    low_battery_interval: {endpoint: 14, min: 5, max: 720, step: 5, unit: 'min', description: 'Time between two measurements while the battery is low'},
    temperature_change: {
        endpoint: 11,
        min: 0,
        max: 5,
        step: 0.1,
        decimals: 1,
        unit: '°C',
        description: 'Report only when the temperature has changed by this much since the last report. 0 reports every measurement',
    },
    humidity_change: {
        endpoint: 16,
        min: 0,
        max: 20,
        step: 1,
        unit: '%',
        description: 'With temperature change set: also report when the humidity has changed by this much. 0 ignores humidity',
    },
    pressure_change: {
        endpoint: 17,
        min: 0,
        max: 10,
        step: 0.1,
        decimals: 1,
        unit: 'hPa',
        description: 'With temperature change set: also report when the pressure has changed by this much. 0 ignores pressure',
    },
    max_silent_interval: {
        endpoint: 15,
        min: 10,
        max: 1440,
        step: 10,
        unit: 'min',
        description: 'Longest time without a report while nothing changes (used with temperature change)',
    },
};
// Values travel as 32 bit floats, so they are rounded to the precision of the setting
const roundSetting = (setting, value) => Number(Number(value).toFixed(setting.decimals ?? 0));
// The settings in the order of the checksum, which must match settingsChecksum() in the firmware
const SETTING_ORDER = ['report_interval', 'low_battery_threshold', 'low_battery_interval', 'temperature_change', 'max_silent_interval', 'humidity_change', 'pressure_change'];
// The device reports a checksum of its settings on this endpoint and waits for one of these answers
const SYNC_ENDPOINT = 18;
const SYNC_OK = 70000; // nothing (more) to change
const SYNC_SEND = 70001; // settings not known here: report all of them

// Settings with a decimal travel as tenths inside the device
const checksum = (state) => SETTING_ORDER.reduce((sum, key) => (sum * 31 + Math.round(state[key] * 10 ** (SETTINGS[key].decimals ?? 0))) % 65521, 0);

// Reverse lookup: endpoint number -> setting name
const SETTING_BY_ENDPOINT = Object.fromEntries(Object.entries(SETTINGS).map(([key, s]) => [s.endpoint, key]));

// Bits 0 and 1 of the device status are the sensors that failed to read, bit 2 is low battery mode
const SENSOR_STATUS = ['ok', 'aht20_fault', 'bmp280_fault', 'aht20_and_bmp280_fault'];
const STATUS_LOW_BATTERY = 0x04;

// Dew point and absolute humidity (Magnus formula). Temperature and humidity arrive in
// separate reports, so the one that is missing comes from the last known state.
const derived = (temperature, humidity) => {
    if (typeof temperature !== 'number' || typeof humidity !== 'number' || humidity <= 0) return {};
    const magnus = (17.62 * temperature) / (243.12 + temperature);
    const gamma = Math.log(humidity / 100) + magnus;
    const vapourPressure = (humidity / 100) * 6.112 * Math.exp(magnus); // hPa
    return {
        dew_point: Math.round(((243.12 * gamma) / (17.62 - gamma)) * 10) / 10,
        absolute_humidity: Math.round(((216.7 * vapourPressure) / (273.15 + temperature)) * 10) / 10,
    };
};

// Device -> Zigbee2MQTT
const fzLocal = {
    // Standard clusters send hundredths of a degree / percent
    temperature: {
        cluster: 'msTemperatureMeasurement',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options, meta) => {
            if (msg.data.measuredValue === undefined) return;
            const temperature = msg.data.measuredValue / 100;
            return {temperature, ...derived(temperature, meta.state?.humidity)};
        },
    },
    humidity: {
        cluster: 'msRelativeHumidity',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options, meta) => {
            if (msg.data.measuredValue === undefined) return;
            const humidity = msg.data.measuredValue / 100;
            return {humidity, ...derived(meta.state?.temperature, humidity)};
        },
    },
    // Zigbee reports battery in half percent steps
    battery: {
        cluster: 'genPowerCfg',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg) => {
            if (msg.data.batteryPercentageRemaining !== undefined) return {battery: msg.data.batteryPercentageRemaining / 2};
        },
    },
    // Float readings, told apart by the endpoint they arrive on
    analog_input: {
        cluster: 'genAnalogInput',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options) => {
            const value = msg.data.presentValue;
            if (value === undefined) return;
            switch (msg.endpoint.ID) {
                case 11: {
                    // The device sends station pressure. With an altitude set it is converted
                    // to sea level pressure (barometric formula).
                    const altitude = Number(options?.altitude) || 0;
                    const pressure = altitude > 0 ? value / Math.pow(1 - altitude / 44330, 5.255) : value;
                    return {pressure: Math.round(pressure * 10) / 10};
                }
                case 12:
                    return {voltage: Math.round(value * 1000)};
                case 13:
                    return {active_interval: Math.round(value)};
                case 14:
                    return {awake_time: Math.round(value)};
                case 16: {
                    // Three bytes: join, reports and wait for the settings answer, in units of 10 ms
                    const stages = Math.round(value);
                    return {join_time: ((stages >> 16) & 0xff) * 10, report_time: ((stages >> 8) & 0xff) * 10, sync_time: (stages & 0xff) * 10};
                }
                case 15: {
                    const status = Math.round(value);
                    return {sensor_status: SENSOR_STATUS[status & 0x03], low_battery_mode: (status & STATUS_LOW_BATTERY) !== 0};
                }
            }
        },
    },
    // Settings sync. The device does not send its settings every time, only a checksum of
    // them. The values Home Assistant wants are in the state; when their checksum is the same
    // there is nothing to do. Otherwise all settings are written, and the device applies them
    // before it goes back to sleep. Either way the last write tells it that nothing more comes.
    settings: {
        cluster: 'genAnalogOutput',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options, meta) => {
            if (msg.data.presentValue === undefined) return;
            const state = meta.state ?? {};

            if (msg.endpoint.ID === SYNC_ENDPOINT) {
                const answer = async () => {
                    if (SETTING_ORDER.some((key) => typeof state[key] !== 'number')) {
                        await msg.endpoint.write('genAnalogOutput', {presentValue: SYNC_SEND});
                        return;
                    }
                    if (checksum(state) !== Math.round(msg.data.presentValue)) {
                        for (const key of SETTING_ORDER) {
                            await msg.device.getEndpoint(SETTINGS[key].endpoint).write('genAnalogOutput', {presentValue: state[key]});
                        }
                    }
                    await msg.endpoint.write('genAnalogOutput', {presentValue: SYNC_OK});
                };
                // A failure only means the device is asleep again; its next report starts over
                answer().catch(() => {});
                return;
            }

            // A setting reported by the device, after SYNC_SEND or while pairing. It is only
            // taken over when nothing is known yet: otherwise Home Assistant's value counts.
            const key = SETTING_BY_ENDPOINT[msg.endpoint.ID];
            if (key && typeof state[key] !== 'number') return {[key]: roundSetting(SETTINGS[key], msg.data.presentValue)};
        },
    },
};

// Zigbee2MQTT -> device
const tzLocal = {
    settings: {
        key: Object.keys(SETTINGS),
        convertSet: async (entity, key, value, meta) => {
            const setting = SETTINGS[key];
            const wanted = Math.min(setting.max, Math.max(setting.min, roundSetting(setting, value)));
            // Usually asleep: remember the value, the settings sync delivers it on the next report.
            // The attempt below only succeeds during the pairing window after a reset.
            meta.device
                .getEndpoint(setting.endpoint)
                ?.write('genAnalogOutput', {presentValue: wanted})
                .catch(() => {});
            return {state: {[key]: wanted}};
        },
    },
};

export default {
    zigbeeModel: ['XIAO_C6_Outdoor'],
    model: 'XIAO_C6_Outdoor',
    vendor: 'CustomDIY',
    description: 'XIAO ESP32-C6 battery outdoor sensor (AHT20 + BMP280)',
    fromZigbee: [fzLocal.temperature, fzLocal.humidity, fzLocal.battery, fzLocal.analog_input, fzLocal.settings],
    toZigbee: [tzLocal.settings],
    exposes: [
        e.temperature(),
        e.humidity(),
        e.pressure(),
        e.battery(),
        e.battery_voltage(),
        e
            .numeric('active_interval', ea.STATE)
            .withUnit('min')
            .withDescription('Interval the device is currently using (longer while the battery is low)')
            .withCategory('diagnostic'),
        e.numeric('dew_point', ea.STATE).withUnit('°C').withDescription('Dew point, calculated from temperature and humidity'),
        e.numeric('absolute_humidity', ea.STATE).withUnit('g/m³').withDescription('Water in the air, calculated from temperature and humidity'),
        e.enum('sensor_status', ea.STATE, SENSOR_STATUS).withDescription('Sensors that failed to read on the last report').withCategory('diagnostic'),
        e
            .binary('low_battery_mode', ea.STATE, true, false)
            .withDescription('The device is using the low battery interval')
            .withCategory('diagnostic'),
        e
            .numeric('awake_time', ea.STATE)
            .withUnit('ms')
            .withDescription('How long the device was awake for the report before this one')
            .withCategory('diagnostic'),
        ...[
            ['join_time', 'rejoining the network'],
            ['report_time', 'sending the reports and waiting for their confirmations'],
            ['sync_time', 'waiting for the answer to the settings checksum'],
        ].map(([name, what]) =>
            e.numeric(name, ea.STATE).withUnit('ms').withDescription(`Part of the awake time spent ${what} (2550 at most)`).withCategory('diagnostic'),
        ),
        ...Object.entries(SETTINGS).map(([key, s]) =>
            e
                .numeric(key, ea.STATE_SET)
                .withValueMin(s.min)
                .withValueMax(s.max)
                .withValueStep(s.step)
                .withUnit(s.unit)
                .withDescription(`${s.description}. Applied the next time the device reports`)
                .withCategory('config'),
        ),
    ],
    // Shown under the device's settings in Zigbee2MQTT
    options: [
        e
            .numeric('altitude', ea.SET)
            .withUnit('m')
            .withValueMin(0)
            .withDescription('Altitude of the sensor. Above 0 the pressure is reported as sea level pressure instead of station pressure'),
    ],
    // Runs once after pairing: bind every reporting cluster to the coordinator. The firmware
    // sends its reports to bound targets only, so nothing arrives without these binds.
    configure: async (device, coordinatorEndpoint) => {
        const binds = {
            10: ['msTemperatureMeasurement', 'msRelativeHumidity', 'genPowerCfg'],
            11: ['genAnalogInput', 'genAnalogOutput'],
            12: ['genAnalogInput', 'genAnalogOutput'],
            13: ['genAnalogInput', 'genAnalogOutput'],
            14: ['genAnalogInput', 'genAnalogOutput'],
            15: ['genAnalogInput', 'genAnalogOutput'],
            16: ['genAnalogInput', 'genAnalogOutput'],
            17: ['genAnalogOutput'],
            18: ['genAnalogOutput'],
        };
        for (const [id, clusters] of Object.entries(binds)) {
            const endpoint = device.getEndpoint(Number(id));
            for (const cluster of clusters) {
                await endpoint.bind(cluster, coordinatorEndpoint);
            }
        }
    },
};
