# Prepare and publish Crystal Goxel

The first preview can be built locally and uploaded to GitHub Releases without running Actions. Release downloads contain the editor, its self-contained helper, documentation and license notices. Source is released from the same committed revision; native game resources, authoring caches and helper debug symbols containing local source paths are excluded. Debug configurations retain symbols for development.

## Actions and runner costs

Keep repository-level GitHub Actions disabled while the repository is private. Every job also requires `github.event.repository.private == false` before running, including manual dispatch and tagged release jobs. The release contract checks assert that guard and restrict jobs to the chosen standard runners. No private-repository CI run is needed to prepare local release assets.

The workflow uses standard `windows-2022`, `macos-15` and `ubuntu-24.04` runners. Standard GitHub-hosted runner execution is free for public repositories; larger runners are billed even for public repositories. Private repositories consume their included minutes and can incur charges above the allowance. [GitHub runner reference](https://docs.github.com/en/actions/reference/runners/github-hosted-runners), [billing documentation](https://docs.github.com/en/billing/concepts/product-billing/github-actions). Workflow artifacts have short retention; published downloads live in GitHub Releases rather than permanent Actions artifacts. Cache limits are not raised and paid runners are not configured.

GitHub evaluates a job's `if` condition before applying its matrix, so the guard belongs on each job rather than on a build step. See [workflow syntax](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax#jobsjob_idif). Keep the repository-level switch off until visibility has been confirmed public; the guard is additional protection against accidental execution.

## Build an exact revision

Set `CRYSTAL_VERSION` to `X.Y.Z-preview.N` for a preview or `X.Y.Z` for a stable release. The tag must be `crystal-vVERSION`, keeping the fork's tags distinct from upstream Goxel tags. Commit the changes before building; a dirty source tree is allowed for development packages but rejected by release archiving. The file's name avoids shadowing C++'s `<version>` header on case-insensitive filesystems.

Build both packages from the same source commit with the [Windows and Mac commands](../README.md#get-started). The package scripts record version, source commit and tree, and whether the build was dirty in `release.json`. They also check native dependencies and notices. The Mac check inspects every bundled Mach-O architecture and deployment target and verifies signatures.

Use Python 3.11 or later. On each build host, archive the checked package:

```sh
version="$(python3 scripts/crystal-release.py version)"
python3 scripts/crystal-release.py archive 'dist/Crystal Goxel Windows' \
  --platform windows-x64 --output "dist/assets/Crystal-Goxel-$version-windows-x64.zip"
```

```sh
version="$(python3 scripts/crystal-release.py version)"
python3 scripts/crystal-release.py archive 'dist/Crystal Goxel.app' \
  --platform macos-arm64 --output "dist/assets/Crystal-Goxel-$version-macos-arm64.zip"
```

Copy the platform ZIPs into one inputs folder, then assemble and verify the release from that same clean revision:

```sh
version="$(python3 scripts/crystal-release.py version)"
python3 scripts/crystal-release.py assemble dist/assets \
  --tag "crystal-v$version" --output "dist/release-$version"
python3 scripts/crystal-release.py verify "dist/release-$version"
```

Outputs must not already exist. The tools refuse dirty or mismatched builds, missing platforms, unsafe ZIP paths, missing helpers/notices and invalid checksums. The final folder contains platform ZIPs, a matching source ZIP, release notes, `release.json` and `SHA256SUMS`. The archive writer preserves Mac executable permissions. Build provenance identifies source; it is not a claim of byte-identical builds across SDK or toolchain updates.

Run the focused bridge and native authoring checks described in [verification](../CRYSTAL_BRIDGE.md#verification). Release contract checks use `python3 -m unittest discover -s tests -p 'release_test.py'`; install pinned `PyYAML==6.0.3` into a virtual environment for workflow-policy checks. Validate YAML with `actionlint` and shell scripts with `shellcheck`. Checks without private game files cannot establish native rendering or gameplay compatibility.

## Publication order

1. Review and integrate the source, then rebuild and verify the committed release revision
2. If staging a private hosted repository, create it empty and disable Actions before pushing source; verify the disabled repository setting after creation
3. Upload the prepared files as a draft release, using the exact source tag and marking preview versions as prereleases. Review the packaged notes and platform limitations
4. After publication approval, make the repository public and publish the draft. Verify unauthenticated access to the repository and downloads
5. Read repository visibility again, then enable Actions and run the public build checks. Do not enable Actions while visibility is private

Creating the hosted repository, pushing, changing visibility, publishing a release and enabling Actions are external operations requiring the maintainer's explicit approval. The local preparation scripts perform none of them. [GitHub release management](https://docs.github.com/en/repositories/releasing-projects-on-github/managing-releases-in-a-repository).

## Tagged releases

Public main pushes, pull requests and manual dispatch build and check the supported packages. Pushing a `crystal-v*` tag also validates it against `CRYSTAL_VERSION` and assembles a draft release after both platform builds and release contract checks pass. The draft contains both platform ZIPs, matching source, checksums and notes. It does not automatically publish a release. Release creation has write permission only in the tag job; pull request builds retain read-only repository access.

If the same draft tag already exists, the workflow stops rather than overwriting its assets. Inspect and resolve that release deliberately before retrying. A published release must not be replaced silently. The Windows GUI, minimum-version Mac runtime, signing and notarization remain separate validation or delivery steps. Update [release notes](RELEASE_NOTES.md) when that evidence changes.
