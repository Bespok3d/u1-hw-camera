"""On firmware 2.1.0 the printer's video service can lose its camera and show black until it is
asked for a photo, which makes it rebuild its camera pipeline. The capture asks for that photo
itself when the tap answers but its camera stays silent, so the stream comes back on its own.

The timing rule runs compiled; the wiring to it is pinned in the places that depend on it.
"""

import json
import re
from functools import reduce
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
RECOVERY_HEADER = "src/v4l2-mpp/common/capture-common/freeze_recovery.h"
TAP_CLIENT = (
    REPO_ROOT / "src/v4l2-mpp/common/capture-common/frame_tap_client.h"
).read_text(encoding="utf-8")
PHOTO_REQUEST = (
    REPO_ROOT / "src/v4l2-mpp/common/capture-common/camera_photo_request.h"
).read_text(encoding="utf-8")
CAPTURE = (
    REPO_ROOT / "src/v4l2-mpp/apps/capture-v4l2-raw-mpp/main.c"
).read_text(encoding="utf-8")
INIT_SCRIPT = (
    REPO_ROOT / "plugin/files/etc/init.d/s65camera-hw"
).read_text(encoding="utf-8")
CONFIG_TEMPLATE = (REPO_ROOT / "plugin/files/camera.conf.tmpl").read_text(encoding="utf-8")
MANIFEST = json.loads((REPO_ROOT / "plugin/manifest.json").read_text(encoding="utf-8"))

CAMERA_WORKING = "(freeze_recovery_t){0}"
SILENT_SINCE_100 = f"freeze_recovery_camera_silent({CAMERA_WORKING}, 100)"
STILL_SILENT_AT_105 = f"freeze_recovery_camera_silent({SILENT_SINCE_100}, 105)"
PHOTO_ASKED_AT_110 = f"freeze_recovery_photo_asked({SILENT_SINCE_100}, 110)"


def photos_asked_at(recovery, seconds):
    return reduce(
        lambda asked_so_far, second: f"freeze_recovery_photo_asked({asked_so_far}, {second})",
        seconds,
        recovery,
    )


FOUR_PHOTOS = photos_asked_at(SILENT_SINCE_100, [110, 170, 230, 290])
FIVE_PHOTOS = photos_asked_at(SILENT_SINCE_100, [110, 170, 230, 290, 350])


def test_a_photo_is_asked_for_only_after_ten_seconds_of_silence(run_c_checks):
    result = run_c_checks(
        RECOVERY_HEADER,
        [
            f"!freeze_recovery_photo_is_due({CAMERA_WORKING}, 1000)",
            f"!freeze_recovery_photo_is_due({SILENT_SINCE_100}, 109)",
            f"freeze_recovery_photo_is_due({SILENT_SINCE_100}, 110)",
            f"freeze_recovery_photo_is_due({STILL_SILENT_AT_105}, 110)",
            f"!freeze_recovery_photo_is_due({PHOTO_ASKED_AT_110}, 111)",
        ],
    )
    assert result.returncode == 0, result.stdout


def test_a_camera_that_stays_frozen_is_asked_again_once_a_minute(run_c_checks):
    result = run_c_checks(
        RECOVERY_HEADER,
        [
            f"!freeze_recovery_photo_is_due({PHOTO_ASKED_AT_110}, 169)",
            f"freeze_recovery_photo_is_due({PHOTO_ASKED_AT_110}, 170)",
        ],
    )
    assert result.returncode == 0, result.stdout


def test_a_freeze_that_five_photos_did_not_end_is_left_alone(run_c_checks):
    result = run_c_checks(
        RECOVERY_HEADER,
        [
            f"freeze_recovery_photo_is_due({FOUR_PHOTOS}, 350)",
            f"!freeze_recovery_photo_is_due({FIVE_PHOTOS}, 100000)",
        ],
    )
    assert result.returncode == 0, result.stdout


def test_the_service_restarting_its_camera_does_not_start_a_new_freeze(run_c_checks):
    restarted = f"freeze_recovery_answer_without_picture({PHOTO_ASKED_AT_110})"
    silent_again = f"freeze_recovery_camera_silent({restarted}, 115)"
    restarted_after_five = f"freeze_recovery_answer_without_picture({FIVE_PHOTOS})"
    silent_after_five = f"freeze_recovery_camera_silent({restarted_after_five}, 400)"
    result = run_c_checks(
        RECOVERY_HEADER,
        [
            f"!freeze_recovery_photo_is_due({silent_again}, 125)",
            f"freeze_recovery_photo_is_due({silent_again}, 170)",
            f"!freeze_recovery_photo_is_due({silent_after_five}, 100000)",
        ],
    )
    assert result.returncode == 0, result.stdout


def test_a_picture_ends_the_freeze_but_photos_stay_a_minute_apart(run_c_checks):
    working_after_five = f"freeze_recovery_picture_arrived({FIVE_PHOTOS})"
    frozen_again_later = f"freeze_recovery_camera_silent({working_after_five}, 1000)"
    working_after_one = f"freeze_recovery_picture_arrived({PHOTO_ASKED_AT_110})"
    frozen_again_at_once = f"freeze_recovery_camera_silent({working_after_one}, 120)"
    result = run_c_checks(
        RECOVERY_HEADER,
        [
            f"!freeze_recovery_photo_is_due({working_after_five}, 100000)",
            f"!freeze_recovery_photo_is_due({frozen_again_later}, 1009)",
            f"freeze_recovery_photo_is_due({frozen_again_later}, 1010)",
            f"!freeze_recovery_photo_is_due({frozen_again_at_once}, 169)",
            f"freeze_recovery_photo_is_due({frozen_again_at_once}, 170)",
        ],
    )
    assert result.returncode == 0, result.stdout


