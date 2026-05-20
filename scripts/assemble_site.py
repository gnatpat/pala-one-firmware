#!/usr/bin/env python3
"""Assemble the ESP Web Tools installer site bundle.

Reads the per-env PlatformIO build outputs plus install/ static assets and
writes a self-contained directory (default `site/`) that can be served by
any static HTTP server — locally for testing or as a GitHub Pages artifact.

Single source of truth for the bundle layout: this script is invoked both
from the deploy-installer GitHub workflow and by developers wanting to
exercise the installer page locally.
"""

import argparse
import json
import shutil
import sys
from pathlib import Path


def require_file(p: Path) -> Path:
    if not p.is_file():
        sys.exit(f"missing: {p}\nRun `pio run -e wireless-paper-v1_1` and "
                 f"`pio run -e wireless-paper-v1_2` first.")
    return p


def find_boot_app0() -> Path:
    p = (Path.home() / ".platformio" / "packages"
         / "framework-arduinoespressif32" / "tools" / "partitions"
         / "boot_app0.bin")
    if not p.is_file():
        sys.exit(f"boot_app0.bin not found at {p}\nBuild with PIO at least "
                 f"once so the framework package is installed.")
    return p


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--version", default="dev-local",
                        help="Version string injected into manifest JSON "
                             "(default: dev-local).")
    parser.add_argument("--out", default="site",
                        help="Output directory (default: site).")
    args = parser.parse_args()

    repo = Path(__file__).resolve().parents[1]
    out  = (repo / args.out).resolve()
    v12  = repo / ".pio" / "build" / "wireless-paper-v1_2"
    v11  = repo / ".pio" / "build" / "wireless-paper-v1_1"
    inst = repo / "install"

    out.mkdir(parents=True, exist_ok=True)

    # bootloader / partitions / boot_app0 are byte-identical across the two
    # envs (same chip + partition table). Take them once from v1.2.
    shutil.copy(require_file(v12 / "bootloader.bin"), out / "bootloader.bin")
    shutil.copy(require_file(v12 / "partitions.bin"), out / "partitions.bin")
    shutil.copy(find_boot_app0(),                     out / "boot_app0.bin")

    shutil.copy(require_file(v12 / "firmware.bin"), out / "firmware-v1_2.bin")
    shutil.copy(require_file(v11 / "firmware.bin"), out / "firmware-v1_1.bin")

    shutil.copy(require_file(inst / "index.html"), out / "index.html")

    for name in ("manifest-v1_1.json", "manifest-v1_2.json"):
        data = json.loads(require_file(inst / name).read_text())
        data["version"] = args.version
        (out / name).write_text(json.dumps(data, indent=2) + "\n")

    print(f"Assembled installer bundle -> {out}  (version: {args.version})")
    for p in sorted(out.iterdir()):
        print(f"  {p.name:30s} {p.stat().st_size:>10d} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
