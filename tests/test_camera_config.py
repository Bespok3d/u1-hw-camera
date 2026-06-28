import json
from pathlib import Path

PLUGIN = Path(__file__).resolve().parent.parent / "plugin"
MANIFEST = json.loads((PLUGIN / "manifest.json").read_text())
CONF_TMPL = (PLUGIN / "files/camera.conf.tmpl").read_text()
HW = (PLUGIN / "files/etc/init.d/s65camera-hw").read_text()
USB = (PLUGIN / "files/etc/init.d/s65camera-usb").read_text()


def config_keys():
    return {field["key"] for field in MANIFEST.get("config", [])}


def test_manifest_exposes_webrtc_and_resolution_config():
    assert {"WEBRTC_ENABLED", "CAMERA_RESOLUTION"} <= config_keys()


def test_webrtc_defaults_on():
    # Default ON so an UPDATE never silently blanks a camera registered for the WebRTC stream
    # (webcam-builtin registers service=webrtc-camerastreamer). Turning it off is opt-in and must be
    # paired with switching the webcam registration to MJPEG. Device-verified regression.
    webrtc = next(field for field in MANIFEST["config"] if field["key"] == "WEBRTC_ENABLED")
    assert webrtc["type"] == "toggle"
    assert webrtc["default"] == webrtc["onValue"] == "1"


def test_resolution_defaults_to_1080p():
    resolution = next(field for field in MANIFEST["config"] if field["key"] == "CAMERA_RESOLUTION")
    assert resolution["default"] == "1080p"
    assert set(resolution["options"]) == {"1080p", "720p"}


def test_camera_conf_is_rendered_into_the_plugin_dir():
    # The daemon unpacks the plugin into the same dir the init scripts call $PLUGIN, so `render`
    # writes camera.conf exactly where the scripts source it ($PLUGIN/camera.conf). It must NOT also
    # be a symlink: a symlink from camera.conf -> $PLUGIN/camera.conf resolves to itself (a loop)
    # and clobbers the rendered file. (Device-verified: that loop made the scripts use defaults.)
    templates = MANIFEST["install"]["templates"]
    assert {"from": "files/camera.conf.tmpl", "to": "camera.conf"} in templates
    symlink_targets = [link["from"] for link in MANIFEST["install"]["symlinks"]]
    assert "camera.conf" not in symlink_targets
    assert "$WEBRTC_ENABLED" in CONF_TMPL
    assert "$CAMERA_RESOLUTION" in CONF_TMPL


def test_both_init_scripts_source_camera_conf():
    for script in (HW, USB):
        assert '. "$PLUGIN/camera.conf"' in script


def test_webrtc_is_toggleable_and_defaults_on_in_both_scripts():
    for script in (HW, USB):
        # fallback when camera.conf is absent matches the manifest default (ON), so an update or an
        # older package never blanks a webrtc-registered camera
        assert 'WEBRTC_ENABLED=1' in script
        assert 'if [ "$WEBRTC_ENABLED" = "1" ]; then' in script
    # the stream server is handed a webrtc socket only through the gated arg, never hardcoded
    assert "$WEBRTC_SOCK_ARG" in HW
    assert "$WEBRTC_SOCK_ARG" in USB
    assert "camera-stream.py" in HW
    assert "--webrtc-sock" not in HW.split("camera-stream.py", 1)[1]


def test_mipi_capture_uses_the_resolution_toggle():
    assert "--width $CAM_WIDTH --height $CAM_HEIGHT" in HW
    assert "720p) CAM_WIDTH=1280; CAM_HEIGHT=720" in HW
