#include "ui/Focus.h"

#include <algorithm>
#include <cmath>

namespace ui {

void FocusManager::beginFrame()
{
    m_items.clear();
    m_nodes.clear();
    m_stack.clear();
    m_building = true;
}

void FocusManager::beginContainer(uint64_t key, bool horizontal)
{
    Node node;
    node.key = key;
    node.horizontal = horizontal;
    node.parent = m_stack.empty() ? -1 : m_stack.back();
    m_nodes.push_back(node);
    const int index = static_cast<int>(m_nodes.size()) - 1;
    if (node.parent >= 0) m_nodes[node.parent].children.push_back(index);
    m_stack.push_back(index);
}

void FocusManager::endContainer()
{
    if (!m_stack.empty()) m_stack.pop_back();
}

void FocusManager::add(FocusId id, gfx::Rect rect)
{
    if (id == FocusNone || rect.empty()) return;
    const int container = m_stack.empty() ? -1 : m_stack.back();
    m_items.push_back(Item{ id, rect, container });
    if (container >= 0) m_nodes[container].items.push_back(static_cast<int>(m_items.size()) - 1);
}

void FocusManager::endFrame()
{
    m_building = false;
    // A focused item that is gone (page changed under the cursor) would leave
    // the D-pad inert, so fall back to the first thing registered.
    if (m_current != FocusNone && !has(m_current)) m_current = FocusNone;
    if (m_current == FocusNone) focusFirst();

    if (m_current != FocusNone) {
        // Remember the focus in every container on the path to the root, so
        // re-entering a rail from above lands where the user left it.
        int node = containerOf(m_current);
        while (node >= 0) {
            m_remembered[m_nodes[node].key] = m_current;
            node = m_nodes[node].parent;
        }
    }
}

void FocusManager::focusFirst()
{
    if (!m_items.empty()) m_current = m_items.front().id;
}

bool FocusManager::has(FocusId id) const { return indexOf(id) >= 0; }

int FocusManager::indexOf(FocusId id) const
{
    for (size_t i = 0; i < m_items.size(); i++)
        if (m_items[i].id == id) return static_cast<int>(i);
    return -1;
}

gfx::Rect FocusManager::rectOf(FocusId id) const
{
    const int i = indexOf(id);
    return i >= 0 ? m_items[i].rect : gfx::Rect{};
}

int FocusManager::containerOf(FocusId id) const
{
    const int i = indexOf(id);
    return i >= 0 ? m_items[i].container : -1;
}

bool FocusManager::isAncestor(int ancestor, int node) const
{
    while (node >= 0) {
        if (node == ancestor) return true;
        node = m_nodes[node].parent;
    }
    return false;
}

FocusId FocusManager::descend(int node) const
{
    if (node < 0 || node >= static_cast<int>(m_nodes.size())) return FocusNone;

    auto remembered = m_remembered.find(m_nodes[node].key);
    if (remembered != m_remembered.end()) {
        const int idx = indexOf(remembered->second);
        if (idx >= 0 && isAncestor(node, m_items[idx].container)) return remembered->second;
    }
    if (!m_nodes[node].items.empty()) return m_items[m_nodes[node].items.front()].id;
    for (int child : m_nodes[node].children) {
        const FocusId id = descend(child);
        if (id != FocusNone) return id;
    }
    return FocusNone;
}

bool FocusManager::moveInTree(Dir dir)
{
    const int start = containerOf(m_current);
    if (start < 0) return false;

    const bool horizontal = dir == Dir::Left || dir == Dir::Right;
    const int step = (dir == Dir::Right || dir == Dir::Down) ? 1 : -1;

    if (horizontal) {
        // Stepping along a rail: only the items directly inside the nearest
        // horizontal container are candidates, which is what stops Right on
        // the last card from jumping into the row below.
        int node = start;
        while (node >= 0 && !m_nodes[node].horizontal) node = m_nodes[node].parent;
        if (node < 0) return false;

        const auto& items = m_nodes[node].items;
        auto it = std::find_if(items.begin(), items.end(),
                               [&](int i) { return m_items[i].id == m_current; });
        if (it == items.end()) return false;
        const auto pos = static_cast<int>(std::distance(items.begin(), it)) + step;
        if (pos < 0 || pos >= static_cast<int>(items.size())) return false;
        m_current = m_items[items[pos]].id;
        return true;
    }

    // Vertical: walk up to the enclosing section and step to the sibling
    // row, entering it at its remembered card.
    int child = start;
    int node = m_nodes[start].parent;
    while (node >= 0 && m_nodes[node].horizontal) {
        child = node;
        node = m_nodes[node].parent;
    }
    if (node < 0) {
        // The focus sits directly in a vertical container with no row nesting.
        if (m_nodes[start].horizontal) return false;
        node = start;
        child = -1;
    }

    const Node& section = m_nodes[node];
    if (child >= 0) {
        auto it = std::find(section.children.begin(), section.children.end(), child);
        if (it != section.children.end()) {
            const auto pos = static_cast<int>(std::distance(section.children.begin(), it)) + step;
            if (pos < 0 || pos >= static_cast<int>(section.children.size())) return false;
            const FocusId id = descend(section.children[pos]);
            if (id == FocusNone) return false;
            m_current = id;
            return true;
        }
    }

    const auto& items = section.items;
    auto it = std::find_if(items.begin(), items.end(),
                           [&](int i) { return m_items[i].id == m_current; });
    if (it == items.end()) return false;
    const auto pos = static_cast<int>(std::distance(items.begin(), it)) + step;
    if (pos < 0 || pos >= static_cast<int>(items.size())) return false;
    m_current = m_items[items[pos]].id;
    return true;
}

bool FocusManager::moveByRect(Dir dir)
{
    const int fromIndex = indexOf(m_current);
    if (fromIndex < 0) return false;

    const gfx::Rect from = m_items[fromIndex].rect;
    const float fromCx = from.cx();
    const float fromCy = from.cy();
    const bool horizontal = dir == Dir::Left || dir == Dir::Right;
    const int fromContainer = m_items[fromIndex].container;

    // Three passes, scored exactly as the web client does: same-band first,
    // then a 45-degree cone with a 16x cross penalty, then anything in the
    // half-plane with a 64x penalty.
    struct Scored { float score; FocusId id; };
    std::vector<Scored> inLine, offLine, anywhere;

    for (size_t i = 0; i < m_items.size(); i++) {
        if (static_cast<int>(i) == fromIndex) continue;
        const gfx::Rect r = m_items[i].rect;
        if (r.empty()) continue;

        const float dx = r.cx() - fromCx;
        const float dy = r.cy() - fromCy;
        switch (dir) {
            case Dir::Left:  if (dx >= -4.0f) continue; break;
            case Dir::Right: if (dx <= 4.0f) continue; break;
            case Dir::Up:    if (dy >= -4.0f) continue; break;
            case Dir::Down:  if (dy <= 4.0f) continue; break;
        }

        const bool sameBand = horizontal
                                  ? (r.y < from.bottom() && r.bottom() > from.y)
                                  : (r.x < from.right() && r.right() > from.x);
        const float primary = std::fabs(horizontal ? dx : dy);
        const float cross = std::fabs(horizontal ? dy : dx);

        if (sameBand) {
            inLine.push_back(Scored{ primary, m_items[i].id });
        } else if (horizontal && fromContainer >= 0 && m_items[i].container != fromContainer &&
                   m_nodes[fromContainer].horizontal) {
            // Off-band candidates outside the active rail would let Right on
            // the last card land in another row.
            continue;
        } else if (cross <= primary) {
            offLine.push_back(Scored{ primary * primary + 16.0f * cross * cross, m_items[i].id });
        } else {
            anywhere.push_back(Scored{ primary * primary + 64.0f * cross * cross, m_items[i].id });
        }
    }

    auto pick = [&](std::vector<Scored>& v) -> bool {
        if (v.empty()) return false;
        auto best = std::min_element(v.begin(), v.end(),
                                     [](const Scored& a, const Scored& b) { return a.score < b.score; });
        m_current = best->id;
        return true;
    };

    return pick(inLine) || pick(offLine) || pick(anywhere);
}

FocusId FocusManager::hitTest(float x, float y) const
{
    for (size_t i = m_items.size(); i-- > 0;)
        if (m_items[i].rect.contains(x, y)) return m_items[i].id;
    return FocusNone;
}

bool FocusManager::focusNearest(gfx::Rect viewport)
{
    const float cx = viewport.cx();
    const float cy = viewport.cy();
    FocusId best = FocusNone;
    float bestScore = 0.0f;
    for (const Item& item : m_items) {
        if (viewport.intersect(item.rect).empty()) continue;
        const float dx = item.rect.cx() - cx;
        const float dy = item.rect.cy() - cy;
        // Vertical distance counts for more: lists run down the page, so the
        // row matters more than the column when picking where to resume.
        const float score = dx * dx + dy * dy * 4.0f;
        if (best == FocusNone || score < bestScore) {
            best = item.id;
            bestScore = score;
        }
    }
    if (best == FocusNone) return false;
    m_current = best;
    return true;
}

bool FocusManager::move(Dir dir)
{
    if (m_items.empty()) return false;
    if (m_current == FocusNone) {
        focusFirst();
        return true;
    }
    const FocusId before = m_current;
    if (!moveInTree(dir)) moveByRect(dir);
    return m_current != before;
}

} // namespace ui
