cmake_minimum_required(VERSION 3.22)

# Standalone check of the echogram codecs the app decodes (KEC in src/kec, echocodec in src/codec) against the
# codec repository's test vectors. Offline, no device, no Qt. Not part of the application build;
# tools/codec_check/run.ps1 builds and runs it.
project(codec_check LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

set(KOGGER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
# Another copy of src/codec can be checked instead of the tree's (a mutation run): -DCODEC_SRC=<dir>
set(CODEC_SRC ${KOGGER_SRC}/codec CACHE PATH "directory with chart_v3.* and chart_codec_host.*")

add_executable(codec_check
    main.cpp
    ${KOGGER_SRC}/kec/kec.c
    ${CODEC_SRC}/chart_codec_host.c
    ${CODEC_SRC}/chart_v3.cpp)
set_source_files_properties(${CODEC_SRC}/chart_codec_host.c PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
target_include_directories(codec_check PRIVATE ${CODEC_SRC} ${KOGGER_SRC}/codec ${KOGGER_SRC}/kec)
