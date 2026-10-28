#!/usr/bin/env python3
"""Fetch, verify, extract and patch the pinned sources used by NetSurf Modern.

Every archive is SHA-256 checked before anything is written.  Existing source
trees are never overwritten, so local edits survive re-running this script.

    python tools/setup_deps.py                 # download what is missing
    python tools/setup_deps.py --cache DIR     # reuse archives from DIR
"""
import argparse
import hashlib
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[1]
DEPS = ROOT / "deps"
MODERN = DEPS / "netsurf-modern"
REVISION = "29d910fc28e6e0c816eea4a86bae4e40f2ac699d"
MARKER = MODERN / ".nsw-version"

# name -> (url, sha256, destination group inside deps/netsurf-modern or None)
ARCHIVES = {
    "netsurf-modern-source.tar.gz": (
        f"https://codeload.github.com/etonedemid/netsurf-switch/tar.gz/{REVISION}",
        "2e3b34d914a5a92f21dec59c8aca4074cb48a70d07edd83a53d5e7dff3d94635", None),
    "quickjs-v0.17.0.tar.gz": (
        "https://codeload.github.com/quickjs-ng/quickjs/tar.gz/refs/tags/v0.17.0",
        "559bc4c420475e55c7ab4510adbc562f55d7524d75e8e89d79ce4bb02f5687d9", "quickjs"),
    "freetype-2.14.3.tar.gz": (
        "https://codeload.github.com/freetype/freetype/tar.gz/refs/tags/VER-2-14-3",
        "dc49de6b01a266eef4876a4dd34d9842c475d3e28ff2eff63bd2fb760ab56261", "freetype"),
    "expat-2.8.5.tar.gz": (
        "https://github.com/libexpat/libexpat/releases/download/R_2_8_5/expat-2.8.5.tar.gz",
        "920dde485e15eda0cce8d2310b41d492c534e5e3d89ad407a0b4176dd2ff88fe", "expat"),
}
SOURCE_GROUPS = {
    "netsurf", "libcss", "libdom", "libhubbub", "libparserutils",
    "libwapcaplet", "libnsutils", "buildsystem", "libsvgtiny",
}
EXTRA = {
    "libwebp-1.6.0.tar.gz": (
        "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.6.0.tar.gz",
        "e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564"),
    "zlib-51b7f2ab.tar.gz": (
        "https://codeload.github.com/madler/zlib/tar.gz/51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf",
        None),  # filled in by ZLIB_SHA256 below
}
ZLIB_SHA256 = "d9e270d46252734aa49770fbc544125391617956266f220bd63216c834f3a522"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def fetch(cache: Path, name: str, url: str, expected: str) -> Path:
    path = cache / name
    if not path.exists():
        print(f"Downloading {name}")
        partial = path.with_name(path.name + ".partial")
        request = urllib.request.Request(url, headers={"User-Agent": "netsurf-windows-setup"})
        with urllib.request.urlopen(request, timeout=180) as response, partial.open("wb") as out:
            shutil.copyfileobj(response, out, 1 << 20)
        partial.replace(path)
    actual = sha256(path)
    if actual != expected:
        raise SystemExit(f"SHA256 mismatch for {name}: {actual}")
    return path


def safe_target(base: Path, parts) -> Path:
    target = base.joinpath(*parts)
    if not target.resolve().is_relative_to(base.resolve()):
        raise SystemExit(f"Unsafe archive path: {'/'.join(parts)}")
    return target


def extract(archive: Path, base: Path, accept) -> int:
    count = 0
    with tarfile.open(archive) as tar:
        for member in tar:
            parts = PurePosixPath(member.name).parts[1:]
            if not parts or ".." in parts or not member.isfile():
                continue
            relative = accept(parts)
            if relative is None:
                continue
            target = safe_target(base, relative)
            target.parent.mkdir(parents=True, exist_ok=True)
            with tar.extractfile(member) as source:
                target.write_bytes(source.read())
            count += 1
    return count


def apply_patches() -> None:
    for patch in sorted((ROOT / "patches").glob("*.patch")):
        for mode in (["--check"], []):
            result = subprocess.run(
                ["git", "apply", *mode, "--whitespace=nowarn",
                 f"--directory={MODERN.relative_to(ROOT).as_posix()}", "--", str(patch)],
                cwd=ROOT, capture_output=True, text=True)
            if result.returncode:
                raise SystemExit(f"Patch {patch.name} failed:\n{result.stderr}")
    print(f"Applied {len(list((ROOT / 'patches').glob('*.patch')))} patches")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, default=DEPS / "downloads")
    args = parser.parse_args()
    cache = args.cache
    cache.mkdir(parents=True, exist_ok=True)

    paths = {name: fetch(cache, name, url, digest) for name, (url, digest, _) in ARCHIVES.items()}
    webp = fetch(cache, "libwebp-1.6.0.tar.gz", *EXTRA["libwebp-1.6.0.tar.gz"])
    zlib_url = EXTRA["zlib-51b7f2ab.tar.gz"][0]
    zlib = fetch(cache, "zlib-51b7f2ab.tar.gz", zlib_url, ZLIB_SHA256)

    if MARKER.exists():
        if MARKER.read_text().strip() != REVISION:
            raise SystemExit("Unexpected netsurf-modern revision in deps/")
        print("netsurf-modern already prepared; local edits preserved")
    else:
        if MODERN.exists() and any(MODERN.iterdir()):
            raise SystemExit(f"Refusing to overwrite unmarked tree: {MODERN}")
        for name, (_, _, group) in ARCHIVES.items():
            if group is None:
                n = extract(paths[name], MODERN,
                            lambda p: PurePosixPath(*p).parts if p[0] in SOURCE_GROUPS else None)
            else:
                n = extract(paths[name], MODERN, lambda p, g=group: (g, *p))
            print(f"Extracted {name} ({n} files)")
        apply_patches()
        MARKER.write_text(REVISION + "\n")

    for tarball, dest, keep in ((webp, DEPS / "libwebp", ("src", "COPYING", "AUTHORS")),
                                (zlib, DEPS / "zlib", None)):
        if dest.exists() and any(dest.iterdir()):
            print(f"{dest.name} already present")
            continue
        n = extract(tarball, dest, lambda p, k=keep: p if (k is None or p[0] in k) else None)
        print(f"Extracted {tarball.name} ({n} files)")
    print("Dependencies ready.")


if __name__ == "__main__":
    sys.exit(main())
