//  Import an AVFrame produced by a VA‑API decoder into OpenGL, extract its
//  luma, down‑sample to 32 × 32 and copy the result into users memory
//
//  Handles (every)? VA‑API DRM_PRIME format that Mesa/Intel exposes:
//      ‑ NV12 / P010 / P016 / YUY2 … → imports Y‑plane only.
//      ‑ ARGB / ABGR / XRGB …        → imports full RGB and converts in GLSL.
//
//  Works on 8‑/10‑/16‑bit inputs
//  Zero exceptions, caller checks the boolean return
//  and optionally inspects the thread‑local last_error()
//
// LumaExtractor.cpp — VAAPI → DRM_PRIME → EGL → OpenGL luma‑extraction pipeline

#include "LumaExtractor.h"
#include "GLContext.h"

#include <GL/glew.h>

#include <cstdint>
#include <errno.h>
#include <functional>
#include <mutex>
#include <spdlog/spdlog.h>
#include <string.h>
#include <unistd.h>
#include <utility>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <va/va.h>
#include <va/va_drmcommon.h>
}

#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>

namespace lumaext {

char const* to_string(Err e) noexcept
{
    switch (e) {
    case Err::Ok:
        return "Ok";
    case Err::NotInitialised:
        return "NotInitialised";
    case Err::UnsupportedFormat:
        return "UnsupportedFormat";
    case Err::EGLCreateFailed:
        return "EGLCreateFailed";
    case Err::GLImportFailed:
        return "GLImportFailed";
    case Err::FboIncomplete:
        return "FboIncomplete";
    default:
        return "Unknown";
    }
}

namespace {
thread_local Err g_last_err = Err::Ok;
}

inline void _set_err(Err e) noexcept { g_last_err = e; }
Err last_error() noexcept { return g_last_err; }

namespace _detail {

constexpr int kTileW = 32;
constexpr int kTileH = 32;

// RAII helper for original VA DRM_PRIME FDs
struct OriginalVADRMPRIMEfdsCloser {
    VADRMPRIMESurfaceDescriptor& desc_ref;
    bool enabled { true };

    explicit OriginalVADRMPRIMEfdsCloser(VADRMPRIMESurfaceDescriptor& d) noexcept
        : desc_ref(d)
    {
    }

    OriginalVADRMPRIMEfdsCloser(OriginalVADRMPRIMEfdsCloser const&) = delete;
    OriginalVADRMPRIMEfdsCloser& operator=(OriginalVADRMPRIMEfdsCloser const&) = delete;
    OriginalVADRMPRIMEfdsCloser(OriginalVADRMPRIMEfdsCloser&&) = delete;
    OriginalVADRMPRIMEfdsCloser& operator=(OriginalVADRMPRIMEfdsCloser&&) = delete;

    ~OriginalVADRMPRIMEfdsCloser()
    {
        if (!enabled)
            return;

        for (uint32_t i = 0; i < desc_ref.num_objects; ++i) {
            int& fd = desc_ref.objects[i].fd;
            if (fd >= 0) {
                if (::close(fd) != 0) {
                    spdlog::warn("[lumaext] OriginalVADRMPRIMEfdsCloser: failed to close fd {}: {}", fd, strerror(errno));
                }
                fd = -1; // mark as closed to avoid accidental reuse
            }
        }
    }

    void release() noexcept { enabled = false; }
};

struct Texture final {
    GLuint id { 0 };
    explicit Texture(GLenum target)
    {
        glGenTextures(1, &id);
        if (id)
            glBindTexture(target, id);
        else
            spdlog::error("[Texture] glGenTextures failed");
    }
    ~Texture()
    {
        if (id)
            glDeleteTextures(1, &id);
    }
    Texture(Texture&& o) noexcept
        : id(std::exchange(o.id, 0))
    {
    }
    Texture& operator=(Texture&& o) noexcept
    {
        std::swap(id, o.id);
        return *this;
    }
    Texture(Texture const&) = delete;
    Texture& operator=(Texture const&) = delete;
};

struct Framebuffer final {
    GLuint id { 0 };
    Framebuffer() { glGenFramebuffers(1, &id); }
    ~Framebuffer()
    {
        if (id)
            glDeleteFramebuffers(1, &id);
    }
    Framebuffer(Framebuffer&& o) noexcept
        : id(std::exchange(o.id, 0))
    {
    }
    Framebuffer& operator=(Framebuffer&& o) noexcept
    {
        std::swap(id, o.id);
        return *this;
    }
    Framebuffer(Framebuffer const&) = delete;
    Framebuffer& operator=(Framebuffer const&) = delete;
};

struct EglImage final {
    EGLDisplay dpy { EGL_NO_DISPLAY };
    EGLImage img { EGL_NO_IMAGE_KHR };
    int owned_fd { -1 };

