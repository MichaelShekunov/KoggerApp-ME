cmake_minimum_required(VERSION 3.22)

# Standalone check of src/track_smoother.* -- how pings between GNSS fixes are placed. Offline: a
# simulated survey with the GODIO boat's float32 coordinates, fed in the order Dataset sees it, the
# old pairwise placement against the windowed fit. Not part of the application build.
project(interp_check LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core)

set(KOGGER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
# A copy of the class can be checked instead of the tree's (a mutation run): -DTRACKSMOOTHER_SRC=<dir>
set(TRACKSMOOTHER_SRC ${KOGGER_SRC} CACHE PATH "directory with track_smoother.h/.cpp")

add_executable(interp_check
    main.cpp
    ${TRACKSMOOTHER_SRC}/track_smoother.h
    ${TRACKSMOOTHER_SRC}/track_smoother.cpp)

target_include_directories(interp_check PRIVATE ${TRACKSMOOTHER_SRC})
target_link_libraries(interp_check PRIVATE Qt6::Core)
