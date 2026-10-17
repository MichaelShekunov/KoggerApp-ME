cmake_minimum_required(VERSION 3.22)

# Standalone check of MosaicIndexProvider::tilesInQuadNed (src/mosaic_index_provider.cpp), the tile
# cover behind the epoch index and the visible tiles, against the QPainterPath test it replaced.
# Not part of the application build. Configure with the Qt kit on CMAKE_PREFIX_PATH.
project(tiles_check LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui)

set(KOGGER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
# A copy of the source can be checked instead of the tree's (a mutation run): -DPROVIDER_SRC=<file>
set(PROVIDER_SRC ${KOGGER_SRC}/mosaic_index_provider.cpp CACHE FILEPATH "mosaic_index_provider.cpp to check")
add_executable(tiles_check main.cpp ${PROVIDER_SRC})
target_include_directories(tiles_check PRIVATE ${KOGGER_SRC} ${KOGGER_SRC}/data_processor)
target_link_libraries(tiles_check PRIVATE Qt6::Core Qt6::Gui)
