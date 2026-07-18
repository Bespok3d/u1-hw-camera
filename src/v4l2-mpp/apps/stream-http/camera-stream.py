#!/usr/bin/env python3

import argparse
import fcntl
import json
import os
import socket
import struct
import time
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

ALLOWED_PATHS = {
    '/': 'index.html',
    '/player': 'player.html',
    '/webrtc': 'webrtc.html',
    '/control': 'control.html',
}

INSTALLED_HTML_DIR = '/usr/share/camera-stream/html'

SIOCOUTQ = 0x5411
MJPEG_SEND_TIMEOUT_MS = 10000

# A long-lived MJPEG response must survive a capture-side hiccup: the capture binary can briefly
# stop feeding our one connection (or drop it after its own slow-client timeout), and a plain
# `<img>` stream does not reconnect, so the viewer freezes until a manual refresh. We give the
# capture-socket read a timeout and, on a stall, reconnect internally and keep streaming. A stall
# longer than STALL_PLACEHOLDER_S surfaces a "stream interrupted" placeholder frame while the
# response stays open, so the tile shows a clear message instead of a frozen image and resumes live
# frames the moment the capture recovers.
CAPTURE_READ_TIMEOUT_S = 2.0
RECONNECT_BACKOFF_S = 0.5
STALL_PLACEHOLDER_S = 5.0

PLACEHOLDER_FILENAME = 'placeholder.jpg'

JPEG_SOI = b'\xff\xd8'
JPEG_EOI = b'\xff\xd9'

FRAME_SEND = 'send'
FRAME_DROP = 'drop'
FRAME_STALE = 'stale'

CAPTURE_FRAME = 'frame'
CAPTURE_STALLED = 'stalled'


def log(msg):
    ts = time.strftime('%H:%M:%S')
    print(f"[{ts}] {msg}", flush=True)


def extract_jpeg_frames(buffer):
    """Split a byte buffer into complete JPEG frames (SOI..EOI) and the still-incomplete tail.

    Pure (no I/O): returns (frames, remaining). Bytes before the first SOI are discarded; a marker
    split across reads is preserved in `remaining` so the next read can complete it.
    """
    frames = []
    remaining = buffer
    while True:
        start = remaining.find(JPEG_SOI)
        if start == -1:
            return frames, remaining[-1:] if remaining else b''
        end = remaining.find(JPEG_EOI, start + 2)
        if end == -1:
            return frames, remaining[start:]
        frames.append(remaining[start:end + 2])
        remaining = remaining[end + 2:]


def first_jpeg(chunks):
    """The first complete JPEG (SOI..EOI) assembled from an iterable of byte chunks, or b'' when the
    chunks run out without one. Pure (no I/O), so the snapshot assembly is unit-testable."""
    buffer = b''
    for chunk in chunks:
        frames, buffer = extract_jpeg_frames(buffer + chunk)
        if frames:
            return frames[0]
    return b''


def classify_frame(elapsed, frame_interval, unsent_bytes, last_sent_size, send_timeout_ms):
    """Decide what to do with one MJPEG frame for a client, given the seconds elapsed since the last
    send. Pure: the caller supplies the clock and the socket's unsent-byte count. Throttle to the
    target fps, drop while the client's send buffer is backing up, and call the client stale once it
    has not drained for send_timeout_ms.
    """
    if frame_interval > 0 and elapsed < frame_interval:
        return FRAME_DROP
    if unsent_bytes < last_sent_size:
        return FRAME_SEND
    if elapsed * 1000 > send_timeout_ms:
        return FRAME_STALE
    return FRAME_DROP


def close_quietly(sock):
    if sock is None:
        return
    try:
        sock.close()
    except OSError:
        pass


def read_chunk(sock, chunk_size=65536):
    """Receive up to chunk_size bytes; None on a read timeout, a closed peer, or no socket."""
    if sock is None:
        return None
    try:
        chunk = sock.recv(chunk_size)
    except OSError:
        return None
    return chunk or None


def read_socket(sock_path, chunk_size=65536, timeout_s=CAPTURE_READ_TIMEOUT_S):
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(timeout_s)
    sock.connect(sock_path)
    try:
        while True:
            chunk = sock.recv(chunk_size)
            if not chunk:
                break
            yield chunk
    finally:
        sock.close()


