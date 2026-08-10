#include "echogram_deconvolution_cl.h"
#include "echogram_deconvolution.h"

#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>

#include <algorithm>
#include <cmath>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace echo_deconv {

namespace {

constexpr float kDegToRad     = 3.14159265358979323846f / 180.0f;
constexpr float kFwhmToSigma  = 1.0f / 2.35482004503f;
constexpr int   kMaxHalfWidth = 64; // matches Params::maxKernelRadius ceiling

// One work-item per sample. The row's kernel is read from a per-row slice, and
// rows whose radius is zero fall through untouched - the same rows the CPU path
// skips, for the same reason.
const char* kSource = R"CLC(
__kernel void rl_ratio(__global const float* observed,
                       __global const float* estimate,
                       __global       float* ratio,
                       __global const float* kernels,
                       __global const int*   radii,
                       const int width, const int maxHalf, const float eps)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= width) { return; }

    const int r = radii[y];
    const int base = y * width;
    if (r < 1) { ratio[base + x] = 1.0f; return; }

    __global const float* k = kernels + (long)y * (maxHalf + 1);
    float acc = estimate[base + x] * k[0];
    for (int i = 1; i <= r; ++i) {
        const int a = clamp(x - i, 0, width - 1);
        const int b = clamp(x + i, 0, width - 1);
        acc += (estimate[base + a] + estimate[base + b]) * k[i];
    }
    ratio[base + x] = observed[base + x] / (acc > eps ? acc : eps);
}

__kernel void rl_apply(__global       float* estimate,
                       __global const float* ratio,
                       __global const float* kernels,
                       __global const int*   radii,
                       const int width, const int maxHalf)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= width) { return; }

    const int r = radii[y];
    if (r < 1) { return; }

    const int base = y * width;
    __global const float* k = kernels + (long)y * (maxHalf + 1);
    float acc = ratio[base + x] * k[0];
    for (int i = 1; i <= r; ++i) {
        const int a = clamp(x - i, 0, width - 1);
        const int b = clamp(x + i, 0, width - 1);
        acc += (ratio[base + a] + ratio[base + b]) * k[i];
    }
    estimate[base + x] *= acc;
}
)CLC";

// ---- the slice of OpenCL this needs, resolved at runtime -------------------
#define CL_FUNCS(X) \
    X(clGetPlatformIDs) X(clGetDeviceIDs) X(clGetDeviceInfo) \
    X(clCreateContext) X(clCreateCommandQueue) \
    X(clCreateProgramWithSource) X(clBuildProgram) X(clGetProgramBuildInfo) \
    X(clCreateKernel) X(clSetKernelArg) X(clEnqueueNDRangeKernel) \
    X(clCreateBuffer) X(clEnqueueWriteBuffer) X(clEnqueueReadBuffer) X(clFinish) \
    X(clReleaseKernel) X(clReleaseProgram) X(clReleaseMemObject) \
    X(clReleaseCommandQueue) X(clReleaseContext)

#define DECL(name) static decltype(&::name) p_##name = nullptr;
CL_FUNCS(DECL)
#undef DECL

void* openLibrary()
{
#ifdef _WIN32
    return reinterpret_cast<void*>(LoadLibraryA("OpenCL.dll"));
#else
    return dlopen("libOpenCL.so.1", RTLD_NOW);
#endif
}

void* symbolOf(void* lib, const char* name)
{
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}

bool loadSymbols(void* lib)
{
    bool ok = true;
#define LOAD(name) \
    p_##name = reinterpret_cast<decltype(&::name)>(symbolOf(lib, #name)); \
    ok = ok && (p_##name != nullptr);
    CL_FUNCS(LOAD)
#undef LOAD
    return ok;
}

} // namespace


struct ClBackend::Impl {
    cl_context       context = nullptr;
    cl_command_queue queue   = nullptr;
    cl_program       program = nullptr;
    cl_kernel        ratioK  = nullptr;
    cl_kernel        applyK  = nullptr;
    cl_device_id     device  = nullptr;
    std::string      name;

    // Reused across passes: an echogram redraws constantly and reallocating
    // device memory each time would cost more than the arithmetic.
    cl_mem bufObserved = nullptr;
    cl_mem bufEstimate = nullptr;
    cl_mem bufRatio    = nullptr;
    cl_mem bufKernels  = nullptr;
    cl_mem bufRadii    = nullptr;
    size_t capSamples  = 0;
    size_t capRows     = 0;

