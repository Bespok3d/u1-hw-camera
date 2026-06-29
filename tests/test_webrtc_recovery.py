"""Guards the WebRTC camera resilience invariants.

The streaming server (ours) must not tear down a healthy session on a wall-clock timer; recycling is
an opt-in, off-by-default user setting. Recovery from a genuinely dropped connection is client-side
(only the browser can re-negotiate a dead PeerConnection), so we do not patch a frontend we do not
own: Mainsail and our own /webrtc player already self-heal, and Fluidd's WebRTC tile does not (a
Fluidd-side limitation we advise the user about and address with an upstream PR, never a minified
bundle patch). These tests lock the server behaviour, the self-healing clients we rely on, and the
user-facing advisory.
"""
import json
from pathlib import Path

import pytest

CAMERA_DIR = Path(__file__).resolve().parent.parent
PLUGINS_DIR = CAMERA_DIR.parent

WEBRTC_SERVER = CAMERA_DIR / "src" / "v4l2-mpp" / "apps" / "stream-webrtc" / "main.cpp"
OWN_PLAYER = CAMERA_DIR / "plugin" / "files" / "html" / "webrtc.html"
README = CAMERA_DIR / "plugin" / "doc" / "README.md"

# Mainsail ships its webrtc-camerastreamer client in both the stable and bleeding-edge variants and
# self-heals upstream; assert a re-vendor never drops that (we depend on it for the "Mainsail
# recovers" advice in our docs).
MAINSAIL_CLIENTS = {
    "mainsail": PLUGINS_DIR / "mainsail-plugin" / "mainsail" / "files" / "html" / "assets",
    "mainsail-bleeding-edge": PLUGINS_DIR
    / "mainsail-plugin" / "mainsail-bleeding-edge" / "files" / "html" / "assets",
}


def _client_text(assets_dir, filename_glob):
    if not assets_dir.is_dir():
        pytest.skip("frontend sibling not present (standalone camera checkout)")
    matches = sorted(assets_dir.glob(filename_glob))
    if not matches:
        raise AssertionError(f"no {filename_glob} client bundle under {assets_dir}")
    return matches[0].read_text(encoding="utf-8")


@pytest.mark.parametrize("frontend", sorted(MAINSAIL_CLIENTS))
def test_mainsail_webrtc_reconnects_on_dead_connection(frontend):
    text = _client_text(MAINSAIL_CLIENTS[frontend], "WebrtcCameraStreamer*.js")
    assert "onconnectionstatechange" in text
    assert "restartStream" in text


def test_own_webrtc_player_reconnects_on_dead_connection():
    text = OWN_PLAYER.read_text(encoding="utf-8")
    assert "onconnectionstatechange" in text
    assert "initializeStream()" in text


def test_server_session_recycle_is_opt_in_off_by_default():
    text = WEBRTC_SERVER.read_text(encoding="utf-8")
    # The old always-on wall-clock cap (DEFAULT_SESSION_S / MAX_SESSION_WITHOUT_TIMEOUT_S) is gone.
    assert "DEFAULT_SESSION_S" not in text
    assert "MAX_SESSION_WITHOUT_TIMEOUT_S" not in text
    # The recycle is now a user-configured arg, default off (0), gated so it never fires unless set.
    assert "g_session_timeout_s = 0" in text
    assert "session-timeout-s" in text
    assert "g_session_timeout_s > 0 && elapsed >= g_session_timeout_s" in text
    # A genuinely dead client is still always reaped (connect timeout + keepalive pong + cleanup).
    assert "PONG_TIMEOUT_MS" in text
    assert "CONNECT_TIMEOUT_MS" in text


def test_session_recycle_is_surfaced_as_an_off_by_default_user_setting():
    manifest = json.loads((CAMERA_DIR / "plugin" / "manifest.json").read_text(encoding="utf-8"))
    field = next(
        (c for c in manifest["config"] if c["key"] == "WEBRTC_SESSION_TIMEOUT_MIN"), None
    )
    assert field is not None, "the recycle setting must be a user-visible config field"
    assert field["default"] == "0", "the recycle backstop must default to off"
    assert field["userEditable"] is True
    init_script = (
        CAMERA_DIR / "plugin" / "files" / "etc" / "init.d" / "s65camera-hw"
    ).read_text(encoding="utf-8")
    assert "WEBRTC_SESSION_TIMEOUT_MIN" in init_script
    assert "--session-timeout-s" in init_script


def test_docs_advise_the_fluidd_reconnect_limitation():
    # Recovery is client-side; since we do not patch Fluidd, the docs must tell a Fluidd user that
    # its WebRTC tile does not auto-reconnect yet (refresh to recover), so the advice is honest.
    text = README.read_text(encoding="utf-8").lower()
    assert "fluidd" in text
    assert "refresh" in text
