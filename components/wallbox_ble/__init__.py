import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import ble_client
from esphome.const import CONF_ID

CODEOWNERS = ["@si27645"]
DEPENDENCIES = ["ble_client"]
AUTO_LOAD = ["json"]
MULTI_CONF = True

CONF_PIN = "pin"
CONF_WALLBOX_BLE_ID = "wallbox_ble_id"

wallbox_ble_ns = cg.esphome_ns.namespace("wallbox_ble")
WallboxBleHub = wallbox_ble_ns.class_(
    "WallboxBleHub", ble_client.BLEClientNode, cg.PollingComponent
)

CONFIG_SCHEMA = (
    cv.COMPONENT_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(WallboxBleHub),
            # Only needed if the charger has a BAPI PIN configured
            # (Settings > Bluetooth PIN in the Wallbox app). Leave unset
            # if you've never set one.
            cv.Optional(CONF_PIN): cv.string_strict,
        }
    )
    .extend(ble_client.BLE_CLIENT_SCHEMA)
    .extend(cv.polling_component_schema("10s"))
)

WALLBOX_CLIENT_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_WALLBOX_BLE_ID): cv.use_id(WallboxBleHub),
    }
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)

    if pin := config.get(CONF_PIN):
        cg.add(var.set_pin(pin))
