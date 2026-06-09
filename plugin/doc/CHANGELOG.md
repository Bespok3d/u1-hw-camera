# Changelog

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
