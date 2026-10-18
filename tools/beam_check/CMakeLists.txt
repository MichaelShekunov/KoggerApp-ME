cmake_minimum_required(VERSION 3.22)

# Standalone check of src/data_processor/mosaic_beam.h -- the sonar's angular and range response,
# measured from the echograms and taken out of the mosaic. Plain C++, no Qt. Not part of the
# application build.
project(beam_check LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(KOGGER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
# A copy of the header can be checked instead of the tree's (a mutation run): -DMOSAICBEAM_SRC=<dir>
set(MOSAICBEAM_SRC ${KOGGER_SRC}/data_processor CACHE PATH "directory with mosaic_beam.h")

add_executable(beam_check main.cpp ${MOSAICBEAM_SRC}/mosaic_beam.h)
target_include_directories(beam_check PRIVATE ${MOSAICBEAM_SRC})
