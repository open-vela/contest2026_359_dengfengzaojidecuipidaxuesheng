#!/usr/bin/env python3
"""Build a module-only feature candidate against the frozen verified loader."""
import argparse,hashlib,json,re,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--name',required=True)
p.add_argument('--core',type=Path,required=True)
p.add_argument('--target',type=Path,required=True)
p.add_argument('--library',type=Path,required=True)
p.add_argument('--toolchain',type=Path,required=True)
p.add_argument('--input-loader-profile',type=Path,help='explicit derived loader with opt-in input cancellation')
p.add_argument('--network-loader-profile',type=Path,help='board-verified direct-connect loader; no firmware mutation')
p.add_argument('--verified-loader-profile',type=Path,help='already deployed firmware retaining the verified loader contract')
p.add_argument('--network-trace',action='store_true')
p.add_argument('--platform-source',type=Path,help='isolated, complete platform source snapshot')
a=p.parse_args();root=Path(__file__).resolve().parents[1]
platform=a.platform_source.resolve() if a.platform_source else root/'platform/openvela'
if not re.fullmatch(r'[a-z][a-z0-9]*(?:-[a-z0-9]+)*',a.name):p.error('invalid workarea name')
build=root/'build/openvela'/a.name;dist=root/'dist/candidates'/a.name
if not (build/'workarea.json').is_file():p.error('prepare the feature workarea first')
release=root/'dist/releases/esp32p4-0.5.0'
baseline=json.loads((release/'release-manifest.json').read_text())
prior=json.loads((release/'live-native-app/com.netsurf.browser/native/module.json').read_text())
host=a.target/'nuttx/nuttx.bin'
loader_hash=hashlib.sha256(host.read_bytes()).hexdigest()
if sum(bool(x) for x in [a.input_loader_profile,a.network_loader_profile,a.verified_loader_profile])>1:
    p.error('select only one loader profile')
if a.verified_loader_profile:
    from openvela_loader_profile import validate
    profile=validate(a.verified_loader_profile,a.target)
    prior={'imports':profile['exports'],'host_abi':profile['host_abi']}
elif a.network_loader_profile:
    profile=json.loads(a.network_loader_profile.read_text())
    base=Path(profile['baseline']);target=Path(profile['target'])
    digest=lambda path:hashlib.sha256(path.read_bytes()).hexdigest()
    verified=json.loads((root/'dist/candidates/direct-connect-retry/board-verification.json').read_text())
    if (target.resolve()!=a.target.resolve() or loader_hash!=profile['firmware_sha256'] or
        loader_hash!=verified['firmware_sha256'] or not verified['module_executed_on_p4'] or
        digest(target/'nuttx/.config')!=profile['config_sha256'] or
        digest(target/'libbrowser-loader.a')!=profile['loader_archive_sha256'] or
        digest(base/'nuttx/nuttx.bin')!=profile['baseline_sha256'] or
        (target/'apps/system/desktop/qpk_runtime.c').read_bytes()!=(base/'apps/system/desktop/qpk_runtime.c').read_bytes() or
        any(digest(target/name)!=want for name,want in profile['tls_source_sha256'].items())):
        p.error('network loader does not match its board-verified profile')
    prior=json.loads((root/'build/openvela/direct-connect-retry/module/module.json').read_text())
    prior['imports']=profile['retained_exports']
elif a.input_loader_profile:
    from openvela_prompt_cancel import apply
    profile=json.loads(a.input_loader_profile.read_text())
    base=Path(profile['baseline']);target=Path(profile['target'])
    digest=lambda path:hashlib.sha256(path.read_bytes()).hexdigest()
    runtime=Path('apps/system/desktop/qpk_runtime.c')
    if (not profile.get('linked_prompt_capability_verified') or
        target.resolve()!=a.target.resolve() or profile.get('firmware_sha256')!=loader_hash or
        profile['baseline_sha256']!=baseline['firmware_sha256'] or
        digest(base/'nuttx/nuttx.bin')!=baseline['firmware_sha256'] or
        (target/runtime).read_text()!=apply((base/runtime).read_text()) or
        digest(target/'nuttx/.config')!=digest(base/'nuttx/.config') or
        digest(target/'libbrowser-loader.a')!=digest(base/'libbrowser-loader.a')):
        p.error('derived input loader does not match its explicit baseline and patch')
