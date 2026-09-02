import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number
from esphome.const import DEVICE_CLASS_CURRENT, UNIT_AMPERE, ENTITY_CATEGORY_CONFIG
from .. import wallbox_ble_ns, WALLBOX_CLIENT_SCHEMA, CONF_WALLBOX_BLE_ID

DEPENDENCIES = ["wallbox_ble"]

CONF_MAX_CURRENT = "max_current"

WallboxMaxCurrentNumber = wallbox_ble_ns.class_(
    "WallboxMaxCurrentNumber", number.Number, cg.Component
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_MAX_CURRENT): number.number_schema(
            WallboxMaxCurrentNumber,
            unit_of_measurement=UNIT_AMPERE,
            device_class=DEVICE_CLASS_CURRENT,
            entity_category=ENTITY_CATEGORY_CONFIG,
            icon="mdi:current-ac",
        ),
    }
).extend(WALLBOX_CLIENT_SCHEMA)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_WALLBOX_BLE_ID])

    if max_current_config := config.get(CONF_MAX_CURRENT):
        # 6 A is the practical minimum for EV charging (IEC 61851); 32 A
        # covers single-phase Pulsar MAX installs. If yours is wired for
        # more, raise max_value here.
        n = await number.new_number(
            max_current_config, min_value=6, max_value=32, step=1
        )
        await cg.register_component(n, max_current_config)
        cg.add(n.set_hub(hub))
