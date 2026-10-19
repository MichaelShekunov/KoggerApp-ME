target_sources(KoggerApp PRIVATE
    map_export_config.h
    map_export_geo.h        map_export_geo.cpp
    map_export_formats.h    map_export_formats.cpp
    map_export_meta.h       map_export_meta.cpp
    map_export_writer.h     map_export_writer.cpp
    map_package.h           map_package.cpp
    map_library.h           map_library.cpp
    map_layer.h             map_layer.cpp
    map_exporter.h          map_exporter.cpp)

target_include_directories(KoggerApp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
