#pragma once

#include <algorithm>
#include <limits>
#include <windows.h>

namespace od {

inline int ClampPanelScroll(long long requested, int contentHeight, int pageHeight)
{
    const long long maximum = (std::max)(0LL,
        static_cast<long long>((std::max)(0, contentHeight)) - (std::max)(1, pageHeight));
    return static_cast<int>((std::clamp)(requested, 0LL, maximum));
}

// Consume one wheel message and return one final position. Positive wheel
// deltas move upward; sub-notch input is retained until it reaches a notch.
inline int PanelWheelScroll(int current, int wheelDelta, unsigned lines, int linePixels,
                            int pageHeight, int contentHeight, int& remainder)
{
    const long long accumulated = static_cast<long long>(remainder) + wheelDelta;
    const long long steps = accumulated / WHEEL_DELTA;
    remainder = static_cast<int>(accumulated % WHEEL_DELTA);
    current = ClampPanelScroll(current, contentHeight, pageHeight);
    if (steps == 0 || lines == 0) return current;

    const long long pixelsPerStep = lines == WHEEL_PAGESCROLL
        ? (std::max)(1, pageHeight)
        : static_cast<long long>(lines) * (std::max)(0, linePixels);
    const int maximum = ClampPanelScroll((std::numeric_limits<int>::max)(), contentHeight, pageHeight);
    // A single step longer than the entire scroll range already reaches an
    // edge. Capping it here keeps even extreme input/settings multiplication
    // within 64 bits without changing the clamped result.
    const long long movement = steps * (std::min)(pixelsPerStep, static_cast<long long>(maximum));
    return ClampPanelScroll(static_cast<long long>(current) - movement, contentHeight, pageHeight);
}

inline bool ShouldRevealPanelFocus(UINT message, bool focusChanged)
{
    return focusChanged && message >= WM_KEYFIRST && message <= WM_KEYLAST;
}

} // namespace od
