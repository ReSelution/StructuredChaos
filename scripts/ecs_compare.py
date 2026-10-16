#!/usr/bin/env python3
"""Builds and runs the ECS comparison benchmarks and prints the results side by side.

The executables, one per library, are those of tests/ecs_compare. They are
built in build-bench-ecs, which is configured as an optimized build with
-Decs_comparison=true if it does not exist yet:

    scripts/ecs_compare.py
    scripts/ecs_compare.py --only sc,flecs --threads 8
    scripts/ecs_compare.py --markdown --save results.json
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = ROOT / "build-bench-ecs"
PREFIX = "ecs_compare_"

# What the build directory is configured with if the script creates it.
SETUP_OPTIONS = ["-Dbuildtype=release", "-Db_ndebug=true", "-Decs_comparison=true"]

# Column titles, in the order of the columns. Libraries not listed here follow
# in alphabetical order under the name of their executable.
LIBRARIES = {
    "sc": "sc::ecs",
    "sc2": "sc::ecs2",
    "sc2_huge": "sc::ecs2 huge pages",
    "sc2_small": "sc::ecs2 small pages",
    "entt": "EnTT",
    "flecs": "Flecs",
    "gaia": "gaia-ecs",
    "mustache": "mustache",
}

# How tests/ecs_compare/common.hpp marks the two runs of a parallel benchmark.
# How an executable marks a time it measured without Catch2.
REPORTED = "ecs_compare"

SERIAL = " [1 thread]"
PARALLEL = " [all threads]"

# The start of the name of a section whose values are bytes instead of
# nanoseconds, and the tag of the test cases that measure them.
MEMORY = "memory"
MEMORY_TAG = "[memory]"

RESET = "\033[0m"
BOLD = "\033[1m"
DIM = "\033[2m"
RED = "\033[31m"
GREEN = "\033[32m"
YELLOW = "\033[33m"
CYAN = "\033[36m"


# --- building --------------------------------------------------------------


def read_introspection(build_dir: Path, name: str):
    with open(build_dir / "meson-info" / name, encoding="utf-8") as file:
        return json.load(file)


def read_build_options(build_dir: Path) -> dict:
    return {option["name"]: option["value"] for option in read_introspection(build_dir, "intro-buildoptions.json")}


def meson(*arguments: str) -> bool:
    """Runs meson with its output on stderr, so that stdout stays the report."""
    try:
        return subprocess.run(["meson", *arguments], stdout=sys.stderr).returncode == 0
    except OSError as error:
        print(f"error: cannot run meson: {error}", file=sys.stderr)
        return False


def configure(build_dir: Path, native_file: Path | None) -> bool:
    """Sets the build directory up, or turns the comparison on in an existing one."""
    if not (build_dir / "meson-private" / "coredata.dat").exists():
        arguments = ["setup", str(build_dir), str(ROOT), *SETUP_OPTIONS]
        if native_file:
            arguments += ["--native-file", str(native_file)]
        return meson(*arguments)

    if native_file:
        warn(f"{build_dir} is configured already, --native-file is ignored")
    try:
        build_options = read_build_options(build_dir)
    except (OSError, ValueError):
        return True
    if build_options.get("ecs_comparison") is not True:
        return meson("configure", str(build_dir), "-Decs_comparison=true")
    return True


def build(build_dir: Path, only: list[str] | None) -> bool:
    """Builds the executables of the comparison and nothing else of the project."""
    try:
        targets = [
            target["name"]
            for target in read_introspection(build_dir, "intro-targets.json")
            if target["type"] == "executable" and target["name"].startswith(PREFIX)
        ]
    except (OSError, ValueError, KeyError):
        targets = []
    if only:
        targets = [name for name in targets if name[len(PREFIX) :] in only]
    if not targets:
        # Without the names meson builds everything, or there is nothing to build.
        return not only and meson("compile", "-C", str(build_dir))
    return meson("compile", "-C", str(build_dir), *targets)


# --- running ---------------------------------------------------------------


def find_executables(build_dir: Path) -> dict[str, Path]:
    directory = build_dir / "tests" / "ecs_compare"
    found = {}
    for path in sorted(directory.glob(PREFIX + "*")):
        if path.is_file() and os.access(path, os.X_OK) and path.suffix in ("", ".exe"):
            found[path.stem[len(PREFIX) :]] = path
    return found


def parse_report(path: Path) -> dict[str, dict[str, float]]:
    """Mean time in nanoseconds of every benchmark, by test case."""
    sections: dict[str, dict[str, float]] = {}
    for case in ET.parse(path).getroot().iter("TestCase"):
        benchmarks = sections.setdefault(case.get("name"), {})
        for result in case.iter("BenchmarkResults"):
            mean = result.find("mean")
            if mean is not None:
                benchmarks[result.get("name")] = float(mean.get("value"))
    return sections


def read_reported(output: str, sections: dict[str, dict[str, float]]) -> None:
    """Adds what an executable measured itself, see report in tests/ecs_compare/common.hpp."""
    for line in output.splitlines():
        fields = line.split("\t")
        if len(fields) == 4 and fields[0] == REPORTED:
            try:
                sections.setdefault(fields[1], {})[fields[2]] = float(fields[3])
            except ValueError:
                pass


def run_memory(executable: Path, env: dict[str, str], sections: dict[str, dict[str, float]]) -> None:
    """Runs every test case that measures memory in a process of its own.

    A world built in the memory an earlier one has left behind would seem
    smaller than it is.
    """
    listing = subprocess.run(
        [str(executable), MEMORY_TAG, "--list-tests", "--verbosity", "quiet", "--order", "decl"],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
    )  # fmt: skip
    for case in filter(None, (line.strip() for line in listing.stdout.splitlines())):
        process = subprocess.run([str(executable), case], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        read_reported(process.stdout, sections)
        if process.returncode != 0:
            warn(f"{executable.name} exited with {process.returncode} in '{case}'")
            if process.stdout.strip():
                print(process.stdout.strip(), file=sys.stderr)


def run(executable: Path, samples: int, threads: int | None) -> dict[str, dict[str, float]]:
    env = dict(os.environ)
    if threads:
        env["ECS_COMPARE_THREADS"] = str(threads)
    with tempfile.TemporaryDirectory() as directory:
        report = Path(directory) / "report.xml"
        command = [
            str(executable),
            "[!benchmark]",
            "--order", "decl",
            "--benchmark-samples", str(samples),
            "--reporter", f"xml::out={report}",
        ]  # fmt: skip
        process = subprocess.run(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            sections = parse_report(report)
        except (OSError, ET.ParseError):
            sections = {}
        read_reported(process.stdout, sections)
        if process.returncode != 0:
            warn(f"{executable.name} exited with {process.returncode}, its results may be incomplete")
            if process.stdout.strip():
                print(process.stdout.strip(), file=sys.stderr)
        run_memory(executable, env, sections)
        return sections


# --- formatting ------------------------------------------------------------


def format_time(nanoseconds: float) -> str:
    for unit, factor in (("s", 1e9), ("ms", 1e6), ("µs", 1e3)):
        if nanoseconds >= factor:
            return f"{float(f'{nanoseconds / factor:.3g}'):g} {unit}"
    return f"{float(f'{nanoseconds:.3g}'):g} ns"


def format_bytes(count: float) -> str:
    for unit, factor in (("GiB", 2**30), ("MiB", 2**20), ("KiB", 2**10)):
        if count >= factor:
            return f"{float(f'{count / factor:.3g}'):g} {unit}"
    return f"{count:.0f} B"


def format_factor(factor: float) -> str:
    return f"{factor:.2f}x" if factor < 10 else f"{factor:.0f}x" if factor >= 100 else f"{factor:.1f}x"


def title_of(library: str) -> str:
    return LIBRARIES.get(library, library)


def ordered(libraries) -> list[str]:
    known = [name for name in LIBRARIES if name in libraries]
    return known + sorted(name for name in libraries if name not in LIBRARIES)


class Cell:
    """Text of a table cell and how the terminal shows it.

    The detail follows the text and is the first thing to go when the table is
    too wide for the terminal.
    """

    def __init__(self, text: str = "", style: str = "", bold: bool = False, detail: str = ""):
        self.text = text
        self.style = style
        self.bold = bold
        self.detail = detail

    def full(self, with_detail: bool) -> str:
        return f"{self.text}  {self.detail}" if with_detail and self.detail else self.text


def time_cells(row: dict[str, float], libraries: list[str], baseline: str | None, form=format_time) -> list[Cell]:
    """Time and factor of every library, colored by the distance to the fastest.

    Or, with another form, anything else of which less is better.
    """
    fastest = min(row.values())
    reference = row.get(baseline, fastest) if baseline else fastest
    cells = []
    for library in libraries:
        if library not in row:
            cells.append(Cell("—", DIM))
            continue
        value = row[library]
        behind = value / fastest if fastest else 1.0 if value == fastest else float("inf")
        if value == fastest:
            style = BOLD + GREEN
        elif behind <= 1.25:
            style = GREEN
        elif behind >= 10:
            style = RED
        elif behind >= 3:
            style = YELLOW
        else:
            style = ""
        factor = f"{format_factor(value / reference):>5}" if reference else ""
        cells.append(Cell(form(value), style, bold=value == fastest, detail=factor))
    return cells


def speedup_cells(serial: dict[str, float], parallel: dict[str, float], libraries: list[str]) -> list[Cell]:
    speedups = {name: serial[name] / parallel[name] for name in libraries if name in serial and name in parallel}
    best = max(speedups.values(), default=0.0)
    cells = []
    for library in libraries:
        if library not in speedups:
            cells.append(Cell("—", DIM))
            continue
        value = speedups[library]
        style = BOLD + GREEN if value == best else RED if value < 1 else ""
        cells.append(Cell(format_factor(value), style, bold=value == best))
    return cells


def print_table(
    title: str, note: str, header: list[str], rows: list[tuple[str, list[Cell]]], options, detail_note: str = ""
) -> None:
    """Prints the rows under the title. The note has a place for the detail note, "{detail}"."""
    if options.markdown:
        print(f"### {title}\n")
        print(f"{note.format(detail=detail_note)}\n")
        print("| " + " | ".join(["benchmark"] + header) + " |")
        print("| --- |" + " ---: |" * len(header))
        for name, cells in rows:
            texts = [" ".join(cell.full(True).split()) for cell in cells]
            texts = [f"**{text}**" if cell.bold else text for text, cell in zip(texts, cells)]
            print("| " + " | ".join([name] + texts) + " |")
        print()
        return

    color = options.color
    paint = (lambda text, style: f"{style}{text}{RESET}" if style else text) if color else (lambda text, style: text)

    name_width = max([len("benchmark")] + [len(name) for name, _ in rows])

    def measure(with_detail: bool):
        widths = [
            max([len(header[column])] + [len(cells[column].full(with_detail)) for _, cells in rows])
            for column in range(len(header))
        ]
        return widths, name_width + sum(widths) + 3 * len(widths)

    with_detail = True
    widths, total = measure(with_detail)
    if sys.stdout.isatty() and total > shutil.get_terminal_size().columns:
        # The colors still tell how far a library is behind.
        with_detail = False
        widths, total = measure(with_detail)
    note = note.format(detail=detail_note if with_detail else "")
    separator = paint(" │ ", DIM)
    rule = paint("─" * total, DIM)

    print(paint(title, BOLD))
    print(paint(note, DIM))
    print(rule)
    titles = [
        paint(header[column].rjust(widths[column]), BOLD + CYAN if column == 0 else BOLD)
        for column in range(len(header))
    ]
    print(paint("benchmark".ljust(name_width), BOLD) + separator + separator.join(titles))
    print(rule)
    for name, cells in rows:
        texts = [
            paint(cells[column].full(with_detail).rjust(widths[column]), cells[column].style)
            for column in range(len(header))
        ]
        print(name.ljust(name_width) + separator + separator.join(texts))
    print(rule)
    print()


# --- report ----------------------------------------------------------------


def merge(results: dict[str, dict[str, dict[str, float]]]) -> dict[str, dict[str, dict[str, float]]]:
    """results[library][section][benchmark] to merged[section][benchmark][library], in order of appearance."""
    merged: dict[str, dict[str, dict[str, float]]] = {}
    for library in ordered(results):
        for section, benchmarks in results[library].items():
            for benchmark, value in benchmarks.items():
                merged.setdefault(section, {}).setdefault(benchmark, {})[library] = value
    return merged


def print_report(results, options, threads: int) -> None:
    libraries = ordered(results)
    header = [title_of(name) for name in libraries]
    baseline = options.relative_to
    time_note = "mean time{detail}, lower is better"
    factor_note = f" and factor to {title_of(baseline) if baseline else 'the fastest'}"

    for section, benchmarks in merge(results).items():
        serial = {name[: -len(SERIAL)]: row for name, row in benchmarks.items() if name.endswith(SERIAL)}
        parallel = {name[: -len(PARALLEL)]: row for name, row in benchmarks.items() if name.endswith(PARALLEL)}
        plain = {name: row for name, row in benchmarks.items() if not name.endswith((SERIAL, PARALLEL))}

        if plain and section.startswith(MEMORY):
            rows = [(name, time_cells(row, libraries, baseline, format_bytes)) for name, row in plain.items()]
            note = "memory the process holds more of with the world than without{detail}, lower is better"
            smallest_note = f" and factor to {title_of(baseline) if baseline else 'the smallest'}"
            print_table(section, note, header, rows, options, smallest_note)
        elif plain:
            rows = [(name, time_cells(row, libraries, baseline)) for name, row in plain.items()]
            print_table(section, "time{detail}, lower is better", header, rows, options, factor_note)
        if serial:
            rows = [(name, time_cells(row, libraries, baseline)) for name, row in serial.items()]
            print_table(f"{section} · 1 thread", time_note, header, rows, options, factor_note)
        if parallel:
            rows = [(name, time_cells(row, libraries, baseline)) for name, row in parallel.items()]
            print_table(f"{section} · {threads} threads", time_note, header, rows, options, factor_note)
            rows = [
                (name, speedup_cells(serial[name], row, libraries)) for name, row in parallel.items() if name in serial
            ]
            if rows:
                note = f"time on 1 thread divided by the time on {threads}, higher is better"
                print_table(f"{section} · speedup", note, header, rows, options)


def cpu_name() -> str:
    try:
        with open("/proc/cpuinfo", encoding="utf-8") as cpuinfo:
            for line in cpuinfo:
                if line.startswith("model name"):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or platform.machine()


def warn(message: str) -> None:
    print(f"warning: {message}", file=sys.stderr)


def check_build(build_dir: Path) -> None:
    """Unoptimized builds and assertions distort the comparison."""
    try:
        build_options = read_build_options(build_dir)
    except (OSError, ValueError):
        return
    if build_options.get("optimization") not in ("2", "3"):
        warn(f"optimization={build_options.get('optimization')}, use -Dbuildtype=release")
    asserts_off = build_options.get("b_ndebug") == "true" or (
        build_options.get("b_ndebug") == "if-release" and build_options.get("buildtype") == "release"
    )
    if not asserts_off:
        warn("assertions are enabled, use -Db_ndebug=true")
    # A list in newer versions of meson, a string in older ones.
    sanitizers = build_options.get("b_sanitize") or "none"
    if sanitizers not in ("none", ["none"]):
        warn(f"b_sanitize={sanitizers}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", type=Path, default=BUILD_DIR, help="meson build directory (default: build-bench-ecs)")
    parser.add_argument("--native-file", type=Path, help="for a new build directory, e.g. cross/linux-clang.ini")
    parser.add_argument("--no-build", action="store_true", help="run what is built already, without meson")
    parser.add_argument("--samples", type=int, default=50, help="samples per benchmark (default: 50)")
    parser.add_argument("--threads", type=int, help="threads of the parallel benchmarks (default: all)")
    parser.add_argument("--only", help="comma separated libraries to run, e.g. sc,flecs")
    parser.add_argument("--relative-to", metavar="LIBRARY", help="show factors to this library instead of the fastest")
    parser.add_argument("--markdown", action="store_true", help="print markdown tables")
    parser.add_argument("--no-color", action="store_true", help="plain text also on a terminal")
    parser.add_argument("--save", type=Path, metavar="FILE", help="write the results as JSON")
    parser.add_argument("--load", type=Path, metavar="FILE", help="print saved results instead of running")
    options = parser.parse_args()
    options.color = not options.no_color and not options.markdown and sys.stdout.isatty() and "NO_COLOR" not in os.environ

    if options.load:
        with open(options.load, encoding="utf-8") as file:
            saved = json.load(file)
        results, info = saved["results"], saved["info"]
    else:
        wanted = options.only.split(",") if options.only else None
        if not options.no_build:
            if not configure(options.build_dir, options.native_file) or not build(options.build_dir, wanted):
                print("error: the build failed", file=sys.stderr)
                return 1
            print(file=sys.stderr)
        executables = find_executables(options.build_dir)
        if wanted:
            for name in wanted:
                if name not in executables:
                    warn(f"no executable for '{name}'")
            executables = {name: path for name, path in executables.items() if name in wanted}
        if not executables:
            print(
                f"error: no {PREFIX}* executables in {options.build_dir / 'tests' / 'ecs_compare'}",
                file=sys.stderr,
            )
            return 1
        check_build(options.build_dir)

        info = {
            "cpu": cpu_name(),
            "threads": options.threads or os.cpu_count() or 1,
            "samples": options.samples,
            "system": f"{platform.system()} {platform.release()}",
        }
        results = {}
        for name in ordered(executables):
            print(f"running {title_of(name)} ...", end="", file=sys.stderr, flush=True)
            start = time.monotonic()
            results[name] = run(executables[name], options.samples, options.threads)
            print(f" {time.monotonic() - start:.0f} s", file=sys.stderr)
        print(file=sys.stderr)

    if options.relative_to and options.relative_to not in results:
        warn(f"no results for '{options.relative_to}', showing factors to the fastest")
        options.relative_to = None

    if options.save:
        with open(options.save, "w", encoding="utf-8") as file:
            json.dump({"info": info, "results": results}, file, indent=2)

    summary = f"{info['cpu']} · {info['threads']} threads · {info['system']} · {info['samples']} samples"
    if options.markdown:
        print(f"## ECS comparison\n\n{summary}\n")
    else:
        print((BOLD + "ECS comparison" + RESET) if options.color else "ECS comparison")
        print(summary)
        print()
    print_report(results, options, info["threads"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
