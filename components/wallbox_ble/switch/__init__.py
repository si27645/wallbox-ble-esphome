import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import DEVICE_CLASS_SWITCH
from .. import wallbox_ble_ns, WALLBOX_CLIENT_SCHEMA, CONF_WALLBOX_BLE_ID

DEPENDENCIES = ["wallbox_ble"]

CONF_CHARGING = "charging"

WallboxChargingSwitch = wallbox_ble_ns.class_(
    "WallboxChargingSwitch", switch.Switch, cg.Component
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_CHARGING): switch.switch_schema(
            WallboxChargingSwitch,
            device_class=DEVICE_CLASS_SWITCH,
            icon="mdi:ev-station",
        ),
    }
).extend(WALLBOX_CLIENT_SCHEMA)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_WALLBOX_BLE_ID])

    if charging_config := config.get(CONF_CHARGING):
        s = await switch.new_switch(charging_config)
        await cg.register_component(s, charging_config)
        cg.add(s.set_hub(hub))
