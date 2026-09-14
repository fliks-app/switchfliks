#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "gfx/dkfw/CCmdMemRing.h"
#include "gfx/dkfw/CDescriptorSet.h"
#include "gfx/dkfw/CMemPool.h"
#include "gfx/dkfw/CShader.h"
#include "gfx/dkfw/common.h"

#include "gfx/Color.h"

namespace gfx {

using TexId = int;
constexpr TexId TexNone = 0;      // 1x1 opaque white, so solids share the path
constexpr TexId TexInvalid = -1;

enum class Filter { Linear, Nearest };

// Everything the UI draws is one of these quads. The rounded-rect description
// travels per-vertex rather than in a uniform so that a card's fill, its focus
// ring and its artwork can sit in the same batch.
struct Vertex {
    float pos[2];
    float uv[2];
    uint32_t color;
    float local[2];
    float shape[4];   // halfW, halfH, radius, strokeWidth
    float mode;       // 0 solid, 1 rgba, 2 alpha mask
};

class Renderer {
public:
    static constexpr unsigned NumFramebuffers = 3;
    static constexpr unsigned MaxTextures = 768;
    static constexpr unsigned MaxQuads = 8192;

    bool init();
    void shutdown();

    // Re-creates the swapchain for the current operation mode (docked 1080p /
    // handheld 720p). Safe to call every frame; returns true if it changed.
    bool syncResolution(bool force = false);

    // hbmenu over the album gets a much smaller nvservices allotment than a
    // title takeover; the artwork cache has to size itself accordingly.
    static bool titleTakeover();
    static size_t textureBudget();

    // False when the swapchain could not hand over an image this frame;
    // the caller must then skip drawing entirely.
    bool beginFrame();
    void endFrame();

    float width() const { return m_virtW; }
    float height() const { return m_virtH; }
    float scale() const { return m_scale; }
    uint32_t fbWidth() const { return m_fbW; }
    uint32_t fbHeight() const { return m_fbH; }

    void clear(Color c);

    void fillRect(Rect r, Color c, float radius = 0.0f);
    void strokeRect(Rect r, Color c, float radius, float lineWidth);
    void drawImage(TexId tex, Rect r, Color tint, float radius = 0.0f,
                   Rect uv = Rect{ 0, 0, 1, 1 });
    void drawAlphaQuad(TexId tex, Rect r, Rect uv, Color c);

    void pushClip(Rect r);
    void popClip();
    Rect currentClip() const { return m_clipStack.back(); }

    // Main thread only: deko3d objects are not thread safe.
    TexId createTexture(uint32_t w, uint32_t h, DkImageFormat fmt, const void* pixels,
                        Filter filter = Filter::Linear);
    void updateTexture(TexId id, const void* pixels);
    void updateTextureRegion(TexId id, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                             const void* pixels, uint32_t srcRowPitch);
    void destroyTexture(TexId id);
    bool textureValid(TexId id) const;
    uint32_t textureWidth(TexId id) const;
    size_t liveTextures() const { return m_slots.size() - m_freeSlots.size(); }
    size_t stagingInFlight() const { return m_stagingInFlight.size(); }
    uint32_t textureHeight(TexId id) const;

    // Escape hatch for the video pass, which binds its own shaders and samples
    // three planes at once.
    dk::CmdBuf rawCmdBuf() { return m_dyncmd; }
    dk::Device rawDevice() { return m_device; }
    CMemPool& dataPool() { return *m_poolData; }
    CMemPool& codePool() { return *m_poolCode; }
    void flushBatches();
    DkResHandle textureHandle(TexId id) const;
    void restoreUiPipeline();

private:
    struct Slot {
        dk::Image image;
        CMemPool::Handle mem;
        uint32_t w = 0, h = 0;
        uint32_t bpp = 0;
        DkImageFormat fmt = DkImageFormat_RGBA8_Unorm;
        uint32_t samplerId = 0;
        bool used = false;
    };

    struct Batch {
        DkResHandle handle;
        DkScissor scissor;
        uint32_t firstIndex;
        uint32_t indexCount;
    };

    void createFramebuffers();
    void destroyFramebuffers();
    void buildIndexBuffer();
    void pushQuad(TexId tex, Rect r, Rect uv, Color c, float radius, float stroke, float mode);
    DkScissor scissorFromClip(Rect r) const;
    CMemPool::Handle acquireStaging(uint32_t size, uint32_t align);
    void recycleStaging();

    dk::UniqueDevice m_device;
    dk::UniqueQueue m_queue;

    std::optional<CMemPool> m_poolImages;
    std::optional<CMemPool> m_poolCode;
    std::optional<CMemPool> m_poolData;
    std::optional<CMemPool> m_poolStaging;

    dk::UniqueCmdBuf m_cmdbuf;      // static state
    dk::UniqueCmdBuf m_dyncmd;      // per-frame draws
    dk::UniqueCmdBuf m_uploadcmd;   // texture transfers
    CCmdMemRing<NumFramebuffers> m_dynmem;
    // Transfers need the same fencing as the draws: submitCommands is
    // asynchronous, so reusing one flat allocation would overwrite command
    // data the GPU is still reading.
    CCmdMemRing<NumFramebuffers> m_uploadmem;

    CDescriptorSet<MaxTextures> m_imageDescriptors;
    CDescriptorSet<2> m_samplerDescriptors;

    CShader m_vsh, m_fsh;

    CMemPool::Handle m_viewportUbo;
    CMemPool::Handle m_indexBuffer;
    CMemPool::Handle m_vertexBuffers[NumFramebuffers];

    CMemPool::Handle m_framebufferMem[NumFramebuffers];
    dk::Image m_framebuffers[NumFramebuffers];
    dk::UniqueSwapchain m_swapchain;

    std::vector<Slot> m_slots;
    std::vector<uint32_t> m_freeSlots;

    std::vector<Vertex> m_verts;
    size_t m_uploadedVerts = 0;
    std::vector<Batch> m_batches;
    std::vector<Rect> m_clipStack;
    std::vector<std::pair<int, CMemPool::Handle>> m_stagingInFlight;

    DkResHandle m_pendingHandle = 0;
    DkScissor m_pendingScissor{};
    uint32_t m_pendingFirstIndex = 0;
    bool m_hasPending = false;
    bool m_uploadDirty = false;

    uint32_t m_fbW = 0, m_fbH = 0;
    float m_virtW = 0, m_virtH = 0;
    float m_scale = 1.0f;
    unsigned m_slotIndex = 0;
    unsigned m_framesLogged = 0;
    bool m_swapchainLost = false;
    unsigned m_drawsThisFrame = 0;
    uint64_t m_frameCounter = 0;
    int m_acquiredSlot = -1;
    bool m_ready = false;
};

} // namespace gfx
