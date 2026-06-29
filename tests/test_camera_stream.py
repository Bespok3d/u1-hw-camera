import itertools

import camera_stream

FRAME_A = b'\xff\xd8aaa\xff\xd9'
FRAME_B = b'\xff\xd8bbb\xff\xd9'

TIMEOUT = 'timeout'
EOF = 'eof'
REFUSE = 'refuse'


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
    """Hands out the scripted sockets in order, one per (re)connect. A REFUSE entry (or running out)
    raises, standing in for the capture socket being briefly gone."""

    def __init__(self, sockets):
        self.sockets = list(sockets)
        self.calls = 0

    def __call__(self):
        self.calls += 1
        if not self.sockets:
            raise ConnectionRefusedError
        nxt = self.sockets.pop(0)
        if nxt == REFUSE:
            raise ConnectionRefusedError
        return nxt


def make_stream(connect, clock, stall_after_s=5.0, advance=3.0):
    # Each reconnect's backoff sleep advances the injected clock by `advance` seconds, so a handful of
    # frameless cycles deterministically crosses (advance < stall) or stays under (advance small) the
    # stall threshold.
    return camera_stream.CaptureStream(
        '/capture.sock', connect=connect, monotonic=clock,
        sleep=lambda _seconds: clock.advance(advance), stall_after_s=stall_after_s,
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


# ── first_jpeg (snapshot frame assembly) ───────────────────────────────────────────────────────

def test_first_jpeg_returns_first_complete_frame():
    assert camera_stream.first_jpeg([b'\xff\xd8a', b'aa\xff\xd9 trailing']) == FRAME_A


def test_first_jpeg_empty_when_chunks_lack_a_complete_frame():
    assert camera_stream.first_jpeg([b'\xff\xd8parti', b'al']) == b''


# ── load_placeholder (interrupted-stream image) ────────────────────────────────────────────────

def test_load_placeholder_reads_committed_asset(tmp_path):
    (tmp_path / camera_stream.PLACEHOLDER_FILENAME).write_bytes(FRAME_A)
    assert camera_stream.load_placeholder(str(tmp_path)) == FRAME_A


def test_load_placeholder_missing_returns_empty(tmp_path):
    assert camera_stream.load_placeholder(str(tmp_path)) == b''


# ── CaptureStream reconnect + placeholder (the freeze fix) ─────────────────────────────────────

FRAME = camera_stream.CAPTURE_FRAME
STALLED = (camera_stream.CAPTURE_STALLED, None)


def test_capture_stream_reconnects_across_a_brief_stall_without_placeholder():
    # a sub-threshold gap is absorbed by the silent reconnect: frames resume, no message flashes
    clock = Clock()
    sockets = [FakeSocket([FRAME_A, TIMEOUT]), FakeSocket([FRAME_B, TIMEOUT])]
    connect = ConnectFactory(sockets)
    items = list(itertools.islice(make_stream(connect, clock, advance=0.5).frames(), 2))
    assert items == [(FRAME, FRAME_A), (FRAME, FRAME_B)]
    assert STALLED not in items
    assert sockets[0].closed


def test_capture_stream_reconnects_when_capture_drops_us():
    # capture closing our client (EOF) is a stall, not the end of the viewer's stream
    clock = Clock()
    connect = ConnectFactory([FakeSocket([FRAME_A, EOF]), FakeSocket([FRAME_B, TIMEOUT])])
    items = list(itertools.islice(make_stream(connect, clock, advance=0.5).frames(), 2))
    assert items == [(FRAME, FRAME_A), (FRAME, FRAME_B)]


def test_capture_stream_emits_placeholder_after_a_prolonged_stall():
    # capture down past the stall threshold -> the response stays open and signals a placeholder,
    # repeatedly, instead of ending (the old giveup that froze the tile)
    clock = Clock()
    connect = ConnectFactory([FakeSocket([FRAME_A, TIMEOUT])])  # one frame, then refusals
    items = list(itertools.islice(make_stream(connect, clock, advance=3.0).frames(), 3))
    assert items[0] == (FRAME, FRAME_A)
    assert STALLED in items[1:]


def test_capture_stream_resumes_real_frames_after_a_placeholder():
    # a stall shows the placeholder, then a recovered capture delivers live frames again, all within
    # the one never-ending response
    clock = Clock()
    connect = ConnectFactory([FakeSocket([FRAME_A, TIMEOUT]), REFUSE, REFUSE,
                              FakeSocket([FRAME_B, TIMEOUT])])
    items = list(itertools.islice(make_stream(connect, clock, advance=3.0).frames(), 4))
    assert items[0] == (FRAME, FRAME_A)
    assert STALLED in items
    assert (FRAME, FRAME_B) in items


# ── MjpegPacer (per-connection send state) ─────────────────────────────────────────────────────

def test_pacer_counts_sent_then_drops_on_backpressure():
    clock = Clock()
    pacer = camera_stream.MjpegPacer(0, monotonic=clock)
    assert pacer.classify(0) == camera_stream.FRAME_SEND
    pacer.note_sent(frame_size=2000, header_size=60)
    assert pacer.sent == 1
    assert pacer.classify(3000) == camera_stream.FRAME_DROP
    assert pacer.dropped == 1
