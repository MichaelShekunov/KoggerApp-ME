cmake_minimum_required(VERSION 3.22)

# Standalone check of the supply-voltage protocol side: the DATASET bit the "Supply voltage" switch
# sets (IDBinDataset::MASK_VOLTAGE_V0) and the frame it goes out in, and ID_VOLTAGE parsing on
# frames built byte by byte to the contract. Offline, no device. Not part of the application build.
#   voltage_check
# The application side (DevDriver -> Dataset) is checked live by voltage_live.ps1.
project(voltage_check LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui)   # Gui: math_defs.h uses QVector3D

set(KOGGER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
# Another copy of the parser can be checked instead of the tree's -- the control run builds a
# mutated copy, and the check has to fail on it: -DVOLTAGE_SRC=<dir with id_binnary.h/.cpp>
set(VOLTAGE_SRC ${KOGGER_SRC} CACHE PATH "directory with id_binnary.h/.cpp")

add_executable(voltage_check
    main.cpp
    ${VOLTAGE_SRC}/id_binnary.h
    ${VOLTAGE_SRC}/id_binnary.cpp
    ${KOGGER_SRC}/device/wifi_ops.h
    ${KOGGER_SRC}/device/wifi_ops.cpp   # id_binnary.cpp calls WifiPorts
    ${KOGGER_SRC}/kec/kec.c
    ${KOGGER_SRC}/codec/chart_v3.cpp        # id_binnary.cpp: ID_CHART V3
    ${KOGGER_SRC}/codec/chart_codec_host.c)

# stub/ first: its core.h stands in for the application's
target_include_directories(voltage_check PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stub ${VOLTAGE_SRC} ${KOGGER_SRC} ${KOGGER_SRC}/kec ${KOGGER_SRC}/device)
target_link_libraries(voltage_check PRIVATE Qt6::Core Qt6::Gui)
