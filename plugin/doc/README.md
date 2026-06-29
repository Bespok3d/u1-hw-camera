# Camera HW Accel

Hardware-accelerated camera streaming for the U1, using the Rockchip video pipeline
(MPP/VPU) instead of CPU-encoding every frame. You get smooth, low-latency video from the
built-in MIPI camera and any attached USB camera, even with two cameras at once.

## Features

- Hardware-accelerated MJPEG over HTTP and low-latency WebRTC streaming.
- Built-in MIPI camera and USB camera support, with hot-plug detection for USB.
- Optional RTSP streaming.
- Works in any browser; compatible with AI detection and Snapmaker Cloud features.

## Accessing your cameras

- **Built-in camera:** `http://<printer-ip>/webcam/`
- **USB camera:** `http://<printer-ip>/webcam2/`

![USB camera](images/usb_cam.png)

## Showing cameras in Fluidd and Mainsail

This plugin serves the streams. To make them appear as camera tiles in Fluidd and
Mainsail without adding them by hand, also install:

- **webcam-builtin** for the MIPI camera.
- **webcam-usb** for the USB camera.

Each registers the matching `[webcam]` entry pointing at the URLs above.

## Streaming modes

WebRTC is the default and gives the best quality and latency. The webcam-* plugins let you
pick a friendly name for each camera; the stream URLs are wired automatically:

- WebRTC: `/webcam/webrtc`, `/webcam2/webrtc`
- Snapshots: `/webcam/snapshot.jpg`, `/webcam2/snapshot.jpg`

## Settings

Open the plugin's **Config** tab to adjust these:

- **WebRTC stream** (default on): serves the low-latency WebRTC stream that the Fluidd/Mainsail
  camera tile uses. Turn it off only if you have switched your camera tile to the MJPEG stream,
  otherwise the tile goes blank.
- **Built-in camera resolution** (default 1080p): the capture resolution for the MIPI camera. 720p
  uses less memory and CPU, handy on low-RAM boards. The USB camera always uses its own native
  resolution.
- **Recycle WebRTC viewers after (minutes)** (default 0 = off): see "Staying connected" below.

## When a stream is interrupted

If a camera you are watching stops, the tile shows a clear **"Stream interrupted"** message in place
of the last frame, instead of silently freezing on a stale image. It covers the WebRTC and MJPEG
streams and the snapshot, with no browser changes, and a brief sub-second hiccup is absorbed silently
so the message never flashes.

What happens next depends on why it stopped:

- **The camera itself stalled or restarted** (the common case): the message clears and live video
  resumes on its own the moment the camera is back. You do not need to do anything.
- **The connection dropped but the camera is healthy** (for example a recycled WebRTC viewer on a
  Fluidd tile, which does not auto-reconnect): the message stays up, and you **refresh the page** to
  restart the stream, as the message suggests.

This is server-side, so it works the same in any browser. The bundled raw-H264 player (`/stream.h264`)
and RTSP output are not covered.

## Staying connected

The streaming server no longer drops a healthy WebRTC connection on a timer, so the camera stays up
as long as you watch it. If a connection does drop (a network blip or CPU pressure during a print),
recovery depends on the viewer:

- **Mainsail** and this plugin's built-in player (`/webcam/webrtc`) reconnect on their own.
- **Fluidd's** WebRTC camera tile does **not** auto-reconnect yet: if it ever drops, **refresh the
  page** to bring it back. This is a Fluidd-side limitation (its WebRTC component has no reconnect on
  a failed connection); a fix is proposed upstream. Until it lands, a Fluidd user just refreshes on
  the rare drop.

**Recycle WebRTC viewers after (minutes)** is an optional memory-safety backstop, **off by default**:

- **0 (recommended):** a camera stays connected for as long as you watch it. Dead or abandoned
  viewers are always cleaned up regardless of this setting, so leaving it off does not leak memory.
- **A number of minutes:** each WebRTC viewer connection is periodically recycled. This only matters
  on a very constrained board where you want to cap how long any single connection lives. Note: a
  recycle forces a reconnect, which Mainsail and the built-in player handle on their own but a Fluidd
  tile would need a refresh for, so leave this at 0 if you watch cameras in Fluidd.

## Notes

- The built-in camera also feeds timelapses.
- Only one streaming mode is active per camera at a time.