elif loader_hash!=baseline['firmware_sha256']:
    p.error('target is not the frozen verified loader; use a separate firmware integration task')
dist.mkdir(parents=True,exist_ok=True)
def source_hashes():
    files=[f for directory in ['platform/openvela_module','quickapp/netsurf_browser','patches']
           for f in (root/directory).rglob('*') if f.is_file()]
    files += list((root/'tools').glob('*openvela*.py'))
    files += [root/'tools/package_quickapp.py',root/'tools/setup_deps.py']
    hashes={str(f.relative_to(root)).replace('\\','/'):hashlib.sha256(f.read_bytes()).hexdigest() for f in files}
    hashes.update({'platform/openvela/'+str(f.relative_to(platform)):hashlib.sha256(f.read_bytes()).hexdigest()
                   for f in platform.rglob('*') if f.is_file()})
    return hashes
started=source_hashes()
library_hash=hashlib.sha256(a.library.read_bytes()).hexdigest()
(dist/'candidate.json').unlink(missing_ok=True)
(dist/'build-in-progress.json').write_text(json.dumps({'source_sha256':started},indent=2)+'\n')
def run(name,command):
    with (build/(name+'.log')).open('wb') as log:
        subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True)
cc=str(a.toolchain/'riscv32-esp-elf-gcc')
run('frontend',['python3',str(root/'tools/check_openvela.py'),'--core',str(a.core),
    '--target',str(a.target),'--cc',cc,'--webjs','--output',str(build/'frontend')]+
    (['--network-trace'] if a.network_trace else [])+['--platform-source',str(platform)])
run('module',['python3',str(root/'tools/build_openvela_module.py'),'--target',str(a.target),
    '--library',str(a.library),'--frontend',str(build/'frontend'),'--toolchain',str(a.toolchain),
    '--output',str(build/'module')])
current=json.loads((build/'module/module.json').read_text())
extra=sorted(set(current['imports'])-set(prior['imports']))
if extra:raise RuntimeError('module needs new host exports; create a firmware workarea: '+str(extra))
if current['host_abi']!=prior['host_abi']:raise RuntimeError('frozen loader ABI changed')
run('audit',['python3',str(root/'tools/verify_openvela_native.py'),'--target',str(a.target),
    '--module',str(build/'module'),'--toolchain',str(a.toolchain),'--output',str(dist/'host-audit')])
run('package',['python3',str(root/'tools/package_quickapp.py'),'--native-module',str(build/'module'),
    '--output',str(dist/'app')])
files=source_hashes()
if files!=started:raise RuntimeError('source changed during build; rebuild before packaging/installing')
if hashlib.sha256(a.library.read_bytes()).hexdigest()!=library_hash:
    raise RuntimeError('core archive changed during build; rebuild before installing')
if a.verified_loader_profile:validate(a.verified_loader_profile,a.target)
(dist/'candidate.json').write_text(json.dumps({'feature':a.name,'status':'cross-built; hardware pending',
    'loader_sha256':loader_hash,'module_sha256':current['sha256'],
    'input_cancel_loader':bool(a.input_loader_profile),
    'network_loader_profile':str(a.network_loader_profile) if a.network_loader_profile else None,
    'verified_loader_profile':str(a.verified_loader_profile) if a.verified_loader_profile else None,
    'network_trace':a.network_trace,'platform_source':str(platform),
    'imports_subset_of_verified_loader':True,'core_archive_sha256':hashlib.sha256(a.library.read_bytes()).hexdigest(),
    'source_sha256':files},indent=2)+'\n')
(dist/'build-in-progress.json').unlink()
print('FEATURE_CANDIDATE_READY',dist)
