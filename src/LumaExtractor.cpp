// SPDX‑License‑Identifier: MIT
// luma_extractor.cpp – format‑agnostic, VA‑API‑accelerated luma scaler
// ---------------------------------------------------------------
// • Accepts *any* VA‑API decoded pixel format (NV12, P010, RGB32, etc.)
// • If the format already has an explicit luma plane (YUV / grayscale)
//      ‑ imports only the Y plane via dma‑buf → EGLImage → GL_R* texture
// • If the format is RGB* (no luma plane)
//      ‑ imports full RGB texture and shader converts to luma on‑the‑fly
// • Always renders to a 128×128 GL_R8 FBO and writes frame_XX.png
// • First 10 frames only, for hashing workflows
// ---------------------------------------------------------------
// Build (pkg‑config):
//    c++ -std=c++17 -Wall -O2 luma_extractor.cpp -o extract \
//        $(pkg-config --cflags --libs libavformat libavcodec libavutil \
//                     egl libva libva-drm x11 gl) -ldl
// ---------------------------------------------------------------

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <va/va_drmcommon.h>
#include <vector>

#include <X11/Xlib.h>

#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/pixdesc.h>
#include <va/va.h>
#include <va/va_drm.h>
}

#include <drm_fourcc.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static void die(const char* m)
{
    std::fprintf(stderr, "fatal: %s\n", m);
    std::exit(1);
}

