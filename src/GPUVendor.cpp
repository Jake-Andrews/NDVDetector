#include "GPUVendor.h"
#include <cstring>

#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>

extern "C" {
#include <libavutil/hwcontext.h>
}

static GPUVendor detect_via_opengl()
{
    if (!QGuiApplication::instance()) {
        // detect_gpu() called before QApplication was constructed
        return GPUVendor::Unknown;
    }

    // 2) Create a throw‑away OpenGL context and surface
    QOpenGLContext ctx;
    if (!ctx.create())
        return GPUVendor::Unknown;

    QOffscreenSurface surf;
    surf.setFormat(ctx.format());
    surf.create();

    if (!ctx.makeCurrent(&surf))
        return GPUVendor::Unknown;

    // 3) Query vendor string
    char const* vendorStr = reinterpret_cast<char const*>(ctx.functions()->glGetString(GL_VENDOR));

    ctx.doneCurrent();

    if (!vendorStr)
        return GPUVendor::Unknown;

    if (strstr(vendorStr, "NVIDIA"))
        return GPUVendor::Nvidia;
    if (strstr(vendorStr, "AMD") || strstr(vendorStr, "ATI"))
        return GPUVendor::AMD;
    if (strstr(vendorStr, "Intel"))
        return GPUVendor::Intel;
    if (strstr(vendorStr, "Apple"))
        return GPUVendor::Apple;

    return GPUVendor::Unknown;
}

GPUVendor detect_gpu()
{
    return detect_via_opengl();
}

std::vector<std::pair<std::string, AVHWDeviceType>>
make_priority_list(GPUVendor v)
{
    using Pair = std::pair<std::string, AVHWDeviceType>;
    switch (v) {
    case GPUVendor::Nvidia:
        return { Pair { "_cuvid", AV_HWDEVICE_TYPE_CUDA },
            Pair { "", AV_HWDEVICE_TYPE_NONE } };
    case GPUVendor::AMD:
        return { Pair { "", AV_HWDEVICE_TYPE_VAAPI },
            Pair { "", AV_HWDEVICE_TYPE_NONE } };
    case GPUVendor::Intel:
        return { Pair { "_qsv", AV_HWDEVICE_TYPE_QSV },
            Pair { "", AV_HWDEVICE_TYPE_VAAPI },
            Pair { "", AV_HWDEVICE_TYPE_NONE } };
    default:
        return { Pair { "", AV_HWDEVICE_TYPE_NONE } };
    }
}
