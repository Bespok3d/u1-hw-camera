"""The camera hands the printer's video stack a whole picture or nothing at all.

Three things used to let half a picture through. The shim asked for 1920x1080x2 bytes when an NV12
picture is 1920x1080x1.5, so every read returned one picture plus the top of the next one, stitched.
It then accepted any number of bytes as a finished picture, and left the length of the previous one
behind when a read failed. And the socket the printer's own video stack reads had no drop rule at
all, so a reader that fell behind was written into anyway and cut off mid picture.

These run the real rules, compiled, and pin them to the places that ask them the question.
"""

from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
GEOMETRY_HEADER = "src/v4l2-imposter/frame_geometry.h"
BACKLOG_HEADER = "src/v4l2-mpp/common/capture-common/frame_backlog.h"
IMPOSTER = (REPO_ROOT / "src/v4l2-imposter/main.c").read_text(encoding="utf-8")
SOCK_CTX = (
    REPO_ROOT / "src/v4l2-mpp/common/capture-common/sock_ctx.h"
).read_text(encoding="utf-8")
FAKE_SERVICE_SOURCE = "src/fake-service/main.c"
FAKE_SERVICE = (REPO_ROOT / FAKE_SERVICE_SOURCE).read_text(encoding="utf-8")
CAPTURE = (
    REPO_ROOT / "src/v4l2-mpp/apps/capture-v4l2-raw-mpp/main.c"
).read_text(encoding="utf-8")
INIT_SCRIPT = (
    REPO_ROOT / "plugin/files/etc/init.d/s65camera-hw"
).read_text(encoding="utf-8")

# 1920 * 1080 * 3 / 2. The size the shim used to report was 4147200, which is this frame plus a
# third of the next one.
NV12_1080P_BYTES = 3110400


def test_an_nv12_picture_is_one_and_a_half_bytes_per_pixel(run_c_checks):
    result = run_c_checks(
        GEOMETRY_HEADER,
        [
            f"whole_frame_bytes(FRAME_FORMAT_NV12, 1920, 1080) == {NV12_1080P_BYTES}",
            f"capture_buffer_bytes(FRAME_FORMAT_NV12, 1920, 1080) == {NV12_1080P_BYTES}",
            "whole_frame_bytes(FRAME_FORMAT_NV12, 1280, 720) == 1382400",
            "frame_bytes_per_line(FRAME_FORMAT_NV12, 1920) == 1920",
            "whole_frame_bytes(FRAME_FORMAT_YUYV, 1920, 1080) == 4147200",
            "frame_bytes_per_line(FRAME_FORMAT_YUYV, 1920) == 3840",
        ],
    )
    assert result.returncode == 0, result.stdout


def test_a_picture_that_ends_early_is_not_a_picture(run_c_checks):
    result = run_c_checks(
        GEOMETRY_HEADER,
        [
            f"frame_read_is_whole({NV12_1080P_BYTES}, {NV12_1080P_BYTES})",
            f"!frame_read_is_whole({NV12_1080P_BYTES}, {NV12_1080P_BYTES - 1})",
            f"!frame_read_is_whole({NV12_1080P_BYTES}, 1024)",
            f"!frame_read_is_whole({NV12_1080P_BYTES}, 0)",
        ],
    )
    assert result.returncode == 0, result.stdout


def test_a_jpeg_ends_where_it_ends(run_c_checks):
    """MJPEG has no fixed length, so any bytes at all are the whole picture and none are none."""
    result = run_c_checks(
        GEOMETRY_HEADER,
        [
            "whole_frame_bytes(FRAME_FORMAT_MJPEG, 1920, 1080) == 0",
            "frame_bytes_per_line(FRAME_FORMAT_MJPEG, 1920) == 0",
            "capture_buffer_bytes(FRAME_FORMAT_JPEG, 1920, 1080) == 4147200",
            "frame_read_is_whole(0, 4096)",
            "!frame_read_is_whole(0, 0)",
        ],
    )
    assert result.returncode == 0, result.stdout


