# iPad Second Display

[简体中文](README.md) | English

Version **0.3.0-preview** is a self-use Windows sender for the unmodified OpenDisplay iPad receiver. It creates a Windows extended display with the separately installed Parsec VDD driver, captures and encodes that display, and sends it over USB or a trusted private Wi-Fi network. Touch and scroll input return to Windows. The app connects and streams to **one iPad at a time**; configured devices can serve as ordered fallback targets.

This preview uses the native Windows desktop on the extended display, a scrollable control panel, stable receiver IDs, and network-scoped MAC-assisted address fallback. Configuration v3 preserves connection preferences while disabling the old shortcut launcher and taskbar routing.

Download the versioned installer or portable EXE from [Releases](https://github.com/tukitohosi/ipad-second-display/releases) and compare its SHA-256 with `SHA256SUMS-0.3.0-preview.txt`. The installer is unsigned and does not install or remove a display driver or change firewall or network settings. See the [Chinese setup and safety instructions](README.md) before using Parsec VDD.

The Windows Release build, 10 local CTest checks, and isolated UI preview passed. Real-iPad operation, a 60-minute stream, and installation on a clean Windows machine have **not** been verified for this version. The receiver must stay in the foreground; Wi-Fi uses OpenDisplay v3's unencrypted protocol on a trusted private network. This app does not control native iPadOS apps, stream audio, or share the clipboard.

Build with Visual Studio or Build Tools C++, a Windows SDK, and CMake 3.25 or newer:

```powershell
.\scripts\build.ps1
.\scripts\package.ps1 -Version 0.3.0-preview
```

The corresponding source and third-party notices are included in this repository and the release source ZIP. See [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).
