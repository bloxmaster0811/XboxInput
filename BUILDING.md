# Building

Requirements:

- Official Xbox 360 XDK with the Xbox 360 Visual C++ toolset.
- Visual Studio 2010 SP1.
- XexTool for retail conversion.

Open `source/XboxInput.vcxproj` and build **Release Retail | Xbox 360**. The
project produces `source/Release Retail/XboxInput.xex`.

Alternatively you can use MSBuild to build it from the command line rather than opening up visual studio.

Convert it for retail DashLaunch use:

```text
XexTool.exe -r a -m r "Release Retail\XboxInput.xex"
```

Deploy the resulting XEX using the install instructions in `README.md`.

