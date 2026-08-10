cmake_minimum_required(VERSION 3.22)

project(deconv_check LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(KOGGER_PROCESSING_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../src/processing)

# No Qt: the deconvolution core is deliberately free of it, so this runs
# anywhere a compiler does.
add_executable(deconv_check
    main.cpp
    ${KOGGER_PROCESSING_DIR}/echogram_deconvolution.h
    ${KOGGER_PROCESSING_DIR}/echogram_deconvolution.cpp)

target_include_directories(deconv_check PRIVATE ${KOGGER_PROCESSING_DIR})
