#include "gfx/Text.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include <switch.h>

namespace gfx {

namespace {

constexpr const char* kEllipsis = "…";

// Latin first, then the private-use button glyphs, then the CJK sets. A miss
// in one face falls through to the next, which is how the system font behaves.
constexpr PlSharedFontType kFontOrder[] = {
    PlSharedFontType_Standard,
    PlSharedFontType_NintendoExt,
    PlSharedFontType_ChineseSimplified,
    PlSharedFontType_ExtChineseSimplified,
    PlSharedFontType_ChineseTraditional,
    PlSharedFontType_KO,
};

} // namespace

uint32_t utf8Next(const std::string& s, size_t& i)
{
    if (i >= s.size()) return 0;
    const auto b0 = static_cast<uint8_t>(s[i]);
    auto cont = [&](size_t k) -> uint32_t {
        return (i + k < s.size()) ? (static_cast<uint8_t>(s[i + k]) & 0x3fu) : 0u;
    };
    if (b0 < 0x80) { i += 1; return b0; }
    if ((b0 & 0xe0) == 0xc0) { uint32_t c = ((b0 & 0x1fu) << 6) | cont(1); i += 2; return c; }
    if ((b0 & 0xf0) == 0xe0) {
        uint32_t c = ((b0 & 0x0fu) << 12) | (cont(1) << 6) | cont(2);
        i += 3;
        return c;
    }
    if ((b0 & 0xf8) == 0xf0) {
        uint32_t c = ((b0 & 0x07u) << 18) | (cont(1) << 12) | (cont(2) << 6) | cont(3);
        i += 4;
        return c;
    }
    i += 1;
    return 0xfffd;
}

bool TextRenderer::init(Renderer& renderer)
{
    m_renderer = &renderer;
    if (FT_Init_FreeType(&m_ft) != 0) return false;

    for (PlSharedFontType type : kFontOrder) {
        PlFontData data{};
        if (R_FAILED(plGetSharedFontByType(&data, type))) continue;
        FT_Face face = nullptr;
        if (FT_New_Memory_Face(m_ft, static_cast<const FT_Byte*>(data.address),
                               static_cast<FT_Long>(data.size), 0, &face) == 0)
            m_faces.push_back(face);
    }
    return !m_faces.empty();
}

void TextRenderer::shutdown()
{
    for (FT_Face f : m_faces) FT_Done_Face(f);
    m_faces.clear();
    if (m_ft) FT_Done_FreeType(m_ft);
    m_ft = nullptr;
    m_cache.clear();
    m_pages.clear();
}

uint32_t TextRenderer::pixelSize(float size) const
{
    const float px = size * (m_renderer ? m_renderer->scale() : 1.0f);
    const int rounded = static_cast<int>(px + 0.5f);
    return static_cast<uint32_t>(std::max(6, std::min(rounded, 256)));
}

FT_Face TextRenderer::faceFor(uint32_t codepoint, uint32_t* glyphIndexOut)
{
    for (FT_Face face : m_faces) {
        const FT_UInt idx = FT_Get_Char_Index(face, codepoint);
        if (idx != 0) {
            *glyphIndexOut = idx;
            return face;
        }
    }
    *glyphIndexOut = 0;
    return m_faces.empty() ? nullptr : m_faces.front();
}

bool TextRenderer::packGlyph(const uint8_t* bitmap, uint32_t w, uint32_t h, uint32_t pitch,
                             Glyph& out)
{
    if (w == 0 || h == 0) {
        out.tex = TexInvalid;
        return true;
    }
    const uint32_t pad = 1;
    for (Page& page : m_pages) {
        if (page.penX + w + pad > m_atlasSize) {
            page.penX = 0;
            page.penY += page.rowH + pad;
            page.rowH = 0;
        }
        if (page.penY + h + pad > m_atlasSize) continue;

        m_renderer->updateTextureRegion(page.tex, page.penX, page.penY, w, h, bitmap, pitch);
        const float inv = 1.0f / static_cast<float>(m_atlasSize);
        out.tex = page.tex;
        out.uv = Rect{ page.penX * inv, page.penY * inv, w * inv, h * inv };
        page.penX += w + pad;
        page.rowH = std::max(page.rowH, h);
        return true;
    }

    std::vector<uint8_t> blank(static_cast<size_t>(m_atlasSize) * m_atlasSize, 0);
    TexId tex = m_renderer->createTexture(m_atlasSize, m_atlasSize, DkImageFormat_R8_Unorm,
                                          blank.data(), Filter::Linear);
    if (tex == TexInvalid) return false;
    m_pages.push_back(Page{ tex, 0, 0, 0 });
    return packGlyph(bitmap, w, h, pitch, out);
}

const TextRenderer::Glyph& TextRenderer::glyph(uint32_t codepoint, uint32_t pxSize, bool bold)
{
    static const Glyph kEmpty{};
    const uint64_t key = (static_cast<uint64_t>(codepoint) << 20) |
                         (static_cast<uint64_t>(pxSize) << 4) | (bold ? 1u : 0u);
    auto it = m_cache.find(key);
    if (it != m_cache.end()) return it->second;

    uint32_t glyphIndex = 0;
    FT_Face face = faceFor(codepoint, &glyphIndex);
    if (!face) return kEmpty;

    Glyph g{};
    FT_Set_Pixel_Sizes(face, 0, pxSize);
    if (FT_Load_Glyph(face, glyphIndex, FT_LOAD_DEFAULT) != 0) {
        m_cache.emplace(key, g);
        return m_cache[key];
    }
    if (bold && face->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
        // ~7% of the em, which reads as the same weight step the web client
        // gets from a real bold face at these sizes.
        const FT_Pos strength = static_cast<FT_Pos>(pxSize) * 64 / 14;
        FT_Outline_Embolden(&face->glyph->outline, strength);
    }
    if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) {
        m_cache.emplace(key, g);
        return m_cache[key];
    }

