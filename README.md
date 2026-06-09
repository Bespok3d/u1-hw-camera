# u1-hw-camera

The Bespok3d **Camera HW Accel** plugin for the Snapmaker U1: hardware-accelerated MJPEG +
WebRTC streaming for the MIPI and USB cameras, tapping the Rockchip SoC video pipeline instead
of CPU-encoding every frame. This repo holds the plugin source, its arm64 build toolchain, and
the CI that publishes a `.b3` release and registers the plugin in the official list.

It is a worked example of a Bespok3d plugin repo. For the concepts see the Bespok3d docs:
`doc/anatomy-of-a-plugin.md`, `doc/package-format.md`, `doc/anatomy-of-a-list.md`.

## Layout

```text
plugin/                  the plugin itself
  manifest.json          metadata + install directives (single source of truth)
  files/{etc,html,udev}  shipped, hand-maintained payload
  files/bin/             BUILD OUTPUT, gitignored: binaries staged here at pack time
  doc/                   onboard docs rendered in the app
src/{fake-service,v4l2-imposter}   hand-written C sources (compiled by the toolchain)
toolchain/               the arm64 (Rockchip MPP) build
  Dockerfile, build.sh   builds the binaries (clones paxx12/v4l2-mpp, compiles src/)
  lib/                   shared docker helpers (base image + build/extract)
  dist/                  BUILD OUTPUT, gitignored: the compiled binaries
scripts/
  pack.sh                stage binaries -> compute checksums -> zip the .b3 into dist/
  generate-atom.mjs      emit the index atom (catalog entry) for main-index
.github/workflows/release.yml   CI: build -> release -> commit the atom to main-index
```

## Build locally

Requires Docker (the binaries are arm64; the build runs `--platform linux/arm64`, emulated on
non-arm hosts), plus `zip` and `jq`.

One command does the whole thing (binaries, then `.b3`):

```sh
./scripts/build.sh
```

Or run the steps individually:

```sh
sh toolchain/build.sh            # compiles src/ into toolchain/dist/ (Docker; the slow step)
sh scripts/pack.sh               # stages binaries + packs dist/camera-hw-accel-<version>.b3
node scripts/generate-atom.mjs   # writes dist/camera-hw-accel.atom.json (local dry-run url; CI sets the real one)
```

## Releasing

Bump `plugin/manifest.json` `version` and push to `main`. CI builds the arm64 binaries, packs
the `.b3`, publishes a `camera-hw-accel-v<version>` GitHub release with the `.b3` asset, and
commits the atom (with the release asset's API download URL) into `Bespok3d/main-index/atoms/`,
which rebuilds the published `index.json`.

**Required secret:** `MAIN_INDEX_TOKEN` - a fine-grained PAT with `contents:write` on
`Bespok3d/main-index` (the per-repo `GITHUB_TOKEN` cannot write a sibling repo).

**ARM64 build:** CI builds under QEMU emulation on a standard runner. To switch to a native
arm64 runner, set `runs-on: ubuntu-24.04-arm` and delete the "Set up QEMU" step in
`.github/workflows/release.yml`; nothing else changes.

Signing is intentionally deferred during private testing; the `.b3` ships unsigned.
