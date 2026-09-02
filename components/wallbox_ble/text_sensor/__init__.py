import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC
from .. import WALLBOX_CLIENT_SCHEMA, CONF_WALLBOX_BLE_ID

DEPENDENCIES = ["wallbox_ble"]

CONF_STATUS = "status"
CONF_LAST_ERROR = "last_error"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_STATUS): text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:ev-station",
        ),
        cv.Optional(CONF_LAST_ERROR): text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:alert-circle-outline",
        ),
    }
).extend(WALLBOX_CLIENT_SCHEMA)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_WALLBOX_BLE_ID])

    if status_config := config.get(CONF_STATUS):
        s = await text_sensor.new_text_sensor(status_config)
        cg.add(hub.set_status_text_sensor(s))

    if last_error_config := config.get(CONF_LAST_ERROR):
        s = await text_sensor.new_text_sensor(last_error_config)
        cg.add(hub.set_last_error_text_sensor(s))
