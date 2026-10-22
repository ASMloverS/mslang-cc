"""Reject compiler outputs outside the build directory."""

import os
import sys
from pathlib import Path

ARTIFACT_SUFFIXES = frozenset({
    ".a",
    ".dll",
    ".dylib",
    ".exe",
    ".ilk",
    ".lib",
    ".o",
    ".obj",
    ".out",
    ".pdb",
    ".so",
})
EXCLUDED_DIRS = frozenset({".git", ".worktrees", "build"})


def find_build_artifacts(root: Path) -> list[Path]:
    """Return compiler artifacts below root, excluding allowed output trees."""
    artifacts: list[Path] = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [name for name in dirnames if name not in EXCLUDED_DIRS]
        for filename in filenames:
            if Path(filename).suffix.lower() in ARTIFACT_SUFFIXES:
                artifacts.append(Path(dirpath) / filename)
    return sorted(artifacts)


def main(argv: list[str]) -> int:
    """Check the repository root supplied as the first argument."""
    if len(argv) > 1:
        root = Path(argv[1]).resolve()
    else:
        root = Path(__file__).resolve().parents[1]
    if not root.is_dir():
        print(f"Repository root does not exist: {root}", file=sys.stderr)
        return 2

    artifacts = find_build_artifacts(root)
    if not artifacts:
        print("No build artifacts found outside build/.")
        return 0

    print("Build artifacts outside build/ are forbidden:", file=sys.stderr)
    for artifact in artifacts:
        print(f"  {artifact.relative_to(root).as_posix()}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
