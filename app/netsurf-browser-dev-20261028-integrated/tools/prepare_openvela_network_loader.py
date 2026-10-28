#!/usr/bin/env python3
"""Build an isolated network loader without changing configuration/runtime.

The base mode adds three nonblocking-webclient exports. --bounded-tcp adds an
explicit opt-in TLS wrapper while preserving its old request layout and API.
The baseline must match the selected candidate's recorded firmware hash.
"""
import argparse, hashlib, json, os, shutil, subprocess
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--baseline', type=Path, required=True)
p.add_argument('--target', type=Path, required=True)
p.add_argument('--core', type=Path, required=True)
p.add_argument('--library', type=Path, required=True)
p.add_argument('--toolchain', type=Path, required=True)
p.add_argument('--resume', action='store_true')
p.add_argument('--name',default='direct-timeouts')
p.add_argument('--baseline-feature',default='network-connectivity')
p.add_argument('--bounded-tcp',action='store_true')
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
import re,difflib
if any(not re.fullmatch(r'[a-z][a-z0-9-]*',v) for v in [a.name,a.baseline_feature]):p.error('invalid feature')
out = root/'build/openvela'/a.name
dest = root/'dist/candidates'/a.name
base, target = a.baseline.resolve(), a.target.resolve()
digest = lambda f: hashlib.sha256(f.read_bytes()).hexdigest()
prior = json.loads((root/'dist/candidates'/a.baseline_feature/'candidate.json').read_text())
if digest(base/'nuttx/nuttx.bin') != prior['loader_sha256']:
    p.error('baseline is not the board-verified input-cancel loader')
if base == target or base in target.parents or target in base.parents:
    p.error('target must be independent')
if target.exists() and not a.resume: p.error('target must be new')
profile_path = out/'network-loader-profile.json'
if a.resume:
    old = json.loads(profile_path.read_text())
    if old['target'] != str(target) or old['baseline'] != str(base):
        p.error('resume profile mismatch')
else:
    target.mkdir(parents=True)
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
            before = path.read_text(); after = before.replace(str(base)+'/', str(target)+'/')
            if after != before: path.write_text(after)
    shutil.copy2(base/'libbrowser-loader.a', target/'libbrowser-loader.a')
command = [part.replace(str(base), str(target)) for part in json.loads((base/'build-command.json').read_text())]
profile = {'baseline': str(base), 'target': str(target), 'baseline_sha256': prior['loader_sha256'],
           'change': 'export poll, webclient_abort and webclient_get_poll_info; no configuration/runtime/TLS change',
           'config_sha256': digest(base/'nuttx/.config'), 'command': command}
profile_path.write_text(json.dumps(profile, indent=2)+'\n')
(target/'build-command.json').write_text(json.dumps(command, indent=2)+'\n')
tls_paths=['apps/system/desktop/glass_https.c','apps/system/desktop/glass_https.h']
for name in ['apps/system/desktop/qpk_runtime.c', 'nuttx/.config']+([] if a.bounded_tcp else tls_paths):
    if digest(target/name) != digest(base/name): p.error('unrelated candidate change: '+name)
if a.bounded_tcp:
    import openvela_tcp_budget
    patch=[]
    for name,apply in zip(tls_paths,[openvela_tcp_budget.source,openvela_tcp_budget.header]):
        before=(base/name).read_text(); after=apply(before)
        current=(target/name).read_text()
        previous_generated = a.resume and old.get('tls_source_sha256',{}).get(name)==digest(target/name)
        if current not in (before,after) and not previous_generated:p.error('unrelated TLS candidate changes: '+name)
        (target/name).write_text(after)
        patch.extend(difflib.unified_diff(before.splitlines(True),after.splitlines(True),fromfile=name,tofile=name))
    (out/'tcp-budget.diff').write_text(''.join(patch))
    profile['change']='opt-in bounded browser TCP connector and retained export superset; old TLS API/runtime/config preserved'
    profile['tls_source_sha256']={name:digest(target/name) for name in tls_paths}
def source_hashes():
    return {str(f.relative_to(root)):digest(f) for directory in
            ['platform/openvela', 'platform/openvela_module', 'quickapp/netsurf_browser']
            for f in (root/directory).rglob('*') if f.is_file()}
started = source_hashes()
(dest/'build-in-progress.json').write_text(json.dumps({'source_sha256':started}, indent=2)+'\n')
def run(name, args, **kwargs):
    with (out/(name+'.log')).open('wb') as log:
        subprocess.run(args, stdout=log, stderr=subprocess.STDOUT, check=True, **kwargs)
