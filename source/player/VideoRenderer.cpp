#include "player/VideoRenderer.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "util/Log.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace player {

namespace {

constexpr std::array<DkVtxAttribState, 2> VideoAttribs = { {
    { 0, 0, offsetof(VideoRenderer::Vertex, pos), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0 },
    { 0, 0, offsetof(VideoRenderer::Vertex, uv), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0 },
} };

constexpr std::array<DkVtxBufferState, 1> VideoBufState = { {
    { sizeof(VideoRenderer::Vertex), 0 },
} };

struct VideoParams {
    float config[4];   // x: 1 when chroma is one interleaved plane (NV12)
    float row0[4];
    float row1[4];
    float row2[4];
};

// Y'CbCr -> R'G'B' as a 3x4 matrix over (Y, U, V, 1), with the 16/235 and
// 128 offsets folded into the constant column.
void fillMatrix(VideoParams& params, bool bt709, bool fullRange)
{
    const float kr = bt709 ? 0.2126f : 0.299f;
    const float kb = bt709 ? 0.0722f : 0.114f;
    const float kg = 1.0f - kr - kb;

    const float yScale = fullRange ? 1.0f : 255.0f / 219.0f;
    const float cScale = fullRange ? 1.0f : 255.0f / 224.0f;
    const float yOffset = fullRange ? 0.0f : 16.0f / 255.0f;

    const float vr = cScale * 2.0f * (1.0f - kr);
    const float ub = cScale * 2.0f * (1.0f - kb);
    const float ug = -cScale * 2.0f * (1.0f - kb) * kb / kg;
    const float vg = -cScale * 2.0f * (1.0f - kr) * kr / kg;

    params.row0[0] = yScale;  params.row0[1] = 0.0f; params.row0[2] = vr;
    params.row1[0] = yScale;  params.row1[1] = ug;   params.row1[2] = vg;
    params.row2[0] = yScale;  params.row2[1] = ub;   params.row2[2] = 0.0f;

    const float half = 0.5f;
    params.row0[3] = -(yScale * yOffset + vr * half);
    params.row1[3] = -(yScale * yOffset + ug * half + vg * half);
    params.row2[3] = -(yScale * yOffset + ub * half);
}

} // namespace

bool VideoRenderer::init(gfx::Renderer& renderer)
{
    if (!m_vsh.load(renderer.codePool(), "romfs:/shaders/video_vsh.dksh")) {
        FLIKS_LOG("video: missing romfs:/shaders/video_vsh.dksh");
        return false;
    }
    if (!m_fsh.load(renderer.codePool(), "romfs:/shaders/video_fsh.dksh")) {
        FLIKS_LOG("video: missing romfs:/shaders/video_fsh.dksh");
        return false;
    }

    m_vertexBuffer = renderer.dataPool().allocate(4 * sizeof(Vertex), alignof(Vertex));
    m_paramsUbo = renderer.dataPool().allocate(sizeof(VideoParams), DK_UNIFORM_BUF_ALIGNMENT);
    m_ready = m_vertexBuffer && m_paramsUbo;
    return m_ready;
}

void VideoRenderer::shutdown(gfx::Renderer& renderer)
{
    releasePlanes(renderer);
    m_vertexBuffer.destroy();
    m_paramsUbo.destroy();
    m_ready = false;
}

void VideoRenderer::clear(gfx::Renderer& renderer)
{
    releasePlanes(renderer);
    m_colorspace = -1;
    m_pixFmt = -1;
    m_range = -1;
}

void VideoRenderer::releasePlanes(gfx::Renderer& renderer)
{
    // m_planes[2] may alias m_planes[1] in NV12; destroying it twice would
    // hand the same slot back to the pool twice.
    if (m_nv12) m_planes[2] = gfx::TexInvalid;
    for (gfx::TexId& plane : m_planes) {
        if (plane != gfx::TexInvalid) renderer.destroyTexture(plane);
        plane = gfx::TexInvalid;
    }
    m_width = m_height = 0;
    m_nv12 = false;
}

