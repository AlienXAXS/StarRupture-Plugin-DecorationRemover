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
| `Debug` | `EntityDump` | `false` | Enable the F9 entity dump (see below) |
| `Debug` | `EntityDumpRadius` | `0` | Only dump entities within this many world units of the player (100 units = 1 m). `0` dumps the whole world |
| `Debug` | `EntityDumpInstances` | `true` | Include per-instance coordinates. Turn off for a much smaller file with components and counts only |

## Debug entity dump

With `[Debug] EntityDump` enabled, **F9** writes a pretty-printed JSON snapshot of the world to
`Plugins/EntityDumps/EntityDump_<timestamp>.json`. It is read-only — nothing in the world is modified — but a
whole-world walk hitches the game for a moment and can produce a very large file, so set `EntityDumpRadius`
when you only care about what is around you.

Sections in the dump:

| Section | Contents |
|---|---|
| `massVisualization` | Every `UMassVisualizationComponent`: its authored mesh descriptions and its live ISM components, with mesh name, instance count, and per-instance world coordinates |
| `massAgentActors` | Actors carrying a `UMassAgentComponent` — actor name, class, and world location |
| `massPersistentEntities` | The `CrMassPersistentIDSubsystem` roster: every persistent Mass entity handle (index, serial number) and its stable ID |
| `biomesContainers` / `biomesStandaloneSpawners` | The biomes decoration system: species names, mesh assets, spawn distances, instance components and coordinates, plus whether the current filters would remove each species |
| `summary` | Per-section counts, plus whether the instance rows were capped |

Note on coverage: a plugin cannot reach `FMassEntityManager`, the archetype/chunk storage that actually owns each
Mass entity's fragments, so there is no path to per-entity `FTransformFragment`s. Entity *positions* in the dump
therefore come from the other end of the pipeline — the visualisation ISM instances that each rendered entity
produces — and the persistent-ID registry supplies the handle roster.

## Key Files

| File | Role |
|---|---|
| [`plugin.cpp`](DecorationRemover/plugin.cpp) | `GetPluginInfo` / `PluginInit` / `PluginShutdown` C exports |
| [`plugin_config.h`](DecorationRemover/plugin_config.h) | Schema-based `DecorationRemover.ini` config with typed accessors |
| [`plugin_helpers.h`](DecorationRemover/plugin_helpers.h) | Logging macros and helper functions |
| [`entity_dump.cpp`](DecorationRemover/entity_dump.cpp) | Debug-only JSON world/entity dump (F9) |
| [`dllmain.cpp`](DecorationRemover/dllmain.cpp) | Standard Windows DLL entry point |

## License

This project is provided as-is for modding purposes. See the StarRupture ModLoader documentation for distribution terms.
