# Crystal Goxel development

Crystal Goxel extends Goxel for offline Crystal Edit voxel construction against native Crystal Project terrain. Keep upstream changes small and put game-specific logic in the bridge module.

Read CONTRIBUTING.md for upstream C conventions. Use C++17 in the bridge and retain C-compatible entrypoints for Goxel. Preserve Goxel's existing GPL license. New code comments explain non-obvious source authority, coordinate transforms, metadata preservation, and failure consequences, using ASCII and no terminal period.

Never track game assemblies, assets, decoded databases, decompiled code, user projects, or local installation paths. Tests use synthetic fixtures. Load native resources only from the user-selected installation and retain version and fingerprint information in generated user artifacts.

Native context is reference content. It must never enter Crystal Edit authored exports. Preserve unsupported project fields and unrelated entities. Report unsupported state instead of guessing material identity or conditional NPC behavior.

Run focused bridge tests and the native render smoke check before review. Keep the canonical checkout on main, use wt task worktrees, and obtain explicit approval before local integration or teardown. Never push without a separate explicit request.
