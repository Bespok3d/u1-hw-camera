import importlib.util
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

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


# The whole-frame rules are C, and the printer is the only place the shim itself runs, so the two
# headers that hold the arithmetic and the drop decision are written to compile on their own: no
# kernel headers, no sockets. That lets these tests run the real rule here rather than read the
# source and hope.
REPO_ROOT = Path(__file__).resolve().parent.parent

HARNESS = """
#include <stdio.h>
#include "{header}"

#define CHECK(claim) do {{ if (!(claim)) {{ printf("%s\\n", #claim); return 1; }} }} while (0)

int main(void)
{{
{checks}
    return 0;
}}
"""


@pytest.fixture(name="run_c_checks")
def fixture_run_c_checks(tmp_path):
    """Compile the named header with C claims about it and report the first one that is false."""

    def run_c_checks(header_path, checks):
        compiler = shutil.which("cc") or shutil.which("gcc")
        if compiler is None:
            pytest.skip("no C compiler on this machine, so the frame rules cannot be run")
        source = tmp_path / "harness.c"
        header = REPO_ROOT / header_path
        claims = "\n".join(f"    CHECK({claim});" for claim in checks)
        source.write_text(HARNESS.format(header=header.name, checks=claims), encoding="utf-8")
        program = tmp_path / "harness"
        subprocess.run(
            [compiler, "-Wall", "-Werror", f"-I{header.parent}", "-o", str(program), str(source)],
            check=True,
        )
        return subprocess.run([str(program)], capture_output=True, text=True, check=False)

    return run_c_checks


@pytest.fixture(name="compile_c_source")
def fixture_compile_c_source():
    """Compile one C source for its errors alone, so it is checked without a printer."""

    def compile_c_source(source_path):
        compiler = shutil.which("cc") or shutil.which("gcc")
        if compiler is None:
            pytest.skip("no C compiler on this machine, so the source cannot be compiled")
        return subprocess.run(
            [compiler, "-Wall", "-fsyntax-only", str(REPO_ROOT / source_path)],
            capture_output=True,
            text=True,
            check=False,
        )

    return compile_c_source