def read_h264_from_keyframe(sock_path, chunk_size=65536):
    yield from read_socket(sock_path, chunk_size)


def socket_req_and_resp(sock_path, request):
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(5.0)
    try:
        sock.connect(sock_path)
        data = json.dumps(request) + '\n'
        sock.sendall(data.encode())
        response = b''
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            response += chunk
            if b'\n' in response:
                break
        return json.loads(response.decode().strip())
    finally:
        sock.close()


class CaptureStream:
    """JPEG frames from a capture unix socket, transparently reconnecting across capture-side stalls
    so a brief producer hiccup never ends the viewer's MJPEG response. `frames()` yields a
    (CAPTURE_FRAME, jpeg) for each real frame and a (CAPTURE_STALLED, None) signal once the capture
    has been frameless past STALL_PLACEHOLDER_S (repeated each reconnect cycle while it stays down),
    so the caller can surface a placeholder while the response stays open. It keeps reconnecting and
    only stops when the consumer stops reading (the client is gone). `connect`/`monotonic`/`sleep`
    are injectable so the reconnect behaviour is unit-testable.
    """

    def __init__(self, sock_path, connect=None, monotonic=time.monotonic, sleep=time.sleep,
                 stall_after_s=STALL_PLACEHOLDER_S):
        self.sock_path = sock_path
        self._connect = connect or self._default_connect
        self._monotonic = monotonic
        self._sleep = sleep
        self.stall_after_s = stall_after_s

    def _default_connect(self):
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(CAPTURE_READ_TIMEOUT_S)
        sock.connect(self.sock_path)
        return sock

    def _open(self):
        try:
            return self._connect()
        except OSError:
            return None

    def _reopen(self, sock):
        close_quietly(sock)
        self._sleep(RECONNECT_BACKOFF_S)
        return self._open()

    def _stall_signal(self, last_frame_at):
        if self._monotonic() - last_frame_at >= self.stall_after_s:
            yield (CAPTURE_STALLED, None)

    def frames(self):
        buffer = b''
        sock = self._open()
        last_frame_at = self._monotonic()
        try:
            while True:
                chunk = read_chunk(sock)
                if chunk is None:
                    yield from self._stall_signal(last_frame_at)
                    sock = self._reopen(sock)
                    continue
                complete, buffer = extract_jpeg_frames(buffer + chunk)
                if complete:
                    last_frame_at = self._monotonic()
                yield from ((CAPTURE_FRAME, frame) for frame in complete)
        finally:
            close_quietly(sock)


class MjpegPacer:
    """Per-connection MJPEG send pacing: throttle to a target fps and drop frames while the client's
    send buffer backs up, declaring the client stale once it has not drained for too long.
    """

    def __init__(self, fps, monotonic=time.monotonic):
        self.frame_interval = 1.0 / fps if fps > 0 else 0.0
        self._monotonic = monotonic
        self.last_sent_at = monotonic()
        self.last_sent_size = 1024
        self.sent = 0
        self.dropped = 0

    def classify(self, unsent_bytes):
        elapsed = self._monotonic() - self.last_sent_at
        decision = classify_frame(elapsed, self.frame_interval, unsent_bytes,
                                  self.last_sent_size, MJPEG_SEND_TIMEOUT_MS)
        if decision == FRAME_DROP:
            self.dropped += 1
        return decision

    def note_sent(self, frame_size, header_size):
        self.last_sent_at = self._monotonic()
        self.last_sent_size = header_size + frame_size + 2
        self.sent += 1

    def stats(self):
        return f"sent {self.sent}, dropped {self.dropped}"


