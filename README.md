# XboxInput  — Xbox 360 DashLaunch plugin

Use wired official Xbox One and Xbox Series controllers as standard Xbox 360
gamepads on a modded Xbox 360 running retail dashboard 2.0.17559.0.


Controllers compatible with this includes Xbox One, One S, Elite, Elite Series
2, and Series X|S wired controllers.

Full Xbox 360 Controller functionality with rumble.
Controllers with more buttons than the original do not have any mappings nor can they be mapped.
Long press on the guide button is not supported with the current setup.

Tested working with Xbox One S controller and Xbox Series Controller

## Install
Copy to the Hard drive or any other location to be added from within dashlaunch or installed manually via the launch.ini

1. Copy `XboxInput.xex` to the hard drive or any usb location.
2. Add `pluginN = HDD:\XboxInputGip.xex` to the `[Plugins]` section of the
   DashLaunch `launch.ini`; see `launch.ini.example`.
3. Reboot with the controller disconnected. Once the dashboard is running,
   connect it by USB.

The controller hook isn't very consistent. If the controller does not connect straight away unplug and replug the cable.

## Building
See `BUILDING.md`.

## Source and licensing
This project requires the official Xbox 360 XDK and Visual Studio 2010 Xbox 360 toolset. 

This release is GPL-3.0. See `LICENSE` and `THIRD_PARTY_NOTICES.md`.
