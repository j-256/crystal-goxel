# Crystal Goxel @VERSION@

Build voxel NPC constructions against native Crystal Project terrain, edit across tile boundaries and distant locations in one document, and export the combined work as a Crystal Edit project JSON. `mod.json` is the project itself; there is no Crystal Edit compilation step.

Choose the platform ZIP, extract it completely, and keep the editor and bundled helper together. Both downloads include their own .NET runtime; neither needs a separate runtime installation. Use your own Windows or Mac Crystal Project 1.7.0.1 installation to prepare native reference content. Mac users can select `Crystal Project.app` directly. Windows 1.6.9.0 caches remain supported; create a new cache when changing game builds. Game assemblies, assets, and caches are not included.

## Game compatibility

Windows and Mac 1.7.0.1 resource sources have passed native authoring and original reader/construction checks on an Apple Silicon Mac. This release recognizes both source builds while preserving older cache fingerprints. These checks do not establish Windows application GUI compatibility or a complete in-game playtest.

## Fixes since 0.1.0-preview.1

- Correct save-path memory lifetime during Save and Save As
- Treat displayed hint text as data and bound native file-dialog filter handling
- Reject malformed voxel imports and unsafe dimensions before allocating buffers
- Bound rendering, texture, mesh, and export size calculations, along with allocation arithmetic in bundled libraries
- Correct Windows build dependencies and clean-source provenance checks

## Platform limits

Windows graphical authoring, native file dialogs, and Crystal Edit UI behavior remain unverified. Windows CI and helper checks cover building, process launching, and synthetic behavior; they do not establish graphical compatibility. The stable release designation does not expand platform validation.

| Download | Target | Validation limits |
| --- | --- | --- |
| `Crystal-Goxel-@VERSION@-windows-x64.zip` | Windows 11 x64 with OpenGL 2.1 and framebuffer object support | Experimental: Windows graphical authoring and native dialogs need desktop testing; helper checks do not establish graphical compatibility |
| `Crystal-Goxel-@VERSION@-macos-arm64.zip` | Apple Silicon, macOS 15 or later | Ad hoc signed, not notarized; macOS 15 execution needs contributor testing |
| `Crystal-Goxel-@VERSION@-source.zip` | Matching source and build instructions | No game resources or generated authoring data |

Windows binaries are unsigned. Windows may show a security prompt for an unrecognized download. On Mac, downloaded apps can require individual approval in System Settings > Privacy & Security; follow [Apple's instructions](https://support.apple.com/en-us/102445) if you trust the download. Signing and notarization are planned separately. Intel Mac, Linux, and native Windows ARM64 packages are not part of this release.

Read the included README before creating a world. Save your editable `.gox` before export and again if export marks it changed, preserving newly allocated entity IDs and project identity. The exported JSON is usable immediately. Native reference terrain remains read-only and never enters mod exports.

`SHA256SUMS` covers the downloads, source, release notes and `release.json`. Package manifests record the exact version, source commit and source tree. Preserve those when reporting an issue. See the included Windows testing guide for platform evidence, smoke commands and a contributor report template. Do not upload game files, private caches or user projects with a public report.
