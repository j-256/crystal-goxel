# Crystal Goxel prototype

Crystal Goxel is an offline Goxel fork for building Crystal Edit voxel objects across a tiled view of the native Crystal Project world. The editor and viewport are C/C++17. A .NET 10 C# helper reads the user's game resources and invokes the original game's CPU mesh builders through reflection. No game assemblies, textures, world data or decompiled source are distributed with this repository.

The prototype supports the fingerprinted Windows Crystal Project 1.6.9.0 installation and Crystal Edit source-project format 34. The desktop app has been exercised on an Apple Silicon Mac. Its helper launcher supports macOS and Linux; Windows desktop integration needs a process-launch adapter and testing. The native Mac game installation is not an accepted resource source.

## Build and open

On macOS, install Xcode command line tools, Python 3, .NET SDK 10, and `brew install glfw pkgconf`, then run:

```sh
scripts/package-crystal-macos
```

The command creates `dist/Crystal Goxel.app` with its own .NET runtime and GLFW library. Build-time dependency restoration needs network access. The generated app runs offline without Homebrew or a separate .NET installation. Its macOS deployment target follows the installed GLFW library, with a floor of macOS 13; only the build host has been exercised. It uses local ad hoc signing for development; it is not a notarized release.

Create a world cache from your own Windows game installation. Choose a new private output directory outside the repository:

```sh
"dist/Crystal Goxel.app/Contents/Resources/Bridge/crystal-bridge" world \
  --game "/path/to/Crystal Project" \
  --output "/path/to/world-cache"

"dist/Crystal Goxel.app/Contents/MacOS/CrystalGoxel" \
  --crystal-context "/path/to/world-cache/world.json"
```

You can also use Create tiled world in the Crystal Project panel: choose the Windows game folder and a new cache folder, then create the cache. It stores shared palette assets and a fingerprinted `world.json`, with terrain tiles added as needed. No scene export from the running game is required. Native resources are read directly from the selected installation.

A tiled document uses global game coordinates and holds every authored location in one `.gox` project. World X and World Z are horizontal; Height Y is vertical. Go to location loads neighboring native tiles and frames that location. Save named locations to return to them from the panel. Load terrain while navigating follows the camera's orbit target as you pan. Changing the view replaces reference geometry while preserving all authored layers, offscreen edits, original imported entities and entity ID reservations.

The initial location `(1, 99, 1)` is the vanilla new-game starting floor. The native Spawn Point marker is `(1, 110, 1)`; field setup moves the player down to the ground before play starts. Randomizer redirects and existing saves can start elsewhere. Supported authored coordinates span X and Z from -10000 through 10000 and Y from 0 through 255.

Terrain is cached in native-aligned tiles with a 16-cell edge. Each tile emits only its owned cells, while the original mesh builder can read neighboring cells across the edge. Adjacent tiles share the same global coordinate transform and texture atlas; they require no manual stitching. Brush strokes, lines, shapes, extrusion and moves prepare their complete operation bounds before changing authored content. A failed tile load cancels the gesture instead of committing a partial edit. Undo and redo work across tile boundaries and locations.

Tile preparation is synchronous. An uncached area reads the native world archive and can briefly pause navigation. Cached tiles can be reopened without the game installation; new areas require the exact supported resources at the installation path recorded in `world.json`. Keep that selected installation available when exploring. A damaged cache is reported rather than silently replaced. Retry terrain loading retries the active location; revisit a location after restoring a missing resource. Each preparation request is limited to 125 tiles, and native and authored preview meshes are each limited to four million vertices. Very large selections need smaller operations. Goxel's long laser ray usually exceeds these bounds and is rejected in tiled mode.

Earlier bounded contexts remain supported. To prepare one, use `prepare --game DIR --center 1,99,1 --size 48,48,48 --output NEW_DIR`, then open its `context.json`. A crop is limited to 300000 cells, with each dimension between 8 and 96. Crop documents retain their local coordinate frame and export bounds; a different crop requires a new authored document. Start a new document to switch between a crop and a tiled world.