    const FT_Bitmap& bmp = face->glyph->bitmap;
    g.w = static_cast<float>(bmp.width);
    g.h = static_cast<float>(bmp.rows);
    g.bearingX = static_cast<float>(face->glyph->bitmap_left);
    g.bearingY = static_cast<float>(face->glyph->bitmap_top);
    g.advance = static_cast<float>(face->glyph->advance.x) / 64.0f;
    g.valid = packGlyph(bmp.buffer, bmp.width, bmp.rows, bmp.pitch, g);

    auto res = m_cache.emplace(key, g);
    return res.first->second;
}

float TextRenderer::measure(const std::string& utf8, float size, bool bold)
{
    const uint32_t px = pixelSize(size);
    const float inv = 1.0f / m_renderer->scale();
    float width = 0;
    size_t i = 0;
    while (i < utf8.size()) {
        const uint32_t cp = utf8Next(utf8, i);
        if (cp == '\n') continue;
        width += glyph(cp, px, bold).advance;
    }
    return width * inv;
}

void TextRenderer::draw(const std::string& utf8, float x, float y, const TextStyle& style)
{
    const uint32_t px = pixelSize(style.size);
    const float scale = m_renderer->scale();
    const float inv = 1.0f / scale;
    const float baseline = y + ascent(style.size);

    float penX = x * scale;
    const float penY = baseline * scale;
    size_t i = 0;
    while (i < utf8.size()) {
        const uint32_t cp = utf8Next(utf8, i);
        if (cp == '\n') continue;
        const Glyph& g = glyph(cp, px, style.bold);
        if (g.tex != TexInvalid && g.w > 0) {
            const Rect dst{ (penX + g.bearingX) * inv, (penY - g.bearingY) * inv, g.w * inv,
                            g.h * inv };
            m_renderer->drawAlphaQuad(g.tex, dst, g.uv, style.color);
        }
        penX += g.advance;
    }
}