    EglImage() = default;
    EglImage(EGLDisplay d, EGLImage i, int fd = -1)
        : dpy(d)
        , img(i)
        , owned_fd(fd)
    {
    }

    ~EglImage()
    {
        if (img != EGL_NO_IMAGE_KHR && dpy != EGL_NO_DISPLAY) {
            auto eglDestroyImageKHR = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
            if (eglDestroyImageKHR)
                eglDestroyImageKHR(dpy, img);
            else
                spdlog::error("[EglImage] eglDestroyImageKHR symbol not found");
        }
        if (owned_fd >= 0) {
            if (::close(owned_fd) != 0)
                spdlog::error("[EglImage] close({}) failed: {}", owned_fd, strerror(errno));
        }
    }

    EglImage(EglImage&& o) noexcept
        : dpy(o.dpy)
        , img(std::exchange(o.img, EGL_NO_IMAGE_KHR))
        , owned_fd(std::exchange(o.owned_fd, -1))
    {
        o.dpy = EGL_NO_DISPLAY;
    }
    EglImage& operator=(EglImage&& o) noexcept
    {
        if (this == &o)
            return *this;
        // destroy current resources first
        this->~EglImage();
        dpy = o.dpy;
        img = std::exchange(o.img, EGL_NO_IMAGE_KHR);
        owned_fd = std::exchange(o.owned_fd, -1);
        o.dpy = EGL_NO_DISPLAY;
        return *this;
    }

    EglImage(EglImage const&) = delete;
    EglImage& operator=(EglImage const&) = delete;
};

inline GLuint compile(GLenum type, char const* src)
{
    GLuint s = glCreateShader(type);
    if (!s) {
        spdlog::error("[compile] glCreateShader failed");
        return 0;
    }
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {};
        GLsizei len = 0;
        glGetShaderInfoLog(s, sizeof log, &len, log);
        spdlog::error("[compile] shader compilation failed: {}", len ? log : "unknown error");
        glDeleteShader(s);
        return 0;
    }
    return s;
}

// singleton GL assets
class GLModule {
    GLuint prog { 0 };
    GLint rgbLoc { -1 }, texLoc { -1 }, texelSizeLoc { -1 };
    GLuint vao { 0 };

    bool initialized { false };
    std::mutex initMutex;
    std::unique_ptr<GLuint, std::function<void(GLuint*)>> rTex_id;
    std::unique_ptr<GLuint, std::function<void(GLuint*)>> fbo_id;

    GLModule()
        : rTex_id(new GLuint(0), [](GLuint* id) { if (*id) glDeleteTextures(1, id); delete id; })
        , fbo_id(new GLuint(0), [](GLuint* id) { if (*id) glDeleteFramebuffers(1, id); delete id; })
    {
    }

public:
    ~GLModule()
    {
        if (vao)
            glDeleteVertexArrays(1, &vao);
        if (prog)
            glDeleteProgram(prog);

        if (rTex_id)
            rTex_id.reset();
        if (fbo_id)
            fbo_id.reset();
    }

    static GLModule& instance()
    {
        static thread_local GLModule tl;
        return tl;
    }

