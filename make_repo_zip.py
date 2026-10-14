#!/usr/bin/env python3
"""
make_repo_zip.py - package exactly what belongs in the GitHub repository.

Run from anywhere; the sketch is the folder this script lives in:

    python3 make_release.py      # first: builds linkscout.bin
    python3 make_repo_zip.py     # then:  dist/<Sketch>-YYYYMMDD.zip

The zip is a mirror of the repository root, under one top folder:

    <Sketch>/
        <Sketch>.ino, *.cpp, *.h ...   the sketch
        README.md, LICENSE, THIRD_PARTY_NOTICES.md, LICENSES/, docs/
        linkscout.bin, launchpad.toml  the release image and its launchpad config
        make_release.py, make_repo_zip.py
        libraries/                     every chipguy_* library the sketch
                                       includes, directly or through another one
        BUILD_INFO.txt                 core, ESP-IDF and library versions
        .gitignore

Unzip it and copy the contents of <Sketch>/ over the repository working tree.
Attach the zip itself to the GitHub release too: it is the complete source that
corresponds to that binary, which the LGPL-licensed Arduino core asks for.

Public, unmodified dependencies (the esp32 core, lvgl) are recorded by version
in BUILD_INFO.txt rather than bundled.

Refuses to write anything when:
  - linkscout.bin is missing, or older than any firmware source file (run
    make_release.py again);
  - linkscout.bin contains a local path (home directory or the folder this
    sketch sits in);
  - any text file looks like it holds a credential (passwords, keys,
    certificates, Wi-Fi/MQTT settings) -- the hits are listed as file:line.
Only the standard library is used.
"""

import argparse
import datetime
import json
import re
import subprocess
import sys
import zipfile
from pathlib import Path

SKETCH_DIR = Path(__file__).resolve().parent
SKETCH = SKETCH_DIR.name
THIS_FILE = Path(__file__).resolve()
RELEASE_IMAGE = SKETCH_DIR / "linkscout.bin"

# Files that become part of the repository besides sources: the image itself
# (binary, so it is checked separately rather than text-scanned).
EXTRA_FILES = [RELEASE_IMAGE]
BINARY_SUFFIXES = {".bin", ".jpg", ".png"}
FIRMWARE_SUFFIXES = {".ino", ".cpp", ".c", ".h", ".hpp", ".S"}

GITIGNORE = """\
dist/
build/
__pycache__/
.DS_Store
"""

FQBN = "esp32:esp32:esp32p4:PSRAM=enabled,FlashSize=32M,CDCOnBoot=cdc"

# What belongs in a source release.  Anything else in the sketch folder (build
# output, editor state, scratch files) is left out.
SOURCE_SUFFIXES = {".ino", ".cpp", ".c", ".h", ".hpp", ".S", ".md", ".txt",
                   ".py", ".csv", ".properties", ".jpg", ".toml"}
LICENSE_NAMES = re.compile(r"^(LICEN[CS]E|COPYING|NOTICE)", re.I)
# "libraries" is skipped when walking the sketch: in the repository layout it
# holds the bundled libraries, which are collected (and placed) separately.
SKIP_DIRS = {"build", "dist", "__pycache__", "node_modules", "libraries"}

# Files that, by name alone, hold credentials in this tree.  Their presence is
# fatal even if the scan below would pass them.
FORBIDDEN_NAMES = re.compile(r"^(creds|secrets?|credentials)(\.|_)|\.(pem|key|p12|pfx)$", re.I)

# Content that looks like a credential.  Deliberately broad: a false positive
# costs a re-run with the file fixed, a false negative publishes a password.
SECRET_PATTERNS = [
    (re.compile(r"-----BEGIN [A-Z ]*(CERTIFICATE|PRIVATE KEY)-----"), "PEM block"),
    (re.compile(r"\b(WIFI_SSID|WIFI_PASSWORD|MQTT_SERVER|MQTT_USER|MQTT_PASSWORD|OTA_PASSWORD|CA_CERT)\b"),
     "credential macro"),
    (re.compile(r"""(?i)\b\w*(password|passwd|passphrase|secret|api_?key|token)\w*\s*[:=]\s*["'][^"']+["']"""),
     "string literal assigned to a secret-named variable"),
    (re.compile(r"""(?i)\.(password|username|server)\s*=\s*["'][^"']+["']"""),
     "literal broker/login setting"),
    (re.compile(r"""(?i)--upload-field\s+password=\S+"""), "OTA password on a command line"),
]


def is_source_file(p: Path) -> bool:
    return p.suffix in SOURCE_SUFFIXES or bool(LICENSE_NAMES.match(p.name))


