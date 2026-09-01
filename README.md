# XboxInput GIP — Xbox 360 DashLaunch plugin

Use wired official Xbox One and Xbox Series controllers as standard Xbox 360
gamepads on a modded Xbox 360 running retail dashboard 2.0.17559.0.

The included `XboxInputGip-17559-retail.xex` is retail-converted and ready for
DashLaunch. It supports ABXY, D-pad, Menu/Start, View/Back, bumpers, stick
clicks, triggers, both analogue sticks, player assignment, controller sleep /
reconnect, Guide tap, and rumble.

Supported wired Microsoft GIP controller IDs are `045E:02D1`, `02DD`, `02E3`,
`02EA`, `0B00`, and `0B12`. This includes Xbox One, One S, Elite, Elite Series
2, and Series X|S wired controllers.

## Install

1. Disable any old XboxInput plugin in DashLaunch and reboot.
2. Copy `XboxInputGip-17559-retail.xex` to `HDD:\XboxInputGip.xex`.
3. Add `pluginN = HDD:\XboxInputGip.xex` to the `[Plugins]` section of the
   DashLaunch `launch.ini`; see `launch.ini.example`.
4. Reboot with the controller disconnected. Once the dashboard is running,
   connect it by USB.

Do not overwrite the active plugin through FTP. Disable it, reboot, copy the
replacement, then enable it and reboot again.

The controller’s Guide button supports a normal tap. The native held-Guide
power menu is not available through the retail 17559 virtual-controller API.
Headset/audio and Share have no matching Xbox 360 controller feature.

## Source and licensing

`source/` is a self-contained native C++ Xbox 360 project. It requires the
official Xbox 360 XDK and Visual Studio 2010 Xbox 360 toolset. See
`BUILDING.md`.

This release is GPL-3.0. See `LICENSE` and `THIRD_PARTY_NOTICES.md`.
