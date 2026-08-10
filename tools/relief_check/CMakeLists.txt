cmake_minimum_required(VERSION 3.22)

project(relief_check LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(KOGGER_PROCESSING_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../src/processing)

# No Qt: the relief core is deliberately free of it.
add_executable(relief_check
    main.cpp
    ${KOGGER_PROCESSING_DIR}/echogram_relief.h
    ${KOGGER_PROCESSING_DIR}/echogram_relief.cpp)

target_include_directories(relief_check PRIVATE ${KOGGER_PROCESSING_DIR})
