#!/usr/bin/env python3
"""Validate the self-contained ESPClaw openvela contest package."""

from __future__ import annotations

import hashlib
import re
import sys
from pathlib import Path
from xml.etree import ElementTree


ROOT = Path(__file__).resolve().parents[1]
TEAM = "contest2026_359_dengfengzaojidecuipidaxuesheng"
MANIFEST = ROOT / f"{TEAM}.xml"

REQUIRED_FILES = (
    ROOT / ".gitattributes",
    ROOT / ".gitignore",
    ROOT / "README.md",
    ROOT / "openvela.xml",
    MANIFEST,
    ROOT / "board/esp32p4-common/Kconfig",
    ROOT / "board/esp32p4-common/scripts/common.ld",
    ROOT / "board/esp32p4-function-ev-board/README.md",
    ROOT / "board/esp32p4-function-ev-board/configs/openvela/defconfig",
    ROOT / "board/esp32p4-function-ev-board/src/esp32p4_bringup.c",
    ROOT / "chip/esp32p4/src/Kconfig",
    ROOT / "chip/esp32p4/include/chip.h",
    ROOT / "tools/apply_esp32p4_overlay.py",
    ROOT / "tools/apply_final_overlays.sh",
    ROOT / "tools/patches/0001-esp32p4-nuttx-overlay.patch",
    ROOT / "tools/patches/0002-esp32p4-apps-overlay.patch",
    ROOT / "tools/patches/0003-esp32p4-sc2336-camera.patch",
    ROOT / "tools/patches/0004-espclaw-apps-homeassistant-lvgl-ui.patch",
    ROOT / "tools/patches/0005-espclaw-nuttx-camera-lvgl-network.patch",
    ROOT / "firmware/历史测试固件/esp32p4-desktop-v1.0-release/nuttx-lvgl-unified-ui.bin",
    ROOT / "firmware/历史测试固件/esp32p4-desktop-v1.0-release/SHA256SUMS",
    ROOT / "firmware/历史测试固件/esp32p4-desktop-v1.0-release/defconfig",
    ROOT / "firmware/历史测试固件/esp32p4-desktop-v1.0-release/TEST_REPORT.md",
    ROOT / "docs/evidence/homeassistant-full-final-boot.log",
    ROOT / "docs/evidence/homeassistant-network.log",
    ROOT / "docs/evidence/lvgl-build-verify.log",
    ROOT / "logs/README.md",
    ROOT / "tools/check_submission.py",
)

REQUIRED_LINKS = {
    (
        "board/esp32p4-function-ev-board",
        "vendor/espressif/boards/esp32p4/esp32p4-function-ev-board",
    ),
    ("board/esp32p4-common", "nuttx/boards/risc-v/esp32p4/common"),
    ("chip/esp32p4/src", "nuttx/arch/risc-v/src/esp32p4"),
    ("chip/esp32p4/include", "nuttx/arch/risc-v/include/esp32p4"),
}

BINARY_SUFFIXES = {
    ".a",
    ".bin",
    ".elf",
    ".gz",
    ".hex",
    ".jpeg",
    ".jpg",
    ".map",
    ".o",
    ".pdf",
    ".png",
    ".zip",
}

FORBIDDEN_BUILD_SUFFIXES = {".a", ".elf", ".map", ".o"}


class CheckResult:
    """Collect all failures so one run gives actionable output."""

    def __init__(self) -> None:
        self.errors: list[str] = []
        self.notices: list[str] = []

    def require(self, condition: bool, message: str) -> None:
        if not condition:
            self.errors.append(message)


def relative(path: Path) -> str:
    return path.relative_to(ROOT).as_posix()


def check_required_files(result: CheckResult) -> None:
    for path in REQUIRED_FILES:
        result.require(path.is_file(), f"missing required file: {relative(path)}")
        if path.is_file():
            result.require(path.stat().st_size > 0, f"empty required file: {relative(path)}")


def check_manifest(result: CheckResult) -> None:
    if not MANIFEST.is_file():
        return

    try:
        root = ElementTree.parse(MANIFEST).getroot()
    except ElementTree.ParseError as exc:
        result.errors.append(f"invalid manifest XML: {exc}")
        return

    projects = [
        node
        for node in root.findall("project")
        if node.attrib.get("name") == TEAM
    ]
    result.require(len(projects) == 1, "manifest must contain exactly one team project")
    if len(projects) != 1:
        return

    project = projects[0]
    result.require(project.attrib.get("path") == TEAM, "team project path is incorrect")
    result.require(
        project.attrib.get("revision") == "codex/espclaw-final",
        "team project revision must be codex/espclaw-final",
    )
    result.require(
        project.attrib.get("remote") == "esp32p4-team",
        "team project remote must be esp32p4-team",
    )

    links = {
        (node.attrib.get("src"), node.attrib.get("dest"))
        for node in project.findall("linkfile")
    }
    missing_links = sorted(REQUIRED_LINKS - links)
    for source, destination in missing_links:
        result.errors.append(f"missing linkfile: {source} -> {destination}")

    removed = {node.attrib.get("name") for node in root.findall("remove-project")}
    result.require(
        not removed.intersection({"nuttx", "nuttx-apps", "apps"}),
        "team manifest must not replace public NuttX/apps projects",
    )

    try:
        base = ElementTree.parse(ROOT / "openvela.xml").getroot()
    except (ElementTree.ParseError, OSError) as exc:
        result.errors.append(f"invalid openvela.xml: {exc}")
        return
    remotes = {node.attrib.get("name") for node in base.findall("remote")}
    result.require("esp32p4-team" in remotes, "openvela.xml missing esp32p4-team remote")


