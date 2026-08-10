cmake_minimum_required(VERSION 3.22)

project(sonifier_check LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Multimedia)

set(KOGGER_AUDIO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../src/audio)

add_executable(sonifier_check
    main.cpp
    ${KOGGER_AUDIO_DIR}/echogram_sonifier.h
    ${KOGGER_AUDIO_DIR}/echogram_sonifier.cpp)

target_include_directories(sonifier_check PRIVATE ${KOGGER_AUDIO_DIR})
target_link_libraries(sonifier_check PRIVATE Qt6::Core Qt6::Multimedia)
