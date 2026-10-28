#!/usr/bin/env python3
"""Clone the verified loader and patch only opt-in prompt cancellation."""
import argparse, difflib, hashlib, json, os, shutil, subprocess
from pathlib import Path
from openvela_prompt_cancel import apply
from openvela_loader_profile import validate

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--baseline', type=Path, required=True)
p.add_argument('--target', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--build', action='store_true')
p.add_argument('--resume', action='store_true',help='rebuild only this tool\'s existing candidate')
p.add_argument('--verified-baseline-profile', type=Path,
               help='validate a later deployed loader instead of the frozen release')
p.add_argument('--response-headers', action='store_true',help='also retain full bounded long response headers')
p.add_argument('--psram-diagnostics', action='store_true',help='capture P4 fault registers before clearing; fatal policy unchanged')
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
base, target, out = a.baseline.resolve(), a.target.resolve(), a.output.resolve()
release = root/'dist/releases/esp32p4-0.5.0/release-manifest.json'
expected = json.loads(release.read_text())['firmware_sha256']
digest = lambda f: hashlib.sha256(f.read_bytes()).hexdigest()
if a.verified_baseline_profile:
    expected = validate(a.verified_baseline_profile, base)['firmware_sha256']
if digest(base/'nuttx/nuttx.bin') != expected: p.error('baseline is not the verified loader')
if base == target or base in target.parents or target in base.parents: p.error('target must be independent')
if target.exists() and not a.resume: p.error('target must be new; --resume requires its own matching profile')
if a.resume:
    profile=json.loads((out/'loader-profile.json').read_text())
    if profile['target']!=str(target) or profile['baseline']!=str(base):p.error('candidate profile mismatch')
runtime_path = Path('apps/system/desktop/qpk_runtime.c')
before = (base/runtime_path).read_text()
after = apply(before)
if a.resume and ((target/runtime_path).read_text()!=after or
                 digest(target/'nuttx/.config')!=digest(base/'nuttx/.config') or
                 digest(target/'libbrowser-loader.a')!=digest(base/'libbrowser-loader.a')):
    p.error('candidate has unrelated changes; refusing to overwrite them')
target.mkdir(parents=True,exist_ok=a.resume); out.mkdir(parents=True, exist_ok=True)
if not a.resume:
    for name in ['apps', 'nuttx']:
        subprocess.run(['cp', '-a', '--reflink=auto', str(base/name), str(target/name)], check=True)
for parent, dirs, files in os.walk(target, followlinks=False):
    for name in dirs+files:
        path = Path(parent)/name
        if path.is_symlink():
            link = os.readlink(path)
            if link.startswith(str(base)+'/'):
                path.unlink(); path.symlink_to(str(target)+link[len(str(base)):])
for path in (target/'apps').rglob('Kconfig'):
    if not path.is_symlink():
        text = path.read_text(); updated = text.replace(str(base)+'/', str(target)+'/')
        if updated != text: path.write_text(updated)
shutil.copy2(base/'libbrowser-loader.a', target/'libbrowser-loader.a')
(target/runtime_path).write_text(after)
if a.psram_diagnostics:
    from openvela_psram_diagnostics import apply as apply_diagnostics
    diagnostic_path=Path('nuttx/arch/risc-v/src/esp32p4/esp-hal-3rdparty/components/esp_psram/system_layer/esp_psram_mspi.c')
    original_diagnostic=(base/diagnostic_path).read_text();patched_diagnostic=apply_diagnostics(original_diagnostic)
    if (target/diagnostic_path).read_text() not in (original_diagnostic,patched_diagnostic):raise RuntimeError('unrelated PSRAM ISR changes')
    (target/diagnostic_path).write_text(patched_diagnostic)
    (out/'psram-diagnostics.diff').write_text(''.join(difflib.unified_diff(original_diagnostic.splitlines(True),patched_diagnostic.splitlines(True),fromfile=str(base/diagnostic_path),tofile=str(target/diagnostic_path))))
if a.response_headers:
    from openvela_response_headers import apply as apply_headers
    webclient=Path('apps/netutils/webclient/webclient.c')
    original=(base/webclient).read_text();patched=apply_headers(original)
    current=(target/webclient).read_text()
    if current not in (original,patched):raise RuntimeError('unrelated webclient changes')
    (target/webclient).write_text(patched)
    (out/'response-headers.diff').write_text(''.join(difflib.unified_diff(original.splitlines(True),patched.splitlines(True),fromfile=str(base/webclient),tofile=str(target/webclient))))
(out/'prompt-cancel.diff').write_text(''.join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
    fromfile=str(base/runtime_path), tofile=str(target/runtime_path))))
command = [arg.replace(str(base), str(target)) for arg in json.loads((base/'build-command.json').read_text())]
(target/'build-command.json').write_text(json.dumps(command, indent=2)+'\n')
profile = {'baseline_sha256': expected, 'baseline': str(base), 'target': str(target),
    'change': 'prompt.input opt-in notifyCancel; prompt.cancelNotification capability',
    'runtime_sha256': digest(target/runtime_path), 'config_sha256': digest(target/'nuttx/.config'),
    'loader_archive_sha256': digest(target/'libbrowser-loader.a'), 'command': command}
if a.verified_baseline_profile:
    profile['verified_baseline_profile'] = str(a.verified_baseline_profile.resolve())
if a.response_headers:
    profile['response_headers_sha256']=digest(target/webclient)
if a.psram_diagnostics:
    profile['psram_diagnostics_sha256']=digest(target/diagnostic_path)
(out/'loader-profile.json').write_text(json.dumps(profile, indent=2)+'\n')
if a.build:
    # NuttX app object names encode their absolute directory. A copied archive
    # can retain both old/new members and silently link the old runtime first.
    # Recreate only this candidate's application archive; keep compiled objects.
    for path in [target/'apps/libapps.a', target/'nuttx/staging/libapps.a',
                 *list((target/'apps').rglob('*.built'))]:
        if not path.resolve().is_relative_to(target):
            raise RuntimeError('refusing to remove an archive/stamp outside candidate: '+str(path))
        path.unlink(missing_ok=True)
    env = dict(os.environ)
    env['PATH'] = str(base.parent/'toolchains/gcc14/bin')+':'+str(base.parent/'.venv/bin')+':'+env['PATH']
    subprocess.run(command, cwd=target/'nuttx', env=env, check=True)
    if digest(target/'nuttx/.config') != profile['config_sha256']:
        raise RuntimeError('configuration changed during build')
    elf=(target/'nuttx/nuttx').read_bytes()
    if b'cancelNotification\0' not in elf or b'notifyCancel\0' not in elf:
        raise RuntimeError('final ELF did not link the patched prompt runtime')
    ar=base.parent/'toolchains/gcc14/bin/riscv32-esp-elf-ar'
    members=subprocess.check_output([str(ar),'t',str(target/'apps/libapps.a')],text=True)
    runtimes=[name for name in members.splitlines() if name.startswith('qpk_runtime.c')]
    if len(runtimes)!=1 or 'integration-netsurf-live-20261021' in runtimes[0]:
        raise RuntimeError('mixed old/new prompt runtime objects: '+str(runtimes))
    profile['linked_prompt_capability_verified']=True
    profile['runtime_archive_members']=runtimes
    profile['firmware_sha256'] = digest(target/'nuttx/nuttx.bin')
    (out/'loader-profile.json').write_text(json.dumps(profile, indent=2)+'\n')
print('INPUT_LOADER_READY', target)