class CameraHandler(SimpleHTTPRequestHandler):
    jpeg_sock = None
    mjpeg_sock = None
    h264_sock = None
    webrtc_sock = None
    control_sock = None
    html_dir = None
    placeholder_jpeg = b''

    def log_message(self, format, *args):
        log(f"HTTP {self.address_string()} - {format % args}")

    def translate_path(self, path):
        parsed_path = urlparse(path).path
        if parsed_path not in ALLOWED_PATHS:
            return None
        return os.path.join(self.html_dir, ALLOWED_PATHS[parsed_path])

    def do_GET(self):
        path = urlparse(self.path).path
        if path == '/snapshot.jpg':
            self.handle_snapshot()
        elif path == '/stream.mjpg':
            query = parse_qs(urlparse(self.path).query)
            self.handle_mjpeg_stream(int(query.get('fps', [0])[0]))
        elif path == '/stream.h264':
            self.handle_h264_stream()
        elif path == '/control' and not self.control_sock:
            self.send_error(503, 'Control not available')
        elif path == '/webrtc' and not self.webrtc_sock:
            self.send_response(302)
            self.send_header('Location', 'player')
            self.end_headers()
        elif path not in ALLOWED_PATHS:
            self.send_error(404, "File not found")
        else:
            SimpleHTTPRequestHandler.do_GET(self)

    def do_POST(self):
        path = urlparse(self.path).path
        if path == '/webrtc' and not self.webrtc_sock:
            self.send_error(503, 'WebRTC not available')
        elif path == '/webrtc':
            self.handle_socket_req_and_resp(self.webrtc_sock)
        elif path == '/control' and not self.control_sock:
            self.send_error(503, 'Control not available')
        elif path == '/control':
            self.handle_socket_req_and_resp(self.control_sock)
        else:
            self.send_error(404, 'Not Found')

    def _snapshot_chunks(self):
        try:
            yield from read_socket(self.jpeg_sock)
        except OSError as e:
            log(f"JPEG error: {e}")

    def _read_one_jpeg(self):
        return first_jpeg(self._snapshot_chunks())

    def _send_jpeg(self, payload):
        self.send_response(200)
        self.send_header('Content-Type', 'image/jpeg')
        self.send_header('Cache-Control', 'no-cache, no-store, must-revalidate')
        self.end_headers()
        try:
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError) as e:
            log(f"JPEG client disconnected: {e}")

    def handle_snapshot(self):
        captured = self._read_one_jpeg() if self.jpeg_sock else b''
        payload = captured or self.placeholder_jpeg
        if not payload:
            self.send_error(503, 'Snapshot not available')
            return
        self._send_jpeg(payload)

    def _begin_mjpeg_response(self):
        self.connection.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 128 * 1024)
        self.send_response(200)
        self.send_header('Content-Type', 'multipart/x-mixed-replace; boundary=frame')
        self.send_header('Cache-Control', 'no-cache, no-store, must-revalidate')
        self.end_headers()

    def _unsent_bytes(self):
        buf = fcntl.ioctl(self.connection.fileno(), SIOCOUTQ, b'\x00' * 4)
        return struct.unpack('I', buf)[0]

    def _send_mjpeg_frame(self, frame):
        header = (b'--frame\r\nContent-Type: image/jpeg\r\nContent-Length: '
                  + str(len(frame)).encode() + b'\r\n\r\n')
        self.connection.sendall(header)
        self.connection.sendall(frame)
        self.connection.sendall(b'\r\n')
        return len(header)

    def _relay_frame(self, frame, pacer):
        decision = pacer.classify(self._unsent_bytes())
        if decision == FRAME_STALE:
            raise TimeoutError(f"no frame sent in {MJPEG_SEND_TIMEOUT_MS}ms")
        if decision == FRAME_DROP:
            return
        header_size = self._send_mjpeg_frame(frame)
        pacer.note_sent(len(frame), header_size)

    def _pump_mjpeg(self, capture, pacer):
        # A stall signal carries no frame; relay the placeholder in its place (skipped only when
        # the placeholder asset is missing) so the viewer sees the interrupted message, not a
        # frozen one.
        for kind, frame in capture.frames():
            payload = self.placeholder_jpeg if kind == CAPTURE_STALLED else frame
            if payload:
                self._relay_frame(payload, pacer)

    def handle_mjpeg_stream(self, fps=0):
        if not self.mjpeg_sock:
            self.send_error(503, 'MJPEG stream not available')
            return
        self._begin_mjpeg_response()
        pacer = MjpegPacer(fps)
        log(f"MJPEG client connected: {self.mjpeg_sock}, fps={fps}")
        try:
            self._pump_mjpeg(CaptureStream(self.mjpeg_sock), pacer)
        except (BrokenPipeError, ConnectionResetError) as e:
            log(f"MJPEG client disconnected: {e} ({pacer.stats()})")
        except TimeoutError as e:
            log(f"MJPEG client stale: {e} ({pacer.stats()})")
        except OSError as e:
            log(f"MJPEG stream error: {e} ({pacer.stats()})")

    def handle_h264_stream(self):
        if not self.h264_sock:
            self.send_error(503, 'H264 stream not available')
            return
        self.send_response(200)
        self.send_header('Content-Type', 'video/h264')
        self.send_header('Cache-Control', 'no-cache, no-store, must-revalidate')
        self.end_headers()
        try:
            for chunk in read_h264_from_keyframe(self.h264_sock):
                self.wfile.write(chunk)
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError) as e:
            log(f"H264 client disconnected: {e}")
        except OSError as e:
            log(f"H264 stream error: {e}")

    def send_json_response(self, code, body):
        data = json.dumps(body).encode()
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', len(data))
        self.end_headers()
        self.wfile.write(data)

    def handle_socket_req_and_resp(self, socket_path):
        try:
            content_length = int(self.headers.get('Content-Length', 0))
            request = json.loads(self.rfile.read(content_length).decode())
            self.send_json_response(200, socket_req_and_resp(socket_path, request))
        except (OSError, ValueError) as e:
            log(f"Socket request/response error: {e}")
            self.send_json_response(500, {'error': str(e)})


