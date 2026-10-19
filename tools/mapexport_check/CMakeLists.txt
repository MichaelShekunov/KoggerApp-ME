cmake_minimum_required(VERSION 3.22)

# Standalone check of the map export (src/map_export, docs/map-export.md): the lat/lon projection
# against the app's own, the mosaic sampling, the depth surface and its isobaths, and the GeoTIFF and
# KMZ files read back by independent readers (a TIFF parser here, Qt's QZipReader). Not part of the
# application build; run.ps1 configures, builds and runs it.
project(mapexport_check LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui)   # Qt6::CorePrivate (QZipReader) comes with Core in 6.8

set(KOGGER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
set(EXPORT_SRC ${KOGGER_SRC}/map_export)
add_executable(mapexport_check
    main.cpp
    ${EXPORT_SRC}/map_export_geo.cpp
    ${EXPORT_SRC}/map_export_formats.cpp
    ${EXPORT_SRC}/map_export_meta.cpp
    ${EXPORT_SRC}/map_export_writer.cpp
    ${EXPORT_SRC}/map_package.cpp)
target_include_directories(mapexport_check PRIVATE ${EXPORT_SRC} ${KOGGER_SRC})
target_link_libraries(mapexport_check PRIVATE Qt6::Core Qt6::Gui Qt6::CorePrivate)
