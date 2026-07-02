# StarRupture-Plugin-DecorationRemover

A world decoration remover plugin for the [StarRupture ModLoader](https://github.com/AlienXAXS) targeting the Chimera (UE5) game. It removes all decorations around the world — rocks, trees, and other environmental clutter — giving you a clean, unobstructed building space.

## Features

- Removes decorative world actors such as rocks and trees
- Configurable via `DecorationRemover.ini` (auto-generated on first run)

## Requirements

- Visual Studio 2022 (MSVC v143, C++20)
- [StarRupture-Game-SDK](https://github.com/AlienXAXS/StarRupture-Game-SDK)
- [StarRupture-Plugin-SDK](https://github.com/AlienXAXS/StarRupture-Plugin-SDK)
- StarRupture with ModLoader installed

## Building

Open `StarRupture-Plugin-DecorationRemover.sln` in Visual Studio 2022 and build, or use MSBuild from the repository root:

```bat
msbuild StarRupture-Plugin-DecorationRemover.sln /p:Configuration="Client Debug" /p:Platform=x64
```

Output is placed at `build/<Configuration>/Plugins/DecorationRemover.dll`.

### Configurations

| Configuration | Description |
|---|---|
| `Client Debug` | Client mod build with debug symbols |
| `Client Release` | Optimised client mod build |
| `Server Debug` | Server-side build with debug symbols |
| `Server Release` | Optimised server-side build |
| `Local SDK Client Debug/Release` | Client build using a local SDK checkout |
| `Local SDK Server Debug/Release` | Server build using a local SDK checkout |

SDK checkout paths can be adjusted in [`Shared.props`](Shared.props) to match your local directory layout.

## Installation

1. Build the DLL (see above).
2. Copy `DecorationRemover.dll` into the game's `Plugins/` directory alongside the other StarRupture mod DLLs.
3. Launch the game — the ModLoader will load the plugin automatically.

On first run, `DecorationRemover.ini` is generated in `<game_dir>/Plugins/config/` with default values.

## Configuration

`DecorationRemover.ini` is auto-created on first run. Available options:

| Section | Key | Default | Description |
|---|---|---|---|
| `General` | `Enabled` | `true` | Enable or disable the plugin entirely |

## Key Files

| File | Role |
|---|---|
| [`plugin.cpp`](DecorationRemover/plugin.cpp) | `GetPluginInfo` / `PluginInit` / `PluginShutdown` C exports |
| [`plugin_config.h`](DecorationRemover/plugin_config.h) | Schema-based `DecorationRemover.ini` config with typed accessors |
| [`plugin_helpers.h`](DecorationRemover/plugin_helpers.h) | Logging macros and helper functions |
| [`dllmain.cpp`](DecorationRemover/dllmain.cpp) | Standard Windows DLL entry point |

## License

This project is provided as-is for modding purposes. See the StarRupture ModLoader documentation for distribution terms.
