// Zigbee2MQTT external converter for the XIAO ESP32-C6 outdoor sensor
// (firmware: ../xiao_c6_outdoor_sensor/xiao_c6_outdoor_sensor.ino)
//
// What it does:
//   - turns the device's reports into temperature, humidity, pressure, battery and voltage
//   - exposes three settings as number entities in Home Assistant
//   - holds a changed setting until the sleeping device wakes up, then writes it
//   - answers every settings report, so the device can go back to sleep without waiting
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
};
// A settings report within this time of the previous answer is the device confirming a value
// it has just applied. It goes to sleep right after, so that one is not answered again.
const ANSWER_HOLDOFF_MS = 5000;
const lastAnswer = new Map();

// Reverse lookup: endpoint number -> setting name
const SETTING_BY_ENDPOINT = Object.fromEntries(Object.entries(SETTINGS).map(([key, s]) => [s.endpoint, key]));

// Device -> Zigbee2MQTT
const fzLocal = {
    // Standard clusters send hundredths of a degree / percent
    temperature: {
        cluster: 'msTemperatureMeasurement',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg) => {
            if (msg.data.measuredValue !== undefined) return {temperature: msg.data.measuredValue / 100};
        },
    },
    humidity: {
        cluster: 'msRelativeHumidity',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg) => {
            if (msg.data.measuredValue !== undefined) return {humidity: msg.data.measuredValue / 100};
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
        convert: (model, msg) => {
            const value = msg.data.presentValue;
            if (value === undefined) return;
            switch (msg.endpoint.ID) {
                case 11:
                    return {pressure: Math.round(value * 10) / 10};
                case 12:
                    return {voltage: Math.round(value * 1000)};
                case 13:
                    return {active_interval: Math.round(value)};
            }
        },
    },
    // The device reports its three settings on every wake-up and stays awake until each one
    // has been answered with a write. The write carries the value Home Assistant wants, which
    // is usually the value the device already has; then it only means "nothing to change".
    settings: {
        cluster: 'genAnalogOutput',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options, meta) => {
            const key = SETTING_BY_ENDPOINT[msg.endpoint.ID];
            if (!key || msg.data.presentValue === undefined) return;
            const onDevice = Math.round(msg.data.presentValue);
            // Last value set from Home Assistant; undefined until the first report or set
            const wanted = typeof meta.state?.[key] === 'number' ? meta.state[key] : onDevice;
            const inSync = wanted === onDevice;

            const id = `${msg.device.ieeeAddr}:${key}`;
            const now = Date.now();
            if (!inSync || now - (lastAnswer.get(id) ?? 0) > ANSWER_HOLDOFF_MS) {
                lastAnswer.set(id, now);
                // Not awaited: a failure only means the device is asleep again, and a setting
                // that is still different is written again on its next report.
                msg.endpoint.write('genAnalogOutput', {presentValue: wanted}).catch(() => {});
            }

            // In sync: publish what the device uses. Otherwise publish nothing and keep the
            // wanted value; the device reports again once it has applied it.
            if (inSync) return {[key]: onDevice};
        },
    },
};

// Zigbee2MQTT -> device
const tzLocal = {
    settings: {
        key: Object.keys(SETTINGS),
        convertSet: async (entity, key, value, meta) => {
            const setting = SETTINGS[key];
            const wanted = Math.min(setting.max, Math.max(setting.min, Math.round(Number(value))));
            // Usually asleep: remember the value, fzLocal.settings delivers it on the next wake-up.
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
        ...Object.entries(SETTINGS).map(([key, s]) =>
            e
                .numeric(key, ea.STATE_SET)
                .withValueMin(s.min)
                .withValueMax(s.max)
                .withValueStep(s.step)
                .withUnit(s.unit)
                .withDescription(`${s.description}. Applied the next time the device wakes up`)
                .withCategory('config'),
        ),
    ],
    // Runs once after pairing: bind every reporting cluster to the coordinator. The firmware
    // sends its reports to bound targets only, so nothing arrives without these binds.
    configure: async (device, coordinatorEndpoint) => {
        const binds = {
            10: ['msTemperatureMeasurement', 'msRelativeHumidity', 'genPowerCfg'],
            11: ['genAnalogInput'],
            12: ['genAnalogInput', 'genAnalogOutput'],
            13: ['genAnalogInput', 'genAnalogOutput'],
            14: ['genAnalogOutput'],
        };
        for (const [id, clusters] of Object.entries(binds)) {
            const endpoint = device.getEndpoint(Number(id));
            for (const cluster of clusters) {
                await endpoint.bind(cluster, coordinatorEndpoint);
            }
        }
    },
};
