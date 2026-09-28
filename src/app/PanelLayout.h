#pragma once

#include <algorithm>
#include <array>
#include <windows.h>

namespace od {

// All panel geometry is calculated here. Painting and native child controls
// share these rectangles, so scrolling and DPI changes cannot drift apart.
struct PanelLayout {
    bool compact = false;
    int contentHeight = 0;
    RECT title{}, subtitle{}, themeButton{};
    RECT statusCard{}, statusTitle{}, statusValue{}, statusDevice{}, statusDetail{}, statusResolution{};
    RECT deviceCard{}, deviceTitle{}, deviceSelector{}, deviceAddressLabel{}, deviceAddress{}, deviceMacLabel{}, deviceMac{};
    RECT deviceConnectButton{}, deviceDefaultButton{}, deviceUpButton{}, deviceRemoveButton{};
    RECT primaryConnectionButton{}, secondaryConnectionButton{};
    RECT settingsCard{}, settingsTitle{}, advancedSettingsButton{}, discoveryLabel{}, discoveryInput{}, discoveryAddressLabel{}, discoveryAddress{}, addDiscoveredButton{};
    RECT devicesLabel{}, devicesInput{}, portLabel{}, portInput{}, fpsLabel{}, fpsInput{}, bitrateLabel{}, bitrateInput{};
    RECT autoReconnectToggle{}, privateNetworkToggle{}, saveSettingsButton{};
    RECT qualityCard{}, qualityTitle{}, qualityHint{}, health{}, displayHint{};
    std::array<RECT, 3> profileCards{};
    RECT displaySettingsButton{}, placeRightButton{}, diagnosticsButton{}, notice{}, footer{};
};

inline PanelLayout CalculatePanelLayout(int clientWidth, unsigned dpi, bool advanced)
{
    dpi = dpi == 0 ? 96 : dpi;
    const auto scale = [dpi](int value) { return static_cast<LONG>((static_cast<long long>(value) * dpi + 48) / 96); };
    const auto rect = [&scale](int x, int y, int w, int h) { return RECT{scale(x), scale(y), scale(x + w), scale(y + h)}; };
    const int width = (std::max)(360, static_cast<int>(static_cast<long long>(clientWidth) * 96 / dpi));
    PanelLayout p;
    p.compact = width < 800;
    const int margin = p.compact ? 20 : 28;
    const int outer = width - margin * 2;
    const int left = margin + 22;
    const int inner = outer - 44;
    const int gap = 12;
    p.title = rect(margin, 24, outer - 118, 32);
    p.subtitle = rect(margin, 64, outer, 24);
    p.themeButton = rect(width - margin - 98, 26, 98, 34);

    int top = 108;
    const int statusHeight = p.compact ? 178 : 150;
    p.statusCard = rect(margin, top, outer, statusHeight);
    p.statusTitle = rect(left, top + 16, inner, 22);
    p.statusValue = rect(left, top + 47, p.compact ? inner : inner - 260, 32);
    p.statusDevice = rect(left, top + 86, inner, 24);
    p.statusResolution = rect(p.compact ? left : width - margin - 242, top + (p.compact ? 114 : 49), p.compact ? inner : 220, 25);
    p.statusDetail = rect(left, top + (p.compact ? 143 : 117), inner, 23);

    top += statusHeight + 16;
    const int actionsHeight = p.compact ? 84 : 38;
    const int deviceHeight = p.compact ? 330 : 238;
    p.deviceCard = rect(margin, top, outer, deviceHeight);
    p.deviceTitle = rect(left, top + 16, inner, 24);
    p.deviceSelector = rect(left, top + 50, inner, 36);
    const int columns = p.compact ? 3 : 6;
    const int actionWidth = (inner - gap * (columns - 1)) / columns;
    std::array<RECT*, 6> actions{&p.deviceConnectButton, &p.primaryConnectionButton, &p.secondaryConnectionButton,
                                &p.deviceDefaultButton, &p.deviceUpButton, &p.deviceRemoveButton};
    for (int i = 0; i < 6; ++i)
        *actions[i] = rect(left + (i % columns) * (actionWidth + gap), top + 98 + (i / columns) * 46, actionWidth, 36);
    const int detailsTop = top + 98 + actionsHeight + 14;
    const int detailWidth = p.compact ? inner : (inner - 20) / 2;
    p.deviceAddressLabel = rect(left, detailsTop, detailWidth, 20);
    p.deviceAddress = rect(left, detailsTop + 25, detailWidth, 30);
    const int macLeft = p.compact ? left : left + detailWidth + 20;
    const int macTop = detailsTop + (p.compact ? 65 : 0);
    p.deviceMacLabel = rect(macLeft, macTop, detailWidth, 20);
    p.deviceMac = rect(macLeft, macTop + 25, detailWidth, 30);

    top += deviceHeight + 16;
    const int discoveryTop = top + 66;
    const int advancedHeight = advanced ? 164 : 0;
    const int settingsHeight = (p.compact ? 354 : 298) + advancedHeight;
    p.settingsCard = rect(margin, top, outer, settingsHeight);
    p.settingsTitle = rect(left, top + 16, inner - 140, 25);
    p.advancedSettingsButton = rect(width - margin - 150, top + 13, 128, 34);
    p.discoveryLabel = rect(left, discoveryTop - 14, inner, 20);
    p.discoveryInput = rect(left, discoveryTop + 14, inner - 132, 36);
    p.addDiscoveredButton = rect(left + inner - 120, discoveryTop + 14, 120, 36);
    p.discoveryAddressLabel = rect(left, discoveryTop + 62, inner, 20);
    p.discoveryAddress = rect(left, discoveryTop + 86, inner, 30);
    int fieldTop = discoveryTop + 132;
    if (advanced) {
        p.devicesLabel = rect(left, fieldTop, inner, 20);
        p.devicesInput = rect(left, fieldTop + 25, inner, 56);
        const int fieldWidth = (inner - 2 * gap) / 3;
        p.portLabel = rect(left, fieldTop + 93, fieldWidth, 20);
        p.portInput = rect(left, fieldTop + 118, fieldWidth, 32);
        p.fpsLabel = rect(left + fieldWidth + gap, fieldTop + 93, fieldWidth, 20);
        p.fpsInput = rect(left + fieldWidth + gap, fieldTop + 118, fieldWidth, 32);
        p.bitrateLabel = rect(left + (fieldWidth + gap) * 2, fieldTop + 93, fieldWidth, 20);
        p.bitrateInput = rect(left + (fieldWidth + gap) * 2, fieldTop + 118, fieldWidth, 32);
        fieldTop += advancedHeight;
    }
    p.autoReconnectToggle = rect(left, fieldTop, p.compact ? inner : 230, 28);
    p.privateNetworkToggle = rect(p.compact ? left : left + 242, fieldTop + (p.compact ? 36 : 0), p.compact ? inner : inner - 242, 28);
    p.saveSettingsButton = rect(left, fieldTop + (p.compact ? 80 : 44), p.compact ? inner : 170, 38);

    top += settingsHeight + 16;
    const int qualityHeight = p.compact ? 368 : 256;
    p.qualityCard = rect(margin, top, outer, qualityHeight);
    p.qualityTitle = rect(left, top + 16, inner, 25);
    p.qualityHint = rect(left, top + 48, inner, 22);
    const int profileWidth = p.compact ? inner : (inner - 2 * gap) / 3;
    for (int i = 0; i < 3; ++i)
        p.profileCards[i] = rect(left + (p.compact ? 0 : i * (profileWidth + gap)), top + 82 + (p.compact ? i * 46 : 0), profileWidth, 36);
    const int infoTop = top + (p.compact ? 225 : 134);
    p.health = rect(left, infoTop, inner, 22);
    p.displayHint = rect(left, infoTop + 30, inner, 36);
    const int utilityWidth = (inner - 2 * gap) / 3;
    p.displaySettingsButton = rect(left, infoTop + 77, utilityWidth, 38);
    p.placeRightButton = rect(left + utilityWidth + gap, infoTop + 77, utilityWidth, 38);
    p.diagnosticsButton = rect(left + (utilityWidth + gap) * 2, infoTop + 77, utilityWidth, 38);
    top += qualityHeight + 14;
    p.notice = rect(margin, top, outer, 44);
    p.footer = rect(margin, top + 50, outer, 24);
    p.contentHeight = static_cast<int>(scale(top + 94));
    return p;
}

} // namespace od
