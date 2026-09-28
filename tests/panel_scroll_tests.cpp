#ifdef NDEBUG
#undef NDEBUG
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "app/PanelScroll.h"

#include <cassert>
#include <limits>

int main()
{
    using namespace od;
    constexpr int content = 1800;
    constexpr int page = 600;
    assert(ClampPanelScroll(-1, content, page) == 0);
    assert(ClampPanelScroll(1200, content, page) == 1200);
    assert(ClampPanelScroll(1201, content, page) == 1200);
    assert(ClampPanelScroll(100, 200, page) == 0);
    assert(ClampPanelScroll(100, -1, page) == 0);
    assert(ClampPanelScroll(100, 200, 0) == 100);
    assert(ClampPanelScroll((std::numeric_limits<long long>::min)(), content, page) == 0);
    assert(ClampPanelScroll((std::numeric_limits<long long>::max)(), content, page) == 1200);

    int remainder = 0;
    assert(PanelWheelScroll(0, WHEEL_DELTA, 3, 32, page, content, remainder) == 0);
    assert(PanelWheelScroll(1200, -WHEEL_DELTA, 3, 32, page, content, remainder) == 1200);
    // One event with several notches is aggregated into one final position.
    assert(PanelWheelScroll(600, -3 * WHEEL_DELTA, 3, 32, page, content, remainder) == 888);
    assert(PanelWheelScroll(600, 2 * WHEEL_DELTA, 3, 32, page, content, remainder) == 408);

    assert(PanelWheelScroll(600, 40, 3, 32, page, content, remainder) == 600);
    assert(remainder == 40);
    assert(PanelWheelScroll(600, 40, 3, 32, page, content, remainder) == 600);
    assert(remainder == 80);
    assert(PanelWheelScroll(600, 40, 3, 32, page, content, remainder) == 504);
    assert(remainder == 0);
    assert(PanelWheelScroll(600, -80, 3, 32, page, content, remainder) == 600);
    assert(remainder == -80);
    assert(PanelWheelScroll(600, 80, 3, 32, page, content, remainder) == 600);
    assert(remainder == 0);
    assert(PanelWheelScroll(600, -60, 3, 32, page, content, remainder) == 600);
    assert(PanelWheelScroll(600, -60, 3, 32, page, content, remainder) == 696);
    assert(remainder == 0);

    assert(PanelWheelScroll(600, WHEEL_DELTA, WHEEL_PAGESCROLL, 32, page, content, remainder) == 0);
    assert(PanelWheelScroll(0, -WHEEL_DELTA, WHEEL_PAGESCROLL, 32, page, content, remainder) == 600);
    assert(PanelWheelScroll(0, -2 * WHEEL_DELTA, WHEEL_PAGESCROLL, 32, page, content, remainder) == 1200);
    assert(PanelWheelScroll(600, -WHEEL_DELTA, 0, 32, page, content, remainder) == 600);
    assert(PanelWheelScroll(600, -WHEEL_DELTA, 3, 0, page, content, remainder) == 600);
    assert(PanelWheelScroll(600, -WHEEL_DELTA, 3, -1, page, content, remainder) == 600);
    // The caller supplies DPI-scaled line size and pixel dimensions.
    assert(PanelWheelScroll(1200, -WHEEL_DELTA, 3, 64, 1200, 3600, remainder) == 1392);

    const int largest = (std::numeric_limits<int>::max)();
    const int smallest = (std::numeric_limits<int>::min)();
    const unsigned mostLines = (std::numeric_limits<unsigned>::max)() - 1;
    remainder = largest;
    assert(PanelWheelScroll(600, largest, mostLines, largest, page, content, remainder) == 0);
    assert(remainder >= 0 && remainder < WHEEL_DELTA);
    remainder = smallest;
    assert(PanelWheelScroll(600, smallest, mostLines, largest, page, content, remainder) == 1200);
    assert(remainder <= 0 && remainder > -WHEEL_DELTA);
    remainder = 0;
    assert(PanelWheelScroll(largest, smallest, mostLines, largest, 1, largest, remainder) == largest - 1);

    for (UINT message : {WM_KEYDOWN, WM_KEYUP, WM_CHAR, WM_SYSKEYDOWN, WM_SYSCHAR}) {
        assert(ShouldRevealPanelFocus(message, true));
        assert(!ShouldRevealPanelFocus(message, false));
    }
    for (UINT message : {WM_MOUSEMOVE, WM_MOUSEWHEEL, WM_LBUTTONDOWN, WM_VSCROLL, WM_TIMER, WM_PAINT}) {
        assert(!ShouldRevealPanelFocus(message, true));
        assert(!ShouldRevealPanelFocus(message, false));
    }
    return 0;
}
