cmake_minimum_required(VERSION 3.22)

# Standalone check of src/device/bus_address.* -- setting and saving the bus address of a sonar with
# the ring firmware. Offline: the class against simulated boards on one line (alone or two in a ring),
# normal runs and failures. --live: the same class on a real board over a serial port.
# Not part of the application build. How to run: docs/RING_UPDATE.md in the fork.
project(busaddr_check LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core SerialPort)

set(KOGGER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
# A copy of the class can be checked instead of the tree's (a mutation run): -DBUSADDR_SRC=<dir>
set(BUSADDR_SRC ${KOGGER_SRC}/device CACHE PATH "directory with bus_address.h/.cpp")

add_executable(busaddr_check
    main.cpp
    ${BUSADDR_SRC}/bus_address.h
    ${BUSADDR_SRC}/bus_address.cpp
    ${KOGGER_SRC}/device/ring_upgrade.h
    ${KOGGER_SRC}/device/ring_upgrade.cpp)

target_include_directories(busaddr_check PRIVATE ${BUSADDR_SRC} ${KOGGER_SRC}/device)
target_link_libraries(busaddr_check PRIVATE Qt6::Core Qt6::SerialPort)
