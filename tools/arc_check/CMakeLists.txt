cmake_minimum_required(VERSION 3.22)

# Standalone check of src/data_processor/mosaic_arc.h -- how the mosaic fills the gap between two
# pings: the old chord where it is kept, the arc of a turning sonar elsewhere. Plain C++, no Qt.
# Not part of the application build.
project(arc_check LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(KOGGER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
# A copy of the header can be checked instead of the tree's (a mutation run): -DMOSAICARC_SRC=<dir>
set(MOSAICARC_SRC ${KOGGER_SRC}/data_processor CACHE PATH "directory with mosaic_arc.h")

add_executable(arc_check main.cpp ${MOSAICARC_SRC}/mosaic_arc.h)
target_include_directories(arc_check PRIVATE ${MOSAICARC_SRC})
