# u1-hw-camera

[![licence](https://img.shields.io/badge/licence-GPL--3.0-blue)](LICENSE)
[![release](https://img.shields.io/github/v/release/Bespok3d/u1-hw-camera)](https://github.com/Bespok3d/u1-hw-camera/releases)
[![version](https://img.shields.io/badge/dynamic/json?url=https%3A%2F%2Fraw.githubusercontent.com%2FBespok3d%2Fu1-hw-camera%2Fmain%2Fplugin%2Fmanifest.json&query=%24.version&label=version&color=blue)](plugin/manifest.json)
![printer](https://img.shields.io/badge/printer-Snapmaker%20U1-informational)
![stock firmware](https://img.shields.io/badge/stock%20firmware-no%20flashing-brightgreen)

The Bespok3d **Camera HW Accel** plugin for the Snapmaker U1: hardware-accelerated MJPEG +
WebRTC streaming for the MIPI and USB cameras, tapping the Rockchip SoC video pipeline instead
of CPU-encoding every frame. This repo holds the plugin source, its arm64 build toolchain, and
the CI that publishes a `.b3` release and registers the plugin in the official list.

It is a worked example of a Bespok3d plugin repo.

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
npm install --prefix ~/.b3-builder github:Bespok3d/b3-builder
~/.b3-builder/node_modules/.bin/b3-builder build --source ./plugin --atom-repo Bespok3d/u1-hw-camera --bake
# -> dist/camera-hw-accel-<ver>.b3 + dist/camera-hw-accel.atom.json
```

The manifest symlinks `files/bin` onto the printer; `--bake` stages the compiled binaries there, so
always build with it. Two traps the commands above avoid: `npx b3-builder` resolves to whatever copy
npm cached earlier, which silently builds against an out of date manifest schema, and a plain
`npm install` run in this repo installs into the nearest `package.json` above it (this repo has none),
which is why the install gets its own prefix directory.

## Releasing

Bump `plugin/manifest.json` `version` and push the tag `plugin-<name>-v<version>` naming that plugin
and that exact number. A push to `main` publishes nothing, and the run is refused if the tag and the
manifest disagree. CI runs the `Bespok3d/b3-builder` Action, which packs the `.b3` and cuts a
release; the `register-atoms` action from `Bespok3d/main-index` then registers the atom. This repo
contributes atoms only and publishes no list of its own. Secrets: `MAIN_INDEX_TOKEN` (contents:write
on main-index) and `REGISTRY_SIGNING_KEY` (the org registry key the `b3-builder` Action signs each
`.b3` and atom with).

## Composition

The C sources in [`src/v4l2-mpp/`](src/v4l2-mpp/) are Bespok3d's fork of paxx12's `v4l2-mpp`,
GPL-3.0-or-later; `src/v4l2-mpp/VENDORING.md` records the commit it was forked from and what changed.

Three libraries are fetched and linked in at build time. They are not stored in this repository, and
in the built `.b3` package they are separate works under their own licences, aggregated with
Bespok3d's code rather than relicensed by it.

| Library | Pin | Licence | Licence text |
| --- | --- | --- | --- |
| Rockchip MPP | `https://github.com/HermanChen/mpp.git` commit `437bfbeb9567cca9cd9080e3f6954aa9d6a94f18` | Apache-2.0 | [LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt) |
| libdatachannel | `https://github.com/paullouisageneau/libdatachannel.git` commit `222529eb2c8ae44f96462504ae38023f62809cec` | MPL-2.0 | [LICENSES/MPL-2.0.txt](LICENSES/MPL-2.0.txt) |
| live555 | `https://download.videolan.org/pub/contrib/live555/live.2025.11.06.tar.gz`, sha256 `7614fa0a293e61b24bfd715a30a1c020fb4fe5490ebb02e71b0dadb5efc1d17c` | LGPL-2.1 | [LICENSES/LGPL-2.1-only.txt](LICENSES/LGPL-2.1-only.txt) |

`plugin/doc/ATTRIBUTIONS.md` names every upstream and carries their copyright notices.

### Corresponding Source

Bespok3d builds every binary this plugin ships (`capture-v4l2-raw-mpp`, `capture-v4l2-jpeg-mpp`,
`stream-webrtc`, `stream-rtsp`, `fake-service`, `libv4l2-imposter.so`), GPL-3.0-or-later, so the
source that corresponds to those binaries is:

- Bespok3d's own source, [`src/v4l2-mpp/`](src/v4l2-mpp/) in this repository
- Bespok3d's build, [`toolchain/`](toolchain/) in this repository, which holds the Dockerfile, the
  build environment and the library pins above
- the three libraries at the pins in the table

All of it is public and is in every release of this repository. Anyone who received the binaries may
take that source and rebuild them. If any part of it is unreachable, ask the Bespok3d org and it will
be provided.

live555 is LGPL and is linked into `stream-rtsp`, so anyone who received that binary is entitled to
the material needed to relink it against their own build of live555. Bespok3d builds live555 itself
rather than taking a built library, so that material is ours to hand over: the pinned tarball and its
sha256 in the table above, the build script
[`src/v4l2-mpp/deps/compile_livemedia.sh`](src/v4l2-mpp/deps/compile_livemedia.sh) with live555's own
`linux-no-std-lib` config, and the toolchain that links the result. The full inventory of every binary Bespok3d ships, with
versions and checksums, is in `Bespok3d_history/doc/gpl-source-inventory.md`.

## Maintainership

These plugins are published and maintained by the Bespok3d org, and several of them repackage or
build on upstream source material. If you own the source material a plugin is based on and would
rather manage it yourself, you are welcome to contact the org to claim it back. The one condition is
that it stays actively maintained: a claimed plugin left to rot will be reclaimed so users are never
stranded on an abandoned package.

## Licence

Copyright (C) 2026 unlucio and the Bespok3d contributors

GPL version 3, for the code in this repository written by Bespok3d. See Composition above for the
rest.

This program is free software: you can redistribute it and/or modify it under the terms of version 3
of the GNU General Public License as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
Public License for more details.

You should have received a copy of the GNU General Public License along with this program. If not,
see <https://www.gnu.org/licenses/>. The full text is in [LICENSE](LICENSE).

Bespok3d's own code elsewhere in the project is AGPL-3.0-or-later. The code here is GPL-3.0-only
instead because the camera work has Extended Firmware lineage, which is GPL-3.0-only. Version 3 of
the GPL and version 3 of the AGPL may be combined in a single work, and section 13 of each licence
says so; what cannot happen is code offered under version 3 of the GPL alone being re-offered under
the AGPL.

Bespok3d is a project of the Bespok3d Organisation, which is not a legal entity. Copyright is held by
the individual authors named above.
