cmake_minimum_required(VERSION 3.22)

# Standalone check of src/device/ring_upgrade.* -- the firmware update of sonars sharing one line as a
# ring. Offline: the class against a simulated ring (two boards, their bootloaders, rates and relays),
# normal runs and failures. --live: the same class updates the real boards over a serial port.
# Not part of the application build. How to run: docs/RING_UPDATE.md in the fork.
project(ringupdate_check LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core SerialPort)

set(KOGGER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src)

add_executable(ringupdate_check
    main.cpp
    ${KOGGER_SRC}/device/ring_upgrade.h
    ${KOGGER_SRC}/device/ring_upgrade.cpp)

target_include_directories(ringupdate_check PRIVATE ${KOGGER_SRC}/device)
target_link_libraries(ringupdate_check PRIVATE Qt6::Core Qt6::SerialPort)