The Crystal Project panel opens an existing `world.json` or legacy `context.json`. Select a block and variant, then use Goxel's brush or shape tools. Left click applies the selected operation; middle click rotates, right click pans, and the wheel zooms. The native world is an independent reference mesh and cannot be changed by the brushes. Toggle its visibility to inspect authored content. Game block IDs are carried by the Crystal palette, so ordinary RGB colors are rejected by Crystal Edit export and displayed as red invalid cells.

Save editable work as a `.gox` project. Open it through File > Open or pass it as application input to restore the authored document and its native context. Its `CPRF` extension chunk retains the context fingerprint, original imported project text, managed entity mappings and export collision preference alongside Goxel's authored layers. Tiled documents also retain their last location, named locations and allocated entity IDs. Changing this metadata or the export preference marks the document as changed. Keep the context directory with the project: the `.gox` file does not embed native assets. If the context is missing or changed, authored layers and original source remain in the project, and the panel reports the failure. Use Open native context to select an identical relocated context.

Import a Crystal Edit source JSON into an empty authored document. Single, stationary, unconditional voxel NPCs anywhere within the supported world bounds become editable cells in a tiled document. Legacy crops import only their bounded region. Conditional, moving, overlapping, unsupported and out-of-bounds objects stay in the preserved source and are reported as preserved; their models are not displayed as context. Unknown project, entity and outfit fields, unrelated entities, editor tree metadata, project identity and large numeric Workshop IDs survive export. Repainting an imported object's original cell retains its entity ID and existing collision settings. Deleting that cell removes the managed entity. Moving it creates a new entity, with default settings, at the new location.

Use Export Crystal Edit project in the Crystal Project panel to create a new source JSON. The save dialog suggests `crystal-project.json`; choose an unused filename because export never overwrites an existing file. All authored layers, including hidden layers, participate. Overlapping layers must be merged, and procedural or cloned layers must be baked before export. The helper rejects unsupported IDs, variants and cells outside the document bounds. A tiled export includes every authored location, regardless of which terrain tiles are visible. Open the JSON in Crystal Edit to review, compile and test it.

New entities receive unused IDs above the inspected vanilla range and a biome from the native cell's owning tile. ID allocation reserves existing entities and editor-tree references throughout the source project. Successful tiled exports retain new IDs and project identity in the authored document, so adding an earlier coordinate or exporting another location does not renumber existing construction. Save the `.gox` after export to persist these reservations across application restarts. Deleted cells retain their reserved IDs for later undo or repainting; moving a cell allocates an ID at its destination. Source entities retain their original array order and unrelated fields.

New objects use the game's zero-gravity NPC free-movement preset, stay spawned, and have no wandering or jumping. Solid new objects is enabled by default: new objects collide with players and NPCs, including decorative blocks whose native terrain definition is non-solid. Uncheck it to export new objects as non-solid decoration while keeping them fixed in place. This is an export preference for all new objects; imported objects keep their existing physics and collision settings. Earlier `.gox` metadata and helper snapshots without the preference default to solid construction.

## Scope and fidelity

Exported construction consists of voxel NPC entities. It does not replace the game's native terrain cells. Solid voxel NPCs use a full-cell collision box, even when the visible model is smaller, such as a lantern. Entity collision and behavior follow NPC rules, so voxel NPCs do not establish native terrain mechanics such as water, stairs, hazards or mining behavior. Large constructions need in-game performance testing. Tiling bounds editor reference work; it does not reduce the number of voxel NPCs created in the game. Source JSON is limited to 16 MiB and embedded bridge metadata to 20 MiB, so a tiled document is not an unlimited mod format.

Native shapes, UVs and variants come from the installed game's mesh builders. Crystal Edit's voxel NPC renderer supplies a variant, not geometric NPC-facing rotation, so the authoring UI exposes only the supported variant. The preview uses an approximate daylight treatment and static geometry: native shaders, weather, animation, dynamic lighting, NPC sprites and gameplay entities are not reproduced. Some original builders omit faces that the game's fixed camera cannot see; orbiting the reference can reveal those omissions. Authored templates use the NPC renderer's air-neighbor geometry. The default view is useful for native-style context, but the app is not a complete game renderer.

