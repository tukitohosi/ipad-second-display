#ifdef NDEBUG
#undef NDEBUG
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "app/PanelLayout.h"

#include <cassert>
#include <vector>

namespace {
bool Overlaps(const RECT& a, const RECT& b)
{
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}
}

int main()
{
    for (unsigned dpi : {96u, 120u, 144u, 192u}) {
        for (int logicalWidth : {590, 620, 799, 800, 960, 1280, 1920}) {
            for (bool advanced : {false, true}) {
                const int width = logicalWidth * static_cast<int>(dpi) / 96;
                const auto p = od::CalculatePanelLayout(width, dpi, advanced);
                assert(p.compact == (width * 96 / static_cast<int>(dpi) < 800));
                std::vector<RECT> contents{
                    p.title, p.subtitle, p.themeButton, p.statusTitle, p.statusValue, p.statusDevice,
                    p.statusDetail, p.statusResolution, p.deviceTitle, p.deviceSelector,
                    p.deviceAddressLabel, p.deviceAddress, p.deviceMacLabel, p.deviceMac,
                    p.deviceConnectButton, p.deviceDefaultButton, p.deviceUpButton, p.deviceRemoveButton,
                    p.primaryConnectionButton, p.secondaryConnectionButton, p.settingsTitle,
                    p.advancedSettingsButton, p.discoveryLabel, p.discoveryInput, p.discoveryAddressLabel,
                    p.discoveryAddress, p.addDiscoveredButton, p.autoReconnectToggle,
                    p.privateNetworkToggle, p.saveSettingsButton, p.qualityTitle, p.qualityHint,
                    p.health, p.displayHint, p.displaySettingsButton, p.placeRightButton,
                    p.diagnosticsButton, p.notice, p.footer,
                };
                contents.insert(contents.end(), p.profileCards.begin(), p.profileCards.end());
                if (advanced) {
                    for (const RECT& rect : {p.devicesLabel, p.devicesInput, p.portLabel, p.portInput,
                                           p.fpsLabel, p.fpsInput, p.bitrateLabel, p.bitrateInput})
                        contents.push_back(rect);
                }
                for (size_t i = 0; i < contents.size(); ++i) {
                    const RECT& rect = contents[i];
                    assert(rect.right > rect.left && rect.bottom > rect.top);
                    assert(rect.left >= 0 && rect.right <= width);
                    assert(rect.top >= 0 && rect.bottom < p.contentHeight);
                    for (size_t j = i + 1; j < contents.size(); ++j)
                        assert(!Overlaps(rect, contents[j]));
                }
                for (int viewport : {360, 480, 800, 1080}) {
                    const int height = viewport * static_cast<int>(dpi) / 96;
                    const int maximumScroll = (std::max)(0, p.contentHeight - height);
                    assert(p.footer.bottom - maximumScroll <= height);
                }
                assert(p.contentHeight >= od::CalculatePanelLayout(width, dpi, false).contentHeight);
            }
        }
    }
    return 0;
}