void VideoRenderer::allocatePlanes(gfx::Renderer& renderer, int width, int height, bool nv12)
{
    releasePlanes(renderer);
    const uint32_t cw = static_cast<uint32_t>((width + 1) / 2);
    const uint32_t ch = static_cast<uint32_t>((height + 1) / 2);

    m_planes[0] = renderer.createTexture(static_cast<uint32_t>(width),
                                         static_cast<uint32_t>(height), DkImageFormat_R8_Unorm,
                                         nullptr, gfx::Filter::Linear);
    if (nv12) {
        // One interleaved chroma plane; the third binding aliases it so the
        // shader always has three valid handles.
        m_planes[1] = renderer.createTexture(cw, ch, DkImageFormat_RG8_Unorm, nullptr,
                                             gfx::Filter::Linear);
        m_planes[2] = m_planes[1];
    } else {
        m_planes[1] = renderer.createTexture(cw, ch, DkImageFormat_R8_Unorm, nullptr,
                                             gfx::Filter::Linear);
        m_planes[2] = renderer.createTexture(cw, ch, DkImageFormat_R8_Unorm, nullptr,
                                             gfx::Filter::Linear);
    }
    if (m_planes[0] == gfx::TexInvalid || m_planes[1] == gfx::TexInvalid ||
        m_planes[2] == gfx::TexInvalid) {
        releasePlanes(renderer);
        return;
    }
    m_width = width;
    m_height = height;
    m_nv12 = nv12;
    FLIKS_LOG("video: planes allocated %dx%d (%s)", width, height, nv12 ? "nv12" : "yuv420p");
}

void VideoRenderer::upload(gfx::Renderer& renderer, AVFrame* frame)
{
    if (!m_ready || !frame || frame->width <= 0 || frame->height <= 0) return;
    // YUV420p from the software decoder, NV12 from the Tegra hardware one.
    // Anything else would need a swscale pass this CPU budget cannot spare.
    const bool nv12 = frame->format == AV_PIX_FMT_NV12;
    const bool planar = frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUVJ420P;
    if (!nv12 && !planar) {
        if (!m_warnedFormat) {
            m_warnedFormat = true;
            FLIKS_LOG("video: unsupported pixel format %d, nothing will display", frame->format);
        }
        return;
    }

    if (frame->width != m_width || frame->height != m_height || frame->format != m_pixFmt) {
        m_pixFmt = frame->format;
        m_colorspace = -1;   // force the matrix to be rewritten for the new layout
        allocatePlanes(renderer, frame->width, frame->height, nv12);
    }
    if (m_planes[0] == gfx::TexInvalid) {
        if (!m_warnedPlanes) {
            m_warnedPlanes = true;
            FLIKS_LOG("video: could not allocate %dx%d planes", frame->width, frame->height);
        }
        return;
    }

    const bool fullRange = frame->color_range == AVCOL_RANGE_JPEG ||
                           frame->format == AV_PIX_FMT_YUVJ420P;
    const bool bt709 = frame->colorspace == AVCOL_SPC_BT709 ||
                       (frame->colorspace == AVCOL_SPC_UNSPECIFIED && frame->height >= 720);
    if (frame->colorspace != m_colorspace || frame->color_range != m_range) {
        m_colorspace = frame->colorspace;
        m_range = frame->color_range;
        VideoParams params{};
        params.config[0] = nv12 ? 1.0f : 0.0f;
        fillMatrix(params, bt709, fullRange);
        std::memcpy(m_paramsUbo.getCpuAddr(), &params, sizeof(params));
    }

    const uint32_t cw = static_cast<uint32_t>((m_width + 1) / 2);
    const uint32_t ch = static_cast<uint32_t>((m_height + 1) / 2);
    if (m_uploadsLogged < 3) {
        m_uploadsLogged++;
        FLIKS_LOG("video: upload %u, linesize %d/%d/%d", m_uploadsLogged, frame->linesize[0],
                  frame->linesize[1], frame->linesize[2]);
    }
    renderer.updateTextureRegion(m_planes[0], 0, 0, static_cast<uint32_t>(m_width),
                                 static_cast<uint32_t>(m_height), frame->data[0],
                                 static_cast<uint32_t>(frame->linesize[0]));
    renderer.updateTextureRegion(m_planes[1], 0, 0, cw, ch, frame->data[1],
                                 static_cast<uint32_t>(frame->linesize[1]));
    // NV12 carries both chroma components in plane 1; plane 2 aliases it.
    if (!m_nv12)
        renderer.updateTextureRegion(m_planes[2], 0, 0, cw, ch, frame->data[2],
                                     static_cast<uint32_t>(frame->linesize[2]));
}

