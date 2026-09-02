import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import DEVICE_CLASS_BATTERY_CHARGING, DEVICE_CLASS_PLUG
from .. import WALLBOX_CLIENT_SCHEMA, CONF_WALLBOX_BLE_ID

DEPENDENCIES = ["wallbox_ble"]

CONF_CAR_CONNECTED = "car_connected"
CONF_CHARGING = "charging"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_CAR_CONNECTED): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_PLUG,
        ),
        cv.Optional(CONF_CHARGING): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_BATTERY_CHARGING,
        ),
    }
).extend(WALLBOX_CLIENT_SCHEMA)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_WALLBOX_BLE_ID])

    if car_connected_config := config.get(CONF_CAR_CONNECTED):
        s = await binary_sensor.new_binary_sensor(car_connected_config)
        cg.add(hub.set_car_connected_binary_sensor(s))

    if charging_config := config.get(CONF_CHARGING):
        s = await binary_sensor.new_binary_sensor(charging_config)
        cg.add(hub.set_charging_binary_sensor(s))