void TextRenderer::drawAligned(const std::string& utf8, Rect box, const TextStyle& style,
                               Align align)
{
    const float w = measure(utf8, style.size, style.bold);
    float x = box.x;
    if (align == Align::Center) x = box.x + (box.w - w) * 0.5f;
    else if (align == Align::Right) x = box.right() - w;
    draw(utf8, x, box.y, style);
}

void TextRenderer::drawEllipsized(const std::string& utf8, Rect box, const TextStyle& style,
                                  Align align)
{
    if (measure(utf8, style.size, style.bold) <= box.w) {
        drawAligned(utf8, box, style, align);
        return;
    }

    const float ellipsisW = measure(kEllipsis, style.size, style.bold);
    const float budget = box.w - ellipsisW;
    std::string out;
    float used = 0;
    size_t i = 0;
    while (i < utf8.size()) {
        const size_t start = i;
        const uint32_t cp = utf8Next(utf8, i);
        const float adv = measure(utf8.substr(start, i - start), style.size, style.bold);
        if (used + adv > budget) break;
        used += adv;
        out.append(utf8, start, i - start);
        (void)cp;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    out += kEllipsis;
    drawAligned(out, box, style, align);
}

namespace {

// Break opportunities are spaces only: the client's `line-clamp` sits on
// prose, and CJK soft-wrapping is not worth a dictionary here.
std::vector<std::string> splitWords(const std::string& s)
{
    std::vector<std::string> words;
    std::string cur;
    for (char ch : s) {
        if (ch == ' ' || ch == '\n') {
            if (!cur.empty()) words.push_back(cur);
            cur.clear();
            if (ch == '\n') words.push_back("\n");
        } else {
            cur.push_back(ch);
        }
    }
    if (!cur.empty()) words.push_back(cur);
    return words;
}

} // namespace

float TextRenderer::drawWrapped(const std::string& utf8, Rect box, const TextStyle& style,
                                int maxLines)
{
    const auto words = splitWords(utf8);
    const float lh = lineHeight(style.size);
    std::vector<std::string> lines;
    std::string line;

    for (const std::string& w : words) {
        if (w == "\n") {
            lines.push_back(line);
            line.clear();
            if (static_cast<int>(lines.size()) >= maxLines) break;
            continue;
        }
        std::string candidate = line.empty() ? w : line + " " + w;
        if (measure(candidate, style.size, style.bold) <= box.w || line.empty()) {
            line = candidate;
        } else {
            lines.push_back(line);
            line = w;
            if (static_cast<int>(lines.size()) >= maxLines) break;
        }
    }
    if (!line.empty() && static_cast<int>(lines.size()) < maxLines) lines.push_back(line);

    const bool truncated = lines.size() == static_cast<size_t>(maxLines) &&
                           measure(utf8, style.size, style.bold) > box.w * maxLines;
    for (size_t i = 0; i < lines.size(); i++) {
        const Rect lineBox{ box.x, box.y + lh * i, box.w, lh };
        if (truncated && i + 1 == lines.size())
            drawEllipsized(lines[i] + " " + kEllipsis, lineBox, style);
        else
            draw(lines[i], lineBox.x, lineBox.y, style);
    }
    return lh * lines.size();
}

float TextRenderer::measureWrappedHeight(const std::string& utf8, float width, float size,
                                         int maxLines, bool bold)
{
    const auto words = splitWords(utf8);
    const float lh = lineHeight(size);
    int count = 0;
    std::string line;
    for (const std::string& w : words) {
        if (w == "\n") {
            count++;
            line.clear();
            if (count >= maxLines) return lh * maxLines;
            continue;
        }
        std::string candidate = line.empty() ? w : line + " " + w;
        if (measure(candidate, size, bold) <= width || line.empty()) {
            line = candidate;
        } else {
            count++;
            line = w;
            if (count >= maxLines) return lh * maxLines;
        }
    }
    if (!line.empty()) count++;
    return lh * std::min(count, maxLines);
}

} // namespace gfx
