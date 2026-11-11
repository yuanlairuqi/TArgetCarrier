target_sources(${COMPONENT_LIB} PRIVATE "${WIRED_TRANSPORT_ROOT}/tc_protocol.c" "${WIRED_TRANSPORT_ROOT}/wl_codec.c" "${WIRED_TRANSPORT_ROOT}/esp_bus.c" "${WIRED_TRANSPORT_ROOT}/wl_esp.c")
target_include_directories(${COMPONENT_LIB} PRIVATE "${WIRED_TRANSPORT_ROOT}")
target_compile_definitions(${COMPONENT_LIB} PRIVATE WL_NODE=${WIRED_NODE})
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-array-parameter -Wno-unused-variable -Wno-unused-function -Wno-misleading-indentation)