def test_a_reader_behind_on_a_big_picture_loses_it_whole(run_c_checks):
    """A picture larger than the socket completes only because the reader keeps taking bytes."""
    result = run_c_checks(
        BACKLOG_HEADER,
        [
            f"reader_cannot_take_whole_frame(1, 212992, {NV12_1080P_BYTES})",
            f"reader_cannot_take_whole_frame(212992, 212992, {NV12_1080P_BYTES})",
            f"!reader_cannot_take_whole_frame(0, 212992, {NV12_1080P_BYTES})",
        ],
    )
    assert result.returncode == 0, result.stdout


def test_a_reader_watching_the_mjpeg_stream_is_allowed_to_lag(run_c_checks):
    """A picture that fits gets the socket's own room: falling behind between frames is normal."""
    result = run_c_checks(
        BACKLOG_HEADER,
        [
            "!reader_cannot_take_whole_frame(40000, 212992, 60000)",
            "reader_cannot_take_whole_frame(60000, 212992, 60000)",
            "reader_cannot_take_whole_frame(120000, 212992, 60000)",
            "!reader_cannot_take_whole_frame(0, 0, 60000)",
        ],
    )
    assert result.returncode == 0, result.stdout


def test_the_shim_measures_every_picture_against_the_declared_size():
    assert "config_width * config_height * 2" not in IMPOSTER
    assert IMPOSTER.count("capture_buffer_bytes(config_format, config_width, config_height)") == 3
    assert "frame_bytes_per_line(config_format, config_width)" in IMPOSTER
    assert "if (!frame_read_is_whole(expected_frame_bytes, (size_t)frame_bytes))" in IMPOSTER


def test_a_failed_read_reports_no_picture_rather_than_the_last_one():
    fetch_frame = IMPOSTER.split("static int fetch_frame(", 1)[1].split("\nstatic ", 1)[0]
    cleared_before_reading = fetch_frame.index("buffer->bytes_used = 0;")
    assert cleared_before_reading < fetch_frame.index("read_fully(")


def test_the_socket_the_printer_reads_drops_whole_pictures():
    assert "raw_frame_sock.allow_drops = true;" in CAPTURE
    assert "sock_client_would_be_torn(client->fd, size)" in SOCK_CTX
    # The old rule compared the backlog with the size of the PREVIOUS write, which is 0 on a fresh
    # reader and never reached by a picture far larger than the socket.
    assert "last_size" not in SOCK_CTX


def test_a_dropped_or_cut_off_picture_is_written_where_it_can_be_read():
    assert 'getenv("V4L2_IMPOSTER_LOG")' in IMPOSTER
    assert "fprintf(stderr," not in IMPOSTER
    assert "export V4L2_IMPOSTER_LOG=$RUN/v4l2-imposter.log" in INIT_SCRIPT
    # The picture is dropped by the capture daemon, whose words go to /dev/null when it is
    # started in the background, so the supervisor it runs under is given a file to put them in.
    # Asked for in the environment, never as an option: the script ships as text beside a
    # binary that ships already built, so an option kills the camera on a stale package.
    assert 'getenv("FAKE_SERVICE_LOG")' in FAKE_SERVICE
    assert "FAKE_SERVICE_LOG=$RUN/capture-mipi-mpp.log" in INIT_SCRIPT


def test_the_camera_log_cannot_fill_the_printer(compile_c_source):
    assert "LOG_MAX_BYTES" in FAKE_SERVICE
    assert FAKE_SERVICE.count("log_empty_when_full(std") == 2
    compiled = compile_c_source(FAKE_SERVICE_SOURCE)
    assert compiled.returncode == 0, compiled.stderr


def test_the_shim_is_told_the_size_the_camera_is_actually_started_at():
    assert "export V4L2_IMPOSTER_WIDTH=$CAM_WIDTH" in INIT_SCRIPT
    assert "export V4L2_IMPOSTER_HEIGHT=$CAM_HEIGHT" in INIT_SCRIPT
