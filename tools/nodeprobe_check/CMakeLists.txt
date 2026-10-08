cmake_minimum_required(VERSION 3.22)

# Standalone check of src/device/node_probe.* -- how a ring sonar behind a recorder takes commands:
# wrapped in ID_NODE_CMD or plain. Offline: the probe against a simulated recorder and boards, built
# from what docs/RING_UPDATE.md records of the real one. Not part of the application build.
project(nodeprobe_check LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core)

set(KOGGER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
# A copy of the class can be checked instead of the tree's (a mutation run): -DNODEPROBE_SRC=<dir>
set(NODEPROBE_SRC ${KOGGER_SRC}/device CACHE PATH "directory with node_probe.h/.cpp")

add_executable(nodeprobe_check
    main.cpp
    ${NODEPROBE_SRC}/node_probe.h
    ${NODEPROBE_SRC}/node_probe.cpp)

target_include_directories(nodeprobe_check PRIVATE ${NODEPROBE_SRC} ${KOGGER_SRC})
target_link_libraries(nodeprobe_check PRIVATE Qt6::Core)
