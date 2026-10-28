#!/usr/bin/env python3
"""Verify the shipped native App archive against its matching host image report."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zipfile

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--distribution',type=Path,required=True)
p.add_argument('--firmware',type=Path,required=True)
a=p.parse_args()
distribution=a.distribution.resolve()
package=json.loads((distribution/'package.json').read_text())
firmware=json.loads((a.firmware/'verification.json').read_text())
archive=distribution/package['archive']
assert package['includes_browser_engine'] is True
assert archive.stat().st_size==package['archive_bytes']
assert hashlib.sha256(archive.read_bytes()).hexdigest()==package['sha256']
with zipfile.ZipFile(archive) as z:
    assert z.testzip() is None
    prefix=package['package']+'/'
    manifest=json.loads(z.read(prefix+'manifest.json'))
    module=json.loads(z.read(prefix+'native/module.json'))
    binary=z.read(prefix+'native/browser.elf')
    assert manifest['entry']=='app.js' and manifest['versionName']==package['version']
    assert manifest['native']['hostAbi']==module['host_abi']==package['host_abi']==firmware['host_abi']
    assert z.read(prefix+'native/host.abi').decode().strip()==module['host_abi']
    assert hashlib.sha256(binary).hexdigest()==module['sha256']==firmware['module_sha256']
    assert len(binary)==module['module_bytes']==firmware['module_bytes']
    assert binary[:6]==b'\x7fELF\x01\x01' # ELF32 little endian.
    assert struct.unpack_from('<HH',binary,16)==(1,243) # ET_REL / RISC-V.
    assert z.read(prefix+'app.js')==(distribution/prefix/'app.js').read_bytes()
assert not firmware['browser_engine_embedded'] and firmware['explicit_module_cache_sync_linked']
assert firmware['layout_passed'] and not firmware['board_tested']
image=a.firmware/'Vela-NativeLoader-P4-UNTESTED.bin'
assert hashlib.sha256(image.read_bytes()).hexdigest()==firmware['sha256']
print('PASS: complete native App ZIP, ELF format/digests, matching host ABI and firmware digest')