/* minimal off‑screen GL context (hidden 1×1 X11 window) */
struct GLContext {
    Display* d {};
    Window w {};
    EGLDisplay ed {};
    EGLContext ctx {};
    GLContext()
    {
        d = XOpenDisplay(nullptr);
        if (!d)
            die("XOpenDisplay");
        w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 1, 1, 0, 0, 0);
        ed = eglGetDisplay((EGLNativeDisplayType)d);
        if (ed == EGL_NO_DISPLAY)
            die("eglGetDisplay");
        if (!eglInitialize(ed, nullptr, nullptr))
            die("eglInit");
        if (!eglBindAPI(EGL_OPENGL_API))
            die("eglBindAPI");
        EGLint ca[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8, EGL_NONE };
        EGLConfig cfg;
        EGLint n;
        eglChooseConfig(ed, ca, &cfg, 1, &n);
        EGLint ctxattr[] = { EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE };
        ctx = eglCreateContext(ed, cfg, EGL_NO_CONTEXT, ctxattr);
        if (ctx == EGL_NO_CONTEXT)
            die("ctx");
        eglMakeCurrent(ed, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
    }
    ~GLContext()
    {
        eglMakeCurrent(ed, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(ed, ctx);
        eglTerminate(ed);
        XDestroyWindow(d, w);
        XCloseDisplay(d);
    }
};

static GLuint compile(GLenum t, char const* src)
{
    GLuint s = glCreateShader(t);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
        die("shader");
    return s;
}
static GLuint make_prog()
{
    char const* vs = "#version 330 core\nconst vec2 p[4]=vec2[](vec2(-1,1),vec2(1,1),vec2(-1,-1),vec2(1,-1));const vec2 tc[4]=vec2[](vec2(0,0),vec2(1,0),vec2(0,1),vec2(1,1));out vec2 v;void main(){gl_Position=vec4(p[gl_VertexID],0,1);v=tc[gl_VertexID];}";
    // FS chooses path by uniform flag rgbMode
    char const* fs = "#version 330 core\nin vec2 v;out vec4 C;uniform sampler2D tex0;uniform bool rgbMode;\nvoid main(){float y;if(rgbMode){vec3 rgb=texture(tex0,v).rgb;y=dot(rgb,vec3(0.2126,0.7152,0.0722));}else{y=texture(tex0,v).r;}C=vec4(y,y,y,1);}";
    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok)
        die("link");
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "tex0"), 0);
    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    return p;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <video> [renderD]", argv[0]);
        return 2;
    }
    char const* devnode = (argc > 2) ? argv[2] : "/dev/dri/renderD128";

    // -------- ffmpeg & VA‑API init ----------
    AVFormatContext* ic = nullptr;
    if (avformat_open_input(&ic, argv[1], nullptr, nullptr) < 0)
        die("open");
    if (avformat_find_stream_info(ic, nullptr) < 0)
        die("info");
    int vs = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vs < 0)
        die("vs");
    AVCodec const* dec = avcodec_find_decoder(ic->streams[vs]->codecpar->codec_id);
    AVCodecContext* ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(ctx, ic->streams[vs]->codecpar);
    int drmfd = open(devnode, O_RDWR);
    if (drmfd < 0)
        die("drm");
    VADisplay vdisp = vaGetDisplayDRM(drmfd);
    int mj, mn;
    if (vaInitialize(vdisp, &mj, &mn))
        die("va");
    AVBufferRef* hwdev = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VAAPI);
    reinterpret_cast<AVHWDeviceContext*>(hwdev->data)->hwctx->display = vdisp;
    if (av_hwdevice_ctx_init(hwdev) < 0)
        die("hwdev");
    ctx->hw_device_ctx = av_buffer_ref(hwdev);
    ctx->get_format = [](AVCodecContext*, const enum AVPixelFormat* f) {while(*f){if(*f==AV_PIX_FMT_VAAPI)return AV_PIX_FMT_VAAPI;++f;}return AV_PIX_FMT_NONE; };
    if (avcodec_open2(ctx, dec, nullptr) < 0)
        die("open2");

    // ------------ GL ---------------
    GLContext gl;
    make_prog();
    GLint rgbLoc = glGetUniformLocation(0, "rgbMode");
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GLuint fbo, outTex;
    glGenTextures(1, &outTex);
    glBindTexture(GL_TEXTURE_2D, outTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 128, 128, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outTex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        die("fbo");

    // --- EGL helpers ---
    auto eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    auto eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    auto glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    AVPacket pkt;
    AVFrame* frm = av_frame_alloc();
    int n = 0;
    while (n < 10 && av_read_frame(ic, &pkt) >= 0) {
        if (pkt.stream_index != vs) {
            av_packet_unref(&pkt);
            continue;
        }
        avcodec_send_packet(ctx, &pkt);
        av_packet_unref(&pkt);
        if (avcodec_receive_frame(ctx, frm) != 0)
            continue;
        vaSyncSurface(vdisp, (VASurfaceID)(uintptr_t)frm->data[3]);

        AVPixFmtDescriptor const* desc = av_pix_fmt_desc_get((AVPixelFormat)frm->format);
        bool rgb = (desc->flags & AV_PIX_FMT_FLAG_RGB);

        // --- dma‑buf import ---
        VASurfaceID s = (VASurfaceID)(uintptr_t)frm->data[3];
        VADRMPRIMESurfaceDescriptor d {};
        if (vaExportSurfaceHandle(vdisp, s, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, VA_EXPORT_SURFACE_SEPARATE_LAYERS, &d))
            die("export");
        EGLImage img;
        uint32_t fourcc = rgb ? d.layers[0].drm_format : d.layers[0].drm_format;
        EGLint attr[] = { EGL_LINUX_DRM_FOURCC_EXT, (EGLint)fourcc, EGL_WIDTH, (EGLint)d.width, EGL_HEIGHT, (EGLint)d.height, EGL_DMA_BUF_PLANE0_FD_EXT, d.objects[d.layers[0].object_index[0]].fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)d.layers[0].offset[0], EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)d.layers[0].pitch[0], EGL_NONE };
        img = eglCreateImageKHR(gl.edpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attr);
        glBindTexture(GL_TEXTURE_2D, tex);
        glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img);
        glUniform1i(rgbLoc, rgb);

        // render
        glViewport(0, 0, 128, 128);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        std::vector<uint8_t> buf(128 * 128);
        glReadPixels(0, 0, 128, 128, GL_RED, GL_UNSIGNED_BYTE, buf.data());
        char name[32];
        std::snprintf(name, sizeof(name), "gray_%02d.png", n);
        stbi_write_png(name, 128, 128, 1, buf.data(), 128);
        std::printf("saved %s (rgbMode=%d)\n", name, rgb);

        eglDestroyImageKHR(gl.edpy, img);
        ::close(d.objects[d.layers[0].object_index[0]].fd);
        av_frame_unref(frm);
        ++n;
    }
    std::puts("done");
}
