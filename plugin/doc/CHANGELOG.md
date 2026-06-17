# Changelog

## 0.1.3

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
