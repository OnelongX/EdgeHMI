set(EDGEHMI_MAIN_SRCS
    "main.c"
    "ui_engine_lvgl.c"
    "ota_config.c"
    "ota_firmware.c"
    "discovery_service.c"
    "local_io.c"
    "fieldbus_ext.c"
    "cellular_modem.c"
    "cloud_mqtt.c"
)

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/font_edgehmi_cjk.c")
    list(APPEND EDGEHMI_MAIN_SRCS "font_edgehmi_cjk.c")
endif()

idf_component_register(
    SRCS
        ${EDGEHMI_MAIN_SRCS}
    INCLUDE_DIRS
        "."
)

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/font_edgehmi_cjk.c")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE EDGEHMI_CUSTOM_FONT=font_edgehmi_cjk)
endif()
