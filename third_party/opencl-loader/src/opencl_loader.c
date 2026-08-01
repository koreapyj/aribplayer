/*
 * Minimal OpenCL 1.2 dispatch library for Android.
 *
 * This library deliberately links no vendor ICD at build time.  Each exported
 * OpenCL entry point resolves against libOpenCL.so the first time it is called.
 */
#define CL_TARGET_OPENCL_VERSION 120
#define CL_USE_DEPRECATED_OPENCL_1_2_APIS
#include <CL/cl.h>
#include <CL/cl_ext.h>

#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>

#define OPENCL_SHIM_TAG "opencl-shim"

static pthread_once_t opencl_library_once = PTHREAD_ONCE_INIT;
static void *opencl_library;
static atomic_flag opencl_platform_diagnostic_logged = ATOMIC_FLAG_INIT;

static void opencl_load_library(void)
{
    opencl_library = dlopen("libOpenCL.so", RTLD_NOW | RTLD_LOCAL);
    if (opencl_library == NULL) {
        const char *error = dlerror();
        __android_log_print(ANDROID_LOG_WARN, OPENCL_SHIM_TAG,
                            "Unable to load libOpenCL.so: %s",
                            error != NULL ? error : "unknown dlopen error");
    } else {
        __android_log_print(ANDROID_LOG_INFO, OPENCL_SHIM_TAG, "Loaded libOpenCL.so");
    }
}

static void opencl_log_platform_diagnostic(cl_int result, cl_uint num_platforms)
{
    if (!atomic_flag_test_and_set_explicit(&opencl_platform_diagnostic_logged,
                                           memory_order_relaxed)) {
        __android_log_print(ANDROID_LOG_WARN, OPENCL_SHIM_TAG,
                            "clGetPlatformIDs returned %d (num_platforms=%u)",
                            result, num_platforms);
    }
}

static void *opencl_lookup(const char *name)
{
    pthread_once(&opencl_library_once, opencl_load_library);
    return opencl_library ? dlsym(opencl_library, name) : NULL;
}

#define DECLARE_RESOLVER(name)                                             \
    static pthread_once_t name##_once = PTHREAD_ONCE_INIT;                 \
    static void *name##_symbol;                                             \
    static void resolve_##name(void) { name##_symbol = opencl_lookup(#name); }

