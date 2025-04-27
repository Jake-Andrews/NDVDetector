#include "GPUVendor.h"

#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <cstring>

extern "C" {
#include <libavutil/hwcontext.h>
}

static GPUVendor detect_via_opengl()
{
    if (!QGuiApplication::instance())
        return GPUVendor::Unknown;
    QOpenGLContext ctx;
    if (!ctx.create())
        return GPUVendor::Unknown;
    QOffscreenSurface surf;
    surf.setFormat(ctx.format());
    surf.create();
    if (!ctx.makeCurrent(&surf))
        return GPUVendor::Unknown;
    char const* vend = reinterpret_cast<char const*>(ctx.functions()->glGetString(GL_VENDOR));
    ctx.doneCurrent();
    if (!vend)
        return GPUVendor::Unknown;
    if (strstr(vend, "NVIDIA"))
        return GPUVendor::Nvidia;
    if (strstr(vend, "AMD") || strstr(vend, "ATI"))
        return GPUVendor::AMD;
    if (strstr(vend, "Intel"))
        return GPUVendor::Intel;
    if (strstr(vend, "Apple"))
        return GPUVendor::Apple;
    return GPUVendor::Unknown;
}

GPUVendor detect_gpu() { return detect_via_opengl(); }

std::vector<std::pair<std::string, AVHWDeviceType>> make_priority_list(GPUVendor v)
{
    using Pair = std::pair<std::string, AVHWDeviceType>;
    switch (v) {
    case GPUVendor::Nvidia:
        return { Pair { "_cuvid", AV_HWDEVICE_TYPE_CUDA }, Pair { "", AV_HWDEVICE_TYPE_NONE } };
    case GPUVendor::AMD:
        return { Pair { "", AV_HWDEVICE_TYPE_VAAPI }, Pair { "", AV_HWDEVICE_TYPE_NONE } };
    case GPUVendor::Intel:
        return { Pair { "_qsv", AV_HWDEVICE_TYPE_QSV }, Pair { "", AV_HWDEVICE_TYPE_VAAPI }, Pair { "", AV_HWDEVICE_TYPE_NONE } };
    default:
        return { Pair { "", AV_HWDEVICE_TYPE_NONE } };
    }
}

