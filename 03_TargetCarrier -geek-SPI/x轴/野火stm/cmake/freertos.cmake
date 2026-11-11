# Native FreeRTOS V11.2.0 integration, outside CubeMX generated CMake.
option(TARGETCARRIER_LED_BLINK "Blink all three STM32 RGB LED channels" OFF)
set(TARGETCARRIER_LED_TOGGLE_MS "100" CACHE STRING "LED toggle interval in ms")
if(NOT TARGETCARRIER_LED_TOGGLE_MS MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "LED toggle interval must be an integer")
endif()
if(TARGETCARRIER_LED_TOGGLE_MS LESS 1 OR TARGETCARRIER_LED_TOGGLE_MS GREATER 60000)
    message(FATAL_ERROR "LED toggle interval must be between 1 and 60000 ms")
endif()
set(FREERTOS_KERNEL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/Third_Party/FreeRTOS-Kernel")
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    App/RTOS/app_rtos.c
    ${FREERTOS_KERNEL_DIR}/tasks.c
    ${FREERTOS_KERNEL_DIR}/list.c
    ${FREERTOS_KERNEL_DIR}/queue.c
    ${FREERTOS_KERNEL_DIR}/portable/GCC/ARM_CM3/port.c
)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    App/RTOS
    ${FREERTOS_KERNEL_DIR}/include
    ${FREERTOS_KERNEL_DIR}/portable/GCC/ARM_CM3
)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    APP_LED_BLINK_ENABLED=$<BOOL:${TARGETCARRIER_LED_BLINK}>
    APP_LED_TOGGLE_MS=${TARGETCARRIER_LED_TOGGLE_MS}
)
target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE -Wextra -Werror)
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE -Wl,--print-memory-usage)
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_PROJECT_NAME}.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin
    VERBATIM
)

# .ioc disables CubeMX SVC/PendSV bodies: the native port owns these vectors.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/stm32f1xx_it.c" TC_IRQ_SOURCE)
if(TC_IRQ_SOURCE MATCHES "void[ \t\r\n]+(SVC_Handler|PendSV_Handler)[ \t\r\n]*\\(")
    message(FATAL_ERROR "CubeMX regenerated SVC/PendSV bodies; restore native FreeRTOS vector ownership")
endif()

# Manual continuous control, no automatic startup motion by default.
option(TARGETCARRIER_CONTINUOUS_ENABLED "Allow explicit continuous forward/reverse commands" ON)
option(TARGETCARRIER_STARTUP_ONESHOT "Run the legacy one-shot pulse test at boot" OFF)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    MOTOR_CONTINUOUS_ENABLED=$<BOOL:${TARGETCARRIER_CONTINUOUS_ENABLED}>
    MOTOR_STARTUP_ONESHOT_ENABLED=$<BOOL:${TARGETCARRIER_STARTUP_ONESHOT}>
)