#define LOAD(name, type)                                                    \
    pthread_once(&name##_once, resolve_##name);                             \
    __typeof__((type)0) fn = (__typeof__((type)0))name##_symbol

DECLARE_RESOLVER(clGetPlatformIDs)
DECLARE_RESOLVER(clGetPlatformInfo)
DECLARE_RESOLVER(clGetDeviceIDs)
DECLARE_RESOLVER(clGetDeviceInfo)
DECLARE_RESOLVER(clCreateContext)
DECLARE_RESOLVER(clReleaseContext)
DECLARE_RESOLVER(clGetContextInfo)
DECLARE_RESOLVER(clCreateCommandQueue)
DECLARE_RESOLVER(clRetainCommandQueue)
DECLARE_RESOLVER(clReleaseCommandQueue)
DECLARE_RESOLVER(clGetSupportedImageFormats)
DECLARE_RESOLVER(clCreateBuffer)
DECLARE_RESOLVER(clCreateSubBuffer)
DECLARE_RESOLVER(clCreateImage)
DECLARE_RESOLVER(clReleaseMemObject)
DECLARE_RESOLVER(clGetMemObjectInfo)
DECLARE_RESOLVER(clGetImageInfo)
DECLARE_RESOLVER(clCreateProgramWithSource)
DECLARE_RESOLVER(clBuildProgram)
DECLARE_RESOLVER(clReleaseProgram)
DECLARE_RESOLVER(clGetProgramBuildInfo)
DECLARE_RESOLVER(clCreateKernel)
DECLARE_RESOLVER(clReleaseKernel)
DECLARE_RESOLVER(clSetKernelArg)
DECLARE_RESOLVER(clEnqueueNDRangeKernel)
DECLARE_RESOLVER(clEnqueueReadBuffer)
DECLARE_RESOLVER(clEnqueueWriteBuffer)
DECLARE_RESOLVER(clEnqueueReadImage)
DECLARE_RESOLVER(clEnqueueWriteImage)
DECLARE_RESOLVER(clEnqueueCopyImage)
DECLARE_RESOLVER(clEnqueueFillBuffer)
DECLARE_RESOLVER(clEnqueueMapImage)
DECLARE_RESOLVER(clEnqueueMapBuffer)
DECLARE_RESOLVER(clEnqueueUnmapMemObject)
DECLARE_RESOLVER(clFlush)
DECLARE_RESOLVER(clFinish)
DECLARE_RESOLVER(clWaitForEvents)
DECLARE_RESOLVER(clReleaseEvent)
DECLARE_RESOLVER(clGetEventProfilingInfo)
DECLARE_RESOLVER(clGetExtensionFunctionAddress)
DECLARE_RESOLVER(clGetExtensionFunctionAddressForPlatform)

CL_API_ENTRY cl_int CL_API_CALL clGetPlatformIDs(cl_uint n, cl_platform_id *p, cl_uint *np)
{
    LOAD(clGetPlatformIDs, cl_int (*)(cl_uint, cl_platform_id *, cl_uint *));
    const cl_int result = fn ? fn(n, p, np) : CL_PLATFORM_NOT_FOUND_KHR;
    const cl_uint num_platforms = result == CL_SUCCESS && np != NULL ? *np : 0;
    if (result != CL_SUCCESS || (np != NULL && num_platforms == 0)) {
        opencl_log_platform_diagnostic(result, num_platforms);
    }
    return result;
}
CL_API_ENTRY cl_int CL_API_CALL clGetPlatformInfo(cl_platform_id p, cl_platform_info q, size_t s, void *v, size_t *r)
{
    LOAD(clGetPlatformInfo, cl_int (*)(cl_platform_id, cl_platform_info, size_t, void *, size_t *));
    return fn ? fn(p, q, s, v, r) : CL_INVALID_PLATFORM;
}
CL_API_ENTRY cl_int CL_API_CALL clGetDeviceIDs(cl_platform_id p, cl_device_type t, cl_uint n, cl_device_id *d, cl_uint *nd)
{
    LOAD(clGetDeviceIDs, cl_int (*)(cl_platform_id, cl_device_type, cl_uint, cl_device_id *, cl_uint *));
    return fn ? fn(p, t, n, d, nd) : CL_INVALID_PLATFORM;
}
CL_API_ENTRY cl_int CL_API_CALL clGetDeviceInfo(cl_device_id d, cl_device_info q, size_t s, void *v, size_t *r)
{
    LOAD(clGetDeviceInfo, cl_int (*)(cl_device_id, cl_device_info, size_t, void *, size_t *));
    return fn ? fn(d, q, s, v, r) : CL_INVALID_DEVICE;
}
CL_API_ENTRY cl_context CL_API_CALL clCreateContext(const cl_context_properties *p, cl_uint n, const cl_device_id *d, void (CL_CALLBACK *cb)(const char *, const void *, size_t, void *), void *u, cl_int *e)
{
    LOAD(clCreateContext, cl_context (*)(const cl_context_properties *, cl_uint, const cl_device_id *, void (CL_CALLBACK *)(const char *, const void *, size_t, void *), void *, cl_int *));
    if (!fn) { if (e) *e = CL_INVALID_OPERATION; return NULL; }
    return fn(p, n, d, cb, u, e);
}
CL_API_ENTRY cl_int CL_API_CALL clReleaseContext(cl_context c)
{
    LOAD(clReleaseContext, cl_int (*)(cl_context)); return fn ? fn(c) : CL_INVALID_CONTEXT;
}
CL_API_ENTRY cl_int CL_API_CALL clGetContextInfo(cl_context c, cl_context_info q, size_t s, void *v, size_t *r)
{
    LOAD(clGetContextInfo, cl_int (*)(cl_context, cl_context_info, size_t, void *, size_t *)); return fn ? fn(c, q, s, v, r) : CL_INVALID_CONTEXT;
}
CL_API_ENTRY cl_command_queue CL_API_CALL clCreateCommandQueue(cl_context c, cl_device_id d, cl_command_queue_properties p, cl_int *e)
{
    LOAD(clCreateCommandQueue, cl_command_queue (*)(cl_context, cl_device_id, cl_command_queue_properties, cl_int *));
    if (!fn) { if (e) *e = CL_INVALID_OPERATION; return NULL; } return fn(c, d, p, e);
}
CL_API_ENTRY cl_int CL_API_CALL clRetainCommandQueue(cl_command_queue q)
{
    LOAD(clRetainCommandQueue, cl_int (*)(cl_command_queue)); return fn ? fn(q) : CL_INVALID_COMMAND_QUEUE;
}
CL_API_ENTRY cl_int CL_API_CALL clReleaseCommandQueue(cl_command_queue q)
{
    LOAD(clReleaseCommandQueue, cl_int (*)(cl_command_queue)); return fn ? fn(q) : CL_INVALID_COMMAND_QUEUE;
}
CL_API_ENTRY cl_int CL_API_CALL clGetSupportedImageFormats(cl_context c, cl_mem_flags f, cl_mem_object_type t, cl_uint n, cl_image_format *i, cl_uint *ni)
{
    LOAD(clGetSupportedImageFormats, cl_int (*)(cl_context, cl_mem_flags, cl_mem_object_type, cl_uint, cl_image_format *, cl_uint *)); return fn ? fn(c, f, t, n, i, ni) : CL_INVALID_CONTEXT;
}
CL_API_ENTRY cl_mem CL_API_CALL clCreateBuffer(cl_context c, cl_mem_flags f, size_t s, void *h, cl_int *e)
{
    LOAD(clCreateBuffer, cl_mem (*)(cl_context, cl_mem_flags, size_t, void *, cl_int *)); if (!fn) { if (e) *e = CL_INVALID_OPERATION; return NULL; } return fn(c, f, s, h, e);
}
CL_API_ENTRY cl_mem CL_API_CALL clCreateSubBuffer(cl_mem b, cl_mem_flags f, cl_buffer_create_type t, const void *i, cl_int *e)
{
    LOAD(clCreateSubBuffer, cl_mem (*)(cl_mem, cl_mem_flags, cl_buffer_create_type, const void *, cl_int *)); if (!fn) { if (e) *e = CL_INVALID_OPERATION; return NULL; } return fn(b, f, t, i, e);
}
CL_API_ENTRY cl_mem CL_API_CALL clCreateImage(cl_context c, cl_mem_flags f, const cl_image_format *i, const cl_image_desc *d, void *h, cl_int *e)
{
    LOAD(clCreateImage, cl_mem (*)(cl_context, cl_mem_flags, const cl_image_format *, const cl_image_desc *, void *, cl_int *)); if (!fn) { if (e) *e = CL_INVALID_OPERATION; return NULL; } return fn(c, f, i, d, h, e);
}
CL_API_ENTRY cl_int CL_API_CALL clReleaseMemObject(cl_mem m)
{
    LOAD(clReleaseMemObject, cl_int (*)(cl_mem)); return fn ? fn(m) : CL_INVALID_MEM_OBJECT;
}
CL_API_ENTRY cl_int CL_API_CALL clGetMemObjectInfo(cl_mem m, cl_mem_info q, size_t s, void *v, size_t *r)
{
    LOAD(clGetMemObjectInfo, cl_int (*)(cl_mem, cl_mem_info, size_t, void *, size_t *)); return fn ? fn(m, q, s, v, r) : CL_INVALID_MEM_OBJECT;
}
CL_API_ENTRY cl_int CL_API_CALL clGetImageInfo(cl_mem m, cl_image_info q, size_t s, void *v, size_t *r)
{
    LOAD(clGetImageInfo, cl_int (*)(cl_mem, cl_image_info, size_t, void *, size_t *)); return fn ? fn(m, q, s, v, r) : CL_INVALID_MEM_OBJECT;
}
CL_API_ENTRY cl_program CL_API_CALL clCreateProgramWithSource(cl_context c, cl_uint n, const char **src, const size_t *l, cl_int *e)
{
    LOAD(clCreateProgramWithSource, cl_program (*)(cl_context, cl_uint, const char **, const size_t *, cl_int *)); if (!fn) { if (e) *e = CL_INVALID_OPERATION; return NULL; } return fn(c, n, src, l, e);
}
CL_API_ENTRY cl_int CL_API_CALL clBuildProgram(cl_program p, cl_uint n, const cl_device_id *d, const char *o, void (CL_CALLBACK *cb)(cl_program, void *), void *u)
{
    LOAD(clBuildProgram, cl_int (*)(cl_program, cl_uint, const cl_device_id *, const char *, void (CL_CALLBACK *)(cl_program, void *), void *)); return fn ? fn(p, n, d, o, cb, u) : CL_INVALID_PROGRAM;
}
CL_API_ENTRY cl_int CL_API_CALL clReleaseProgram(cl_program p)
{
    LOAD(clReleaseProgram, cl_int (*)(cl_program)); return fn ? fn(p) : CL_INVALID_PROGRAM;
}
CL_API_ENTRY cl_int CL_API_CALL clGetProgramBuildInfo(cl_program p, cl_device_id d, cl_program_build_info q, size_t s, void *v, size_t *r)
{
    LOAD(clGetProgramBuildInfo, cl_int (*)(cl_program, cl_device_id, cl_program_build_info, size_t, void *, size_t *)); return fn ? fn(p, d, q, s, v, r) : CL_INVALID_PROGRAM;
}
CL_API_ENTRY cl_kernel CL_API_CALL clCreateKernel(cl_program p, const char *n, cl_int *e)
{
    LOAD(clCreateKernel, cl_kernel (*)(cl_program, const char *, cl_int *)); if (!fn) { if (e) *e = CL_INVALID_OPERATION; return NULL; } return fn(p, n, e);
}
CL_API_ENTRY cl_int CL_API_CALL clReleaseKernel(cl_kernel k)
{
    LOAD(clReleaseKernel, cl_int (*)(cl_kernel)); return fn ? fn(k) : CL_INVALID_KERNEL;
}
CL_API_ENTRY cl_int CL_API_CALL clSetKernelArg(cl_kernel k, cl_uint n, size_t s, const void *v)
{
    LOAD(clSetKernelArg, cl_int (*)(cl_kernel, cl_uint, size_t, const void *)); return fn ? fn(k, n, s, v) : CL_INVALID_KERNEL;
}
CL_API_ENTRY cl_int CL_API_CALL clEnqueueNDRangeKernel(cl_command_queue q, cl_kernel k, cl_uint w, const size_t *o, const size_t *g, const size_t *l, cl_uint n, const cl_event *e, cl_event *r)
{
    LOAD(clEnqueueNDRangeKernel, cl_int (*)(cl_command_queue, cl_kernel, cl_uint, const size_t *, const size_t *, const size_t *, cl_uint, const cl_event *, cl_event *)); return fn ? fn(q, k, w, o, g, l, n, e, r) : CL_INVALID_COMMAND_QUEUE;
}
CL_API_ENTRY cl_int CL_API_CALL clEnqueueReadBuffer(cl_command_queue q, cl_mem b, cl_bool x, size_t o, size_t s, void *p, cl_uint n, const cl_event *e, cl_event *r)
{
    LOAD(clEnqueueReadBuffer, cl_int (*)(cl_command_queue, cl_mem, cl_bool, size_t, size_t, void *, cl_uint, const cl_event *, cl_event *)); return fn ? fn(q, b, x, o, s, p, n, e, r) : CL_INVALID_COMMAND_QUEUE;
}
CL_API_ENTRY cl_int CL_API_CALL clEnqueueWriteBuffer(cl_command_queue q, cl_mem b, cl_bool x, size_t o, size_t s, const void *p, cl_uint n, const cl_event *e, cl_event *r)
{
    LOAD(clEnqueueWriteBuffer, cl_int (*)(cl_command_queue, cl_mem, cl_bool, size_t, size_t, const void *, cl_uint, const cl_event *, cl_event *)); return fn ? fn(q, b, x, o, s, p, n, e, r) : CL_INVALID_COMMAND_QUEUE;
}
CL_API_ENTRY cl_int CL_API_CALL clEnqueueReadImage(cl_command_queue q, cl_mem m, cl_bool b, const size_t *o, const size_t *rgn, size_t rp, size_t sp, void *p, cl_uint n, const cl_event *e, cl_event *r)
{
    LOAD(clEnqueueReadImage, cl_int (*)(cl_command_queue, cl_mem, cl_bool, const size_t *, const size_t *, size_t, size_t, void *, cl_uint, const cl_event *, cl_event *)); return fn ? fn(q, m, b, o, rgn, rp, sp, p, n, e, r) : CL_INVALID_COMMAND_QUEUE;
}
CL_API_ENTRY cl_int CL_API_CALL clEnqueueWriteImage(cl_command_queue q, cl_mem m, cl_bool b, const size_t *o, const size_t *rgn, size_t rp, size_t sp, const void *p, cl_uint n, const cl_event *e, cl_event *r)
{
    LOAD(clEnqueueWriteImage, cl_int (*)(cl_command_queue, cl_mem, cl_bool, const size_t *, const size_t *, size_t, size_t, const void *, cl_uint, const cl_event *, cl_event *)); return fn ? fn(q, m, b, o, rgn, rp, sp, p, n, e, r) : CL_INVALID_COMMAND_QUEUE;
}
CL_API_ENTRY cl_int CL_API_CALL clEnqueueCopyImage(cl_command_queue q, cl_mem a, cl_mem b, const size_t *ao, const size_t *bo, const size_t *rgn, cl_uint n, const cl_event *e, cl_event *r)
{
    LOAD(clEnqueueCopyImage, cl_int (*)(cl_command_queue, cl_mem, cl_mem, const size_t *, const size_t *, const size_t *, cl_uint, const cl_event *, cl_event *)); return fn ? fn(q, a, b, ao, bo, rgn, n, e, r) : CL_INVALID_COMMAND_QUEUE;
}
CL_API_ENTRY cl_int CL_API_CALL clEnqueueFillBuffer(cl_command_queue q, cl_mem b, const void *p, size_t ps, size_t o, size_t s, cl_uint n, const cl_event *e, cl_event *r)
{
    LOAD(clEnqueueFillBuffer, cl_int (*)(cl_command_queue, cl_mem, const void *, size_t, size_t, size_t, cl_uint, const cl_event *, cl_event *)); return fn ? fn(q, b, p, ps, o, s, n, e, r) : CL_INVALID_COMMAND_QUEUE;
}
CL_API_ENTRY void * CL_API_CALL clEnqueueMapImage(cl_command_queue q, cl_mem m, cl_bool b, cl_map_flags f, const size_t *o, const size_t *rgn, size_t *rp, size_t *sp, cl_uint n, const cl_event *e, cl_event *r, cl_int *x)
{
    LOAD(clEnqueueMapImage, void *(*)(cl_command_queue, cl_mem, cl_bool, cl_map_flags, const size_t *, const size_t *, size_t *, size_t *, cl_uint, const cl_event *, cl_event *, cl_int *)); if (!fn) { if (x) *x = CL_INVALID_OPERATION; return NULL; } return fn(q, m, b, f, o, rgn, rp, sp, n, e, r, x);
}
CL_API_ENTRY void * CL_API_CALL clEnqueueMapBuffer(cl_command_queue q, cl_mem b, cl_bool x, cl_map_flags f, size_t o, size_t s, cl_uint n, const cl_event *e, cl_event *r, cl_int *z)
{
    LOAD(clEnqueueMapBuffer, void *(*)(cl_command_queue, cl_mem, cl_bool, cl_map_flags, size_t, size_t, cl_uint, const cl_event *, cl_event *, cl_int *)); if (!fn) { if (z) *z = CL_INVALID_OPERATION; return NULL; } return fn(q, b, x, f, o, s, n, e, r, z);
}
CL_API_ENTRY cl_int CL_API_CALL clEnqueueUnmapMemObject(cl_command_queue q, cl_mem m, void *p, cl_uint n, const cl_event *e, cl_event *r)
{
    LOAD(clEnqueueUnmapMemObject, cl_int (*)(cl_command_queue, cl_mem, void *, cl_uint, const cl_event *, cl_event *)); return fn ? fn(q, m, p, n, e, r) : CL_INVALID_COMMAND_QUEUE;
}
CL_API_ENTRY cl_int CL_API_CALL clFlush(cl_command_queue q)
{
    LOAD(clFlush, cl_int (*)(cl_command_queue)); return fn ? fn(q) : CL_INVALID_COMMAND_QUEUE;
}
CL_API_ENTRY cl_int CL_API_CALL clFinish(cl_command_queue q)
{
    LOAD(clFinish, cl_int (*)(cl_command_queue)); return fn ? fn(q) : CL_INVALID_COMMAND_QUEUE;
}
CL_API_ENTRY cl_int CL_API_CALL clWaitForEvents(cl_uint n, const cl_event *e)
{
    LOAD(clWaitForEvents, cl_int (*)(cl_uint, const cl_event *)); return fn ? fn(n, e) : CL_INVALID_EVENT;
}
CL_API_ENTRY cl_int CL_API_CALL clReleaseEvent(cl_event e)
{
    LOAD(clReleaseEvent, cl_int (*)(cl_event)); return fn ? fn(e) : CL_INVALID_EVENT;
}
CL_API_ENTRY cl_int CL_API_CALL clGetEventProfilingInfo(cl_event e, cl_profiling_info q, size_t s, void *v, size_t *r)
{
    LOAD(clGetEventProfilingInfo, cl_int (*)(cl_event, cl_profiling_info, size_t, void *, size_t *)); return fn ? fn(e, q, s, v, r) : CL_INVALID_EVENT;
}
CL_API_ENTRY void * CL_API_CALL clGetExtensionFunctionAddress(const char *n)
{
    LOAD(clGetExtensionFunctionAddress, void *(*)(const char *)); return fn ? fn(n) : opencl_lookup(n);
}
CL_API_ENTRY void * CL_API_CALL clGetExtensionFunctionAddressForPlatform(cl_platform_id p, const char *n)
{
    LOAD(clGetExtensionFunctionAddressForPlatform, void *(*)(cl_platform_id, const char *)); return fn ? fn(p, n) : opencl_lookup(n);
}
