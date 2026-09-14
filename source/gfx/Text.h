#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "gfx/Renderer.h"

typedef struct FT_LibraryRec_* FT_Library;
typedef struct FT_FaceRec_* FT_Face;

namespace gfx {

enum class Align { Left, Center, Right };

// Nintendo's shared fonts are a single weight, so bold is synthesised by
// emboldening the outline — the same thing the web client's `font-bold` gets
// from the platform UI font on a TV that ships no bold face.
struct TextStyle {
    float size = 18.0f;
    Color color = Color::rgb(0xffffff);
    bool bold = false;
};

class TextRenderer {
public:
    bool init(Renderer& renderer);
    void shutdown();

    float lineHeight(float size) const { return size * 1.35f; }
    float ascent(float size) const { return size * 0.82f; }

    float measure(const std::string& utf8, float size, bool bold = false);

    // y is the top of the line box; the baseline is derived from the size so
    // callers lay out in boxes the way CSS does.
    void draw(const std::string& utf8, float x, float y, const TextStyle& style);
    void drawAligned(const std::string& utf8, Rect box, const TextStyle& style, Align align);

    // `line-clamp-1` with a trailing ellipsis.
    void drawEllipsized(const std::string& utf8, Rect box, const TextStyle& style,
                        Align align = Align::Left);

    // Word-wrapped paragraph, capped at maxLines with an ellipsis on the last.
    // Returns the height actually used.
    float drawWrapped(const std::string& utf8, Rect box, const TextStyle& style, int maxLines);
    float measureWrappedHeight(const std::string& utf8, float width, float size, int maxLines,
                               bool bold = false);

private:
    struct Glyph {
        TexId tex = TexInvalid;
        Rect uv{};
        float w = 0, h = 0;
        float bearingX = 0, bearingY = 0;
        float advance = 0;
        bool valid = false;
    };

    struct Page {
        TexId tex = TexInvalid;
        uint32_t penX = 0, penY = 0, rowH = 0;
    };

    const Glyph& glyph(uint32_t codepoint, uint32_t pxSize, bool bold);
    bool packGlyph(const uint8_t* bitmap, uint32_t w, uint32_t h, uint32_t pitch, Glyph& out);
    FT_Face faceFor(uint32_t codepoint, uint32_t* glyphIndexOut);
    uint32_t pixelSize(float size) const;

    Renderer* m_renderer = nullptr;
    FT_Library m_ft = nullptr;
    std::vector<FT_Face> m_faces;
    std::vector<Page> m_pages;
    std::unordered_map<uint64_t, Glyph> m_cache;
    uint32_t m_atlasSize = 1024;
};

// Minimal UTF-8 walk; returns the codepoint and advances `i`.
uint32_t utf8Next(const std::string& s, size_t& i);

} // namespace gfx