def parse_checksum_line(line: str) -> tuple[str, str] | None:
    stripped = line.strip()
    if not stripped or stripped.startswith("#"):
        return None
    parts = stripped.split(maxsplit=1)
    if len(parts) != 2 or not re.fullmatch(r"[0-9a-fA-F]{64}", parts[0]):
        return None
    return parts[0].lower(), parts[1].lstrip("*")


def check_firmware_hashes(result: CheckResult) -> None:
    checksum_files = sorted((ROOT / "firmware").glob("**/SHA256SUMS"))
    result.require(bool(checksum_files), "no firmware SHA256SUMS files found")
    verified = 0
    for checksum_file in checksum_files:
        for line_number, line in enumerate(
            checksum_file.read_text(encoding="utf-8", errors="replace").splitlines(),
            start=1,
        ):
            parsed = parse_checksum_line(line)
            if parsed is None:
                if line.strip() and not line.lstrip().startswith("#"):
                    result.errors.append(
                        f"invalid checksum line: {relative(checksum_file)}:{line_number}"
                    )
                continue
            expected, name = parsed
            artifact = (checksum_file.parent / name).resolve()
            try:
                artifact.relative_to(ROOT.resolve())
            except ValueError:
                result.errors.append(
                    f"checksum path escapes repository: {relative(checksum_file)}:{line_number}"
                )
                continue
            if not artifact.is_file():
                result.errors.append(
                    f"checksum target missing: {relative(checksum_file.parent / name)}"
                )
                continue
            actual = hashlib.sha256(artifact.read_bytes()).hexdigest()
            if actual != expected:
                result.errors.append(f"firmware hash mismatch: {relative(artifact)}")
            else:
                verified += 1
    result.require(verified > 0, "no firmware artifacts were hash-verified")
    result.notices.append(f"verified firmware artifacts: {verified}")


def check_patch_files(result: CheckResult) -> None:
    for path in sorted((ROOT / "tools/patches").glob("*.patch")):
        text = path.read_text(encoding="utf-8", errors="replace")
        result.require("diff --git " in text, f"not a Git patch: {relative(path)}")


def check_repository_hygiene(result: CheckResult) -> None:
    forbidden_names = {".env", "id_dsa", "id_ed25519", "id_rsa"}
    secret_patterns = {
        "GitHub token": re.compile(r"gh" + r"[pousr]_[A-Za-z0-9]{30,}"),
        "GitHub fine-grained token": re.compile(
            r"github" + r"_pat_[A-Za-z0-9_]{40,}"
        ),
        "OpenAI-style key": re.compile(r"s" + r"k-[A-Za-z0-9_-]{20,}"),
        "Bearer credential": re.compile(
            r"Bearer\s+[A-Za-z0-9._~+/=-]{24,}", re.IGNORECASE
        ),
    }

    for path in ROOT.rglob("*"):
        if not path.is_file() or ".git" in path.parts:
            continue
        if path.name in forbidden_names:
            result.errors.append(f"credential file must not be committed: {relative(path)}")
        if path.suffix.lower() in FORBIDDEN_BUILD_SUFFIXES:
            result.errors.append(f"build artifact must not be committed: {relative(path)}")
        if path.suffix.lower() in BINARY_SUFFIXES or path.stat().st_size > 8 * 1024 * 1024:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for label, pattern in secret_patterns.items():
            if pattern.search(text):
                result.errors.append(f"possible {label} in {relative(path)}")


def main() -> int:
    result = CheckResult()
    check_required_files(result)
    check_manifest(result)
    check_firmware_hashes(result)
    check_patch_files(result)
    check_repository_hygiene(result)

    for notice in result.notices:
        print(f"NOTICE: {notice}")
    if result.errors:
        print("PACKAGE CHECK FAILED")
        for error in result.errors:
            print(f"- {error}")
        return 1

    print("PASS: openvela contest package structure, hashes, patches and hygiene")
    return 0


if __name__ == "__main__":
    sys.exit(main())
