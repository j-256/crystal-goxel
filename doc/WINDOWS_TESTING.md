# Help test Crystal Goxel on Windows

The Windows x64 package needs graphical desktop testing. It targets x64 Windows 11 with an OpenGL-capable driver providing OpenGL 2.1 and framebuffer object support. The helper and native world operations have passed Windows VM checks, but the editor's Windows rendering, interactive authoring and native file dialogs remain unverified. Mac graphical results do not establish Windows compatibility.

## Evidence and limitations

The following record describes validation on 2026-10-08. The Windows package was cross-built on macOS. The test VM used UTM 4.7.5, Windows 11 ARM64 and a `virtio-ramfb-gl` display device. The x64 executable and helper ran under Windows emulation; this is not a native x64 Windows desktop test or a general Windows ARM64 support claim.

| Check | Evidence | Remaining gap |
| --- | --- | --- |
| Windows package | Cross-build, bundled runtime and native DLL import checks passed | Native MSYS2 package build and execution on an x64 Windows desktop |
| Windows helper integration | Real child argument and Unicode path round trips, concurrent launches, output draining, error reporting and temporary cleanup passed in the VM | Graphical application use of the helper on the target desktop |
| Native world operations | World creation, tiles across boundaries and location lookup passed in the VM | Windows reference rendering, navigation, painting and cache behavior through the GUI |
| Original resource readers | Crystal Edit serialization, game deserialization and construction physics passed in Windows CPU fixtures | Crystal Edit UI compilation and a complete in-game construction test |
| Windows graphics | Startup failure reported the unsupported driver and exited cleanly | A successful Windows graphical smoke run and interactive review |
| Mac graphics | Native rendering, painting, navigation, undo/redo, save/reopen and combined exports passed on Apple Silicon | These results do not validate Windows graphics or native dialogs |
| Windows CI | Package, process and synthetic checks are configured in the workflow | Remote workflow execution was unverified in this validation record; these checks do not open the graphical editor or use game resources |

The VM failed before opening the editor with `WGL: The driver does not appear to support OpenGL`. Its GL display-device setting did not supply the OpenGL context required by the Windows application. [UTM's display documentation](https://docs.getutm.app/settings-qemu/devices/display/) describes its VirGL guest-driver requirements. Treat this as evidence about the tested configuration, not a claim that every Windows VM is incapable of running the editor.

Separate private Mesa MinGW and MSVC software-renderer attempts also failed to run the graphical smoke check. Those libraries are not bundled, and no working software-rendering fallback has been validated. Increasing RAM, passing helper tests or running `CrystalGoxel.exe --help` does not demonstrate that an OpenGL window can open.

## Checks without game resources

Build the portable folder using the [Windows build instructions](../README.md#build-the-windows-app), or use the `crystal-goxel-windows-x64` artifact from a successful Windows CI run. Keep the complete folder together. Run these commands in PowerShell, adjusting the package path:

```powershell
$package = 'C:\tools\Crystal Goxel Windows'
& "$package\Bridge\crystal-bridge.exe" test
$LASTEXITCODE
& "$package\CrystalGoxel.exe" --help
$LASTEXITCODE
& "$package\CrystalGoxel.exe"
```

The helper and help commands should return zero. The final command should open an editor window on a graphical desktop. Check rendering and mouse input, then use File > Save As and File > Open to save and reopen an ordinary voxel project. Try a writable folder with spaces and non-ASCII characters, and cancel a dialog to check that the application remains usable. Repeat after moving the complete portable folder; a separately installed .NET runtime or MSYS2 should not be required.

If the window cannot open, capture the terminal diagnostic and your graphics-driver details. Do not classify successful helper tests as a graphical pass.

## Checks with the supported game

Use your own Windows Crystal Project 1.6.9.0 resources and the inspected Crystal Edit source-project format. Create a fresh private world cache from Windows so its installation path belongs to that machine; do not reuse a Mac installation path. Choose new cache and output names for each smoke run. The cache folder must not already exist, and its parent folder must be writable.

```powershell
$package = 'C:\tools\Crystal Goxel Windows'
$game = 'C:\Games\Crystal Project'
$cache = 'C:\private\crystal-goxel\world'
$output = 'C:\private\crystal-goxel\windows-smoke.png'
& "$package\Bridge\crystal-bridge.exe" world --game $game --output $cache
if ($LASTEXITCODE -ne 0) { throw 'World preparation failed' }
& "$package\CrystalGoxel.exe" --crystal-context "$cache\world.json" `
  --crystal-smoke $output 2>&1 | Tee-Object 'C:\private\crystal-goxel\windows-smoke.log'
$LASTEXITCODE
```

The smoke check needs a working OpenGL desktop, returns zero on success, and creates private PNG, `.gox` and exported JSON artifacts. It exercises mouse painting and erasing, tile boundaries, height navigation, undo/redo, native locations, cache recovery, save/reopen and combined exports. Missing-context and save-limit diagnostics are deliberate failure cases; use the exit status and final smoke results to assess success. It substitutes an automated file-selection callback for the export dialog, so native dialog interaction still needs manual testing. See the [verification guide](../CRYSTAL_BRIDGE.md#verification) for output details and CPU checks.

For manual review:

1. Create and open a world through the Crystal Project panel using the native folder dialogs. Check paths containing spaces and non-ASCII characters
2. Visit Spawn Point, use Move up / Move down and Page Up / Page Down in tilted and top-down views, then pan with Follow camera enabled. Check whether height movement and terrain loading feel usable
3. Paint and erase across a tile boundary, undo and redo, and edit at a distant location. Return to the first location and confirm its authored work remains intact
4. Save and reopen the `.gox`, including saved locations. Export to a new Crystal Edit JSON and confirm both locations are included. Cancel the export dialog and check that work remains available
5. Open the source in Crystal Edit, compile it and test a disposable mod with a backed-up save. Check placement, variants, solid collision and non-solid decoration. Native terrain must remain unchanged
6. Record performance with a representative larger construction. The smoke benchmark measures static rendering and readback, not interactive frame rate or in-game entity performance

A failed or blocked step is useful evidence. Report it separately from successful steps, including the point where the workflow stopped.

## Report results

Include the following information in a contributor report or pull request:

```text
Source revision or build/artifact identity:
Windows edition, version/build and native architecture:
Package architecture; native execution or emulation:
Physical machine or VM; hypervisor/version and virtual graphics device:
GPU and driver version; OpenGL version if available:
Game version and Crystal Edit version, if used:
Build, helper and editor-startup results:
Graphical smoke exit status and final summary:
Manual steps passed, failed or blocked:
Expected behavior, observed behavior and reproduction steps:
Redacted console diagnostics:
```

Report native x64, emulated x64, ARM64 and other OS configurations separately. Keep game assemblies, native cache assets, user projects and game-derived screenshots private. Describe the results and share redacted diagnostics rather than uploading those artifacts. Do not include local installation paths or personal folder names in a public report. Synthetic fixtures can reproduce a problem without redistributing game content.
