#!/usr/bin/env python3
"""Block until a capture delivers its first JPEG frame, so a bring-up can serialize on it.

Both cameras share one hardware encoder. Started back-to-back and backgrounded, they cold-initialize
it at the same instant and can come up frameless. Waiting here for the first real frame is what makes
the second camera start against a settled encoder instead of contending with the first.

Exit 0 once a frame arrives, 1 on timeout, 2 on a usage error.
"""

import socket
import sys
import time

FRAME_TIMEOUT_S = 20.0
CONNECT_TIMEOUT_S = 2.0
RETRY_INTERVAL_S = 0.25
CHUNK_SIZE = 65536
USAGE_ERROR = 2


def read_frame_size(jpeg_socket_path: str, connect_timeout_s: float) -> int:
    """Drain the capture's JPEG socket and return the frame size in bytes.

    The capture writes one JPEG and closes it (`--jpeg-sock` is one-frame), so draining to EOF both
    measures the frame and lets the capture close its own side rather than take an EPIPE from us.
    """
    capture_socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    capture_socket.settimeout(connect_timeout_s)
    try:
        capture_socket.connect(jpeg_socket_path)
        return sum(len(chunk) for chunk in iter(lambda: capture_socket.recv(CHUNK_SIZE), b""))
    finally:
        capture_socket.close()


def wait_for_frame(jpeg_socket_path: str, timeout_s: float) -> bool:
    """Retry until the capture hands over a non-empty frame, or the timeout runs out."""
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            if read_frame_size(jpeg_socket_path, CONNECT_TIMEOUT_S) > 0:
                return True
        except OSError:
            pass  # The socket is absent or refusing while the capture starts up: that is what we wait out.
        time.sleep(RETRY_INTERVAL_S)
    return False


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(f"usage: {argv[0]} <jpeg-sock-path> [timeout-seconds]", file=sys.stderr)
        return USAGE_ERROR
    timeout_s = float(argv[2]) if len(argv) > 2 else FRAME_TIMEOUT_S
    return 0 if wait_for_frame(argv[1], timeout_s) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