    std::vector<float> host;
    std::vector<float> kernels;
    std::vector<int>   radii;
};


ClBackend& ClBackend::instance()
{
    static ClBackend backend;
    return backend;
}

ClBackend::~ClBackend()
{
    if (!d_) { return; }
    if (d_->bufObserved) { p_clReleaseMemObject(d_->bufObserved); }
    if (d_->bufEstimate) { p_clReleaseMemObject(d_->bufEstimate); }
    if (d_->bufRatio)    { p_clReleaseMemObject(d_->bufRatio); }
    if (d_->bufKernels)  { p_clReleaseMemObject(d_->bufKernels); }
    if (d_->bufRadii)    { p_clReleaseMemObject(d_->bufRadii); }
    if (d_->ratioK)      { p_clReleaseKernel(d_->ratioK); }
    if (d_->applyK)      { p_clReleaseKernel(d_->applyK); }
    if (d_->program)     { p_clReleaseProgram(d_->program); }
    if (d_->queue)       { p_clReleaseCommandQueue(d_->queue); }
    if (d_->context)     { p_clReleaseContext(d_->context); }
    delete d_;
}

static std::string g_reason;

bool ClBackend::available()
{
    if (tried_) { return d_ != nullptr; }
    tried_ = true;

    void* lib = openLibrary();
    if (!lib) { g_reason = "OpenCL runtime not present"; return false; }
    if (!loadSymbols(lib)) { g_reason = "OpenCL runtime is missing entry points"; return false; }

    cl_uint platformCount = 0;
    if (p_clGetPlatformIDs(0, nullptr, &platformCount) != CL_SUCCESS || platformCount == 0) {
        g_reason = "no OpenCL platform";
        return false;
    }
    std::vector<cl_platform_id> platforms(platformCount);
    p_clGetPlatformIDs(platformCount, platforms.data(), nullptr);

    // Prefer a discrete GPU; a CPU device would only be slower than the threads
    // already in place.
    cl_device_id device = nullptr;
    for (cl_platform_id pf : platforms) {
        cl_uint n = 0;
        if (p_clGetDeviceIDs(pf, CL_DEVICE_TYPE_GPU, 0, nullptr, &n) != CL_SUCCESS || n == 0) {
            continue;
        }
        std::vector<cl_device_id> devices(n);
        p_clGetDeviceIDs(pf, CL_DEVICE_TYPE_GPU, n, devices.data(), nullptr);
        for (cl_device_id dev : devices) {
            cl_bool hostUnified = CL_TRUE;
            p_clGetDeviceInfo(dev, CL_DEVICE_HOST_UNIFIED_MEMORY, sizeof(hostUnified), &hostUnified, nullptr);
            if (!hostUnified) { device = dev; break; }   // discrete
            if (!device) { device = dev; }               // integrated, as a fallback
        }
        if (device) { break; }
    }
    if (!device) { g_reason = "no OpenCL GPU device"; return false; }

    cl_int err = CL_SUCCESS;
    cl_context context = p_clCreateContext(nullptr, 1, &device, nullptr, nullptr, &err);
    if (!context || err != CL_SUCCESS) { g_reason = "could not create an OpenCL context"; return false; }

    cl_command_queue queue = p_clCreateCommandQueue(context, device, 0, &err);
    if (!queue || err != CL_SUCCESS) {
        p_clReleaseContext(context);
        g_reason = "could not create a command queue";
        return false;
    }

    cl_program program = p_clCreateProgramWithSource(context, 1, &kSource, nullptr, &err);
    if (!program || err != CL_SUCCESS) {
        p_clReleaseCommandQueue(queue); p_clReleaseContext(context);
        g_reason = "could not create the program";
        return false;
    }
    if (p_clBuildProgram(program, 1, &device, "-cl-fast-relaxed-math", nullptr, nullptr) != CL_SUCCESS) {
        size_t logSize = 0;
        p_clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, 0, nullptr, &logSize);
        std::string log(logSize, '\0');
        p_clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, logSize, log.data(), nullptr);
        g_reason = "kernel did not build: " + log;
        p_clReleaseProgram(program); p_clReleaseCommandQueue(queue); p_clReleaseContext(context);
        return false;
    }

    auto* impl = new Impl();
    impl->context = context;
    impl->queue   = queue;
    impl->program = program;
    impl->device  = device;
    impl->ratioK  = p_clCreateKernel(program, "rl_ratio", &err);
    impl->applyK  = p_clCreateKernel(program, "rl_apply", &err);
    if (!impl->ratioK || !impl->applyK) {
        g_reason = "could not create the kernels";
        delete impl;
        return false;
    }

    char nameBuf[256] = {};
    p_clGetDeviceInfo(device, CL_DEVICE_NAME, sizeof(nameBuf) - 1, nameBuf, nullptr);
    impl->name = nameBuf;

    d_ = impl;
    g_reason.clear();
    return true;
}

