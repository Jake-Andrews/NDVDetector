#pragma once

#include "GLContext.h"

#include <EGL/egl.h>
#include <cstdint>

namespace glpipe {

class Mean32PipelineGL {
public:
    explicit Mean32PipelineGL(GLContext& ctx);
    ~Mean32PipelineGL();

    /// Run filter from source EGLImage (luma plane). Writes result into dst[1024].
    void process(EGLImageKHR srcImage, uint8_t* dstOut);

private:
    GLContext& m_ctx;
    GLuint m_prog { 0 };
    GLuint m_dstTex { 0 };
};

} // namespace glpipe

