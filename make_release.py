#!/usr/bin/env python3
"""
make_release.py - build the firmware image that ESP Launchpad and esptool flash.

    python3 make_release.py

Compiles the sketch (the folder this script lives in) with arduino-cli, then
merges bootloader, partition table, boot_app0 and app into linkscout.bin,
written at flash address 0x0.  Commit linkscout.bin together with
launchpad.toml; the one-click link in the README flashes whatever is on main.

The core's own .merged.bin is padded to the full 32 MB flash.  This one stops
at the end of the app (about 1.3 MB), which is all the firmware reads.  Gaps
between the parts are filled with 0xFF, so writing it also blanks NVS (0x9000)
and otadata (0xe000), the same as a fresh install.  The launchpad erases the
whole chip before writing in any case.

Local paths are kept out of the image.  assert() and ESP_LOG calls bake
__FILE__ into the firmware, which would otherwise publish this machine's
directory layout -- user name, and on a synced drive the account e-mail in the
mount path.  The build maps the sketch's parent folder, the libraries folder
and the Arduino data folder to short neutral names with -ffile-prefix-map, and
the result is checked: if the home directory still appears anywhere in the
image, nothing is written.

Needs arduino-cli and the esp32 core installed.  Uses the core's own esptool,
so the merge matches what the IDE would produce.  Standard library only.
"""

import argparse
import glob
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SKETCH_DIR = Path(__file__).resolve().parent
SKETCH = SKETCH_DIR.name
FQBN = "esp32:esp32:esp32p4:PSRAM=enabled,FlashSize=32M,CDCOnBoot=cdc"
OUTPUT = SKETCH_DIR / "linkscout.bin"

# ESP32-P4 flash layout used by the core (boards.txt esp32p4.build.bootloader_addr
# and the 0x8000 / 0xe000 / 0x10000 offsets in platform.txt's merge recipe).
BOOTLOADER_ADDR = 0x2000
LAYOUT = [
    (BOOTLOADER_ADDR, "{name}.ino.bootloader.bin"),
    (0x8000,          "{name}.ino.partitions.bin"),
    (0xE000,          "boot_app0.bin"),
    (0x10000,         "{name}.ino.bin"),
]
IMAGE_MAGIC = 0xE9


def default_libraries() -> Path:
    """libraries/ inside the sketch (the repository layout) if present,
    otherwise the Arduino tree's shared libraries folder next to it."""
    own = SKETCH_DIR / "libraries"
    return own if own.is_dir() else SKETCH_DIR.parent / "libraries"


def arduino_data_dir() -> Path:
    for base in (Path.home() / "Library/Arduino15", Path.home() / ".arduino15",
                 Path.home() / "AppData/Local/Arduino15"):
        if base.is_dir():
            return base.resolve()
    return Path.home() / "Library/Arduino15"


def prefix_map_flags(libraries: Path, build: Path) -> str:
    """-ffile-prefix-map options, as one build-property value.  Each source
    root gets its own non-overlapping mapping; the quotes survive arduino-cli's
    recipe splitting, so paths containing spaces are fine."""
    maps = [
        (build.resolve(), "build"),     # sketch .cpp files are compiled from here
        (SKETCH_DIR.parent, "src"),
        (libraries.resolve(), "libraries"),
        (arduino_data_dir(), "Arduino15"),
    ]
    seen, flags = set(), []
    for old, new in maps:
        if str(old) in seen:
            continue
        seen.add(str(old))
        flags.append(f'"-ffile-prefix-map={old}={new}"')
    return " ".join(flags)


def leaked_paths(data: bytes):
    """Local path fragments that must never appear in a published image."""
    needles = {str(Path.home()), str(SKETCH_DIR.parent), str(Path.home().resolve())}
    return sorted(n for n in needles if n.encode() in data)


def find_esptool() -> str:
    for base in (Path.home() / "Library/Arduino15", Path.home() / ".arduino15",
                 Path.home() / "AppData/Local/Arduino15"):
        hits = sorted(glob.glob(str(base / "packages/esp32/tools/esptool_py/*/esptool*")))
        hits = [h for h in hits if Path(h).name in ("esptool", "esptool.exe", "esptool.py")]
        if hits:
            return hits[-1]
    found = shutil.which("esptool") or shutil.which("esptool.py")
    if found:
        return found
    sys.exit("esptool not found -- is the esp32 core installed?")


def run(cmd):
    print("$ " + " ".join(str(c) for c in cmd))
    r = subprocess.run([str(c) for c in cmd])
    if r.returncode != 0:
        sys.exit(f"command failed with exit code {r.returncode}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--libraries", type=Path, default=default_libraries(),
                    help="Arduino libraries folder (default: ./libraries if present, "
                         "else ../libraries)")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory(prefix=f"{SKETCH}-build-") as build:
        build = Path(build)
        flags = prefix_map_flags(args.libraries, build)
        run(["arduino-cli", "compile", "--fqbn", FQBN,
             "--build-property", f"compiler.c.extra_flags={flags}",
             "--build-property", f"compiler.cpp.extra_flags={flags}",
             "--libraries", args.libraries.resolve(), "--build-path", build, SKETCH_DIR])

        merge = [find_esptool(), "--chip", "esp32p4", "merge-bin", "-o", OUTPUT,
                 "--flash-mode", "keep", "--flash-freq", "keep", "--flash-size", "keep"]
        for addr, pattern in LAYOUT:
            part = build / pattern.format(name=SKETCH)
            if not part.is_file():
                sys.exit(f"build output missing: {part.name}")
            merge += [hex(addr), part]
        tmp_out = build / OUTPUT.name
        merge[merge.index(OUTPUT)] = tmp_out
        run(merge)

        data = tmp_out.read_bytes()
        leaks = leaked_paths(data)
        if leaks:
            sys.exit("refusing to write the image: it still contains local paths:\n  "
                     + "\n  ".join(leaks))
        shutil.copyfile(tmp_out, OUTPUT)

    # Sanity: image headers where the ROM and bootloader will look for them.
    data = OUTPUT.read_bytes()
    for addr, what in ((BOOTLOADER_ADDR, "bootloader"), (0x10000, "app")):
        if len(data) <= addr or data[addr] != IMAGE_MAGIC:
            sys.exit(f"{OUTPUT.name}: no image header for the {what} at {addr:#x}")
    if data[0x8000:0x8002] != b"\xaa\x50":
        sys.exit(f"{OUTPUT.name}: no partition table at 0x8000")

    print(f"\nwrote {OUTPUT.name}: {len(data):,} bytes, flash at 0x0")
    print("Next: python3 make_repo_zip.py packages it with everything else that goes")
    print("on GitHub.")


if __name__ == "__main__":
    os.chdir(SKETCH_DIR.parent)
    main()