The coordinate transform preserves orientation while changing the vertical axis. With native origin `(ox, oy, oz)`, a game cell `(x, y, z)` maps to Goxel cell `(x - ox, oz - z - 1, y - oy)`. Tiled documents fix this origin at `(0, 0, 0)` for every location; crop documents retain their original crop origin. The `-1` accounts for reversing a cell interval rather than a point. Per-triangle cell ownership keeps edge clicks and inset shapes on the correct grid cell. Reference geometry is never present in authored layer volumes or Crystal Edit exports.

The helper checks executable, voxel database and native world fingerprints before invoking game code. ARM64 hosting retags only temporary copies of IL-only managed assemblies; it never changes installed files. Generated context and tile artifacts record source and asset SHA-256 values. The world manifest also retains the selected installation path for uncached tile preparation. These generated files include game content and belong with the user's private game resources.

## Verification

Synthetic checks exercise negative tile ownership, cross-edge neighbor sampling, coordinate transforms, bounded requests, offline cached views, failed preparation and asset tampering. Project checks cover distant locations in one document, supported and unsupported NPC imports, construction defaults, imported physics preservation, stable project and entity identity, editor-tree reservations, source order, exact large numbers and overwrite protection:

```sh
dotnet run --project bridge/CrystalBridge.csproj -c Release -- test
```

The GPU smoke check creates a hidden GLFW window on a graphical desktop. It requires a working OpenGL context, not a display-free server. It loads a tiled world or legacy crop, verifies that the reference changes GPU output, drives actual mouse input through Goxel's brush and erase handlers, checks undo/redo, renders a PNG, and exports authored Crystal Edit entities. The export-dialog check exercises the button's request with an automated file-selection callback, verifies a JSON filename suggestion, checks cancellation leaves the project intact, and checks acceptance creates the export. Native operating system dialog interaction needs separate graphical testing. It also imports the exported source, saves and reopens a `.gox` project, checks unknown-field and export-preference preservation, and exercises legacy defaults, missing-context recovery and save limits:

```sh
"dist/Crystal Goxel.app/Contents/MacOS/CrystalGoxel" \
  --crystal-context "/path/to/world-cache/world.json" \
  --crystal-smoke "/path/to/new-smoke.png"
```

For a tiled context it also checks a mouse stroke and shape across a tile edge, undo/redo, an edit at a distant location, camera-follow loading, saved locations, whole-document save/reopen, stable repeated exports and a corrupted neighboring tile that cancels a gesture without changing content or the active view. These location fixtures use the supported vanilla world.

The smoke output includes a PNG, `.png.gox` project and `.png.json` Crystal Edit source, plus imported-project round-trip artifacts. Tiled checks add `.png.world.gox`, `.png.world.json`, repeated-export JSON and a world-view PNG. Use a new basename for each run. Missing-context and save-limit diagnostics are expected during this check.

Check exported entity data with the installed Crystal Edit serializer and the game's binary entity reader:

```sh
"dist/Crystal Goxel.app/Contents/Resources/Bridge/crystal-bridge" check-entities \
  --game "/path/to/Crystal Project" --source "/path/to/new-smoke.png.json"
```

This validates entity identity, coordinates, voxel variants and specified physics settings through the original code without launching the Windows UIs. It also checks the original project reader for projects with an empty model tree, including construction created by the bridge. Non-empty trees need editor UI registration, so the command reports their project-level reader check as `notChecked` while validating entity data.

For a non-empty project containing only stationary, unconditional voxel NPCs, additionally check construction physics:

```sh
"dist/Crystal Goxel.app/Contents/Resources/Bridge/crystal-bridge" check-construction \
  --game "/path/to/Crystal Project" --source "/path/to/new-smoke.png.json"
```

This runs the original game's preset initialization, force application and next-tick integration in CPU fixtures. A walking NPC negative control must fall; construction must remain at its initial height in both air and liquid. For solid outfits, the original player collision resolver must stop motion into every face and ground a player landing on top. Non-solid outfits are reported as decoration. These fixtures use isolated bodies without the world's full tick orchestration, rendering, AI, audio, spawning or liquid-death handling. Neither command constitutes Crystal Edit UI testing or in-game playtesting. Human review is needed for editing feel, collision behavior in the world, gameplay compatibility and performance.

Goxel and the bridge retain GPL-3.0-or-later licensing. GLFW and the .NET runtime retain their own licenses. See [COPYING](COPYING) and the packaged dependency license notices.
