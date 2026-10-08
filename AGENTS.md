# Crystal Goxel development

Crystal Goxel extends Goxel for offline Crystal Edit voxel construction against native Crystal Project terrain. Keep upstream changes small and put game-specific logic in the bridge module.

Read CONTRIBUTING.md for upstream C conventions. Use C++17 in the bridge and retain C-compatible entrypoints for Goxel. Preserve Goxel's existing GPL license. New code comments explain non-obvious source authority, coordinate transforms, metadata preservation, and failure consequences, using ASCII and no terminal period.

Crystal Goxel accepts contributions under GPL-3.0-or-later without a CLA or copyright assignment. The retained files under `doc/cla` describe upstream Goxel's separate policy; do not require or collect signatures for this fork.

Never track game assemblies, assets, decoded databases, decompiled code, user projects, or local installation paths. Tests use synthetic fixtures. Load native resources only from the user-selected installation and retain version and fingerprint information in generated user artifacts.

Native context is reference content. It must never enter Crystal Edit authored exports. Preserve unsupported project fields and unrelated entities. Report unsupported state instead of guessing material identity or conditional NPC behavior.

A `mod.json` file is the Crystal Edit project itself. Export produces project JSON that Crystal Edit can open directly; there is no Crystal Edit compilation step. Successful tiled export can allocate entity IDs and establish project identity in the open document. Export does not save the `.gox`; saving afterward persists those assignments for later exports. Keep the editable `.gox` and the exported project JSON distinct in workflow documentation.

Run focused bridge tests and the native render smoke check before review. Keep the canonical checkout on main, use wt task worktrees, and obtain explicit approval before local integration or teardown. Never push without a separate explicit request.

Keep GitHub Actions disabled while the hosted repository is private. Every workflow job must require `github.event.repository.private == false` before runner allocation and use a documented standard runner. Release archives must come from matching clean source revisions, include package provenance and notices, and exclude private native content. Follow `doc/RELEASING.md`; publishing and enabling Actions require explicit approval.
