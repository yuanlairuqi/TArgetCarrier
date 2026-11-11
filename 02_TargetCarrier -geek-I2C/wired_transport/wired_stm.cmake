target_sources(${WIRED_TARGET} PRIVATE "${WIRED_TRANSPORT_ROOT}/tc_protocol.c" "${WIRED_TRANSPORT_ROOT}/wl_codec.c" "${WIRED_TRANSPORT_ROOT}/stm32_bus.c" "${WIRED_TRANSPORT_ROOT}/wl_stm.c")
target_include_directories(${WIRED_TARGET} PRIVATE "${WIRED_TRANSPORT_ROOT}")
target_compile_definitions(${WIRED_TARGET} PRIVATE WL_NODE=${WIRED_NODE})

set_source_files_properties("${WIRED_TRANSPORT_ROOT}/tc_protocol.c" "${WIRED_TRANSPORT_ROOT}/wl_codec.c" "${WIRED_TRANSPORT_ROOT}/stm32_bus.c" "${WIRED_TRANSPORT_ROOT}/wl_stm.c" PROPERTIES COMPILE_OPTIONS "-Wno-unused-function;-Wno-unused-variable;-Wno-misleading-indentation;-Wno-sign-compare;-Wno-array-parameter")
