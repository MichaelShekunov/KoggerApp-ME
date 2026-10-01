cmake_minimum_required(VERSION 3.22)

# Standalone check of src/vehicle/mav_vehicle.* against ArduPilot SITL over TCP: the same class the app
# uses, driven through a whole mission (upload, read-back, start, waypoint reached, HOLD, RTL, disarm).
# Not part of the application build. How to run: docs/BOAT_MISSION.md in the fork.
project(mission_check LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network)

set(KOGGER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src)

add_executable(mission_check
    main.cpp
    ${KOGGER_SRC}/vehicle/mav_vehicle.h
    ${KOGGER_SRC}/vehicle/mav_vehicle.cpp
    ${KOGGER_SRC}/system_monitor.h
    ${KOGGER_SRC}/system_monitor.cpp
    ${KOGGER_SRC}/device/wifi_ops.h
    ${KOGGER_SRC}/device/wifi_ops.cpp
    ${KOGGER_SRC}/device/device_liveness.h
    ${KOGGER_SRC}/touch_tap_gate.h)

target_include_directories(mission_check PRIVATE
    ${KOGGER_SRC}
    ${KOGGER_SRC}/vehicle
    ${KOGGER_SRC}/device
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party)
target_link_libraries(mission_check PRIVATE Qt6::Core Qt6::Network)