def resolve_html_dir():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    local = os.path.join(script_dir, 'html')
    return local if os.path.isdir(local) else INSTALLED_HTML_DIR


def build_arg_parser(default_html_dir):
    parser = argparse.ArgumentParser(description='Bespok3d camera stream server')
    parser.add_argument('-p', '--port', type=int, default=8080, help='HTTP port')
    parser.add_argument('--bind', default='0.0.0.0', help='Bind address')
    parser.add_argument('--html-dir', default=default_html_dir, help='HTML directory')
    parser.add_argument('--jpeg-sock', required=True, help='JPEG snapshot socket')
    parser.add_argument('--mjpeg-sock', required=True, help='MJPEG stream socket')
    parser.add_argument('--h264-sock', required=True, help='H264 stream socket')
    parser.add_argument('--webrtc-sock', help='WebRTC signaling socket')
    parser.add_argument('--control-sock', help='V4L2 control interface socket')
    return parser


def load_placeholder(html_dir):
    """The "stream interrupted" image shown when the capture is down. It ships beside the player
    HTML (a committed, architecture-independent asset), so the html dir is also where we read it."""
    path = os.path.join(html_dir, PLACEHOLDER_FILENAME)
    try:
        with open(path, 'rb') as handle:
            return handle.read()
    except OSError:
        log(f"placeholder image not found at {path}")
        return b''


def configure_handler(args):
    CameraHandler.html_dir = args.html_dir
    CameraHandler.placeholder_jpeg = load_placeholder(args.html_dir)
    CameraHandler.jpeg_sock = args.jpeg_sock
    CameraHandler.mjpeg_sock = args.mjpeg_sock
    CameraHandler.h264_sock = args.h264_sock
    CameraHandler.webrtc_sock = args.webrtc_sock
    CameraHandler.control_sock = args.control_sock


def log_routes(args):
    log(f"Server running on http://{args.bind}:{args.port}")
    log(f"  HTML directory: {args.html_dir}")
    log("  /snapshot.jpg  - JPEG snapshot")
    log("  /stream.mjpg   - MJPEG stream")
    log("  /stream.h264   - H264 stream")
    log("  /player        - H264 player")
    if args.webrtc_sock:
        log("  /webrtc        - WebRTC player (GET/POST)")
    if args.control_sock:
        log("  /control       - Control interface (GET/POST)")


def main():
    parser = build_arg_parser(resolve_html_dir())
    args = parser.parse_args()
    configure_handler(args)
    server = ThreadingHTTPServer((args.bind, args.port), CameraHandler)
    log_routes(args)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log("Shutting down...")


if __name__ == '__main__':
    main()
