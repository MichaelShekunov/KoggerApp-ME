cmake_minimum_required(VERSION 3.22)

# Standalone check of the sonar acceptance module (src/sonar_qa, docs/SONAR_QA.md) against a simulated nanoSSS:
# the ping analysis, whole runs (baseline, check, units good and faulty), the files and the one-check-per-ten rule.
# Offline, no device. Not part of the application build; tools/sonarqa_check/run.ps1 builds and runs it.
project(sonarqa_check LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui)

set(QA_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src/sonar_qa)

add_executable(sonarqa_check
    main.cpp
    ${QA_SRC}/sonar_qa_analysis.h ${QA_SRC}/sonar_qa_analysis.cpp
    ${QA_SRC}/sonar_qa_store.h    ${QA_SRC}/sonar_qa_store.cpp
    ${QA_SRC}/sonar_qa_runner.h   ${QA_SRC}/sonar_qa_runner.cpp
    ${QA_SRC}/sonar_qa_demo.h     ${QA_SRC}/sonar_qa_demo.cpp
    ${QA_SRC}/sonar_qa_device.h)   # the interface only: the DevDriver adapter needs the application

target_include_directories(sonarqa_check PRIVATE ${QA_SRC})
target_link_libraries(sonarqa_check PRIVATE Qt6::Core Qt6::Gui)
