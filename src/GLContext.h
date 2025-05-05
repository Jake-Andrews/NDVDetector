#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>

#define GL_GLEXT_PROTOTYPES
#include <GL/glew.h>
//
#include <GL/gl.h>

#include <string>

class GLContext {
public:
    GLContext();
    ~GLContext();

    [[nodiscard]] EGLDisplay display() const noexcept { return m_display; }

    /// Make this context current on the calling thread
    void makeCurrent();

    /// Compile and link a compute shader from GLSL source
    GLuint createComputeProgram(const std::string &glsl);

    /// Wrap an existing EGLImage (imported from dma‑buf) as a GL texture
    GLuint createTextureFromEGLImage(EGLImageKHR image);

    /// Read back a 32×32 R8 texture into host memory (caller allocates 1024 B)
    void readTextureR8(GLuint tex, uint8_t *dst);

private:
    EGLDisplay m_display {EGL_NO_DISPLAY};
    EGLContext m_context {EGL_NO_CONTEXT};
    EGLSurface m_surface {EGL_NO_SURFACE}; // pbuffer – tiny 1×1, no framebuffer output
};
