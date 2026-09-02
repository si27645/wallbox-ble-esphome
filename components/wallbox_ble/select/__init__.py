import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import select
from .. import WALLBOX_CLIENT_SCHEMA, CONF_WALLBOX_BLE_ID, wallbox_ble_ns

DEPENDENCIES = ["wallbox_ble"]

CONF_ECO_MODE = "eco_mode"

# Must match WallboxEcoMode / wallbox_eco_mode_to_string() in wallbox_ble.h/.cpp.
ECO_MODE_OPTIONS = ["Disabled", "Full Green", "Solar + Grid"]

WallboxEcoModeSelect = wallbox_ble_ns.class_(
    "WallboxEcoModeSelect", select.Select, cg.Component
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_ECO_MODE): select.select_schema(
            WallboxEcoModeSelect,
            icon="mdi:solar-power",
        ),
    }
).extend(WALLBOX_CLIENT_SCHEMA)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_WALLBOX_BLE_ID])

    if eco_mode_config := config.get(CONF_ECO_MODE):
        s = await select.new_select(eco_mode_config, options=ECO_MODE_OPTIONS)
        await cg.register_component(s, eco_mode_config)
        cg.add(s.set_hub(hub))
        cg.add(hub.set_eco_mode_select(s))
