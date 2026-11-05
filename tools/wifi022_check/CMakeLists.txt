cmake_minimum_required(VERSION 3.22)

# Standalone check of the Wi-Fi module's profiles in the app (firmware 0.22, ID_WIFI_PROFILE 0x5B): the block
# (WifiProfile) byte by byte against the module project's own encoder, the frames (IDBinWifiProfile), the upload
# (WifiProfileUpload) on made-up answers, what a load changes, the rules and the user's files. Offline, no module.
# Not part of the application build; tools/wifi022_check/run.ps1 builds and runs it.
project(wifi022_check LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui)   # Gui: math_defs.h uses QVector3D

set(KOGGER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
# Another copy of the sources can be checked instead of the tree's (a mutation run): -DWIFI022_SRC=<dir with
# id_binnary.h/.cpp and device/wifi_ops.h/.cpp, device/wifi_profile.h/.cpp>
set(WIFI022_SRC ${KOGGER_SRC} CACHE PATH "directory with id_binnary.h/.cpp and device/wifi_ops.h/.cpp, wifi_profile.h/.cpp")

add_executable(wifi022_check
    main.cpp
    ${WIFI022_SRC}/id_binnary.h
    ${WIFI022_SRC}/id_binnary.cpp
    ${WIFI022_SRC}/device/wifi_ops.h
    ${WIFI022_SRC}/device/wifi_ops.cpp
    ${WIFI022_SRC}/device/wifi_profile.h
    ${WIFI022_SRC}/device/wifi_profile.cpp
    ${KOGGER_SRC}/kec/kec.c
    ${KOGGER_SRC}/codec/chart_v3.cpp        # id_binnary.cpp: ID_CHART V3
    ${KOGGER_SRC}/codec/chart_codec_host.c)

# The voltage check's stub/ first: its core.h stands in for the application's
target_include_directories(wifi022_check PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../voltage_check/stub ${WIFI022_SRC} ${WIFI022_SRC}/device ${KOGGER_SRC}
    ${KOGGER_SRC}/kec ${KOGGER_SRC}/device)
target_link_libraries(wifi022_check PRIVATE Qt6::Core Qt6::Gui)
