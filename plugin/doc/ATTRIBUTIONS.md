# Attributions - camera-hw-accel

**Plugin author:** Bespok3d, vendoring v4l2-mpp (paxx12), Rockchip MPP, libdatachannel (Paul-Louis Ageneau) and live555 (Live Networks); two helper programs come from the Extended Firmware camera overlay `60-app-camera` (paxx12)

Hardware-accelerated camera capture and streaming.

| Upstream project | Author | Licence | Needed at runtime | Code ships in this package |
| --- | --- | --- | --- | --- |
| v4l2-mpp | paxx12 | GPL-3.0-or-later | yes | yes |
| Rockchip MPP (Media Process Platform) | Rockchip | Apache-2.0 | yes | no |
| libdatachannel (WebRTC) | Paul-Louis Ageneau and contributors | MPL-2.0 | yes | no |
| live555 streaming media (RTSP) | Live Networks, Inc. | LGPL-2.1 | yes | no |

The capture and streaming binaries are built from a fork of https://github.com/paxx12/v4l2-mpp at
pinned commit `10fc3b9d935d9c79bacc014839c05de4a004c4ac`; that tree lives in this repo under
`src/v4l2-mpp/` and carries its own LICENSE. The web pages in `plugin/files/html/` and the two Python
programs `camera-stream.py` and `control-v4l2.py` are byte-identical copies from that same tree.
Rockchip MPP, libdatachannel and live555 are pulled and linked at build time by the toolchain.

Two smaller programs come from somewhere else: `fake-service` and `libv4l2-imposter.so` are derived
from the `apps/fake-service` and `apps/v4l2-imposter` sources in the Extended Firmware overlay
`60-app-camera`, GPL-3.0, written by paxx12. They live here under `src/fake-service/` and
`src/v4l2-imposter/` and differ from the overlay's originals only in identifier naming.

That overlay was called `12-camera-v4l2-mpp` until it was renamed in March 2026, and other Extended
Firmware contributors have commits in it. One of them made `fake-service` build under a cross
compiler, and that change is in the Makefile shipped here. @justinh-rahb added the V4L2 controls
integration and liberodark corrected a shebang; both of those touched the overlay's own firmware
wiring, which this plugin does not ship, because it places its own init scripts through the Bespok3d
daemon. The Extended Firmware history names every one of them.

## Copyright notices

The capture and streaming binaries this plugin ships are linked against Rockchip MPP, libdatachannel
and live555. Their licences require the notices to travel with the shipped binaries, even though no
source from those three projects is in this repo. The full licence texts are in `LICENSES/` at the
root of this repo.

| Component | Licence | Copyright notice, as the project states it |
| --- | --- | --- |
| v4l2-mpp fork | GPL-3.0-or-later | the project's `LICENSE` names no copyright holder line; the fork is paxx12's |
| Rockchip MPP | Apache-2.0 | `Copyright 2015 Rockchip Electronics Co. LTD` |
| libdatachannel | MPL-2.0 | the project's `LICENSE` is the MPL-2.0 text with no copyright line, which MPL-2.0 does not require |
| live555 | LGPL-2.1 | `Copyright (c) 1996-2026 Live Networks, Inc.  All rights reserved.` |

Read from `inc/rk_mpi.h` at MPP commit `437bfbeb9567cca9cd9080e3f6954aa9d6a94f18`, from `LICENSE` at
libdatachannel commit `222529eb2c8ae44f96462504ae38023f62809cec`, and from a live555 source header,
all retrieved 2026-07-28. The live555 release the toolchain fetches is `live.2025.11.06.tar.gz`,
sha256 `7614fa0a293e61b24bfd715a30a1c020fb4fe5490ebb02e71b0dadb5efc1d17c`, checked by
`src/v4l2-mpp/deps/compile_livemedia.sh`.

live555 is LGPL-2.1, so a user is entitled to what is needed to relink the shipped binary against
their own build of it. Bespok3d compiles live555 itself, so that material is ours to hand over: that
release at the sha256 above, the build script `src/v4l2-mpp/deps/compile_livemedia.sh` with live555's
own `linux-no-std-lib` config, the v4l2-mpp source in `src/v4l2-mpp/`, and the toolchain in
`toolchain/`, all of which are public. libdatachannel is MPL-2.0, so its source is
available from the project at the commit named above.
