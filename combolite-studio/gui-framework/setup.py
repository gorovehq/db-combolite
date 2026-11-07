from __future__ import annotations

import os
import shutil
from pathlib import Path
from setuptools import setup
from setuptools.command.build_py import build_py

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]
ENGINE = REPO_ROOT / "db-combolite"
# The library to bundle: $COMBOLITE_LIB_SRC, else the first studio preset
# build found (CMakePresets.json), else the older build-shared directory.
LIB_CANDIDATES = [
    *([Path(os.environ["COMBOLITE_LIB_SRC"])] if os.environ.get("COMBOLITE_LIB_SRC") else []),
    *(ENGINE / f"build-studio-{b}" / "libcombolite.so" for b in ("cpu", "cuda", "vulkan", "both")),
    ENGINE / "build-shared" / "libcombolite.so",
]


class CustomBuildPy(build_py):
    """Bundle libcombolite.so into package directory if present."""

    def run(self):
        dest_so = HERE / "combolite_gui" / "libcombolite.so"
        src = next((p for p in LIB_CANDIDATES if p.exists()), None)
        # Replace a bundled copy older than the build, so a wheel never
        # carries a stale engine whose structs no longer match binding.py.
        if src is not None and (not dest_so.exists() or src.stat().st_mtime > dest_so.stat().st_mtime):
            print(f"Bundling {src} -> {dest_so}")
            shutil.copy2(src, dest_so)
        super().run()


if __name__ == "__main__":
    setup(
        cmdclass={"build_py": CustomBuildPy},
    )
