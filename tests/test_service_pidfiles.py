"""A leftover pidfile is what killed the USB camera on an update.

start-stop-daemon -S reads the pidfile, finds that number alive, and decides the service is already
running, so the capture never starts and the stream servers serve nothing. The stop that came before
it neither waited for the process to die nor removed its pidfile. These tests run the two guards
from the init scripts for real, in a shell, and pin the wiring in both scripts.
"""

import subprocess
from pathlib import Path

PLUGIN = Path(__file__).resolve().parent.parent / "plugin"
INIT_SCRIPTS = {
    "s65camera-hw": (PLUGIN / "files/etc/init.d/s65camera-hw").read_text(encoding="utf-8"),
    "s65camera-usb": (PLUGIN / "files/etc/init.d/s65camera-usb").read_text(encoding="utf-8"),
}
HELPERS_START = "STOP_WAIT_TRIES="
HELPERS_END = "CMD_CAPTURE="


def pidfile_helpers(script_name):
    text = INIT_SCRIPTS[script_name]
    after_start = HELPERS_START + text.split(HELPERS_START, 1)[1]
    return after_start.split(HELPERS_END, 1)[0]


def run_helpers(script_name, tmp_path, shell_lines):
    harness = tmp_path / "harness.sh"
    harness.write_text(pidfile_helpers(script_name) + "\n" + shell_lines, encoding="utf-8")
    return subprocess.run(
        ["sh", str(harness)], capture_output=True, text=True, timeout=30, check=False
    )


def pid_of_a_process_that_has_exited():
    finished = subprocess.Popen(["sleep", "0"])
    finished.wait()
    return finished.pid


def pid_of_a_process_that_exits_shortly(seconds):
    # Orphaned on purpose: a child of this test would linger as a zombie, and kill -0 answers yes
    # for a zombie, which is not the state under test.
    spawned = subprocess.run(
        # its output goes nowhere on purpose: sharing this call's pipe would hold it open, and the
        # read would sit here until the process it is meant to outlive had already exited
        ["sh", "-c", f"sleep {seconds} >/dev/null 2>&1 & echo $!"],
        capture_output=True,
        text=True,
        check=True,
    )
    return int(spawned.stdout.strip())


def test_a_start_removes_the_pidfile_of_a_process_that_is_already_gone(tmp_path):
    # this is the USB camera's dead capture: the number in the file names nothing
    pidfile = tmp_path / "capture.pid"
    pidfile.write_text(str(pid_of_a_process_that_has_exited()), encoding="utf-8")
    run_helpers("s65camera-usb", tmp_path, f'clear_pidfile_of_dead_process "{pidfile}"\n')
    assert not pidfile.exists()


def test_a_start_keeps_the_pidfile_of_a_process_that_is_running(tmp_path):
    # the service really is up, so the start must stay a no-op and must not orphan the running one
    pidfile = tmp_path / "capture.pid"
    running_pid = pid_of_a_process_that_exits_shortly(30)
    pidfile.write_text(str(running_pid), encoding="utf-8")
    try:
        run_helpers("s65camera-usb", tmp_path, f'clear_pidfile_of_dead_process "{pidfile}"\n')
        assert pidfile.exists()
    finally:
        subprocess.run(["kill", "-9", str(running_pid)], check=False)


def test_a_stop_waits_for_the_process_to_go_and_takes_the_pidfile_with_it(tmp_path):
    # start-stop-daemon -K returns the moment it has sent the signal, so a restart that does not
    # wait starts the new capture while the old one still holds the camera
    pidfile = tmp_path / "capture.pid"
    exiting_pid = pid_of_a_process_that_exits_shortly(1)
    pidfile.write_text(str(exiting_pid), encoding="utf-8")
    stop_call = f'stop_service_and_clear_pidfile "capture" "{pidfile}" /bin/sleep\n'
    run_helpers("s65camera-usb", tmp_path, stop_call)
    assert not pidfile.exists()
    still_alive = subprocess.run(
        ["kill", "-0", str(exiting_pid)], capture_output=True, check=False
    )
    assert still_alive.returncode != 0


def test_both_cameras_clear_a_dead_pidfile_before_every_service_they_start():
    for script_name, text in INIT_SCRIPTS.items():
        lines = text.splitlines()
        starts = [
            number
            for number, line in enumerate(lines)
            if line.strip().startswith("start-stop-daemon -S")
        ]
        assert starts, script_name
        for number in starts:
            guard = lines[number - 1]
            assert "clear_pidfile_of_dead_process" in guard, f"{script_name}:{number + 1}"


def test_both_cameras_stop_their_services_the_same_way():
    # two copies of the guards, one defect: they are pinned identical so a fix to one is a fix to
    # both, and the internal camera never drifts back to the wiring that broke the external one
    assert pidfile_helpers("s65camera-hw") == pidfile_helpers("s65camera-usb")
    for script_name, text in INIT_SCRIPTS.items():
        stop_body = text.split("\nstop() {", 1)[1].split("\n}", 1)[0]
        assert "start-stop-daemon -K" not in stop_body, script_name
        assert stop_body.count("stop_service_and_clear_pidfile") == 4, script_name


def test_the_internal_camera_clears_the_pidfile_of_the_display_service_it_kills_by_name():
    # lmd is killed by name, not through its pidfile, so nothing else would ever clear it
    lmd_stop_body = INIT_SCRIPTS["s65camera-hw"].split("\nlmd_stop() {", 1)[1].split("\n}", 1)[0]
    assert "rm -f $RUN/unisrv.pid" in lmd_stop_body
