#pragma once

#include <optional>
#include <string>
#include <vector>

#include <windows.h>

namespace od {

// Runs the tray-icon GUI: a hidden window hosting a SenderApp, a notification
// icon with a Connect/Disconnect/Settings/Exit menu, and a settings dialog for
// IP/port/auto-connect. Blocks on the message loop until the user exits.
// With exitWhenControlPanelCloses=true, uses isolated sample data and performs
// no configuration writes, device discovery, connection or desktop routing.
// IPAD_UI_PREVIEW_SCENE=empty|long selects extra preview scenarios.
// IPAD_UI_PREVIEW_WIDTH/HEIGHT are logical client dimensions; optional
// IPAD_UI_PREVIEW_DPI simulates geometry and fonts without changing Windows DPI.
// Returns the process exit code.
int RunTray(HINSTANCE hInstance,
            std::optional<std::vector<std::string>> resumeAddresses = std::nullopt,
            bool showControlPanelAtStartup = false,
            bool exitWhenControlPanelCloses = false);

} // namespace od
