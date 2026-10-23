cmake_minimum_required(VERSION 3.22)

# Standalone check of what the app does with the Wi-Fi module's firmware 0.20 additions: the radio switched off
# (ID_WIFI_RF 0x5A, IDBinWifiRf, WifiRf) -- the frames it sends byte by byte against the contract's examples, the
# answers it takes -- and the retransmissions in the link rate report (WifiLinkRate, WifiRetryWindow). Offline, no
# module. Not part of the application build; tools/wifi020_check/run.ps1 builds and runs it.
project(wifi020_check LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui)   # Gui: math_defs.h uses QVector3D

set(KOGGER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
# Another copy of the sources can be checked instead of the tree's (a mutation run): -DWIFI020_SRC=<dir with
# id_binnary.h/.cpp and device/wifi_ops.h/.cpp>
set(WIFI020_SRC ${KOGGER_SRC} CACHE PATH "directory with id_binnary.h/.cpp and device/wifi_ops.h/.cpp")

add_executable(wifi020_check
    main.cpp
    ${WIFI020_SRC}/id_binnary.h
    ${WIFI020_SRC}/id_binnary.cpp
    ${WIFI020_SRC}/device/wifi_ops.h
    ${WIFI020_SRC}/device/wifi_ops.cpp
    ${KOGGER_SRC}/kec/kec.c)

# The voltage check's stub/ first: its core.h stands in for the application's
target_include_directories(wifi020_check PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../voltage_check/stub ${WIFI020_SRC} ${WIFI020_SRC}/device ${KOGGER_SRC}
    ${KOGGER_SRC}/kec ${KOGGER_SRC}/device)
target_link_libraries(wifi020_check PRIVATE Qt6::Core Qt6::Gui)