def test_only_a_silent_camera_counts_as_frozen_never_a_service_that_is_not_there():
    assert (
        "    if (!frame_tap_read_exactly(client->socket_fd, &header, sizeof(header)))\n"
        "        return FRAME_TAP_CAMERA_SILENT;\n"
    ) in TAP_CLIENT
    assert (
        "    if (outcome == FRAME_TAP_CAMERA_SILENT)\n"
        "        return freeze_recovery_camera_silent(recovery, now);\n"
        "    return freeze_recovery_answer_without_picture(recovery);\n"
    ) in TAP_CLIENT
    assert "if (client->wakes_frozen_camera)\n        frame_tap_client_wake_frozen_camera(" in (
        TAP_CLIENT
    )


def photo_request_sent_to_the_service():
    macro = PHOTO_REQUEST.split("#define CAMERA_PHOTO_REQUEST ", 1)[1].split("\n\n", 1)[0]
    literals = re.findall(r'"((?:[^"\\]|\\.)*)"', macro)
    return json.loads("".join(literals).replace('\\"', '"'))


def test_the_photo_is_asked_for_the_way_the_snapmaker_app_asks_for_one():
    request = photo_request_sent_to_the_service()
    assert request["method"] == "camera.take_a_photo"
    assert request["params"]["reason"] == "debug"
    assert request["params"]["filepath"].startswith("/tmp/")
    assert '#define CAMERA_PHOTO_PUBLISHER "/usr/bin/mosquitto_pub"' in PHOTO_REQUEST
    assert '"-t", "camera/request",' in PHOTO_REQUEST


def test_klipper_never_takes_the_service_s_answer_for_one_of_its_own_requests():
    assert photo_request_sent_to_the_service()["id"] == 0


def test_a_broker_that_does_not_answer_never_stops_the_capture():
    assert "waitpid(publisher, &publisher_status, WNOHANG)" in PHOTO_REQUEST
    assert (
        "static void frame_tap_client_ask_for_photo(frame_tap_client_t *client)\n"
        "{\n"
        "    frame_tap_client_end_photo_request(client);\n"
    ) in TAP_CLIENT


def test_a_request_still_running_is_ended_and_reported_as_ended_not_as_failed():
    assert (
        "    frame_tap_client_collect_photo_request(client);\n"
        "    if (client->photo_publisher <= 0)\n"
        "        return;\n"
        "    camera_photo_request_abandon(client->photo_publisher);\n"
        "    client->photo_publisher = -1;\n"
        "    frame_tap_log_photo_request(CAMERA_PHOTO_REQUEST_ENDED);\n"
    ) in TAP_CLIENT
    assert "[CAMERA_PHOTO_REQUEST_ENDED] =\n" in PHOTO_REQUEST


def test_the_photo_publisher_holds_none_of_the_capture_s_files():
    assert "posix_spawn_file_actions_addclosefrom_np(publisher_files, STDERR_FILENO + 1)" in (
        PHOTO_REQUEST
    )
    assert "&publisher_files, NULL,\n" in PHOTO_REQUEST


def test_a_capture_without_the_tap_never_ends_a_publisher_it_did_not_start():
    assert "#define FRAME_TAP_CLIENT_CLOSED { .socket_fd = -1, .photo_publisher = -1 }" in (
        TAP_CLIENT
    )
    assert "frame_tap_client_t frame_tap = FRAME_TAP_CLIENT_CLOSED;" in CAPTURE
    assert "    *client = (frame_tap_client_t)FRAME_TAP_CLIENT_CLOSED;\n" in TAP_CLIENT
    assert "if (client->photo_publisher <= 0)\n        return;" in TAP_CLIENT
    assert "photo_publisher > 0" not in TAP_CLIENT


def test_the_capture_asks_for_the_gnu_extension_before_any_system_header():
    assert CAPTURE.startswith("#define _GNU_SOURCE\n")


def test_the_capture_wakes_the_camera_only_when_asked_to():
    assert '{"wake-frozen-camera", no_argument,' in CAPTURE
    assert "bool wakes_frozen_camera = false;" in CAPTURE
    assert "open_frame_tap(&frame_tap, &v4l2, frame_sock, width, height, wakes_frozen_camera)" in (
        CAPTURE
    )


def test_the_setting_is_on_by_default_and_reaches_the_capture_on_firmware_2_1_0_only():
    setting = next(
        entry for entry in MANIFEST["config"] if entry["key"] == "CAMERA_FREEZE_RECOVERY"
    )
    assert setting["type"] == "toggle"
    assert setting["default"] == "1"
    assert "CAMERA_FREEZE_RECOVERY=$CAMERA_FREEZE_RECOVERY\n" in CONFIG_TEMPLATE
    assert "\nCAMERA_FREEZE_RECOVERY=1\n" in INIT_SCRIPT
    assert (
        'if [ "$FRAME_TAP" = "1" ] && [ "$CAMERA_FREEZE_RECOVERY" = "1" ]; then\n'
        '    CAPTURE_SOURCE="$CAPTURE_SOURCE --wake-frozen-camera"\n'
        "fi\n"
    ) in INIT_SCRIPT
    defaults_end = INIT_SCRIPT.index('[ -f "$PLUGIN/camera.conf" ] && . "$PLUGIN/camera.conf"')
    assert INIT_SCRIPT.index("\nCAMERA_FREEZE_RECOVERY=1\n") < defaults_end
