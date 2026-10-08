# Crystal Goxel

Build Crystal Project mods by placing voxel objects against the game's own terrain. Crystal Goxel is an offline desktop fork of Goxel with native block IDs, Crystal Edit import/export, and a tiled reference world. Work at multiple locations in one `.gox` project and export their construction together.

Construction exports as voxel NPC entities. Native terrain stays read-only: painting in this editor adds authored objects without replacing the game's terrain cells.

## Get started

You need your own **Windows Crystal Project 1.6.9.0 installation** and Crystal Edit to review and compile exported source projects. The bridge checks the exact game resources; other versions and the native Mac game installation are not accepted. Game files are selected locally and are not bundled with this repository or the app.

The Windows package targets x64 Windows 11 with an OpenGL-capable graphics driver. It includes its own helper runtime. The process launcher, packaged helper, synthetic checks and native world preparation have been exercised in a Windows 11 ARM64 VM running the x64 package under emulation. Windows graphical authoring, native file dialogs and Crystal Edit UI behavior remain unverified. The Windows CI job is configured to repeat the process and synthetic checks without game files; those checks do not open the graphical editor. The Mac desktop has been exercised on Apple Silicon. See the [detailed guide](CRYSTAL_BRIDGE.md#build-and-open) for packaging limits.

### Build the Windows app

Install [MSYS2](https://www.msys2.org/) and [.NET SDK 10](https://dotnet.microsoft.com/download/dotnet/10.0). In the **MSYS2 MINGW64** shell, install the compiler and editor dependencies:

```sh
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-glfw \
  mingw-w64-x86_64-libtre-git mingw-w64-x86_64-zlib scons
```

Make sure `dotnet --list-sdks` works in that shell; if needed, add `/c/Program Files/dotnet` to its `PATH`. From the repository root, run:

```sh
scripts/package-crystal-windows
```

Open `dist/Crystal Goxel Windows/CrystalGoxel.exe`. Keep the complete folder together, including `Bridge`; it runs without MSYS2 or a separately installed .NET runtime. The Windows CI job builds the same portable folder as a downloadable artifact and runs synthetic checks without game files. Builds are unsigned.

### Build the Mac app

With Xcode command line tools, Python 3 and .NET SDK 10 installed, run from the repository root:

```sh
brew install glfw pkgconf
scripts/package-crystal-macos
open "dist/Crystal Goxel.app"
```

The package includes its own .NET runtime and GLFW library. Building restores dependencies over the network; the generated app runs offline without Homebrew or a separate .NET installation. This is an ad hoc signed development bundle, not a notarized release.

### Open your first world

1. In the **Crystal Project** panel, expand **Create tiled world**
2. Use **Choose game folder** to select the Windows game installation containing `Crystal Project.exe` and its `Content` folder
3. Use **Choose cache parent** outside the repository, then make sure **New cache folder** names a folder that does not exist yet
4. Click **Create world cache** to open the generated `world.json` and load the starting area

The cache is the **native context**: the terrain, textures and block definitions used as reference while editing. Crystal Goxel reads these from your selected installation, so no scene export from the running game is needed. Open an existing cache with **Open native context** and select its `world.json`.

## Build and export a mod

1. Use **Native locations** to search for an area or place, choose it, and click **Go to selected place**. The list includes home points and landmarks such as Spawn Point; a starter `.gox` is not required
2. Select a **Block** and **Variant** in the Crystal Project panel, then use the brush or shape tools to build. The ordinary Goxel color palette does not carry Crystal block IDs
3. Save your editable work as a `.gox`. Use **Saved locations** for your own named bookmarks, and keep building at other places in the same document
4. Click **Export Crystal Edit project** and choose a new JSON filename. Every authored location exports together, including content outside the visible terrain and in hidden layers. Export does not overwrite existing files
5. Save the `.gox` again to retain the allocated entity IDs, then open the exported JSON in Crystal Edit to review and compile it. Test the compiled mod in the game

New objects stay fixed in place. **Solid new objects** enables full-cell collision; turn it off for non-solid decoration. Imported objects keep their original physics settings.

To edit an existing Crystal Edit source JSON, use **Import Crystal Edit project** in an empty authored document. Supported stationary, unconditional voxel NPCs become editable. Other entities and unknown fields remain in the preserved source, with unsupported objects reported rather than guessed.

## Move around the world

| Task | Control |
| --- | --- |
| Visit a known place | **Native locations**, then **Go to selected place** |
| Visit coordinates | Set **World X**, **Height Y** and **World Z**, then **Go to location** |
| Return to your own work | **Saved locations** |
| Move underground or above ground | **Move down / Move up**, or **Page Down / Page Up** with the pointer over the world |
| Adjust vertical movement | Set **Height step** in blocks; it defaults to 16, and 1 gives fine movement |
| Pan | Right-button drag |
| Rotate | Middle-button drag or arrow keys |
| Zoom | Mouse wheel |

Game **Height Y** is Goxel's vertical **Z** axis. The height controls move along that world axis while preserving horizontal position, viewing angle and zoom. Screen-plane panning depends on camera angle and can barely change height in a top-down view.

**Follow camera** loads terrain as you pan, with nearby tiles prepared eagerly. The height controls also load their destination when camera following is disabled. Moving between locations preserves authored work throughout the document. A failed terrain load leaves the current view available and reports the problem in the panel.

## Know which files to keep

| File | Purpose |
| --- | --- |
| `.gox` | Editable construction, imported source, entity mappings and project bookmarks |
| `world.json` | Reference-cache manifest: game version, installation path, fingerprints, tile size and origin, and block palette |
| Cache assets and terrain tiles | Generated native textures and geometry beside the manifest |
| `locations.json` | Generated vanilla home points and fixed landmarks, separate from project bookmarks |
| Exported Crystal Edit `.json` | Source project containing authored entities for Crystal Edit |

Keep the context directory with your `.gox`; the project does not embed native game assets. The selected installation is needed to prepare uncached areas, while already cached terrain and locations can be reopened offline. Generated reference files contain game content and should stay with your private game resources, outside this repository. See the [detailed guide](CRYSTAL_BRIDGE.md#build-and-open) for relocating a context or recovering missing resources.

## Scope

- Native terrain and location catalogs are reference data and never enter authored mod exports
- Voxel NPCs follow entity rules. They do not reproduce native terrain mechanics such as water, stairs, hazards or mining, and solid objects use full-cell collision even when their visible model is smaller
- Native destinations describe the vanilla world; they do not apply save progression or randomizer redirects
- The preview shows static voxel geometry with approximate daylight, rather than the game's full shaders, weather, animation or gameplay entities
- Large builds create many voxel NPCs and need in-game performance testing. Large editing operations are bounded; the [detailed guide](CRYSTAL_BRIDGE.md#scope-and-fidelity) explains fidelity and resource limits

## Development and upstream

Windows contributors can help close the graphical testing gap. The test VM cannot create the required OpenGL context, and software-renderer attempts failed. See [Windows testing](doc/WINDOWS_TESTING.md) for the evidence collected, remaining checks, smoke commands and a report template. Contributors without the game can still check editor startup and native file dialogs.

See [CRYSTAL_BRIDGE.md](CRYSTAL_BRIDGE.md) for helper commands, legacy bounded contexts, preservation rules and [verification](CRYSTAL_BRIDGE.md#verification). Read [AGENTS.md](AGENTS.md) and [CONTRIBUTING.md](CONTRIBUTING.md) before changing the bridge or upstream editor code.

Crystal Goxel is based on Goxel 0.15.2 by Guillaume Chereau. The [upstream reference](doc/UPSTREAM_README.md) retains Goxel's general documentation and build instructions; upstream downloads do not include the Crystal Project bridge.

Goxel and the bridge retain **GPL-3.0-or-later** licensing. See [COPYING](COPYING) and the [dependency notices](licenses/GLFW-LICENSE.md).
