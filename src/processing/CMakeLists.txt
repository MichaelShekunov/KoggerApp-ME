target_sources(KoggerApp PRIVATE
    echogram_deconvolution.h     echogram_deconvolution.cpp
    echogram_deconvolution_cl.h  echogram_deconvolution_cl.cpp
    echogram_relief.h            echogram_relief.cpp)

target_include_directories(KoggerApp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/third_party/opencl)

# Nothing links OpenCL: the runtime is opened with LoadLibrary when the pass
# first runs. See third_party/opencl/README.md.
