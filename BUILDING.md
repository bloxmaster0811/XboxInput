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

Alternatively you can use the script included.

```text
.\build-release.ps1 -MsBuildPath 'C:\Path\To\MSBuild.exe' -XexToolPath 'C:\Tools\XexTool.exe'
```