    bool ensureInitialized();
    bool bindAndSetup(bool rgbLike, int sourceWidth, int sourceHeight);
    GLuint getOutputTexture() const { return rTex_id ? *rTex_id : 0; }
    GLuint getFramebuffer() const { return fbo_id ? *fbo_id : 0; }
};

bool GLModule::ensureInitialized()
{
    std::lock_guard<std::mutex> lk(initMutex);
    if (initialized)
        return true;

    // shader sources
    static constexpr char vsSrc[] = R"GLSL(#version 330 core
const vec2 p[4]=vec2[](vec2(-1,1),vec2(1,1),vec2(-1,-1),vec2(1,-1));
const vec2 t[4]=vec2[](vec2(0,0),vec2(1,0),vec2(0,1),vec2(1,1));
out vec2 v; void main(){ gl_Position=vec4(p[gl_VertexID],0,1); v=t[gl_VertexID]; })GLSL";

    static constexpr char fsSrc[] = R"GLSL(#version 330 core
in vec2 v; out vec4 C;
uniform sampler2D tex0; uniform bool rgbMode; uniform vec2 texelSize;
float applyMeanFilter(sampler2D tex, vec2 tc, vec2 px, bool rgb){
    float sum=0.0; const int f=7, h=f/2; for(int y=-h;y<=h;++y) for(int x=-h;x<=h;++x){
        vec2 off=vec2(x,y)*px; vec4 t=texture(tex,tc+off);
        float l=rgb?dot(t.rgb,vec3(0.2126,0.7152,0.0722)):t.r; sum+=l; }
    return sum/float(f*f);
}
void main(){ float y=applyMeanFilter(tex0,v,texelSize,rgbMode); C=vec4(y,y,y,1.0);} )GLSL";

    prog = glCreateProgram();
    if (!prog) {
        spdlog::error("[GLModule] glCreateProgram failed");
        return false;
    }
    GLuint vs = compile(GL_VERTEX_SHADER, vsSrc);
    GLuint fs = compile(GL_FRAGMENT_SHADER, fsSrc);
    if (!vs || !fs) {
        glDeleteProgram(prog);
        prog = 0;
        return false;
    }
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512] = {};
        GLsizei len = 0;
        glGetProgramInfoLog(prog, 512, &len, log);
        spdlog::error("[GLModule] link failed: {}", log);
        glDeleteProgram(prog);
        prog = 0;
        return false;
    }

    rgbLoc = glGetUniformLocation(prog, "rgbMode");
    texLoc = glGetUniformLocation(prog, "tex0");
    texelSizeLoc = glGetUniformLocation(prog, "texelSize");
    if (texLoc >= 0) {
        glUseProgram(prog);
        glUniform1i(texLoc, 0);
    }

    // output R8 texture
    glGenTextures(1, rTex_id.get());
    glBindTexture(GL_TEXTURE_2D, *rTex_id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, kTileW, kTileH, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // framebuffer
    glGenFramebuffers(1, fbo_id.get());
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo_id);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *rTex_id, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        spdlog::error("[GLModule] FBO incomplete");
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    glGenVertexArrays(1, &vao);
    initialized = true;
    return true;
}

bool GLModule::bindAndSetup(bool rgbLike, int w, int h)
{
    if (!initialized)
        return false;
    glUseProgram(prog);
    if (rgbLoc >= 0)
        glUniform1i(rgbLoc, rgbLike ? 1 : 0);
    if (texelSizeLoc >= 0)
        glUniform2f(texelSizeLoc, 1.f / float(w), 1.f / float(h));
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo_id);
    glViewport(0, 0, kTileW, kTileH);
    glBindVertexArray(vao);
    return true;
}

} // namespace _detail

// public entry point

