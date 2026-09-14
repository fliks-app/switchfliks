#pragma once

#include "gfx/Renderer.h"
#include "gfx/dkfw/CShader.h"

struct AVFrame;

namespace player {

// Presents decoded frames as three planes sampled by a dedicated shader.
// Going through the UI batcher is not an option: it binds one texture per
// draw, and converting on the CPU would cost more than the decode.
class VideoRenderer {
public:
    bool init(gfx::Renderer& renderer);
    void shutdown(gfx::Renderer& renderer);

    // Main thread. Takes ownership of nothing; the caller releases the frame.
    void upload(gfx::Renderer& renderer, AVFrame* frame);
    void draw(gfx::Renderer& renderer, gfx::Rect dest);

    // Drops the picture and its planes, keeping the shaders. Used when a
    // session ends mid-screen — switching rung reopens at a different
    // resolution, and holding the old frame would both show a stale picture
    // and pin its textures.
    void clear(gfx::Renderer& renderer);

    bool hasFrame() const { return m_planes[0] != gfx::TexInvalid; }
    int width() const { return m_width; }
    int height() const { return m_height; }

    // Letterboxes the video inside `area`, preserving the source aspect.
    gfx::Rect fit(gfx::Rect area) const;

    struct Vertex {
        float pos[2];
        float uv[2];
    };

private:
    void allocatePlanes(gfx::Renderer& renderer, int width, int height, bool nv12);
    void releasePlanes(gfx::Renderer& renderer);

    gfx::TexId m_planes[3] = { gfx::TexInvalid, gfx::TexInvalid, gfx::TexInvalid };
    int m_width = 0;
    int m_height = 0;
    int m_colorspace = -1;
    int m_pixFmt = -1;
    bool m_nv12 = false;
    int m_range = -1;

    CShader m_vsh;
    CShader m_fsh;
    CMemPool::Handle m_vertexBuffer;
    CMemPool::Handle m_paramsUbo;
    bool m_ready = false;
    bool m_warnedFormat = false;
    bool m_warnedPlanes = false;
    unsigned m_drawsLogged = 0;
    unsigned m_uploadsLogged = 0;
};

} // namespace player
