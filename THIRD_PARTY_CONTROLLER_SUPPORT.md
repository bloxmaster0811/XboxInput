# Request third-party controller support

XboxInput currently supports the official wired Microsoft Xbox One and Xbox Series
controller families listed in the README. Third-party controllers are not enabled by
default because different brands can use different USB protocols, input formats and
rumble commands even when they look like an Xbox controller.

This guide explains how to submit a useful compatibility request without risking your
normal XboxInput installation.

## Before you begin

- This process is for **wired USB controllers** only.
- Do not run the normal plugin and the compatibility probe at the same time.
- Do not run any other plugins such as hiddriver360
- Download the probe 

https://github.com/bloxmaster0811/XboxInput/releases/download/v1.2/XboxInputCompatProbe.xex

## Run the compatibility probe

1. Copy `XboxInputCompatProbe.xex` to the console, for example `Hdd:\XboxInputCompatProbe.xex`.
2. Add only the probe as a plugin, then reboot.
3. Wait until the dashboard is fully loaded.
4. Plug in only the controller you want checked. Do not plug in USB storage or other
   controllers during the probe.
5. Wait at least five seconds.
6. Retrieve `HDD:\XboxInputCompatProbe.log` with FTP or USB.
7. Remove the probe entry, and reboot.

The probe is observation only; Its designed to record only the USB information given to the console in order to assess it.

## Open a GitHub issue

Open a new issue using the **ControllerCompatibility** template.
Use a clear title such as:

`[Compatibility] PowerA Enhanced Wired Controller for Xbox Series X|S`

Include all of the following:

- Exact manufacturer and model name.
- A product link or clear label photo, if available.
- Whether it is wired-only, Bluetooth-capable, or uses a wireless adapter.
- Whether it works on an Xbox One/Series console or on a PC.
- Firmware version and USB product ID if known.
- The complete contents of `XboxInputCompatProbe.log`.

## What happens next

The report will be classified as one of these:

| Result | Meaning |
| --- | --- |
| Candidate GIP device | It may be close to the supported wired protocol, but still needs packet and rumble testing. |
| Candidate HID device | It may fit the existing HID path, but needs a HID report descriptor and input capture. |
| New proprietary transport | It needs a model-specific driver path and may require access to physical hardware. |
| Unsupported connection type | Bluetooth and Xbox Wireless Adapter devices require separate transport support. |

Submitting a probe log does not guarantee that a controller can be supported. Compatibility
is added model-by-model and must be tested by you before it is enabled in the normal release.
