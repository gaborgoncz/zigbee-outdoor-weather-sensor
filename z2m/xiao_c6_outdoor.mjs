// Zigbee2MQTT external converter for the XIAO ESP32-C6 outdoor sensor
// (firmware: ../xiao_c6_outdoor_sensor/xiao_c6_outdoor_sensor.ino)
//
// Install: Zigbee2MQTT UI -> Settings -> Dev console -> External converters -> create
// "xiao_c6_outdoor.mjs", paste this file, save, restart Zigbee2MQTT.

import * as exposes from 'zigbee-herdsman-converters/lib/exposes';

const e = exposes.presets;
const ea = exposes.access;

// Settings live in the analog output cluster of these endpoints
const SETTINGS = {
    report_interval: {endpoint: 12, min: 1, max: 240, step: 1, unit: 'min', description: 'Time between two measurements'},
    low_battery_threshold: {endpoint: 13, min: 5, max: 80, step: 1, unit: '%', description: 'Battery level at or below which the low battery interval is used'},
    low_battery_interval: {endpoint: 14, min: 5, max: 720, step: 5, unit: 'min', description: 'Time between two measurements while the battery is low'},
};
const SETTING_BY_ENDPOINT = Object.fromEntries(Object.entries(SETTINGS).map(([key, s]) => [s.endpoint, key]));

const fzLocal = {
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
    battery: {
        cluster: 'genPowerCfg',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg) => {
            if (msg.data.batteryPercentageRemaining !== undefined) return {battery: msg.data.batteryPercentageRemaining / 2};
        },
    },
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
    // The device reports its settings on every wake-up and then listens for about a second.
    // If Home Assistant asked for something else in the meantime, write it now.
    settings: {
        cluster: 'genAnalogOutput',
        type: ['attributeReport', 'readResponse'],
        convert: async (model, msg, publish, options, meta) => {
            const key = SETTING_BY_ENDPOINT[msg.endpoint.ID];
            if (!key || msg.data.presentValue === undefined) return;
            const onDevice = Math.round(msg.data.presentValue);
            const wanted = meta.state?.[key];
            if (typeof wanted === 'number' && wanted !== onDevice) {
                try {
                    await msg.endpoint.write('genAnalogOutput', {presentValue: wanted});
                } catch {
                    // Device went back to sleep, the next wake-up tries again
                }
                return;
            }
            return {[key]: onDevice};
        },
    },
};

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
