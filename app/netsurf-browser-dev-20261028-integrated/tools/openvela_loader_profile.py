#!/usr/bin/env python3
"""Validate an already deployed loader without changing firmware or exports."""
import hashlib
import json
from pathlib import Path


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def validate(profile_path, target):
    profile = json.loads(Path(profile_path).read_text())
    target = Path(target).resolve()
    if Path(profile['target']).resolve() != target:
        raise ValueError('verified loader target mismatch')
    required = {
        'nuttx/nuttx.bin': 'firmware_sha256',
        'nuttx/nuttx': 'elf_sha256',
        'nuttx/.config': 'config_sha256',
        'libbrowser-loader.a': 'loader_archive_sha256',
        'apps/system/desktop/browser/browser_host_abi.h': 'host_abi_header_sha256',
    }
    for name, key in required.items():
        if digest(target / name) != profile[key]:
            raise ValueError('verified loader file changed: ' + name)
    for name, expected in profile['tls_source_sha256'].items():
        if digest(target / name) != expected:
            raise ValueError('verified TLS source changed: ' + name)
    header = (target / 'apps/system/desktop/browser/browser_host_abi.h').read_text()
    if '"' + profile['host_abi'] + '"' not in header:
        raise ValueError('verified loader ABI mismatch')
    reference = Path(profile['export_reference_profile'])
    if digest(reference) != profile['export_reference_profile_sha256']:
        raise ValueError('export reference profile changed')
    original = json.loads(reference.read_text())
    if (sorted(profile['exports']) != sorted(original['retained_exports']) or
            profile['loader_archive_sha256'] != original['loader_archive_sha256'] or
            profile['config_sha256'] != original['config_sha256'] or
            profile['tls_source_sha256'] != original['tls_source_sha256']):
        raise ValueError('existing firmware does not retain the verified loader contract')
    deployment_verified = False
    for evidence in profile['board_evidence']:
        path = Path(evidence['path'])
        if digest(path) != evidence['sha256']:
            raise ValueError('board evidence changed: ' + str(path))
        document = json.loads(path.read_text())
        deployed = document.get('deployment', {})
        deployment_verified |= bool(
            deployed.get('image_sha256') == profile['firmware_sha256'] and
            deployed.get('readback_sha256') == profile['firmware_sha256'] and
            deployed.get('independent_readback_verified') and
            deployed.get('flash_write_verified'))
    if not deployment_verified:
        raise ValueError('no independently verified deployment for this firmware')
    return profile
