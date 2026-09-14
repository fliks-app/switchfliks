#include "gfx/Renderer.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "util/Log.h"

namespace gfx {

namespace {

constexpr std::array<DkVtxAttribState, 6> VertexAttribs = { {
    { 0, 0, offsetof(Vertex, pos), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0 },
    { 0, 0, offsetof(Vertex, uv), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0 },
    { 0, 0, offsetof(Vertex, color), DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, 0 },
    { 0, 0, offsetof(Vertex, local), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0 },
    { 0, 0, offsetof(Vertex, shape), DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0 },
    { 0, 0, offsetof(Vertex, mode), DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0 },
} };

constexpr std::array<DkVtxBufferState, 1> VertexBufState = { {
    { sizeof(Vertex), 0 },
} };

uint32_t bytesPerPixel(DkImageFormat fmt)
{
    switch (fmt) {
        case DkImageFormat_R8_Unorm: return 1;
        case DkImageFormat_RG8_Unorm: return 2;
        case DkImageFormat_RGBA8_Unorm: return 4;
        default: return 4;
    }
}

void deko3dDebug(void* userData, const char* context, DkResult result, const char* message)
{
    (void)userData;
    FLIKS_LOG("deko3d [%s] result=%d: %s", context ? context : "?", static_cast<int>(result),
              message ? message : "");
}

} // namespace

bool Renderer::init()
{
    m_device = dk::DeviceMaker{}.setCbDebug(deko3dDebug).create();
    if (!m_device) {
        FLIKS_LOG("renderer: device creation failed");
        return false;
    }
    m_queue = dk::QueueMaker{ m_device }.setFlags(DkQueueFlags_Graphics).create();

    m_poolImages.emplace(m_device, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, 16 * 1024 * 1024);
    m_poolCode.emplace(m_device,
                       DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code,
                       256 * 1024);
    m_poolData.emplace(m_device, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
                       16 * 1024 * 1024);
    m_poolStaging.emplace(m_device, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
                          8 * 1024 * 1024);

    m_cmdbuf = dk::CmdBufMaker{ m_device }.create();
    CMemPool::Handle staticMem = m_poolData->allocate(64 * 1024);
    m_cmdbuf.addMemory(staticMem.getMemBlock(), staticMem.getOffset(), staticMem.getSize());

    m_dyncmd = dk::CmdBufMaker{ m_device }.create();
    m_dynmem.allocate(*m_poolData, 256 * 1024);

    m_uploadcmd = dk::CmdBufMaker{ m_device }.create();
    m_uploadmem.allocate(*m_poolData, 256 * 1024);
    // Arm the first slice before anything records into it: createTexture
    // below already pushes an image descriptor.
    m_uploadmem.begin(m_uploadcmd);

    m_imageDescriptors.allocate(*m_poolData);
    m_samplerDescriptors.allocate(*m_poolData);

    if (!m_vsh.load(*m_poolCode, "romfs:/shaders/ui_vsh.dksh")) {
        FLIKS_LOG("renderer: missing romfs:/shaders/ui_vsh.dksh");
        return false;
    }
    if (!m_fsh.load(*m_poolCode, "romfs:/shaders/ui_fsh.dksh")) {
        FLIKS_LOG("renderer: missing romfs:/shaders/ui_fsh.dksh");
        return false;
    }

    m_viewportUbo = m_poolData->allocate(sizeof(float) * 4, DK_UNIFORM_BUF_ALIGNMENT);
    for (auto& vb : m_vertexBuffers)
        vb = m_poolData->allocate(MaxQuads * 4 * sizeof(Vertex), alignof(Vertex));
    buildIndexBuffer();

    // Two samplers: artwork is filtered, the glyph atlas and the video planes
    // are sampled at their native scale and stay crisp on nearest.
    {
        dk::Sampler linear, nearest;
        linear.setFilter(DkFilter_Linear, DkFilter_Linear);
        linear.setWrapMode(DkWrapMode_ClampToEdge, DkWrapMode_ClampToEdge, DkWrapMode_ClampToEdge);
        nearest.setFilter(DkFilter_Nearest, DkFilter_Nearest);
        nearest.setWrapMode(DkWrapMode_ClampToEdge, DkWrapMode_ClampToEdge, DkWrapMode_ClampToEdge);

        dk::SamplerDescriptor descs[2];
        descs[0].initialize(linear);
        descs[1].initialize(nearest);
        m_samplerDescriptors.update(m_cmdbuf, 0, descs[0]);
        m_samplerDescriptors.update(m_cmdbuf, 1, descs[1]);
        m_queue.submitCommands(m_cmdbuf.finishList());
        m_queue.waitIdle();
        m_cmdbuf.clear();
    }

    m_slots.resize(MaxTextures);
    // Back of the list is handed out first, so slot 0 goes to the white texel
    // below and every solid fill then shares the textured draw path.
    for (uint32_t i = MaxTextures; i-- > 0;)
        m_freeSlots.push_back(i);

    const uint32_t white = 0xffffffffu;
    if (createTexture(1, 1, DkImageFormat_RGBA8_Unorm, &white, Filter::Nearest) != TexNone) {
        FLIKS_LOG("renderer: white texel did not land in slot 0");
        return false;
    }

    m_clipStack.clear();
    m_verts.reserve(MaxQuads * 4);
    m_batches.reserve(256);

    syncResolution();
    m_ready = true;
    FLIKS_LOG("renderer: %ux%u, scale %.3f, virtual %.0fx%.0f", m_fbW, m_fbH, m_scale, m_virtW,
              m_virtH);
    return true;
}

void Renderer::shutdown()
{
    if (!m_device) return;
    m_queue.waitIdle();
    destroyFramebuffers();
    for (auto& s : m_slots) s.mem.destroy();
    m_slots.clear();
    for (auto& vb : m_vertexBuffers) vb.destroy();
    m_indexBuffer.destroy();
    m_viewportUbo.destroy();
    for (auto& p : m_stagingInFlight) p.second.destroy();
    m_stagingInFlight.clear();
    m_ready = false;
}

void Renderer::buildIndexBuffer()
{
    m_indexBuffer = m_poolData->allocate(MaxQuads * 6 * sizeof(uint16_t), 4);
    auto* idx = static_cast<uint16_t*>(m_indexBuffer.getCpuAddr());
    for (uint32_t q = 0; q < MaxQuads; q++) {
        const uint16_t base = static_cast<uint16_t>(q * 4);
        idx[q * 6 + 0] = base + 0;
        idx[q * 6 + 1] = base + 1;
        idx[q * 6 + 2] = base + 2;
        idx[q * 6 + 3] = base + 0;
        idx[q * 6 + 4] = base + 2;
        idx[q * 6 + 5] = base + 3;
    }
}

bool Renderer::titleTakeover()
{
    // Launched over the album, nvservices hands the process a small slice of
    // GPU memory; a title takeover gets the application allotment instead.
    const AppletType type = appletGetAppletType();
    return type == AppletType_Application || type == AppletType_SystemApplication;
}

size_t Renderer::textureBudget()
{
    return titleTakeover() ? 80u * 1024 * 1024 : 20u * 1024 * 1024;
}

bool Renderer::syncResolution(bool force)
{
    const bool docked = appletGetOperationMode() == AppletOperationMode_Console;
    const uint32_t w = docked ? 1920 : 1280;
    const uint32_t h = docked ? 1080 : 720;
    if (!force && w == m_fbW && h == m_fbH) return false;

    if (m_swapchain) {
        m_queue.waitIdle();
        destroyFramebuffers();
    }
    m_swapchainLost = false;
    m_fbW = w;
    m_fbH = h;
    // The client lays the 10-foot UI out at 960 CSS px (a 1080p TV reports
    // innerWidth=960 at DPR=2), so docked reproduces it exactly and handheld
    // is the same layout at a smaller scale.
    m_scale = static_cast<float>(m_fbW) / 960.0f;
    m_virtW = static_cast<float>(m_fbW) / m_scale;
    m_virtH = static_cast<float>(m_fbH) / m_scale;
    createFramebuffers();
    return true;
}

void Renderer::createFramebuffers()
{
    NWindow* win = nwindowGetDefault();
    nwindowSetDimensions(win, m_fbW, m_fbH);

    dk::ImageLayout layout;
    dk::ImageLayoutMaker{ m_device }
        .setFlags(DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression)
        .setFormat(DkImageFormat_RGBA8_Unorm)
        .setDimensions(m_fbW, m_fbH)
        .initialize(layout);

    const uint32_t size = layout.getSize();
    const uint32_t align = layout.getAlignment();
    std::array<DkImage const*, NumFramebuffers> fbArray;
    for (unsigned i = 0; i < NumFramebuffers; i++) {
        m_framebufferMem[i] = m_poolImages->allocate(size, align);
        m_framebuffers[i].initialize(layout, m_framebufferMem[i].getMemBlock(),
                                     m_framebufferMem[i].getOffset());
        fbArray[i] = &m_framebuffers[i];
    }
    m_swapchain = dk::SwapchainMaker{ m_device, win, fbArray }.create();

    const float invSize[4] = { 2.0f / static_cast<float>(m_fbW),
                               -2.0f / static_cast<float>(m_fbH), 0.0f, 0.0f };
    std::memcpy(m_viewportUbo.getCpuAddr(), invSize, sizeof(invSize));
}

void Renderer::destroyFramebuffers()
{
    m_swapchain.destroy();
    for (auto& mem : m_framebufferMem) mem.destroy();
}

CMemPool::Handle Renderer::acquireStaging(uint32_t size, uint32_t align)
{
    CMemPool::Handle h = m_poolStaging->allocate(size, align);
    // +2, not +1: the GPU can legitimately be a full swapchain behind, and
    // the tick that frees this may land before the copy has executed.
    if (h) m_stagingInFlight.emplace_back(static_cast<int>(NumFramebuffers) + 2, h);
    return h;
}

void Renderer::recycleStaging()
{
    for (auto it = m_stagingInFlight.begin(); it != m_stagingInFlight.end();) {
        if (--it->first <= 0) {
            it->second.destroy();
            it = m_stagingInFlight.erase(it);
        } else {
            ++it;
        }
    }
}

TexId Renderer::createTexture(uint32_t w, uint32_t h, DkImageFormat fmt, const void* pixels,
                              Filter filter)
{
    if (w == 0 || h == 0 || m_freeSlots.empty()) return TexInvalid;
    const uint32_t id = m_freeSlots.back();
    m_freeSlots.pop_back();

    Slot& s = m_slots[id];
    dk::ImageLayout layout;
    dk::ImageLayoutMaker{ m_device }
        .setFlags(0)
        .setFormat(fmt)
        .setDimensions(w, h)
        .initialize(layout);

    s.mem = m_poolImages->allocate(layout.getSize(), layout.getAlignment());
    if (!s.mem) {
        m_freeSlots.push_back(id);
        return TexInvalid;
    }
    s.image.initialize(layout, s.mem.getMemBlock(), s.mem.getOffset());
    s.w = w;
    s.h = h;
    s.fmt = fmt;
    s.bpp = bytesPerPixel(fmt);
    s.samplerId = filter == Filter::Linear ? 0 : 1;
    s.used = true;

    dk::ImageDescriptor desc;
    desc.initialize(dk::ImageView{ s.image });
    m_imageDescriptors.update(m_uploadcmd, id, desc);
    m_uploadDirty = true;

    if (pixels) updateTexture(static_cast<TexId>(id), pixels);
    return static_cast<TexId>(id);
}

void Renderer::updateTexture(TexId id, const void* pixels)
{
    if (!textureValid(id)) return;
    const Slot& s = m_slots[id];
    updateTextureRegion(id, 0, 0, s.w, s.h, pixels, s.w * s.bpp);
}

void Renderer::updateTextureRegion(TexId id, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                   const void* pixels, uint32_t srcRowPitch)
{
    if (!textureValid(id) || !pixels || w == 0 || h == 0) return;
    const Slot& s = m_slots[id];
    const uint32_t rowBytes = w * s.bpp;

    CMemPool::Handle staging = acquireStaging(rowBytes * h, DK_IMAGE_LINEAR_STRIDE_ALIGNMENT);
    if (!staging) return;

    auto* dst = static_cast<uint8_t*>(staging.getCpuAddr());
    const auto* src = static_cast<const uint8_t*>(pixels);
    if (srcRowPitch == rowBytes) {
        std::memcpy(dst, src, static_cast<size_t>(rowBytes) * h);
    } else {
        for (uint32_t row = 0; row < h; row++)
            std::memcpy(dst + static_cast<size_t>(row) * rowBytes,
                        src + static_cast<size_t>(row) * srcRowPitch, rowBytes);
    }

    dk::ImageView view{ s.image };
    DkCopyBuf srcBuf{ staging.getGpuAddr(), 0, 0 };
    DkImageRect rect{ x, y, 0, w, h, 1 };
    m_uploadcmd.copyBufferToImage(srcBuf, view, rect, 0);
    m_uploadDirty = true;
}

void Renderer::destroyTexture(TexId id)
{
    if (id <= 0 || static_cast<size_t>(id) >= m_slots.size()) return;
    Slot& s = m_slots[id];
    if (!s.used) return;
    // The GPU may still be reading it from an in-flight frame; hand the block
    // to the staging recycler rather than freeing it under the queue.
    m_stagingInFlight.emplace_back(static_cast<int>(NumFramebuffers) + 2, s.mem);
    s.mem = CMemPool::Handle{};
    s.used = false;
    s.w = s.h = 0;
    m_freeSlots.push_back(static_cast<uint32_t>(id));
}

bool Renderer::textureValid(TexId id) const
{
    return id >= 0 && static_cast<size_t>(id) < m_slots.size() && m_slots[id].used;
}

uint32_t Renderer::textureWidth(TexId id) const { return textureValid(id) ? m_slots[id].w : 0; }
uint32_t Renderer::textureHeight(TexId id) const { return textureValid(id) ? m_slots[id].h : 0; }

DkResHandle Renderer::textureHandle(TexId id) const
{
    if (!textureValid(id)) return dkMakeTextureHandle(0, 1);
    return dkMakeTextureHandle(static_cast<uint32_t>(id), m_slots[id].samplerId);
}

DkScissor Renderer::scissorFromClip(Rect r) const
{
    auto clampU = [](float v, float lo, float hi) {
        return static_cast<uint32_t>(v < lo ? lo : (v > hi ? hi : v));
    };
    const float fw = static_cast<float>(m_fbW);
    const float fh = static_cast<float>(m_fbH);
    const float x0 = r.x * m_scale, y0 = r.y * m_scale;
    const float x1 = r.right() * m_scale, y1 = r.bottom() * m_scale;
    const uint32_t sx = clampU(x0, 0, fw);
    const uint32_t sy = clampU(y0, 0, fh);
    const uint32_t ex = clampU(x1, 0, fw);
    const uint32_t ey = clampU(y1, 0, fh);
    return DkScissor{ sx, sy, ex > sx ? ex - sx : 0, ey > sy ? ey - sy : 0 };
}

bool Renderer::beginFrame()
{
    recycleStaging();

    // A failed dequeue returns a negative slot. Indexing the framebuffer
    // array with it is an out-of-bounds read feeding straight into
    // bindRenderTargets, so the frame is dropped and the swapchain rebuilt.
    const int slot = m_queue.acquireImage(m_swapchain);
    if (slot < 0 || slot >= static_cast<int>(NumFramebuffers)) {
        if (!m_swapchainLost) {
            FLIKS_LOG("renderer: acquireImage returned %d, rebuilding swapchain", slot);
            m_swapchainLost = true;
        }
        m_acquiredSlot = -1;
        return false;
    }
    m_acquiredSlot = slot;

    m_verts.clear();
    m_batches.clear();
    m_hasPending = false;
    m_pendingFirstIndex = 0;
    m_uploadedVerts = 0;
    m_clipStack.clear();
    m_clipStack.push_back(Rect{ 0, 0, m_virtW, m_virtH });

    m_dynmem.begin(m_dyncmd);

    dk::ImageView colorTarget{ m_framebuffers[m_acquiredSlot] };
    m_dyncmd.bindRenderTargets({ &colorTarget });
    m_dyncmd.setViewports(0, { DkViewport{ 0.0f, 0.0f, static_cast<float>(m_fbW),
                                           static_cast<float>(m_fbH), 0.0f, 1.0f } });
    restoreUiPipeline();
    return true;
}

void Renderer::restoreUiPipeline()
{
    m_dyncmd.bindShaders(DkStageFlag_GraphicsMask, { m_vsh, m_fsh });
    m_dyncmd.bindUniformBuffer(DkStage_Vertex, 0, m_viewportUbo.getGpuAddr(),
                               m_viewportUbo.getSize());
    // A 2D batcher emits quads clockwise in screen space, which the vertex
    // shader's Y-flip turns into back-facing triangles under the CCW default.
    // Nothing here is ever a closed volume, so culling is simply off.
    dk::RasterizerState raster;
    raster.setCullMode(DkFace_None);
    m_dyncmd.bindRasterizerState(raster);
    m_dyncmd.bindColorState(dk::ColorState{}.setBlendEnable(0, true));
    m_dyncmd.bindColorWriteState(dk::ColorWriteState{});
    m_dyncmd.bindDepthStencilState(dk::DepthStencilState{}.setDepthTestEnable(false)
                                                          .setDepthWriteEnable(false));
    // Straight (non-premultiplied) alpha over an opaque target, which is how
    // the browser composites the client's `rgba()` surfaces.
    dk::BlendState blend;
    blend.setFactors(DkBlendFactor_SrcAlpha, DkBlendFactor_InvSrcAlpha, DkBlendFactor_One,
                     DkBlendFactor_InvSrcAlpha);
    m_dyncmd.bindBlendStates(0, { blend });
    m_dyncmd.bindVtxAttribState(VertexAttribs);
    m_dyncmd.bindVtxBufferState(VertexBufState);
    m_dyncmd.bindIdxBuffer(DkIdxFormat_Uint16, m_indexBuffer.getGpuAddr());
    CMemPool::Handle& vb = m_vertexBuffers[m_slotIndex];
    m_dyncmd.bindVtxBuffer(0, vb.getGpuAddr(), vb.getSize());
    m_imageDescriptors.bindForImages(m_dyncmd);
    m_samplerDescriptors.bindForSamplers(m_dyncmd);
}

void Renderer::clear(Color c)
{
    m_dyncmd.clearColor(0, DkColorMask_RGBA, c.r, c.g, c.b, c.a);
}

void Renderer::pushClip(Rect r)
{
    m_clipStack.push_back(r.intersect(m_clipStack.back()));
}

void Renderer::popClip()
{
    if (m_clipStack.size() > 1) m_clipStack.pop_back();
}

void Renderer::pushQuad(TexId tex, Rect r, Rect uv, Color c, float radius, float stroke, float mode)
{
    if (r.empty() || c.a <= 0.0f) return;
    const Rect clip = m_clipStack.back();
    if (clip.empty()) return;
    // The SDF bleeds up to the stroke width outside the rect, so test the
    // grown box — a focus ring on a card at the edge of a rail must survive.
    if (clip.intersect(r.expand(stroke + 1.0f)).empty()) return;

    if (m_verts.size() / 4 >= MaxQuads) return;

    const DkResHandle handle = textureHandle(tex);
    const DkScissor scissor = scissorFromClip(clip);
    const uint32_t firstIndex = static_cast<uint32_t>(m_verts.size() / 4) * 6;

    const bool sameState = m_hasPending && m_pendingHandle == handle &&
                           m_pendingScissor.x == scissor.x && m_pendingScissor.y == scissor.y &&
                           m_pendingScissor.width == scissor.width &&
                           m_pendingScissor.height == scissor.height;
    if (!sameState) {
        if (m_hasPending)
            m_batches.push_back(Batch{ m_pendingHandle, m_pendingScissor, m_pendingFirstIndex,
                                       firstIndex - m_pendingFirstIndex });
        m_pendingHandle = handle;
        m_pendingScissor = scissor;
        m_pendingFirstIndex = firstIndex;
        m_hasPending = true;
    }

    const float s = m_scale;
    const float x0 = r.x * s, y0 = r.y * s;
    const float x1 = r.right() * s, y1 = r.bottom() * s;
    const float hw = (x1 - x0) * 0.5f, hh = (y1 - y0) * 0.5f;
    const float rad = std::min(radius * s, std::min(hw, hh));
    const uint32_t packed = c.packed();

    const float xs[4] = { x0, x1, x1, x0 };
    const float ys[4] = { y0, y0, y1, y1 };
    const float us[4] = { uv.x, uv.right(), uv.right(), uv.x };
    const float vs[4] = { uv.y, uv.y, uv.bottom(), uv.bottom() };
    const float lx[4] = { -hw, hw, hw, -hw };
    const float ly[4] = { -hh, -hh, hh, hh };

    for (int i = 0; i < 4; i++) {
        Vertex v{};
        v.pos[0] = xs[i];
        v.pos[1] = ys[i];
        v.uv[0] = us[i];
        v.uv[1] = vs[i];
        v.color = packed;
        v.local[0] = lx[i];
        v.local[1] = ly[i];
        v.shape[0] = hw;
        v.shape[1] = hh;
        v.shape[2] = rad;
        v.shape[3] = stroke * s;
        v.mode = mode;
        m_verts.push_back(v);
    }
}

void Renderer::fillRect(Rect r, Color c, float radius)
{
    pushQuad(TexNone, r, Rect{ 0, 0, 1, 1 }, c, radius, 0.0f, 0.0f);
}

void Renderer::strokeRect(Rect r, Color c, float radius, float lineWidth)
{
    if (lineWidth <= 0.0f) return;
    pushQuad(TexNone, r, Rect{ 0, 0, 1, 1 }, c, radius, lineWidth, 0.0f);
}

void Renderer::drawImage(TexId tex, Rect r, Color tint, float radius, Rect uv)
{
    pushQuad(tex, r, uv, tint, radius, 0.0f, 1.0f);
}

void Renderer::drawAlphaQuad(TexId tex, Rect r, Rect uv, Color c)
{
    pushQuad(tex, r, uv, c, 0.0f, 0.0f, 2.0f);
}

void Renderer::flushBatches()
{
    if (m_hasPending) {
        const uint32_t endIndex = static_cast<uint32_t>(m_verts.size() / 4) * 6;
        if (endIndex > m_pendingFirstIndex)
            m_batches.push_back(Batch{ m_pendingHandle, m_pendingScissor, m_pendingFirstIndex,
                                       endIndex - m_pendingFirstIndex });
        m_hasPending = false;
    }
    if (m_batches.empty()) return;

    // Vertices accumulate for the whole frame and the buffer keeps its
    // contents: the draws recorded by an earlier flush still point into it,
    // so only the newly appended span is copied and nothing is rewound.
    CMemPool::Handle& vb = m_vertexBuffers[m_slotIndex];
    if (m_verts.size() > m_uploadedVerts) {
        auto* dst = static_cast<Vertex*>(vb.getCpuAddr()) + m_uploadedVerts;
        std::memcpy(dst, m_verts.data() + m_uploadedVerts,
                    (m_verts.size() - m_uploadedVerts) * sizeof(Vertex));
        m_uploadedVerts = m_verts.size();
    }

    for (const Batch& b : m_batches) {
        if (!b.indexCount || !b.scissor.width || !b.scissor.height) continue;
        m_dyncmd.setScissors(0, { b.scissor });
        m_dyncmd.bindTextures(DkStage_Fragment, 0, { b.handle });
        m_dyncmd.drawIndexed(DkPrimitive_Triangles, b.indexCount, 1, b.firstIndex, 0, 0);
        m_drawsThisFrame++;
    }
    m_batches.clear();
}

void Renderer::endFrame()
{
    flushBatches();

    if (m_framesLogged < 3) {
        m_framesLogged++;
        FLIKS_LOG("frame %u: %zu verts, %u draws", m_framesLogged, m_uploadedVerts,
                  m_drawsThisFrame);
    }
    m_drawsThisFrame = 0;

    if (m_uploadDirty) {
        // end() signals this slice's fence; begin() then waits on the next
        // one, so a slice is only rewritten once the GPU is done with it.
        m_queue.submitCommands(m_uploadmem.end(m_uploadcmd));
        m_uploadmem.begin(m_uploadcmd);
        m_uploadDirty = false;
    }

    m_queue.submitCommands(m_dynmem.end(m_dyncmd));
    m_queue.presentImage(m_swapchain, m_acquiredSlot);
    m_slotIndex = (m_slotIndex + 1) % NumFramebuffers;
    m_acquiredSlot = -1;
}

} // namespace gfx
