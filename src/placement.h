#pragma once
#include <windows.h>
#include <algorithm>

namespace widget {

inline int EdgeBeforePeer(int edge, const RECT& bar, const RECT& peer, bool rtl) {
    RECT overlap{};
    if (!IntersectRect(&overlap, &bar, &peer)) return edge;
    return rtl ? std::max(edge, static_cast<int>(overlap.right))
               : std::min(edge, static_cast<int>(overlap.left));
}

} // namespace widget