bool extract_luma_32x32(GLContext& glCtx, VADisplay va, AVFrame const* frame, std::uint8_t* dst) noexcept
{
    using namespace _detail;
    _set_err(Err::Ok);

    try {
        glCtx.makeCurrent();
        if (!frame || !dst) {
            _set_err(Err::NotInitialised);
            return false;
        }

        AVPixFmtDescriptor const* pd = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
        if (!pd) {
            _set_err(Err::UnsupportedFormat);
            return false;
        }
        bool const rgbLike = pd->flags & AV_PIX_FMT_FLAG_RGB;

        // ---- export VA surface ----
        auto sid = static_cast<VASurfaceID>(reinterpret_cast<uintptr_t>(frame->data[3]));
        if (vaSyncSurface(va, sid) != VA_STATUS_SUCCESS) {
            _set_err(Err::UnsupportedFormat);
            return false;
        }
        VADRMPRIMESurfaceDescriptor desc {};
        if (vaExportSurfaceHandle(va, sid, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, VA_EXPORT_SURFACE_SEPARATE_LAYERS, &desc) != VA_STATUS_SUCCESS) {
            _set_err(Err::UnsupportedFormat);
            return false;
        }
        OriginalVADRMPRIMEfdsCloser fdGuard(desc);
        if (!desc.num_layers || !desc.layers[0].num_planes)
            return false;
        int const plane = 0;
        int const obj = desc.layers[0].object_index[plane];
        if (obj < 0 || obj >= static_cast<int>(desc.num_objects))
            return false;

        uint64_t const badMod = (1ull << 56) - 1;
        uint64_t const modifier = desc.objects[obj].drm_format_modifier;
        bool const haveMod = (modifier && modifier != badMod);

        // ---- build EGL attribute list ----
        EGLint attr[19];
        int ai = 0;
        auto push = [&](EGLint k, EGLint v) { attr[ai++] = k; attr[ai++] = v; };
        push(EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(desc.layers[0].drm_format));
        push(EGL_WIDTH, desc.width);
        push(EGL_HEIGHT, desc.height);
        int dupFd = ::dup(desc.objects[obj].fd);
        if (dupFd < 0) {
            _set_err(Err::EGLCreateFailed);
            return false;
        }
        push(EGL_DMA_BUF_PLANE0_FD_EXT, dupFd);
        push(EGL_DMA_BUF_PLANE0_OFFSET_EXT, static_cast<EGLint>(desc.layers[0].offset[plane]));
        push(EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(desc.layers[0].pitch[plane]));
        if (haveMod) {
            push(EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, static_cast<EGLint>(modifier & 0xffffffffu));
            push(EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, static_cast<EGLint>(modifier >> 32));
        }
        attr[ai] = EGL_NONE;

        // ---- create EGL image ----
        auto eglCreateImageKHR = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
        auto glEGLImageTargetTexture2DOES = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        if (!eglCreateImageKHR || !glEGLImageTargetTexture2DOES) {
            _set_err(Err::NotInitialised);
            ::close(dupFd);
            return false;
        }

        EglImage img(glCtx.display(), eglCreateImageKHR(glCtx.display(), EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attr), dupFd);
        if (img.img == EGL_NO_IMAGE_KHR) {
            _set_err(Err::EGLCreateFailed);
            return false;
        }

        // ---- GL setup ----
        GLuint srcTex = 0;
        glGenTextures(1, &srcTex);
        glBindTexture(GL_TEXTURE_2D, srcTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img.img);
        if (glGetError() != GL_NO_ERROR) {
            glDeleteTextures(1, &srcTex);
            _set_err(Err::GLImportFailed);
            return false;
        }

        auto& mod = GLModule::instance();
        if (!mod.ensureInitialized() || !mod.bindAndSetup(rgbLike, desc.width, desc.height)) {
            glDeleteTextures(1, &srcTex);
            _set_err(Err::NotInitialised);
            return false;
        }

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, srcTex);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        if (glGetError() != GL_NO_ERROR) {
            glDeleteTextures(1, &srcTex);
            _set_err(Err::GLImportFailed);
            return false;
        }

        glFinish();
        glReadPixels(0, 0, _detail::kTileW, _detail::kTileH, GL_RED, GL_UNSIGNED_BYTE, dst);
        if (glGetError() != GL_NO_ERROR) {
            glDeleteTextures(1, &srcTex);
            _set_err(Err::GLImportFailed);
            return false;
        }

        glDeleteTextures(1, &srcTex);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return true;

    } catch (...) {
        _set_err(Err::GLImportFailed);
        return false;
    }
}

} // namespace lumaext
