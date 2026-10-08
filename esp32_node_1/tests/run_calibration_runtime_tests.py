#!/usr/bin/env python3
"""Exercise production calibration/NVS/runtime integration without ESP hardware."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="calibration-runtime-test-") as directory:
        binary = str(Path(directory) / "calibration_runtime_test")
        command = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-I" + str(root / "tests/host_stubs")
        ]
        for component in ("shelf_service", "command_service", "calibration_store", "node_app",
                          "hx711", "pi_uart", "door_session_service"):
            command += ["-I" + str(root / "components" / component / "include")]
        command += [str(root / "tests/calibration_runtime_test.c"),
                    str(root / "components/calibration_store/calibration_store.c"),
                    str(root / "components/node_app/node_app.c"), "-lm", "-o", binary]
        subprocess.run(command, check=True)
        for scenario in ("valid", "missing", "offset-only", "scale-only", "persistence",
                         "failure", "corrupt", "no-free-pages", "new-version"):
            subprocess.run([binary, scenario], check=True)


if __name__ == "__main__":
    main()
