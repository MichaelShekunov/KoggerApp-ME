cmake_minimum_required(VERSION 3.22)

# Standalone check of src/scene2d/plot2D_columns.h -- which ping each screen column of the echogram shows at
# a horizontal scale, and where the echogram's ring cache keeps it. No Qt: the header has none.
# Not part of the application build; tools/echoscale_check/run.ps1 builds and runs it.
project(echoscale_check LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# A copy of the header can be checked instead of the tree's (a mutation run): -DECHOSCALE_SRC=<dir>
set(ECHOSCALE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src/scene2d CACHE PATH "directory with plot2D_columns.h")

add_executable(echoscale_check main.cpp ${ECHOSCALE_SRC}/plot2D_columns.h)
target_include_directories(echoscale_check PRIVATE ${ECHOSCALE_SRC})
