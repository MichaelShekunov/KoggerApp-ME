target_sources(KoggerApp PRIVATE
    sonar_qa_analysis.h   sonar_qa_analysis.cpp
    sonar_qa_store.h      sonar_qa_store.cpp
    sonar_qa_device.h     sonar_qa_device.cpp
    sonar_qa_demo.h       sonar_qa_demo.cpp
    sonar_qa_runner.h     sonar_qa_runner.cpp)

target_include_directories(KoggerApp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
