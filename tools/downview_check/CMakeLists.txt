cmake_minimum_required(VERSION 3.22)

# Standalone check of the down view's arithmetic (src/down_view.h). Offline, header only, no Qt. Not part of the
# application build; tools/downview_check/run.ps1 builds and runs it.
project(downview_check LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Another copy of the header can be checked instead of the tree's (a mutation run): -DDOWNVIEW_SRC=<dir with down_view.h>
set(DOWNVIEW_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src CACHE PATH "directory with down_view.h")

add_executable(downview_check main.cpp)
target_include_directories(downview_check PRIVATE ${DOWNVIEW_SRC})
