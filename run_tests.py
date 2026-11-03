"""Unified test driver: discovers tests/ms/**/*.ms and runs them via mslang."""

import argparse
import difflib
import os
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent
TESTS_MS_DIR = REPO_ROOT / "tests" / "ms"


def find_mslang_binary(explicit: Path | None) -> Path | None:
    """Resolves the CLI from an argument, MSLANG_BIN, or build output."""
    selected = explicit
    if selected is None and os.environ.get("MSLANG_BIN"):
        selected = Path(os.environ["MSLANG_BIN"])
    if selected is not None:
        return selected if selected.is_file() else None
    candidates: list[Path] = []
    for subdir in ("", "Debug", "Release", "RelWithDebInfo", "MinSizeRel"):
        for name in ("mslang", "mslang.exe"):
            candidates.append(REPO_ROOT / "build" / subdir / name)
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return None


def discover_tests(pattern: str) -> list[Path]:
    """Returns sorted tests/ms/**/*.ms paths matching the glob filter."""
    if not TESTS_MS_DIR.is_dir():
        return []
    return sorted(
        path for path in TESTS_MS_DIR.rglob("*.ms") if path.match(pattern)
    )


def expected_exit_code(script: Path) -> int:
    """Reads an optional .exit companion, defaulting to success."""
    companion = script.with_suffix(".exit")
    if not companion.is_file():
        return 0
    return int(companion.read_text(encoding="utf-8").strip())


def run_test(mslang: Path, script: Path) -> tuple[bool, str]:
    """Runs one script and returns its pass status and failure details."""
    expected_exit = expected_exit_code(script)
    result = subprocess.run(
        [str(mslang), str(script)],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    failures: list[str] = []
    if result.returncode != expected_exit:
        failures.append(
            f"exit code: expected {expected_exit}, got {result.returncode}"
        )
    output_companion = script.with_suffix(".out")
    if output_companion.is_file():
        expected_output = output_companion.read_text(encoding="utf-8")
        if result.stdout != expected_output:
            diff = "".join(
                difflib.unified_diff(
                    expected_output.splitlines(keepends=True),
                    result.stdout.splitlines(keepends=True),
                    fromfile="expected stdout",
                    tofile="actual stdout",
                )
            )
            failures.append(f"stdout difference:\n{diff}")
    error_companion = script.with_suffix(".err")
    if error_companion.is_file():
        expected_error = error_companion.read_text(encoding="utf-8").strip()
        if expected_error not in result.stderr:
            failures.append(
                f"stderr: expected substring {expected_error!r}, got {result.stderr!r}"
            )
    if failures and result.stderr:
        failures.append(f"stderr:\n{result.stderr}")
    return not failures, "\n".join(failures)


def main(argv: list[str] | None = None) -> int:
    """Entry point; returns the process exit code."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--mslang",
        type=Path,
        default=None,
        help="path to the mslang interpreter binary",
    )
    parser.add_argument(
        "--filter",
        default="*.ms",
        help="glob filter for test scripts (default: *.ms)",
    )
    args = parser.parse_args(argv)

    tests = discover_tests(args.filter)
    if not tests:
        print("no tests found")
        return 0

    mslang = find_mslang_binary(args.mslang)
    if mslang is None:
        print(
            "error: mslang binary not found; build first or pass --mslang",
            file=sys.stderr,
        )
        return 1

    failed: list[Path] = []
    for script in tests:
        passed, details = run_test(mslang, script)
        relative = script.relative_to(REPO_ROOT)
        if passed:
            print(f"PASS {relative}")
        else:
            failed.append(script)
            print(f"FAIL {relative}")
            print(details, file=sys.stderr)

    print(f"{len(tests) - len(failed)} of {len(tests)} tests passed")
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