def walk(root: Path):
    """Yields release-worthy files under root, skipping hidden and build dirs."""
    for p in sorted(root.rglob("*")):
        rel = p.relative_to(root)
        if any(part.startswith(".") or part in SKIP_DIRS for part in rel.parts):
            continue
        if p.is_file() and is_source_file(p):
            yield p


INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)


def includes_of(files):
    names = set()
    for f in files:
        if f.suffix in {".ino", ".cpp", ".c", ".h", ".hpp"}:
            names.update(INCLUDE_RE.findall(f.read_text(errors="replace")))
    return names


def header_index(libs_dir: Path):
    """Maps header file name -> [library folders providing it], from each
    library's src/ and root.  Built once: stat-ing every library per header is
    slow on a synced drive."""
    index = {}
    for lib in sorted(libs_dir.iterdir()):
        if not lib.is_dir() or lib.name.startswith("."):
            continue
        for d in (lib / "src", lib):
            if d.is_dir():
                for f in d.iterdir():
                    if f.suffix in {".h", ".hpp"}:
                        index.setdefault(f.name, []).append(lib)
    return index


def pick_library(header: str, candidates):
    """Chooses among libraries that all provide `header`, roughly the way the
    Arduino builder does: a library named after the header wins outright."""
    stem = Path(header).stem
    for lib in candidates:
        if lib.name == stem:
            return lib
    if len(candidates) > 1:
        print(f"warning: {header} is provided by "
              + ", ".join(c.name for c in candidates)
              + f"; using {candidates[0].name}", file=sys.stderr)
    return candidates[0]


def resolve_libraries(sketch_files, libs_dir: Path):
    """Returns (bundled chipguy_* libs, other libs referenced) by following
    #includes transitively through the chipguy_* libraries.  A header found in
    the including sketch or library itself is that code's own and is not
    looked up elsewhere."""
    index = header_index(libs_dir)
    bundled, external = {}, {}
    queue = [(sketch_files, {f.name for f in sketch_files})]
    while queue:
        files, own = queue.pop()
        for header in sorted(includes_of(files)):
            name = Path(header).name
            if name in own or name not in index:
                continue
            lib = pick_library(header, index[name])
            if lib.name.startswith("chipguy_"):
                if lib.name not in bundled:
                    bundled[lib.name] = lib
                    lib_files = list(walk(lib))
                    queue.append((lib_files, {f.name for f in lib_files}))
            else:
                external[lib.name] = lib
    return bundled, external


def lib_version(lib: Path) -> str:
    props = lib / "library.properties"
    if props.is_file():
        m = re.search(r"^version\s*=\s*(.+)$", props.read_text(errors="replace"), re.M)
        if m:
            return m.group(1).strip()
    return "unknown"


def core_versions():
    """(core version, ESP-IDF version line), best effort."""
    core = "unknown"
    try:
        out = subprocess.run(["arduino-cli", "core", "list", "--json"],
                             capture_output=True, text=True, timeout=60).stdout
        data = json.loads(out)
        platforms = data.get("platforms", data) if isinstance(data, dict) else data
        for p in platforms or []:
            if p.get("id") == "esp32:esp32":
                core = p.get("installed_version") or p.get("installed") or core
    except Exception:
        pass
    idf = "unknown"
    vfile = (Path.home() / "Library/Arduino15/packages/esp32/tools/esp32p4-libs"
             / core / "versions.txt")
    if not vfile.is_file():   # Linux / Windows layouts
        for base in (Path.home() / ".arduino15", Path.home() / "AppData/Local/Arduino15"):
            cand = base / "packages/esp32/tools/esp32p4-libs" / core / "versions.txt"
            if cand.is_file():
                vfile = cand
                break
    if vfile.is_file():
        m = re.search(r"^esp-idf:\s*(.+)$", vfile.read_text(errors="replace"), re.M)
        if m:
            idf = m.group(1).strip()
    return core, idf


def scan_for_secrets(files):
    hits = []
    for f in files:
        if f.resolve() == THIS_FILE or f.suffix in BINARY_SUFFIXES:
            continue   # self-matching patterns; binaries are checked separately
        if FORBIDDEN_NAMES.search(f.name):
            hits.append(f"{f}: file name suggests credentials")
            continue
        try:
            text = f.read_text(errors="replace")
        except OSError as e:
            hits.append(f"{f}: unreadable ({e})")
            continue
        for lineno, line in enumerate(text.splitlines(), 1):
            for rx, what in SECRET_PATTERNS:
                if rx.search(line):
                    hits.append(f"{f}:{lineno}: {what}")
    return hits


def default_libraries() -> Path:
    """libraries/ inside the sketch (the repository layout) if present,
    otherwise the Arduino tree's shared libraries folder next to it."""
    own = SKETCH_DIR / "libraries"
    return own if own.is_dir() else SKETCH_DIR.parent / "libraries"


