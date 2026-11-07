#!/usr/bin/env python3
"""Check Vulkan loader calls from LD_PRELOAD do not bind to nxrt data slots."""
import os
import subprocess
import sys


def main() -> int:
    nxrt, probe = sys.argv[1:]
    env = os.environ.copy()
    env["LD_PRELOAD"] = probe
    result = subprocess.run([nxrt, "--help"], env=env, capture_output=True, text=True)
    output = result.stdout + result.stderr
    if result.returncode != 2 or "usage: nxrt" not in output:
        print(
            f"FAIL: LD_PRELOAD probe did not reach nxrt argument parsing "
            f"(rc={result.returncode}):\n{output}",
            file=sys.stderr,
        )
        return 1
    print("PASS: Vulkan LD_PRELOAD probe completed without a crash (nxrt usage rc=2)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
