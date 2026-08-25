"""The camera's start script may only ask a staged program for what that program knows.
The plugin ships its shell scripts as text and its programs already built. The built ones are not
in this repository: they are staged into `plugin/files/bin` by the build, and whatever is lying
there is what gets packed. A package cut after a source change but without a rebuild therefore
carries a new script and an old program. That is how the built-in camera stopped connecting: the
script asked fake-service for `--log`, the fake-service that had been staged in July had never
heard of it, so it refused to start and nothing ever captured a picture.

A tree with nothing staged, a fresh clone, has no package to read, so these skip there.
"""

import re
from functools import cache
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
STAGED_BIN = REPO_ROOT / "plugin/files/bin"
INIT_SCRIPT = (REPO_ROOT / "plugin/files/etc/init.d/s65camera-hw").read_text(encoding="utf-8")

# $BIN/fake-service and $CTRL_BIN/control-v4l2.py both name a program the script runs.
STAGED_PROGRAM = re.compile(r"^\$\w*BIN/(\S+)$")
ASSIGNMENT = re.compile(r'^\s*(\w+)="([^"]*)"\s*$')
LONG_OPTION = re.compile(r"^--([a-z0-9][a-z0-9-]*)$")
PRINTABLE_RUN = re.compile(rb"[\x20-\x7e]{2,}")

STAGED_PROGRAMS = sorted(STAGED_BIN.glob("*")) if STAGED_BIN.is_dir() else []
COMPILED_SOURCES = sorted(
    path for path in (REPO_ROOT / "src").rglob("*") if path.suffix in {".c", ".h"}
)

needs_a_staged_package = pytest.mark.skipif(
    not STAGED_PROGRAMS,
    reason="nothing is staged under plugin/files/bin, so there is no package to read",
)


def shell_variables_of(script_text):
    """The script's own quoted assignments, so a command can be read as the words it will run."""
    assigned = (ASSIGNMENT.match(line) for line in script_text.splitlines())
    return {found.group(1): found.group(2) for found in assigned if found and found.group(2)}


def expanded_word(word, shell_variables):
    if not word.startswith("$"):
        return word
    return shell_variables.get(word[1:], word)


def words_of_command(command, shell_variables):
    expanded = " ".join(expanded_word(word, shell_variables) for word in command.split())
    return expanded.split()


def options_asked_in_one_command(words):
    """A program named in a command claims every long option that follows it, until the next one.

    An option in a command that names no staged program is left alone: an option gathered into a
    variable and handed over later cannot be traced back, and guessing would fail an honest script.
    """
    program = None
    asked = []
    for word in words:
        named = STAGED_PROGRAM.match(word)
        option = LONG_OPTION.match(word)
        if named:
            program = named.group(1)
        if option and program:
            asked.append((program, option.group(1)))
    return asked


def options_the_script_asks_of_each_program(script_text):
    shell_variables = shell_variables_of(script_text)
    commands = script_text.replace("\\\n", " ").splitlines()
    return [
        asked
        for command in commands
        for asked in options_asked_in_one_command(words_of_command(command, shell_variables))
    ]


@cache
def words_inside_program(program_path):
    """Every readable run of bytes in a staged program, which is where an option name lives."""
    return frozenset(
        run.decode("ascii") for run in PRINTABLE_RUN.findall(program_path.read_bytes())
    )


def program_knows_option(program_path, option):
    words = words_inside_program(program_path)
    if option in words:
        return True
    spelled_out = re.compile(rf"--{re.escape(option)}(?![\w-])")
    return any(spelled_out.search(word) for word in words)


def options_a_staged_program_would_refuse(script_text):
    asked = options_the_script_asks_of_each_program(script_text)
    return sorted(
        {
            f"{program} --{option}"
            for program, option in asked
            if (STAGED_BIN / program).is_file()
            and not program_knows_option(STAGED_BIN / program, option)
        }
    )


def modified_at(path):
    return path.stat().st_mtime


def source_of_staged_script(staged_script):
    """A staged script is a copy of the one under src/ that carries the same name."""
    named_the_same = [
        path for path in (REPO_ROOT / "src").rglob(staged_script.name) if path.is_file()
    ]
    return named_the_same[0] if named_the_same else None


def source_behind_staged_program(staged_program):
    """Every compiled program shares the C headers, so any C change can change any of them."""
    if staged_program.suffix == ".py":
        return source_of_staged_script(staged_program)
    return max(COMPILED_SOURCES, key=modified_at)


def staged_programs_older_than_their_source():
    staged = ((program, source_behind_staged_program(program)) for program in STAGED_PROGRAMS)
    return sorted(
        program.name
        for program, source in staged
        if source is not None and modified_at(program) < modified_at(source)
    )


@needs_a_staged_package
def test_the_start_script_only_asks_a_staged_program_for_what_it_knows():
    refused = options_a_staged_program_would_refuse(INIT_SCRIPT)
    assert refused == [], f"the package would start the camera with: {refused}"


@needs_a_staged_package
def test_an_option_the_staged_program_never_heard_of_is_caught():
    """The shape of the outage: the log asked for on the command line of an old supervisor."""
    old_script = INIT_SCRIPT.replace(
        "$CMD_FAKE_SERVICE_ARGS $CMD_CAPTURE",
        "$CMD_FAKE_SERVICE_ARGS --log $RUN/capture-mipi-mpp.log $CMD_CAPTURE",
    )
    assert old_script != INIT_SCRIPT, "the capture command has moved, so this no longer tests it"
    assert options_a_staged_program_would_refuse(old_script) == ["fake-service --log"]


@needs_a_staged_package
def test_nothing_staged_was_built_before_the_source_it_was_built_from_was_changed():
    """A source change is only in the package once the build has been run over it again."""
    stale = staged_programs_older_than_their_source()
    assert stale == [], (
        f"a package cut now would ship these from before the source changed: {stale}. "
        "Build with b3-builder --bake."
    )
