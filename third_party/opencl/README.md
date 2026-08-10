# OpenCL headers

Official Khronos headers, vendored so the build needs no vendor SDK.

| | |
| --- | --- |
| Source | https://github.com/KhronosGroup/OpenCL-Headers, branch `main` |
| Files | `CL/cl.h`, `CL/cl_platform.h`, `CL/cl_version.h` |
| Retrieved | 2026-08-10 |
| Licence | Apache License 2.0 (stated in each file's header) |

Apache-2.0 is one-way compatible with GPL-3.0: Apache-licensed code may be
combined into a GPLv3 work, which is the direction this needs. KoggerApp is
GPLv3.

**Not taken from the CUDA toolkit.** It ships its own copies of these headers at
`CUDA/v13.2/include/CL/`, and using them would have been easier, but they come
under NVIDIA's terms rather than Khronos'. Three files from upstream avoid the
question entirely.

Only these three are needed: `cl.h` includes the other two and nothing else.
Verified by compiling a translation unit that includes `CL/cl.h` alone.

## No link-time dependency

Nothing links `OpenCL.lib`. `src/processing/echogram_deconvolution_cl.cpp` opens
`OpenCL.dll` with `LoadLibrary` at runtime and resolves what it needs, so:

- the build needs no SDK and no import library;
- `windeployqt` has nothing extra to carry;
- a machine with no OpenCL runtime loses the GPU path and keeps the CPU one,
  rather than failing to start.

These headers therefore supply types and constants only.
