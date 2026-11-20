cmake_minimum_required(VERSION 3.16)
project(driver_seamless_pico_mt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(driver_seamless_pico_mt SHARED
    src/driver_main.cpp
    src/pico_tracker_device.cpp
)

target_include_directories(driver_seamless_pico_mt PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../protocol"
)

target_link_libraries(driver_seamless_pico_mt PRIVATE ws2_32)

set_target_properties(driver_seamless_pico_mt PROPERTIES
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin/win64"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/bin/win64"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin/win64"
)
