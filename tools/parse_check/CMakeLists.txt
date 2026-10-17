cmake_minimum_required(VERSION 3.22)

# Standalone check of src/proto_binnary.h, the frame parser a file and every link go through: a log
# file parsed as DeviceManager::openFile does, a fingerprint of every frame printed. Plain C++, no Qt.
# Not part of the application build. -DPROTO_SRC=<dir> checks a copy of the header instead (compare.ps1).
project(parse_check LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PROTO_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src CACHE PATH "directory with proto_binnary.h and mav_link_conf.h")

add_executable(parse_check main.cpp)
target_include_directories(parse_check PRIVATE ${PROTO_SRC})
