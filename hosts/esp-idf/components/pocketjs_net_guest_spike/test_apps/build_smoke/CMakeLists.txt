cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "../.."
    "../../../pocketjs_esp_guest"
    "../../../pocketjs_net_esp_http"
)
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pocketjs_net_guest_spike_build_smoke)
