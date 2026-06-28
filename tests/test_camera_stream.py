import camera_stream

FRAME_A = b'\xff\xd8aaa\xff\xd9'
FRAME_B = b'\xff\xd8bbb\xff\xd9'

TIMEOUT = 'timeout'
EOF = 'eof'


class Clock:
    def __init__(self):
        self.now = 0.0

    def __call__(self):
        return self.now

    def advance(self, seconds):
        self.now += seconds


class FakeSocket:
    """A capture socket driven by a scripted list of reads: bytes are returned, TIMEOUT raises a
    socket timeout, EOF returns b'' (peer closed). Once the script is exhausted it keeps timing out,
    standing in for a capture that has gone quiet."""

    def __init__(self, actions):
        self.actions = list(actions)
        self.closed = False

    def recv(self, _size):
        if not self.actions:
            raise TimeoutError
        action = self.actions.pop(0)
        if action == TIMEOUT:
            raise TimeoutError
        if action == EOF:
            return b''
        return action

    def settimeout(self, _seconds):
        pass

    def close(self):
        self.closed = True


class ConnectFactory:
    """Hands out the scripted sockets in order, one per (re)connect; raises once exhausted so a
    further reconnect attempt looks like the capture socket being gone."""

    def __init__(self, sockets):
        self.sockets = list(sockets)
        self.calls = 0

    def __call__(self):
        self.calls += 1
        if not self.sockets:
            raise ConnectionRefusedError
        return self.sockets.pop(0)


def make_stream(connect, clock):
    jump = camera_stream.RECOVER_WINDOW_S + 1
    return camera_stream.CaptureStream(
        '/capture.sock', connect=connect, monotonic=clock,
        sleep=lambda _seconds: clock.advance(jump),
    )


# ── extract_jpeg_frames (pure frame boundary parsing) ──────────────────────────────────────────

def test_extract_two_complete_frames():
    frames, rest = camera_stream.extract_jpeg_frames(FRAME_A + FRAME_B)
    assert frames == [FRAME_A, FRAME_B]
    assert rest == b''


def test_extract_drops_garbage_before_soi():
    frames, rest = camera_stream.extract_jpeg_frames(b'junk' + FRAME_A)
    assert frames == [FRAME_A]
    assert rest == b''


def test_extract_holds_incomplete_frame_until_completed():
    frames, rest = camera_stream.extract_jpeg_frames(b'\xff\xd8partial')
    assert frames == []
    assert rest == b'\xff\xd8partial'
    frames, rest = camera_stream.extract_jpeg_frames(rest + b'more\xff\xd9')
    assert frames == [b'\xff\xd8partialmore\xff\xd9']
    assert rest == b''


def test_extract_keeps_split_soi_marker_across_reads():
    frames, rest = camera_stream.extract_jpeg_frames(b'noframe\xff')
    assert frames == []
    assert rest == b'\xff'
    frames, rest = camera_stream.extract_jpeg_frames(rest + b'\xd8body\xff\xd9')
    assert frames == [b'\xff\xd8body\xff\xd9']


# ── classify_frame (throttle / backpressure / stale) ───────────────────────────────────────────

def test_classify_throttles_within_frame_interval():
    assert camera_stream.classify_frame(0.05, 0.1, 0, 1000, 10000) == camera_stream.FRAME_DROP


def test_classify_sends_when_send_buffer_clear():
    assert camera_stream.classify_frame(1.0, 0.0, 0, 1000, 10000) == camera_stream.FRAME_SEND


def test_classify_drops_on_backpressure_within_timeout():
    assert camera_stream.classify_frame(1.0, 0.0, 5000, 1000, 10000) == camera_stream.FRAME_DROP


def test_classify_stale_when_backpressure_exceeds_timeout():
    assert camera_stream.classify_frame(11.0, 0.0, 5000, 1000, 10000) == camera_stream.FRAME_STALE


# ── read_chunk ─────────────────────────────────────────────────────────────────────────────────

def test_read_chunk_none_for_missing_socket():
    assert camera_stream.read_chunk(None) is None


def test_read_chunk_none_on_timeout():
    assert camera_stream.read_chunk(FakeSocket([TIMEOUT])) is None


def test_read_chunk_none_on_closed_peer():
    assert camera_stream.read_chunk(FakeSocket([EOF])) is None


def test_read_chunk_returns_data():
    assert camera_stream.read_chunk(FakeSocket([b'xyz'])) == b'xyz'


# ── CaptureStream reconnect (the freeze fix) ───────────────────────────────────────────────────

def test_capture_stream_reconnects_across_a_stall():
    clock = Clock()
    sockets = [FakeSocket([FRAME_A, TIMEOUT]), FakeSocket([FRAME_B, TIMEOUT]),
               FakeSocket([TIMEOUT])]
    connect = ConnectFactory(sockets)
    frames = list(make_stream(connect, clock).frames())
    assert frames == [FRAME_A, FRAME_B]
    assert connect.calls == 3
    assert sockets[0].closed


def test_capture_stream_reconnects_when_capture_drops_us():
    # capture closing our client (EOF) is a stall, not the end of the viewer's stream
    clock = Clock()
    connect = ConnectFactory([FakeSocket([FRAME_A, EOF]), FakeSocket([FRAME_B, TIMEOUT]),
                              FakeSocket([TIMEOUT])])
    assert list(make_stream(connect, clock).frames()) == [FRAME_A, FRAME_B]


def test_capture_stream_gives_up_after_recover_window():
    clock = Clock()
    connect = ConnectFactory([FakeSocket([TIMEOUT]), FakeSocket([TIMEOUT])])
    assert list(make_stream(connect, clock).frames()) == []


def test_capture_stream_survives_a_failed_reconnect():
    # a refused reconnect (capture socket briefly gone) is just another stall, recovered from later
    clock = Clock()
    connect = ConnectFactory([FakeSocket([FRAME_A, TIMEOUT])])  # only one socket, then refusals
    frames = list(make_stream(connect, clock).frames())
    assert frames == [FRAME_A]


# ── MjpegPacer (per-connection send state) ─────────────────────────────────────────────────────

def test_pacer_counts_sent_then_drops_on_backpressure():
    clock = Clock()
    pacer = camera_stream.MjpegPacer(0, monotonic=clock)
    assert pacer.classify(0) == camera_stream.FRAME_SEND
    pacer.note_sent(frame_size=2000, header_size=60)
    assert pacer.sent == 1
    assert pacer.classify(3000) == camera_stream.FRAME_DROP
    assert pacer.dropped == 1
