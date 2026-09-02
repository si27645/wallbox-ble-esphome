import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_ENERGY,
    DEVICE_CLASS_POWER,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_AMPERE,
    UNIT_KILOWATT,
    UNIT_KILOWATT_HOURS,
)
from .. import WALLBOX_CLIENT_SCHEMA, CONF_WALLBOX_BLE_ID

DEPENDENCIES = ["wallbox_ble"]

CONF_POWER = "power"
CONF_ENERGY = "energy"
CONF_MAX_CURRENT = "max_current"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_POWER): sensor.sensor_schema(
            unit_of_measurement=UNIT_KILOWATT,
            device_class=DEVICE_CLASS_POWER,
            state_class=STATE_CLASS_MEASUREMENT,
            accuracy_decimals=2,
        ),
        cv.Optional(CONF_ENERGY): sensor.sensor_schema(
            unit_of_measurement=UNIT_KILOWATT_HOURS,
            device_class=DEVICE_CLASS_ENERGY,
            state_class=STATE_CLASS_TOTAL_INCREASING,
            accuracy_decimals=2,
        ),
        cv.Optional(CONF_MAX_CURRENT): sensor.sensor_schema(
            unit_of_measurement=UNIT_AMPERE,
            device_class=DEVICE_CLASS_CURRENT,
            state_class=STATE_CLASS_MEASUREMENT,
            accuracy_decimals=0,
        ),
    }
).extend(WALLBOX_CLIENT_SCHEMA)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_WALLBOX_BLE_ID])

    if power_config := config.get(CONF_POWER):
        s = await sensor.new_sensor(power_config)
        cg.add(hub.set_power_sensor(s))

    if energy_config := config.get(CONF_ENERGY):
        s = await sensor.new_sensor(energy_config)
        cg.add(hub.set_energy_sensor(s))

    if max_current_config := config.get(CONF_MAX_CURRENT):
        s = await sensor.new_sensor(max_current_config)
        cg.add(hub.set_max_current_sensor(s))
