#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "gfx/Color.h"

namespace ui {

enum class Dir { Left, Right, Up, Down };

using FocusId = uint64_t;
constexpr FocusId FocusNone = 0;

inline FocusId focusId(uint32_t kind, uint32_t index)
{
    return (static_cast<FocusId>(kind) << 32) | index;
}

// Port of the web client's `tv-spatial-nav`: an opt-in container tree decides
// the common cases (step along a rail, drop into the next rail at its
// remembered card) and a three-pass rect score handles everything outside it.
class FocusManager {
public:
    void beginFrame();

    // `key` must be stable across frames — it is what carries the
    // remembered child when focus leaves and comes back.
    void beginContainer(uint64_t key, bool horizontal);
    void endContainer();

    void add(FocusId id, gfx::Rect rect);
    void endFrame();

    FocusId current() const { return m_current; }
    void setCurrent(FocusId id) { m_current = id; }
    bool isFocused(FocusId id) const { return m_current == id && id != FocusNone; }
    bool has(FocusId id) const;
    gfx::Rect rectOf(FocusId id) const;
    gfx::Rect currentRect() const { return rectOf(m_current); }

    // The item under a point, or FocusNone. Answers against the rects
    // registered by the last frame's draw, which is the layout the user was
    // actually looking at when they touched the screen. Later items win, so
    // something drawn over another thing takes the tap.
    FocusId hitTest(float x, float y) const;

    // The visible item nearest the middle of `viewport`. After a touch
    // scroll the cursor is wherever the pad left it, usually far off screen;
    // resuming on the pad should carry on from what the user is looking at,
    // not jump back.
    bool focusNearest(gfx::Rect viewport);

    bool move(Dir dir);
    // Used when a screen is entered or its content changed under the cursor.
    void focusFirst();

private:
    struct Item {
        FocusId id;
        gfx::Rect rect;
        int container;
    };

    struct Node {
        uint64_t key;
        bool horizontal;
        int parent;
        std::vector<int> items;      // indices into m_items
        std::vector<int> children;   // indices into m_nodes
    };

    int containerOf(FocusId id) const;
    int indexOf(FocusId id) const;
    bool isAncestor(int ancestor, int node) const;
    FocusId descend(int node) const;
    bool moveInTree(Dir dir);
    bool moveByRect(Dir dir);

    std::vector<Item> m_items;
    std::vector<Node> m_nodes;
    std::vector<int> m_stack;
    std::unordered_map<uint64_t, FocusId> m_remembered;
    FocusId m_current = FocusNone;
    bool m_building = false;
};

} // namespace ui
