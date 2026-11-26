if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(EDGEHMI_JSON_DEP espressif__cjson)
else()
    set(EDGEHMI_JSON_DEP json)
endif()

idf_component_register(
    SRCS
        "src/data_bus.c"
        "src/modbus_rtu.c"
        "src/modbus_tcp.c"
        "src/can_codec.c"
        "src/calc_engine.c"
        "src/cellular_proto.c"
        "src/history_log.c"
        "src/recipe_sched.c"
        "src/rule_engine.c"
        "src/config_loader.c"
        "src/ota_package.c"
        "src/discovery_proto.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        ${EDGEHMI_JSON_DEP}
)
