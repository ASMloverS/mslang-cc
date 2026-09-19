"""Unified test driver: discovers tests/ms/**/*.ms and runs them via mslang."""

import argparse
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent
TESTS_MS_DIR = REPO_ROOT / "tests" / "ms"


def find_mslang_binary(explicit: Path | None) -> Path | None:
    """Resolves the mslang CLI binary, honoring --mslang when given."""
    if explicit is not None:
        return explicit if explicit.is_file() else None
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


def run_test(mslang: Path, script: Path) -> bool:
    """Runs one script; passes when the interpreter exits with code 0."""
    result = subprocess.run(
        [str(mslang), str(script)],
        capture_output=True,
        text=True,
        check=False,
    )
    return result.returncode == 0


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
        if run_test(mslang, script):
            print(f"PASS {script.relative_to(REPO_ROOT)}")
        else:
            failed.append(script)
            print(f"FAIL {script.relative_to(REPO_ROOT)}")

    print(f"{len(tests) - len(failed)} of {len(tests)} tests passed")
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
