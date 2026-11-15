# Release build

The `build-release.ps1` script builds Rufus in **Release** configuration (not Debug) using Visual Studio's MSBuild.

## Requirements

- Visual Studio 2022 or newer (Community is fine), with the **Desktop development with C++** workload.
- For ARM64 builds, the **MSVC ARM64 build tools** component is also required.

The script finds MSBuild automatically through `vswhere`, so you don't need to open a *Developer PowerShell*.

## Quick start (x64)

From the repository root:

```powershell
.\build-release.ps1
```

This produces a **Release x64** build. The executable is written to:

```
x64\Release\rufus.exe
```

## x64 examples

| Goal | Command |
|---|---|
| Incremental Release x64 build (default) | `.\build-release.ps1` |
| Same, with the platform stated explicitly | `.\build-release.ps1 -Platform x64` |
| Full rebuild (clean and recompile everything) | `.\build-release.ps1 -Rebuild` |
| Force the VS 2022 toolset | `.\build-release.ps1 -Toolset v143` |
| Force the VS 2026 toolset | `.\build-release.ps1 -Toolset v145` |
| Full rebuild with a specific toolset | `.\build-release.ps1 -Rebuild -Toolset v143` |

### Build and run straight away

```powershell
.\build-release.ps1; if ($?) { .\x64\Release\rufus.exe }
```

### Build and copy the executable to another folder

```powershell
.\build-release.ps1
Copy-Item .\x64\Release\rufus.exe C:\Tools\rufus.exe -Force
```

### Check the version and hash of the built executable

```powershell
(Get-Item .\x64\Release\rufus.exe).VersionInfo.FileVersion
Get-FileHash .\x64\Release\rufus.exe -Algorithm SHA256
```

## Other platforms

```powershell
.\build-release.ps1 -Platform x86     # -> x86\Release\rufus.exe
.\build-release.ps1 -Platform arm64   # -> arm64\Release\rufus.exe
```

## Parameters

| Parameter | Values | Default | Description |
|---|---|---|---|
| `-Platform` | `x64`, `x86`, `arm64` | `x64` | Target architecture. |
| `-Toolset` | e.g. `v143`, `v145` | automatic | MSVC platform toolset to use. |
| `-Rebuild` | switch | off | Full rebuild instead of an incremental build. |

## Toolset (v143 vs v145)

The project (`.vs\rufus.vcxproj`) is set to the **v145** toolset (Visual Studio 2026). If that toolset isn't installed, the script uses the newest one it finds (for example **v143** from VS 2022) and prints a warning:

```
WARNING: Project toolset 'v145' is not installed; using 'v143' instead.
```

The override applies to that build only; the project files are not modified. To pick the toolset yourself, pass `-Toolset`.

## Troubleshooting

**"running scripts is disabled on this system"**: the PowerShell execution policy is blocking the script. Run it like this:

```powershell
powershell -ExecutionPolicy Bypass -File .\build-release.ps1
```

To allow local scripts permanently (for your user only):

```powershell
Set-ExecutionPolicy -Scope CurrentUser RemoteSigned
```

**"No Visual Studio installation with MSBuild and C++ tools found."**: install the *Desktop development with C++* workload from the Visual Studio Installer.

**Odd linker errors after switching toolsets**: do a full rebuild:

```powershell
.\build-release.ps1 -Rebuild
```

## Notes

- The output folders (`x64\`, `x86\`, `arm64\`) are in `.gitignore`, so built binaries never end up in commits.
- The Release build uses optimizations and link-time code generation, so it takes longer than a Debug build.
