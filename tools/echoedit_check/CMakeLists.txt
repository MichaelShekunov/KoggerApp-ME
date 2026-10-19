cmake_minimum_required(VERSION 3.22)

# Standalone check of src/echogram_edit/echo_edit_model.* -- the history of echogram edits, the file of
# edits next to a recording, the pencil's magnet (docs/echogram-editing.md). Offline, Qt Core only.
# Not part of the application build; tools/echoedit_check/run.ps1 builds and runs it.
project(echoedit_check LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core)

set(KOGGER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
# A copy of the model can be checked instead of the tree's (a mutation run): -DECHOEDIT_SRC=<dir>
set(ECHOEDIT_SRC ${KOGGER_SRC}/echogram_edit CACHE PATH "directory with echo_edit_model.h/.cpp")

add_executable(echoedit_check
    main.cpp
    ${ECHOEDIT_SRC}/echo_edit_model.h
    ${ECHOEDIT_SRC}/echo_edit_model.cpp)

target_include_directories(echoedit_check PRIVATE ${ECHOEDIT_SRC})
target_link_libraries(echoedit_check PRIVATE Qt6::Core)
