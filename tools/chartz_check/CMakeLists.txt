cmake_minimum_required(VERSION 3.22)

# Standalone check of the compressed-echogram path (IDBinChartZ -> IDBinChart) on a capture from
# a real two-channel sonar. Offline, no device needed. Not part of the application build.
#   chartz_check nanosss_v119_both.bin
project(chartz_check LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui)   # Gui: math_defs.h uses QVector3D

set(KOGGER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
# Another copy of the parser can be checked instead of the tree's -- the control run builds it
# against the commit that refused v1, and the check has to fail there: -DCHARTZ_SRC=<dir>
set(CHARTZ_SRC ${KOGGER_SRC} CACHE PATH "directory with id_binnary.h/.cpp")

add_executable(chartz_check
    main.cpp
    ${CHARTZ_SRC}/id_binnary.h
    ${CHARTZ_SRC}/id_binnary.cpp
    ${KOGGER_SRC}/device/wifi_ops.h
    ${KOGGER_SRC}/device/wifi_ops.cpp   # id_binnary.cpp calls WifiPorts
    ${KOGGER_SRC}/kec/kec.c)

# stub/ first: its core.h stands in for the application's
target_include_directories(chartz_check PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stub ${CHARTZ_SRC} ${KOGGER_SRC} ${KOGGER_SRC}/kec ${KOGGER_SRC}/device)
target_link_libraries(chartz_check PRIVATE Qt6::Core Qt6::Gui)
