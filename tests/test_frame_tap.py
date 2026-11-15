"""On firmware 2.1.0 the printer's own video service and our capture share the built-in camera.

That firmware keeps the camera device to itself: while our capture held it, the service's photos,
its timelapse and the Snapmaker apps saw only black. There the capture asks a tap preloaded into the
service for each picture instead, and firmware 2.0.0 keeps the old path, where the capture owns the
device and feeds the service through the shim.

These run the real picture rules, compiled, and pin them to the places that depend on them.
"""

import json
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
WIRE_HEADER = "src/v4l2-mpp/common/capture-common/frame_tap_wire.h"
STRIDED_HEADER = "src/mipi-frame-tap/strided_nv12.h"
TAP = (REPO_ROOT / "src/mipi-frame-tap/main.c").read_text(encoding="utf-8")
TAP_CLIENT = (
    REPO_ROOT / "src/v4l2-mpp/common/capture-common/frame_tap_client.h"
).read_text(encoding="utf-8")
CAPTURE = (
    REPO_ROOT / "src/v4l2-mpp/apps/capture-v4l2-raw-mpp/main.c"
).read_text(encoding="utf-8")
INIT_SCRIPT = (
    REPO_ROOT / "plugin/files/etc/init.d/s65camera-hw"
).read_text(encoding="utf-8")
DOCKERFILE = (REPO_ROOT / "toolchain/Dockerfile").read_text(encoding="utf-8")
MANIFEST = json.loads((REPO_ROOT / "plugin/manifest.json").read_text(encoding="utf-8"))

NV12_1080P_BYTES = 3110400

# A 4x2 picture inside 8 byte lines and a 4 line brightness plane: the picture's bytes are 1 to 8
# and 10 to 13, everything else in the block is padding (99).
PADDED_BLOCK = (
    "(const uint8_t[40]){"
    "1,2,3,4,99,99,99,99, 5,6,7,8,99,99,99,99, "
    "99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99, "
    "10,11,12,13,99,99,99,99}"
)
SMALL_PICTURE = "(strided_nv12_t){4, 2, 8, 4, 40}"
PADDED_1080P = "(strided_nv12_t){1920, 1080, 1920, 1088, 0}"


def test_the_capture_takes_only_a_picture_of_the_size_it_was_started_at(run_c_checks):
    whole_1080p = f"&(frame_tap_header_t){{FRAME_TAP_MAGIC, 1920, 1080, {NV12_1080P_BYTES}}}"
    unmarked_1080p = f"&(frame_tap_header_t){{0, 1920, 1080, {NV12_1080P_BYTES}}}"
    short_1080p = "&(frame_tap_header_t){FRAME_TAP_MAGIC, 1920, 1080, 1024}"
    result = run_c_checks(
        WIRE_HEADER,
        [
            f"frame_tap_nv12_bytes(1920, 1080) == {NV12_1080P_BYTES}",
            f"frame_tap_header_is_valid({whole_1080p}, 1920, 1080)",
            f"!frame_tap_header_is_valid({whole_1080p}, 1280, 720)",
            f"!frame_tap_header_is_valid({unmarked_1080p}, 1920, 1080)",
            f"!frame_tap_header_is_valid({short_1080p}, 1920, 1080)",
        ],
    )
    assert result.returncode == 0, result.stdout


def test_a_picture_is_copied_only_when_the_copy_stays_inside_its_block(run_c_checks):
    result = run_c_checks(
        STRIDED_HEADER,
        [
            f"strided_nv12_is_packable(&{SMALL_PICTURE})",
            f"strided_nv12_reach_bytes(&{PADDED_1080P}) == 1920 * 1088 + 1920 * 540",
            "!strided_nv12_is_packable(&(strided_nv12_t){4, 2, 8, 4, 39})",
            "!strided_nv12_is_packable(&(strided_nv12_t){10, 2, 8, 4, 40})",
            "!strided_nv12_is_packable(&(strided_nv12_t){4, 6, 8, 4, 56})",
            "!strided_nv12_is_packable(&(strided_nv12_t){3, 2, 8, 4, 40})",
            "!strided_nv12_is_packable(&(strided_nv12_t){4, 3, 8, 4, 40})",
            "!strided_nv12_is_packable(&(strided_nv12_t){0, 2, 8, 4, 40})",
        ],
    )
    assert result.returncode == 0, result.stdout


