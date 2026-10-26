cmake_minimum_required(VERSION 3.16)

# ESP32 TinyAI Controller
# Support -DTINYAI_ENABLED=ON/OFF CMake option
if(DEFINED TINYAI_ENABLED)
    if(TINYAI_ENABLED)
        set(SDKCONFIG_DEFAULTS "sdkconfig.defaults" CACHE STRING "" FORCE)
    else()
        set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.noai" CACHE STRING "" FORCE)
    endif()
endif()

# ESP-IDF entry point. Application components live under components/.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_tinyai_controller C)
