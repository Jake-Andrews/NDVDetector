#include "GPUVendor.h"

#include <EGL/egl.h>
#include <cstring>

extern "C" {
#include <libavutil/hwcontext.h>
}

static GPUVendor detect_via_egl()
{
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr))
        return GPUVendor::Unknown;

    char const* vendor = eglQueryString(dpy, EGL_VENDOR);
    eglTerminate(dpy);

    if (!vendor)
        return GPUVendor::Unknown;
    if (strstr(vendor, "NVIDIA"))
        return GPUVendor::Nvidia;
    if (strstr(vendor, "AMD") || strstr(vendor, "Mesa") || strstr(vendor, "RADV"))
        return GPUVendor::AMD;
    if (strstr(vendor, "Intel"))
        return GPUVendor::Intel;
    if (strstr(vendor, "Apple"))
        return GPUVendor::Apple;

    return GPUVendor::Unknown;
}

GPUVendor detect_gpu()
{
    return detect_via_egl();
}

// Maps GPU vendor to preferred FFmpeg HWDeviceType and name
std::vector<std::pair<std::string, AVHWDeviceType>> make_priority_list(GPUVendor vendor)
{
    using Pair = std::pair<std::string, AVHWDeviceType>;

    switch (vendor) {
    case GPUVendor::Nvidia:
        return {
            Pair { "_cuvid", AV_HWDEVICE_TYPE_CUDA },
            Pair { "", AV_HWDEVICE_TYPE_NONE }
        };

    case GPUVendor::AMD:
    case GPUVendor::Intel:
        return {
            Pair { "", AV_HWDEVICE_TYPE_VAAPI },
            Pair { "", AV_HWDEVICE_TYPE_NONE }
        };

    default:
        return {
            Pair { "", AV_HWDEVICE_TYPE_NONE }
        };
    }
}