prefix = str(a.toolchain.resolve()/'riscv32-esp-elf-')
run('frontend', ['python3', str(root/'tools/check_openvela.py'), '--core', str(a.core),
    '--target', str(target), '--cc', prefix+'gcc', '--webjs', '--network-trace', '--output', str(out/'frontend')])
run('module', ['python3', str(root/'tools/build_openvela_module.py'), '--target', str(target),
    '--library', str(a.library), '--frontend', str(out/'frontend'), '--toolchain', str(a.toolchain),
    '--output', str(out/'module'), '--defer-import-check'])
metadata = json.loads((out/'module/module.json').read_text())
old_module = json.loads((root/'build/openvela'/a.baseline_feature/'module/module.json').read_text())
extra = sorted(set(metadata['imports'])-set(old_module['imports']))
expected=['glass_https_bounded_tls_ops'] if a.bounded_tcp else ['poll', 'webclient_abort', 'webclient_get_poll_info']
if extra != expected:
    raise RuntimeError('unexpected new imports: '+str(extra))
if metadata['host_abi'] != old_module['host_abi']: raise RuntimeError('ABI/configuration changed')
compile_command = json.loads((out/'module/host-compile.json').read_text())
exports=sorted(set(metadata['imports'])|set(old_module['imports']))
# Retain the old API export too, so a known-good older module remains loadable.
lines=['#include <nuttx/symtab.h>']
lines += [f'extern const char ov_import_{i}[] __asm__("{s}");' for i,s in enumerate(exports)]
lines += ['const struct symtab_s ov_browser_exports[] = {']
lines += [f'{{"{s}", ov_import_{i}}},' for i,s in enumerate(exports)]
lines += ['};',f'const int ov_browser_nexports={len(exports)};']
(out/'module/browser_exports.c').write_text('\n'.join(lines)+'\n')
export_obj = target/'browser_exports.o'
run('exports', [*compile_command, '-c', str(out/'module/browser_exports.c'), '-o', str(export_obj)])
subprocess.run([prefix+'ar', 'rcs', str(target/'libbrowser-loader.a'), str(export_obj)], check=True)
members = subprocess.check_output([prefix+'ar', 't', str(target/'libbrowser-loader.a')], text=True).splitlines()
if members.count('browser_exports.o') != 1: raise RuntimeError('ambiguous loader export object')
shutil.copy2(out/'module/browser_host_abi.h', target/'apps/system/desktop/browser/browser_host_abi.h')
# Absolute source paths are encoded in NuttX archive members; recreate only
# this independent candidate's application archive/stamps, keeping objects.
for path in [target/'apps/libapps.a', target/'nuttx/staging/libapps.a',
             *list((target/'apps').rglob('*.built'))]:
    if not path.resolve().is_relative_to(target): raise RuntimeError('archive outside candidate: '+str(path))
    path.unlink(missing_ok=True)
env = dict(os.environ)
env['PATH'] = str(a.toolchain.resolve())+':'+str(base.parent/'.venv/bin')+':'+env['PATH']
run('firmware', command, cwd=target/'nuttx', env=env)
if digest(target/'nuttx/.config') != profile['config_sha256']: raise RuntimeError('build changed configuration')
run('audit', ['python3', str(root/'tools/verify_openvela_native.py'), '--target', str(target),
    '--module', str(out/'module'), '--toolchain', str(a.toolchain), '--output', str(dest/'host-audit')])
run('package', ['python3', str(root/'tools/package_quickapp.py'), '--native-module', str(out/'module'),
    '--output', str(dest/'app')])
if source_hashes() != started: raise RuntimeError('source changed during build')
profile.update(firmware_sha256=digest(target/'nuttx/nuttx.bin'), additional_exports=extra,
               retained_exports=exports,
               loader_archive_sha256=digest(target/'libbrowser-loader.a'))
profile_path.write_text(json.dumps(profile, indent=2)+'\n')
candidate = {'feature':a.name, 'status':'cross-built; hardware pending',
             'loader_sha256':profile['firmware_sha256'], 'module_sha256':metadata['sha256'],
             'baseline_loader_sha256':prior['loader_sha256'], 'additional_exports':extra,
             'network_trace':True, 'core_archive_sha256':digest(a.library), 'source_sha256':started}
(dest/'candidate.json').write_text(json.dumps(candidate, indent=2)+'\n')
for name in started:
    copy = dest/'source'/name; copy.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(root/name, copy)
(dest/'build-in-progress.json').unlink()
print('DIRECT_TIMEOUT_CANDIDATE_READY', dest)
