cmake_minimum_required(VERSION 3.16)
project(esp32_tinyai_host_tests C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

set(COMPONENT_SOURCES
    ${ROOT}/components/baremetal_gpio/GPIO_Driver.c
    ${ROOT}/components/board_profile/board_profile.c
    ${ROOT}/components/pin_manager/PinManager.c
    ${ROOT}/components/device_registry/device_registry.c
    ${ROOT}/components/config_store/config_store.c
    ${ROOT}/components/tinyml/tinyml.c
    ${ROOT}/components/model_data/model_data.c
    ${ROOT}/components/entity_extractor/entity_extractor.c
    ${ROOT}/components/automation/button_fsm.c
    ${ROOT}/components/automation/Automation.c
    ${ROOT}/components/sensor_manager/SensorManager.c
    ${ROOT}/components/command_engine/command_engine.c
    ${ROOT}/components/serial_console/SerialConsole.c
    ${ROOT}/tests/generated/tinyai_golden.c
)

set(COMPONENT_INCLUDES
    ${ROOT}/components/baremetal_gpio/include
    ${ROOT}/components/board_profile/include
    ${ROOT}/components/pin_manager/include
    ${ROOT}/components/device_registry/include
    ${ROOT}/components/config_store/include
    ${ROOT}/components/tinyml/include
    ${ROOT}/components/model_data
    ${ROOT}/components/model_data/include
    ${ROOT}/components/entity_extractor/include
    ${ROOT}/components/automation/include
    ${ROOT}/components/sensor_manager/include
    ${ROOT}/components/command_engine/include
    ${ROOT}/components/serial_console/include
    ${ROOT}/tests/generated
)

add_executable(host_tests
    test_main.c
    test_tinyml.c
    test_gpio.c
    test_pin_manager.c
    test_entity.c
    test_command_engine.c
    test_sensor_automation.c
    test_console.c
    test_serialization.c
    gpio_reg_sim.c
    ${COMPONENT_SOURCES}
)

target_include_directories(host_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${COMPONENT_INCLUDES}
)

target_compile_definitions(host_tests PRIVATE
    GPIO_DRIVER_HOST_TEST=1
    PIN_MANAGER_HOST_TEST=1
    COMMAND_ENGINE_HOST_TEST=1
    SENSOR_MANAGER_HOST_TEST=1
)

if (MSVC)
    target_compile_options(host_tests PRIVATE /W4)
else()
    target_compile_options(host_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_link_libraries(host_tests PRIVATE m)

add_executable(host_benchmark
    benchmark_host.c
    ${ROOT}/components/tinyml/tinyml.c
    ${ROOT}/components/model_data/model_data.c
)

target_include_directories(host_benchmark PRIVATE ${COMPONENT_INCLUDES})

if (MSVC)
    target_compile_options(host_benchmark PRIVATE /W4)
else()
    target_compile_options(host_benchmark PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_link_libraries(host_benchmark PRIVATE m)
