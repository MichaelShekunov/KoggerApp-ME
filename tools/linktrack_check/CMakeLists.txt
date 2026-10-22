cmake_minimum_required(VERSION 3.22)

# Standalone check of src/scene3d/link_track_geometry.h: the triangles of the boat's track by link quality over the
# 3D view. Not part of the application build; tools/linktrack_check/run.ps1 builds and runs it.
project(linktrack_check LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui)   # Gui: QColor

# A copy of the header can be checked instead of the tree's (a mutation run): -DLINKTRACK_SRC=<dir>
set(LINKTRACK_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src/scene3d CACHE PATH "directory with link_track_geometry.h")

add_executable(linktrack_check main.cpp ${LINKTRACK_SRC}/link_track_geometry.h)
target_include_directories(linktrack_check PRIVATE ${LINKTRACK_SRC})
target_link_libraries(linktrack_check PRIVATE Qt6::Core Qt6::Gui)
