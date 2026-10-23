cmake_minimum_required(VERSION 3.22)

# Standalone check of a ring board's sync counters on the protocol side (ID_RING_STAT, IDBinRingStat): the frames
# the app sends and the device's frames it takes, byte by byte. Offline, no device. Not part of the
# application build; tools/ringstat_check/run.ps1 builds and runs it.
project(ringstat_check LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui)   # Gui: math_defs.h uses QVector3D

set(KOGGER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
# Another copy of the parser can be checked instead of the tree's (a mutation run): -DRINGSTAT_SRC=<dir with
# id_binnary.h/.cpp>
set(RINGSTAT_SRC ${KOGGER_SRC} CACHE PATH "directory with id_binnary.h/.cpp")

add_executable(ringstat_check
    main.cpp
    ${RINGSTAT_SRC}/id_binnary.h
    ${RINGSTAT_SRC}/id_binnary.cpp
    ${KOGGER_SRC}/device/wifi_ops.h
    ${KOGGER_SRC}/device/wifi_ops.cpp   # id_binnary.cpp calls WifiPorts
    ${KOGGER_SRC}/kec/kec.c)

# The voltage check's stub/ first: its core.h stands in for the application's
target_include_directories(ringstat_check PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../voltage_check/stub ${RINGSTAT_SRC} ${KOGGER_SRC} ${KOGGER_SRC}/kec ${KOGGER_SRC}/device)
target_link_libraries(ringstat_check PRIVATE Qt6::Core Qt6::Gui)
