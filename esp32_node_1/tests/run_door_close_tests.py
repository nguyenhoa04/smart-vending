#!/usr/bin/env python3
"""Compile/execute the production snapshot code with small host platform stubs."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="weight-snapshot-test-") as directory:
        binary = str(Path(directory) / "door_close_test")
        command = shlex.split(os.environ.get("CC", "cc")) + ["-std=gnu11", "-Wall", "-Wextra", "-Werror"]
        command += ["-I" + str(root / "tests/host_stubs")]
        for component in ("lock", "pi_uart", "door_session_service"):
            command += ["-I" + str(root / "components" / component / "include")]
        command += [str(root / "tests/door_close_test.c"), "-lm", "-o", binary]
        subprocess.run(command, check=True)
        subprocess.run([binary], check=True)


if __name__ == "__main__":
    main()