gfx::Rect VideoRenderer::fit(gfx::Rect area) const
{
    if (m_width <= 0 || m_height <= 0 || area.empty()) return area;
    const float videoAspect = static_cast<float>(m_width) / static_cast<float>(m_height);
    const float areaAspect = area.w / area.h;
    if (videoAspect > areaAspect) {
        const float h = area.w / videoAspect;
        return gfx::Rect{ area.x, area.y + (area.h - h) * 0.5f, area.w, h };
    }
    const float w = area.h * videoAspect;
    return gfx::Rect{ area.x + (area.w - w) * 0.5f, area.y, w, area.h };
}

void VideoRenderer::draw(gfx::Renderer& renderer, gfx::Rect dest)
{
    if (!m_ready || m_planes[0] == gfx::TexInvalid || dest.empty()) return;

    if (m_drawsLogged < 3) {
        m_drawsLogged++;
        FLIKS_LOG("video: draw %u at %.0fx%.0f", m_drawsLogged, dest.w, dest.h);
    }

    // Anything the UI queued before this has to land first: the video pass
    // rebinds the pipeline out from under the batcher.
    renderer.flushBatches();

    const float sx = renderer.scale();
    const float fw = static_cast<float>(renderer.fbWidth());
    const float fh = static_cast<float>(renderer.fbHeight());
    const float x0 = dest.x * sx / fw * 2.0f - 1.0f;
    const float x1 = dest.right() * sx / fw * 2.0f - 1.0f;
    const float y0 = 1.0f - dest.y * sx / fh * 2.0f;
    const float y1 = 1.0f - dest.bottom() * sx / fh * 2.0f;

    Vertex verts[4] = {
        { { x0, y0 }, { 0.0f, 0.0f } },
        { { x1, y0 }, { 1.0f, 0.0f } },
        { { x0, y1 }, { 0.0f, 1.0f } },
        { { x1, y1 }, { 1.0f, 1.0f } },
    };
    std::memcpy(m_vertexBuffer.getCpuAddr(), verts, sizeof(verts));

    dk::CmdBuf cmdbuf = renderer.rawCmdBuf();
    cmdbuf.setScissors(0, { DkScissor{ 0, 0, renderer.fbWidth(), renderer.fbHeight() } });
    cmdbuf.bindShaders(DkStageFlag_GraphicsMask, { m_vsh, m_fsh });
    cmdbuf.bindUniformBuffer(DkStage_Fragment, 0, m_paramsUbo.getGpuAddr(), m_paramsUbo.getSize());
    cmdbuf.bindVtxAttribState(VideoAttribs);
    cmdbuf.bindVtxBufferState(VideoBufState);
    cmdbuf.bindVtxBuffer(0, m_vertexBuffer.getGpuAddr(), m_vertexBuffer.getSize());

    const DkResHandle handles[3] = { renderer.textureHandle(m_planes[0]),
                                     renderer.textureHandle(m_planes[1]),
                                     renderer.textureHandle(m_planes[2]) };
    cmdbuf.bindTextures(DkStage_Fragment, 0, { handles[0], handles[1], handles[2] });
    cmdbuf.draw(DkPrimitive_TriangleStrip, 4, 1, 0, 0);

    renderer.restoreUiPipeline();
}

} // namespace player