std::string ClBackend::description()
{
    return available() ? d_->name : std::string();
}

std::string ClBackend::unavailableReason()
{
    return available() ? std::string() : g_reason;
}


bool ClBackend::alongTrack(int16_t* const* columns, int width, int height,
                           const float* rangeAt, const float* alongTrackM,
                           const float* /*pitchRad*/, float fallbackSpacingM,
                           const Params& params, Stats& stats)
{
    if (!available() || !columns || width < 3 || height <= 0 || !rangeAt) {
        return false;
    }
    if (!(params.angularResolutionDeg > 0.0f) || params.iterations <= 0) {
        return false;
    }
    // Geometry compensation stays on the CPU: it resamples per row, which is
    // sequential in a way this kernel is not, and it is off by default anyway.
    if (params.compensateGeometry) {
        return false;
    }

    // Kernel widths are decided host-side, exactly as the CPU path decides them,
    // so the two produce the same image and the GPU is a speed choice only.
    float spacing = fallbackSpacingM;
    if (alongTrackM) {
        std::vector<float> steps;
        steps.reserve(size_t(width));
        bool complete = true;
        for (int c = 0; c < width; ++c) {
            if (!std::isfinite(alongTrackM[c])) { complete = false; break; }
        }
        if (complete) {
            for (int c = 1; c < width; ++c) {
                const float dstep = alongTrackM[c] - alongTrackM[c - 1];
                if (std::isfinite(dstep) && dstep > 0.0f) { steps.push_back(dstep); }
            }
            if (int(steps.size()) * 2 >= width) {
                const size_t mid = steps.size() / 2;
                std::nth_element(steps.begin(), steps.begin() + mid, steps.end());
                spacing = steps[mid];
            }
        }
    }
    if (!(spacing > 0.0f)) { return false; }

    const int maxHalf = std::min(params.maxKernelRadius, kMaxHalfWidth);
    const size_t samples = size_t(width) * size_t(height);

    d_->host.resize(samples);
    d_->kernels.assign(size_t(height) * size_t(maxHalf + 1), 0.0f);
    d_->radii.assign(size_t(height), 0);

    for (int row = 0; row < height; ++row) {
        const float sigma = footprintMetres(rangeAt[row], params.angularResolutionDeg)
                            * kFwhmToSigma / spacing;
        int radius = 0;
        if (sigma > params.minSigmaColumns) {
            radius = int(std::ceil(3.0f * sigma));
            radius = std::min(radius, maxHalf);
            radius = std::min(radius, (width - 1) / 2);
        }

        float rowMax = 0.0f;
        float* dst = d_->host.data() + size_t(row) * size_t(width);
        for (int c = 0; c < width; ++c) {
            const float v = float(columns[c][row]);
            dst[c] = v > 0.0f ? v : 0.0f;
            rowMax = std::max(rowMax, dst[c]);
        }
        if (rowMax <= 0.0f) { radius = 0; }

        if (radius < 1) {
            d_->radii[size_t(row)] = 0;
            ++stats.rowsSkipped;
            continue;
        }
        d_->radii[size_t(row)] = radius;
        stats.maxRadius = std::max(stats.maxRadius, radius);
        ++stats.rowsProcessed;

        float* k = d_->kernels.data() + size_t(row) * size_t(maxHalf + 1);
        float sum = 0.0f;
        for (int i = 0; i <= radius; ++i) {
            const float x = float(i) / sigma;
            k[i] = std::exp(-0.5f * x * x);
            sum += (i == 0) ? k[i] : 2.0f * k[i];
        }
        for (int i = 0; i <= radius; ++i) { k[i] /= sum; }
    }

    if (stats.rowsProcessed <= 0) { return false; }

    cl_int err = CL_SUCCESS;
    auto ensure = [&](cl_mem& buf, size_t& cap, size_t want, cl_mem_flags flags) {
        if (buf && cap >= want) { return true; }
        if (buf) { p_clReleaseMemObject(buf); buf = nullptr; }
        buf = p_clCreateBuffer(d_->context, flags, want, nullptr, &err);
        if (!buf || err != CL_SUCCESS) { return false; }
        cap = want;
        return true;
    };

    size_t capA = d_->capSamples * sizeof(float);
    size_t capB = capA, capC = capA;
    const size_t wantSamples = samples * sizeof(float);
    if (!ensure(d_->bufObserved, capA, wantSamples, CL_MEM_READ_ONLY) ||
        !ensure(d_->bufEstimate, capB, wantSamples, CL_MEM_READ_WRITE) ||
        !ensure(d_->bufRatio,    capC, wantSamples, CL_MEM_READ_WRITE)) {
        return false;
    }
    d_->capSamples = samples;

    size_t capK = d_->capRows * size_t(maxHalf + 1) * sizeof(float);
    size_t capR = d_->capRows * sizeof(int);
    const size_t wantK = size_t(height) * size_t(maxHalf + 1) * sizeof(float);
    const size_t wantR = size_t(height) * sizeof(int);
    if (!ensure(d_->bufKernels, capK, wantK, CL_MEM_READ_ONLY) ||
        !ensure(d_->bufRadii,   capR, wantR, CL_MEM_READ_ONLY)) {
        return false;
    }
    d_->capRows = size_t(height);

    if (p_clEnqueueWriteBuffer(d_->queue, d_->bufObserved, CL_FALSE, 0, wantSamples, d_->host.data(), 0, nullptr, nullptr) != CL_SUCCESS ||
        p_clEnqueueWriteBuffer(d_->queue, d_->bufEstimate, CL_FALSE, 0, wantSamples, d_->host.data(), 0, nullptr, nullptr) != CL_SUCCESS ||
        p_clEnqueueWriteBuffer(d_->queue, d_->bufKernels,  CL_FALSE, 0, wantK, d_->kernels.data(), 0, nullptr, nullptr) != CL_SUCCESS ||
        p_clEnqueueWriteBuffer(d_->queue, d_->bufRadii,    CL_FALSE, 0, wantR, d_->radii.data(), 0, nullptr, nullptr) != CL_SUCCESS) {
        return false;
    }

    const float eps = 1.0e-3f;
    auto setArgs = [&](cl_kernel k, bool isRatio) {
        cl_uint a = 0;
        if (isRatio) {
            p_clSetKernelArg(k, a++, sizeof(cl_mem), &d_->bufObserved);
            p_clSetKernelArg(k, a++, sizeof(cl_mem), &d_->bufEstimate);
            p_clSetKernelArg(k, a++, sizeof(cl_mem), &d_->bufRatio);
        }
        else {
            p_clSetKernelArg(k, a++, sizeof(cl_mem), &d_->bufEstimate);
            p_clSetKernelArg(k, a++, sizeof(cl_mem), &d_->bufRatio);
        }
        p_clSetKernelArg(k, a++, sizeof(cl_mem), &d_->bufKernels);
        p_clSetKernelArg(k, a++, sizeof(cl_mem), &d_->bufRadii);
        p_clSetKernelArg(k, a++, sizeof(int), &width);
        p_clSetKernelArg(k, a++, sizeof(int), &maxHalf);
        if (isRatio) { p_clSetKernelArg(k, a++, sizeof(float), &eps); }
    };
    setArgs(d_->ratioK, true);
    setArgs(d_->applyK, false);

    const size_t local[2]  = {64, 1};
    const size_t global[2] = {((size_t(width) + 63) / 64) * 64, size_t(height)};

    for (int it = 0; it < params.iterations; ++it) {
        if (p_clEnqueueNDRangeKernel(d_->queue, d_->ratioK, 2, nullptr, global, local, 0, nullptr, nullptr) != CL_SUCCESS ||
            p_clEnqueueNDRangeKernel(d_->queue, d_->applyK, 2, nullptr, global, local, 0, nullptr, nullptr) != CL_SUCCESS) {
            return false;
        }
    }

    if (p_clEnqueueReadBuffer(d_->queue, d_->bufEstimate, CL_TRUE, 0, wantSamples, d_->host.data(), 0, nullptr, nullptr) != CL_SUCCESS) {
        return false;
    }
    p_clFinish(d_->queue);

    for (int row = 0; row < height; ++row) {
        if (d_->radii[size_t(row)] < 1) { continue; }
        const float* src = d_->host.data() + size_t(row) * size_t(width);
        for (int c = 0; c < width; ++c) {
            columns[c][row] = int16_t(std::lround(std::clamp(src[c], 0.0f, 255.0f)));
        }
    }

    return true;
}

} // namespace echo_deconv
