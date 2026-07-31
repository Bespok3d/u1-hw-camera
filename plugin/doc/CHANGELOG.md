# Changelog

## 0.1.10

- A camera tile that is still starting up now says **"Connecting..."** instead of flashing the
  "Stream interrupted" message and asking you to refresh a page that was never ready. The interrupted
  message is unchanged and still appears the moment a stream you were actually watching stops. A tile
  that never receives any picture gives up after 30 seconds and shows the interrupted message, so a
  camera that is genuinely absent still tells you so.
- Updating the plugin no longer leaves a camera with nothing reading it behind the picture servers.
  Stopping a camera returned before its parts had actually exited, and left behind records saying
  they were still running, so the next start ran against a camera that could still be held and
  against records it could not trust. Both cameras now wait for every part to really be gone and
  clear its record, and a start throws away a record whose process has already died.
- A camera stop now only ever signals the camera's own processes, so a record left behind can never
  make the printer signal something else.

## 0.1.9

- Licensing only, nothing about how the plugin works has changed. The files it installs that came
  from other projects now carry those projects' own licence notices, plus a line recording what
  Bespok3d changed in them and when.

## 0.1.8

- The frame wait 0.1.4 announced now actually ships. Its helper was never included in the package, so
  every start skipped the wait and reported a timeout it had not waited for. Installing both cameras
  could therefore still leave them serving no frames (nginx 502) on a cold start, which is exactly
  what 0.1.4 set out to fix.

## 0.1.7

- A camera URL that carries a query (`/?action=stream`, `/player?fps=15`) no longer drops the
  connection. The stream server accepted the address but then looked it up with the query still
  attached, found nothing, and killed the request, which the printer's web server showed as a
  502 error page. The streams themselves (`/stream.mjpg`, `/snapshot.jpg`) were never affected.

## 0.1.6

- When a camera you are watching stops, the tile now shows a clear "Stream
  interrupted" message instead of freezing on its last frame. It covers the WebRTC
  and MJPEG streams and the snapshot, with no browser changes. If the camera just
  stalled or restarted, live video comes back on its own the moment it recovers; if
  the WebRTC connection was recycled or dropped, the message is shown to the viewer
  before the connection closes, and a refresh restores it (a Fluidd tile needs that
  refresh; Mainsail and the built-in player reconnect on their own). A normal
  sub-second hiccup is absorbed silently, so the message never flashes.

## 0.1.5

- The WebRTC camera view no longer freezes after a while. The streaming server
  used to recycle a viewer's connection on a hidden timer (even a perfectly
  healthy one), which left the camera tile frozen on its last frame until you
  refreshed. A connection is now kept alive for as long as you are watching, and
  dropped only when it is genuinely dead (the keepalive and connect checks still
  reap an abandoned viewer). Mainsail and the built-in viewer reconnect on their
  own if a connection ever does drop; Fluidd's WebRTC tile does not yet (refresh
  to recover), a Fluidd-side limitation with an upstream fix proposed.
- The periodic recycle is now your choice, not a hidden default. A new
  "Recycle WebRTC viewers after (minutes)" setting is off by default; set it to a
  number of minutes if you want a memory-safety backstop on a very constrained
  board.

## 0.1.4

- The camera stream no longer freezes until you refresh. If the capture pipeline briefly stalls or
  drops the connection, the server now reconnects to it internally and keeps the video flowing to
  your browser, instead of leaving a dead image. (Tip: Fluidd's "adaptive" webcam mode also recovers
  on its own.)
- New setting to turn WebRTC off and save memory. It stays ON by default (the camera tile is set up
  for the WebRTC stream, so nothing changes when you update). Turn it off only if your camera is
  switched to the MJPEG stream, otherwise the tile goes blank. Handy on low-RAM boards.
- New built-in-camera resolution setting (1080p or 720p). 720p uses less memory and CPU, handy on
  low-RAM boards. The external USB camera keeps its own native resolution.

- Fix a wedge where installing both cameras left them serving no frames (the web UI
  showed nginx 502 Bad Gateway). The two pipelines were brought up back-to-back with
  every daemon backgrounded, so they cold-initialized the shared hardware encoder at
  the same instant over an unsettled system and could come up frameless. Each
  camera's start now waits for its capture to deliver a real frame before continuing
  (`bin/wait-for-frame.py`), which also serializes a back-to-back bring-up so the
  captures no longer contend. A capture that never delivers within the timeout is
  reported instead of leaving a silent dead stream.

## 0.1.2

- Clean teardown: on uninstall or version change the plugin now stops capture and
  bounces the display compositor (lmd) and Moonraker, so switching versions no
  longer leaves a wedged screen.

## 0.1.1

- Stop lmd/unisrv with a hard kill instead of a polite one. The closed U1
  compositor crashed on the polite signal (large core dump, dark screen, rare
  display wedge) on every camera install or uninstall; the hard kill is clean.

## 0.1.0

- First release. HW-accelerated MJPEG over HTTP and WebRTC streaming for the
  built-in MIPI camera and an external USB camera.
