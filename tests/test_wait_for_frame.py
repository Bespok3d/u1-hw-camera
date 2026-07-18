import importlib.util
import shutil
import socket
import tempfile
import threading
from collections.abc import Iterator
from pathlib import Path

import pytest

SOURCE = (
    Path(__file__).resolve().parent.parent / "src" / "wait-for-frame" / "wait-for-frame.py"
)

spec = importlib.util.spec_from_file_location("wait_for_frame", SOURCE)
wait_for_frame_module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wait_for_frame_module)

FAKE_FRAME = b"\xff\xd8" + b"jpeg-payload" * 64 + b"\xff\xd9"


def serve_one_frame(jpeg_socket_path: Path, frame: bytes) -> threading.Thread:
    """Stand in for a capture: hand the first caller one frame and close, as `--jpeg-sock` does."""
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(str(jpeg_socket_path))
    listener.listen(1)

    def accept_once() -> None:
        connection, _ = listener.accept()
        connection.sendall(frame)
        connection.close()
        listener.close()

    thread = threading.Thread(target=accept_once, daemon=True)
    thread.start()
    return thread


@pytest.fixture
def jpeg_socket_path() -> Iterator[Path]:
    # Not pytest's tmp_path: an AF_UNIX path is capped near 104 chars and that one blows the cap.
    socket_dir = Path(tempfile.mkdtemp(prefix="/tmp/b3cam-"))
    try:
        yield socket_dir / "capture-jpeg.sock"
    finally:
        shutil.rmtree(socket_dir, ignore_errors=True)


def test_waits_until_a_capture_delivers_a_frame(jpeg_socket_path: Path) -> None:
    serve_one_frame(jpeg_socket_path, FAKE_FRAME)
    assert wait_for_frame_module.wait_for_frame(str(jpeg_socket_path), timeout_s=5.0) is True


def test_reports_the_whole_frame_not_just_the_first_chunk(jpeg_socket_path: Path) -> None:
    serve_one_frame(jpeg_socket_path, FAKE_FRAME)
    delivered = wait_for_frame_module.read_frame_size(str(jpeg_socket_path), connect_timeout_s=5.0)
    assert delivered == len(FAKE_FRAME)


def test_times_out_when_no_capture_ever_appears(jpeg_socket_path: Path) -> None:
    assert wait_for_frame_module.wait_for_frame(str(jpeg_socket_path), timeout_s=0.5) is False


def test_a_capture_that_delivers_nothing_is_not_a_frame(jpeg_socket_path: Path) -> None:
    serve_one_frame(jpeg_socket_path, b"")
    assert wait_for_frame_module.wait_for_frame(str(jpeg_socket_path), timeout_s=0.5) is False


def test_the_init_scripts_get_a_nonzero_exit_on_timeout(jpeg_socket_path: Path) -> None:
    exit_code = wait_for_frame_module.main(["wait-for-frame.py", str(jpeg_socket_path), "0.5"])
    assert exit_code == 1


def test_a_missing_socket_argument_is_a_usage_error() -> None:
    assert wait_for_frame_module.main(["wait-for-frame.py"]) == wait_for_frame_module.USAGE_ERROR
