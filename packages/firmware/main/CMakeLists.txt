# Auto-pick generated pet-art sprite .c files. They are produced by
# scripts/build_pet_art.py from packages/simulator/public/pet-art/*.png
# and re-emitted as LVGL lv_image_dsc_t. Safe to GLOB because the dir
# contains nothing else.
file(GLOB PET_ART_SRCS "pet_art/*.c")

# CJK font subset (~78KB source / ~35KB binary): 70 unique Chinese chars
# + ASCII 0x20-0x7E, generated from Arial Unicode via lv_font_conv. Regen
# by running scripts/build_font.sh after editing the UI strings (see
# main/font_cjk/lv_font_cjk_14.h for the command).
file(GLOB FONT_CJK_SRCS "font_cjk/*.c")

# Transport picks one of two .c files. They both implement the same
# bridge_client.h API; only one compiles in. We always declare `bt`
# and `esp_wifi`/`esp_netif` in REQUIRES because conditional REQUIRES
# is evaluated *before* Kconfig values are resolved, so it can't react
# to CONFIG_DOUDOU_TRANSPORT_*. When the corresponding stack is
# disabled in sdkconfig the component is mostly empty, so the
# unused dep adds little code (mostly init shims that get dead-code
# eliminated). For a true BLE-only slim build, set
# CONFIG_ESP_WIFI_ENABLED=n via menuconfig — the project still builds,
# net.c just becomes inert (its calls bail at ESP_ERR_NOT_SUPPORTED).
if(CONFIG_DOUDOU_TRANSPORT_BLE)
  set(TRANSPORT_SRC "ble_transport.c")
else()
  set(TRANSPORT_SRC "bridge_client.c")
endif()

idf_component_register(
  SRCS
    "main.c"
    "display.c"
    "touch.c"
    "lvgl_port.c"
    "pet_ui.c"
    "net.c"
    "${TRANSPORT_SRC}"
    "protocol_parse.c"
    ${PET_ART_SRCS}
    ${FONT_CJK_SRCS}
  INCLUDE_DIRS
    "."
    "pet_art"
    "font_cjk"
  REQUIRES
    driver
    esp_lcd
    esp_timer
    esp_event
    esp_netif
    esp_wifi
    nvs_flash
    json
    bt
)