def check_release_image(firmware_files):
    if not RELEASE_IMAGE.is_file():
        sys.exit(f"{RELEASE_IMAGE.name} not found -- run make_release.py first")
    built = RELEASE_IMAGE.stat().st_mtime
    newer = [f for f in firmware_files if f.stat().st_mtime > built]
    if newer:
        sys.exit(f"{RELEASE_IMAGE.name} is older than its source -- run make_release.py "
                 "again.  Changed since the build:\n  "
                 + "\n  ".join(str(f) for f in newer[:10]))
    data = RELEASE_IMAGE.read_bytes()
    needles = {str(Path.home()), str(Path.home().resolve()), str(SKETCH_DIR.parent)}
    leaks = sorted(n for n in needles if n.encode() in data)
    if leaks:
        sys.exit(f"{RELEASE_IMAGE.name} contains local paths -- rebuild it with "
                 "make_release.py, which maps them out:\n  " + "\n  ".join(leaks))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--libraries", type=Path, default=default_libraries(),
                    help="Arduino libraries folder (default: ./libraries if present, "
                         "else ../libraries)")
    ap.add_argument("--out", type=Path, default=SKETCH_DIR / "dist",
                    help="output folder (default: ./dist)")
    args = ap.parse_args()

    if not args.libraries.is_dir():
        sys.exit(f"libraries folder not found: {args.libraries}")

    # A credentials file anywhere in the sketch folder is fatal, whether or not
    # it would have been packaged -- its presence means the sketch may use it.
    for p in SKETCH_DIR.rglob("*"):
        rel = p.relative_to(SKETCH_DIR)
        if p.is_file() and not any(part.startswith(".") or part in SKIP_DIRS for part in rel.parts) \
                and FORBIDDEN_NAMES.search(p.name):
            sys.exit(f"refusing to package: {p} looks like a credentials file")

    # BUILD_INFO.txt is regenerated below; in a clone, the old copy is skipped.
    sketch_files = [f for f in walk(SKETCH_DIR) if f.name != "BUILD_INFO.txt"]
    bundled, external = resolve_libraries(sketch_files, args.libraries)

    lib_files = {name: list(walk(path)) for name, path in bundled.items()}
    everything = sketch_files + [f for fs in lib_files.values() for f in fs]

    check_release_image([f for f in everything if f.suffix in FIRMWARE_SUFFIXES])

    hits = scan_for_secrets(everything)
    if hits:
        print("Possible credentials found -- nothing was written:", file=sys.stderr)
        for h in hits:
            print("  " + h, file=sys.stderr)
        sys.exit(1)

    core, idf = core_versions()
    today = datetime.date.today()
    lines = [
        f"{SKETCH}, packaged {today.isoformat()}",
        "",
        f"esp32 core (espressif/arduino-esp32): {core}",
        f"ESP-IDF in that core's esp32p4-libs:   {idf}",
        "",
        "Bundled libraries (in libraries/):",
        *[f"  {n} {lib_version(p)}" for n, p in sorted(bundled.items())],
        "",
        "External libraries (install from Library Manager, unmodified):",
        *([f"  {n} {lib_version(p)}" for n, p in sorted(external.items())] or ["  (none)"]),
        "",
        "Build, from the folder that contains this one:",
        f'  arduino-cli compile --fqbn "{FQBN}" \\',
        f"      --libraries {SKETCH}/libraries {SKETCH}",
        "or run make_release.py here, which also produces linkscout.bin.",
        "",
        "To relink against a modified Arduino-ESP32 core, install the modified core",
        "in place of the version above and rebuild with the same command.",
        "",
    ]

    root = SKETCH
    args.out.mkdir(parents=True, exist_ok=True)
    zpath = args.out / f"{SKETCH}-{today.strftime('%Y%m%d')}.zip"
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for f in sketch_files + EXTRA_FILES:
            z.write(f, f"{root}/{f.relative_to(SKETCH_DIR).as_posix()}")
        for name, files in lib_files.items():
            for f in files:
                z.write(f, f"{root}/libraries/{name}/{f.relative_to(bundled[name]).as_posix()}")
        z.writestr(f"{root}/BUILD_INFO.txt", "\n".join(lines))
        z.writestr(f"{root}/.gitignore", GITIGNORE)

    print(f"wrote {zpath}")
    print(f"  sketch files: {len(sketch_files)}")
    for name, files in sorted(lib_files.items()):
        print(f"  {name}: {len(files)} files")
    if external:
        print("  not bundled (public): " + ", ".join(f"{n} {lib_version(p)}" for n, p in sorted(external.items())))


if __name__ == "__main__":
    main()
