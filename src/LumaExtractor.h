#pragma once

//  **Isolate** pixel‑format / EGL import quirks 

#include "GLContext.h"

#include <cstdint>

extern "C" {
#include <libavutil/frame.h>
#include <va/va.h>         
}

namespace lumaext {

inline constexpr int kTileW = 32;
inline constexpr int kTileH = 32;

enum class Err {
    Ok = 0,               // success
    NotInitialised,       // GL/EGL module missing / failed
    UnsupportedFormat,    // frame->format cannot be processed
    EGLCreateFailed,
    GLImportFailed,
    FboIncomplete,
};

// Import `frame`'s luma into GL, down‑sample to 32x32 and copy to `dst`.
//
// \param glCtx   – thread‑local GLContext wrapper from your engine.
// \param va      – VADisplay that produced the frame (for vaExportSurfaceHandle).
// \param frame   – AVFrame coming from a VA‑API decoder. (`frame->format` may be
//                  *any* HW format: NV12, P010, P016, AYUV, RGBA, …)
// \param dst     – pointer to at least 32 × 32 bytes.
// \return        – `true` on success; `false` if an *expected* runtime condition
//                  failed (unsupported fourcc, allocation error, …).  Details
//                  can be queried via `last_error()`.
//
// each thread owns its GL context 
// shared static shaders are created once per process.

bool extract_luma_32x32(GLContext& glCtx, VADisplay va, AVFrame const* frame, std::uint8_t* dst) noexcept;

// Retrieve error from the most recent call in this thread
Err last_error() noexcept;

const char* to_string(Err) noexcept;

namespace detail {
    struct EglImage;
    struct Texture2D;
} // namespace detail

} // namespace lumaext

