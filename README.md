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
  files/bin/             BUILD OUTPUT, gitignored: the compiled binaries are staged here
  doc/                   onboard docs rendered in the app
src/{fake-service,v4l2-imposter}   hand-written C sources (compiled by the toolchain)
toolchain/               the arm64 (Rockchip MPP) build
  Dockerfile             builds the binaries (clones paxx12/v4l2-mpp, compiles src/); b3-builder's
                         docker bake step runs it
  dist/                  BUILD OUTPUT, gitignored: the compiled binaries
.github/workflows/release.yml   CI: pack -> release -> register the atom in main-index
```

## Build locally

Needs Node.js 20+, plus Docker for the binaries (they are arm64; the build runs
`--platform linux/arm64`, emulated on non-arm hosts). `--bake` runs the toolchain Dockerfile, stages
the compiled binaries into `plugin/files/bin`, then packs:

```sh
npm install github:Bespok3d/b3-builder
npx b3-builder build --source ./plugin --atom-repo Bespok3d/u1-hw-camera --bake
# -> dist/camera-hw-accel-<ver>.b3 + dist/camera-hw-accel.atom.json
```

The manifest symlinks `files/bin` onto the printer; `--bake` stages the compiled binaries there, so
always build with it.

## Releasing

Bump `plugin/manifest.json` `version` and push to `main`. CI runs the `Bespok3d/b3-builder`
Action, which packs the `.b3` and cuts a release; the `register-atoms` action from
`Bespok3d/main-index` then registers the atom. This repo contributes atoms only and publishes no list
of its own. Secrets: `MAIN_INDEX_TOKEN` (contents:write on main-index) and `REGISTRY_SIGNING_KEY`
(the org registry key the `b3-builder` Action signs each `.b3` and atom with).

## Maintainership

These plugins are published and maintained by the Bespok3d org, and several of them repackage or
build on upstream source material. If you own the source material a plugin is based on and would
rather manage it yourself, you are welcome to contact the org to claim it back. The one condition is
that it stays actively maintained: a claimed plugin left to rot will be reclaimed so users are never
stranded on an abandoned package.
