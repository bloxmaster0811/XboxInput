# XboxInput  — Use Xbox One/Series Controllers on your modded Xbox 360!
![GitHub Downloads (all assets, latest release)](https://img.shields.io/github/downloads/bloxmaster0811/XboxInput/latest/total?logo=github&link=https%3A%2F%2Fgithub.com%2Fbloxmaster0811%2FXboxInput%2Freleases%2F) ![GitHub Release](https://img.shields.io/github/v/release/bloxmaster0811/XboxInput?style=flat&logo=github&link=https%3A%2F%2Fgithub.com%2Fbloxmaster0811%2FXboxInput%2Freleases%2F)
 ![GitHub Repo stars](https://img.shields.io/github/stars/bloxmaster0811/XboxInput?style=flat&logo=github)



Use wired official Xbox One and Xbox Series controllers as standard Xbox 360
gamepads on a modded Xbox 360 .

Controllers compatible with this includes Xbox One, One S, Elite, Elite Series
2, and Series X|S wired controllers.

Full Xbox 360 Controller functionality with rumble.
Controllers with more buttons than the original do not have any mappings for the extra buttons nor can they be mapped.
Long press on the guide button is not supported.

Have a third-party Xbox One/Series Controller and want it supported? Please read the guide and make an issue using the template!
[Third-party Support Guide](https://github.com/bloxmaster0811/XboxInput/blob/main/THIRD_PARTY_CONTROLLER_SUPPORT.md)

Multiple controllers up to 4 connected are supported! Please report any problems to the issues page!

Tested working with Xbox One S controller and Xbox Series Controller

## Installation
Copy to the Hard drive or any other location to be added from within Dashlaunch or installed manually via the launch.ini

Incompatible with Hiddriver 360!

1. Download from [Here](https://github.com/bloxmaster0811/XboxInput/releases/)
2. Copy `XboxInput.xex` to the hard drive or any usb location.
3. Add from within Dashlaunch or add `pluginN = HDD:\XboxInput.xex` to the `[Plugins]` section of the
   DashLaunch `launch.ini` replacing N with the appropriate plugin index; see `launch.ini.example`.
4. Reboot with the controller disconnected. Once the dashboard is running,
   connect it by USB.

If the controller does not connect straight away unplug and replug the cable.

## Building
See `BUILDING.md`.
This project requires the Xbox 360 XDK and Visual Studio 2010. 

## Credits
Thanks to EnTim23 who made the Hiddriver360 project and helped provide the backend virtual controller configuration and mapping https://github.com/EinTim23/hiddriver360

Thanks to Durg5 who made the riffmaster-rgh360 project who saved me the headache of figuring out the GIP transport https://github.com/Durg5/riffmaster-rgh360
## licensing
This release is GPL-3.0. See `LICENSE` and `THIRD_PARTY_NOTICES.md`.
