#include "Mean32PipelineGL.h"
#include <spdlog/spdlog.h>

#define GL_GLEXT_PROTOTYPES
#include <GL/glew.h>

namespace glpipe {

// Compute shader that downsamples a single‑channel (R8 / R16) luma image to a
// 32×32 average grid.  The input is bound with image format r8 so imageLoad().r
// already yields the normalised luma value (0–1).
static char const* kMean32CS = R"GLSL(
#version 430
layout(local_size_x = 8, local_size_y = 8) in;

layout(binding = 0, r8)  readonly  uniform image2D srcImg; // luma‑only
layout(binding = 1, r8)  writeonly uniform image2D dstImg; // 32×32 output

void main() {
    ivec2 gid = ivec2(gl_GlobalInvocationID.xy);
    if (gid.x >= 32 || gid.y >= 32) return;

    ivec2 srcSize = imageSize(srcImg);
    ivec2 srcPos  = (gid * srcSize) / 32;

    float sum = 0.0;
    for (int dy = -3; dy <= 3; ++dy) {
        for (int dx = -3; dx <= 3; ++dx) {
            ivec2 p = clamp(srcPos + ivec2(dx, dy), ivec2(0), srcSize - 1);
            sum += imageLoad(srcImg, p).r;
        }
    }
    float avg = sum / 49.0;
    imageStore(dstImg, gid, vec4(avg, 0.0, 0.0, 1.0));
}
)GLSL";

Mean32PipelineGL::Mean32PipelineGL(GLContext& ctx)
    : m_ctx(ctx)
{
    m_prog = ctx.createComputeProgram(kMean32CS);
    glGenTextures(1, &m_dstTex);
    glBindTexture(GL_TEXTURE_2D, m_dstTex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, 32, 32);
}

Mean32PipelineGL::~Mean32PipelineGL()
{
    glDeleteTextures(1, &m_dstTex);
    glDeleteProgram(m_prog);
}

void Mean32PipelineGL::process(EGLImageKHR srcImage, uint8_t* dstOut)
{
    // Ensure GL context is current
    m_ctx.makeCurrent();

    // Create texture from EGLImage with proper swizzling
    GLuint srcTex = m_ctx.createTextureFromEGLImage(srcImage);

    // Clear any existing GL errors before processing
    GLenum err;
    while ((err = glGetError()) != GL_NO_ERROR) {
        // spdlog::debug("[Mean32] Cleared pre-existing GL error: {}",
        // GLContext::getGLErrorString(err));
    }

    // Bind source texture (R8 format for luma-only processing)
    glBindImageTexture(0, srcTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8);

    // Bind destination texture
    glBindImageTexture(1, m_dstTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);

    // Use compute shader program
    glUseProgram(m_prog);

    // Dispatch compute shader (4×4 workgroups to process 32×32 output)
    glDispatchCompute(4, 4, 1);

    // Ensure compute shader writes are visible
    glMemoryBarrier(GL_ALL_BARRIER_BITS);

    // Check for errors after dispatch
    err = glGetError();
    if (err != GL_NO_ERROR) {
        // spdlog::warn("[Mean32] GL error after dispatch: {}",
        // GLContext::getGLErrorString(err));
    }

    // Read back the results to CPU memory
    m_ctx.readTextureR8(m_dstTex, dstOut);

    // Clean up the source texture
    glDeleteTextures(1, &srcTex);

    // Final error check
    err = glGetError();
    if (err != GL_NO_ERROR) {
        // spdlog::warn("[Mean32] GL error at end of process: {}",
        // GLContext::getGLErrorString(err));
    }
}

} // namespace glpipe
