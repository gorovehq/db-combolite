from __future__ import annotations

import os
import shutil
from pathlib import Path
from setuptools import setup
from setuptools.command.build_py import build_py

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]
LIB_SRC = REPO_ROOT / "db-combolite" / "build-shared" / "libcombolite.so"


class CustomBuildPy(build_py):
    """Bundle libcombolite.so into package directory if present."""

    def run(self):
        # Look for pre-compiled shared library in build directory
        dest_so = HERE / "combolite_gui" / "libcombolite.so"
        if LIB_SRC.exists() and not dest_so.exists():
            print(f"Bundling {LIB_SRC} -> {dest_so}")
            shutil.copy2(LIB_SRC, dest_so)
        super().run()


if __name__ == "__main__":
    setup(
        cmdclass={"build_py": CustomBuildPy},
    )
