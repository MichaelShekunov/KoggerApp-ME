cmake_minimum_required(VERSION 3.22)

# Standalone check of src/device/attitude_arbiter.h -- which of several attitude sources the app follows. No Qt:
# the header has none. Not part of the application build; tools/attitude_check/run.ps1 builds and runs it.
project(attitude_check LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# A copy of the header can be checked instead of the tree's (a mutation run): -DATTITUDE_SRC=<dir>
set(ATTITUDE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src/device CACHE PATH "directory with attitude_arbiter.h")

add_executable(attitude_check main.cpp ${ATTITUDE_SRC}/attitude_arbiter.h)
target_include_directories(attitude_check PRIVATE ${ATTITUDE_SRC})
