// GLContext.cpp
#include "GLContext.h"

#define GL_GLEXT_PROTOTYPES
#include <GL/glew.h>
//
#include <GL/gl.h>

#include <spdlog/spdlog.h>

#include <stdexcept>

static void checkEgl(char const* where)
{
    EGLint err = eglGetError();
    if (err != EGL_SUCCESS) {
        throw std::runtime_error(std::string("EGL error at ") + where + ": 0x" + std::to_string(err));
    }
}

GLContext::GLContext()
{
    // Surfaceless EGL display – required for explicit dma‑buf import on Mesa
    m_display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA,
        EGL_DEFAULT_DISPLAY, nullptr);
    if (m_display == EGL_NO_DISPLAY)
        m_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(m_display, nullptr, nullptr))
        checkEgl("eglInitialize");

    // We need a pbuffer‑capable config that supports an OpenGL 4.x core
    //   profile; without EGL_SURFACE_TYPE=PBUFFER_BIT Mesa may reject the
    //   context creation on surfaceless displays
    EGLint const attr[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_NONE
    };
    EGLConfig cfg;
    EGLint n;
    if (!eglChooseConfig(m_display, attr, &cfg, 1, &n))
        checkEgl("eglChooseConfig");

    eglBindAPI(EGL_OPENGL_API);

    EGLint const ctxAttr[] = {
        EGL_CONTEXT_MAJOR_VERSION, 4,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE
    };

    m_context = eglCreateContext(m_display, cfg, EGL_NO_CONTEXT, ctxAttr);
    if (!m_context)
        checkEgl("eglCreateContext");

    EGLint const pbAttr[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    m_surface = eglCreatePbufferSurface(m_display, cfg, pbAttr);
    if (!m_surface)
        checkEgl("eglCreatePbufferSurface");

    makeCurrent();
}

GLContext::~GLContext()
{
    if (m_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (m_surface)
            eglDestroySurface(m_display, m_surface);
        if (m_context)
            eglDestroyContext(m_display, m_context);
        eglTerminate(m_display);
    }
}

void GLContext::makeCurrent()
{
    if (!eglMakeCurrent(m_display, m_surface, m_surface, m_context))
        checkEgl("eglMakeCurrent");

    // Initialise GLEW **once per thread‑local context**.
    // (GLEW caches function pointers captured from the *current* context.)
    static thread_local bool glewOk = false;
    if (!glewOk) {
        glewExperimental = GL_TRUE; // core profile entry‑points
        GLenum err = glewInit();
        if (err != GLEW_OK)
            throw std::runtime_error(fmt::format(
                "glewInit failed (0x{:x} {})",
                err, reinterpret_cast<char const*>(glewGetErrorString(err))));

        spdlog::info("[GL] GLEW initialised (vendor='{}'  renderer='{}'  version='{}')",
            reinterpret_cast<char const*>(glGetString(GL_VENDOR)),
            reinterpret_cast<char const*>(glGetString(GL_RENDERER)),
            reinterpret_cast<char const*>(glGetString(GL_VERSION)));
        glewOk = true;

        /* Bind a tiny throw‑away texture now that GLEW is initialized */
        GLuint tmpTex;
        glGenTextures(1, &tmpTex);
        glBindTexture(GL_TEXTURE_2D, tmpTex);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, 1, 1);
        glDeleteTextures(1, &tmpTex);
    }

    // Verify required EGL dma‑buf extensions
    static thread_local bool loggedExt = false; // ❶  initialise here
    char const* eglExt = eglQueryString(m_display, EGL_EXTENSIONS);
    if (!loggedExt) {
        spdlog::info("[EGL] extensions: {}", eglExt ? eglExt : "(null)");
        loggedExt = true;
    }
    if (!eglExt || !strstr(eglExt, "EGL_EXT_image_dma_buf_import") || !strstr(eglExt, "EGL_EXT_image_dma_buf_import_modifiers"))
        throw std::runtime_error(
            "EGL_EXT_image_dma_buf_import(_modifiers) missing – driver can't import dma‑bufs");

    // Verify OpenGL can use an EGLImage
    if (!glEGLImageTargetTexture2DOES) {
        // Double‑check via eglGetProcAddress just in case GLEW missed it
        auto* fp = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
            eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        if (!fp)
            throw std::runtime_error(
                "glEGLImageTargetTexture2DOES not available – "
                "driver can't bind EGLImage as texture");
        glEGLImageTargetTexture2DOES = fp;
    }
}

GLuint GLContext::createComputeProgram(std::string const& glsl)
{
    GLuint prog = glCreateProgram();
    GLuint cs = glCreateShader(GL_COMPUTE_SHADER);
    char const* src = glsl.c_str();
    glShaderSource(cs, 1, &src, nullptr);
    glCompileShader(cs);
    GLint ok;
    glGetShaderiv(cs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(cs, 1024, nullptr, log);
        throw std::runtime_error(std::string("Compute shader compile failed: ") + log);
    }
    glAttachShader(prog, cs);
    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(prog, 1024, nullptr, log);
        throw std::runtime_error(std::string("Program link failed: ") + log);
    }
    glDeleteShader(cs);
    return prog;
}

GLuint GLContext::createTextureFromEGLImage(EGLImageKHR image)
{
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    // Configure swizzle parameters for R8 textures
    // This ensures proper RGB mapping when using luma-only textures (PHash)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_RED);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ONE);

    // Import the EGLImage
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);

    // Set filtering parameters
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    return tex;
}

void GLContext::readTextureR8(GLuint tex, uint8_t* dst)
{
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, dst);
}