def test_a_padded_picture_is_packed_without_its_padding(run_c_checks):
    packed_matches = (
        "({ uint8_t packed[12] = { 0 };"
        f" strided_nv12_pack(&{SMALL_PICTURE}, {PADDED_BLOCK}, packed);"
        " memcmp(packed, (const uint8_t[12]){1,2,3,4,5,6,7,8,10,11,12,13}, 12) == 0; })"
    )
    result = run_c_checks(STRIDED_HEADER, [packed_matches])
    assert result.returncode == 0, result.stdout


def test_firmware_2_1_0_starts_the_service_with_the_tap_and_the_capture_on_it():
    assert "if grep -q librockit /usr/bin/unisrv" in INIT_SCRIPT
    assert 'CAPTURE_SOURCE="--frame-sock $FRAME_TAP_SOCK"' in INIT_SCRIPT
    assert "export LD_PRELOAD=$BIN/libmipi-frame-tap.so" in INIT_SCRIPT
    assert "export MIPI_FRAME_TAP_SOCK=$FRAME_TAP_SOCK" in INIT_SCRIPT
    assert 'CMD_CAPTURE="$BIN/capture-v4l2-raw-mpp $CAPTURE_SOURCE ' in INIT_SCRIPT


def test_firmware_2_0_0_keeps_the_capture_on_the_device_and_the_shim_in_the_service():
    assert (
        'CAPTURE_SOURCE="--device /dev/video11 --raw-frame-sock /tmp/capture-mipi-raw.sock"'
        in INIT_SCRIPT
    )
    assert '[ "$FRAME_TAP" = "1" ] || start_lmd_with_imposter' in INIT_SCRIPT


def test_the_service_is_started_before_the_capture_on_firmware_2_1_0():
    """The tap serves only once the service runs the camera, and the capture waits for it."""
    tap_start = INIT_SCRIPT.index('[ "$FRAME_TAP" = "1" ] && start_lmd_with_frame_tap')
    capture_start = INIT_SCRIPT.index('printf "Starting capture-mipi: "')
    assert tap_start < capture_start


def test_the_capture_reads_the_tap_instead_of_the_device_and_outlives_the_service():
    assert '{"frame-sock",' in CAPTURE
    assert "frame_tap_client_receive(&frame_tap, &frame_data, &bytesused)" in CAPTURE
    assert "if (!frame_sock && v4l2_capture_open(" in CAPTURE
    assert "if (!frame_sock && v4l2_capture_start(" in CAPTURE
    receive_failure = TAP_CLIENT[TAP_CLIENT.index("client->in_trouble = true;"):]
    assert "frame_tap_client_hang_up(client);" in receive_failure
    assert "return 0;" in receive_failure


def test_the_tap_stops_before_the_service_takes_the_camera_channel_away():
    disable_hook = TAP[TAP.index("int RK_MPI_VI_DisableChn("):]
    assert disable_hook.index("serving_stop(pipe)") < disable_hook.index("return real_disable(")


def test_the_tap_is_built_and_shipped():
    assert "COPY src/mipi-frame-tap /build/mipi-frame-tap" in DOCKERFILE
    assert "make -C /build/mipi-frame-tap install DESTDIR=/dist" in DOCKERFILE
    assert {
        "path": "libmipi-frame-tap.so",
        "dest": "files/bin/libmipi-frame-tap.so",
        "mode": "0755",
    } in MANIFEST["bake"][0]["members"]
