import importlib.util
import sys
from pathlib import Path

# camera-stream.py is a hyphenated script entry point (run as `python3 camera-stream.py`, never
# imported in production), so load it by path and register it as `camera_stream` for the tests.
SOURCE = (
    Path(__file__).resolve().parent.parent
    / "src" / "v4l2-mpp" / "apps" / "stream-http" / "camera-stream.py"
)

spec = importlib.util.spec_from_file_location("camera_stream", SOURCE)
camera_stream = importlib.util.module_from_spec(spec)
sys.modules["camera_stream"] = camera_stream
spec.loader.exec_module(camera_stream)
