target_sources(KoggerApp PRIVATE
    echo_edit_model.h     echo_edit_model.cpp
    echogram_editor.h     echogram_editor.cpp
    echo_edit_script.h    echo_edit_script.cpp)

target_include_directories(KoggerApp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
