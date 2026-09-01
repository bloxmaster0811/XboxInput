# Building

Requirements:

- Official Xbox 360 XDK with the Xbox 360 Visual C++ toolset.
- Visual Studio 2010 (or a compatible setup that provides toolset `2010-01`).
- XexTool for retail conversion.

Open `source/riffmaster.vcxproj` and build **Release Retail | Xbox 360**. The
project produces `source/Release Retail/riffmaster.xex`.

Convert it for retail DashLaunch use:

```text
XexTool.exe -r a -m r "Release Retail\riffmaster.xex"
```

Deploy the resulting XEX using the install instructions in `README.md`.

The project intentionally skips USB-stack reset at plugin load. Boot to the
dashboard first, then connect the controller.
