#!/usr/bin/env python3
"""Test the native module boundary with a real host shared object."""
import argparse
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--target', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
out = a.output.resolve(); out.mkdir(parents=True, exist_ok=True)
sd = out/'sd/qpk'
app = sd/'com.netsurf.browser/native'; app.mkdir(parents=True, exist_ok=True)
(app/'host.abi').unlink(missing_ok=True)
(out/'browser_host_abi.h').write_text('#define OV_BROWSER_HOST_ABI "'+'a'*64+'"\n')
flags = ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
    '-Wno-cast-function-type', '-I', str(root/'platform/openvela_module'), '-I', str(out),
    '-I', str(a.target/'apps/graphics/lvgl'),
    '-I', str(a.target/'apps/interpreters/quickjs/quickjs'),
    '-DLV_CONF_PATH='+str(root/'tests/openvela/lv_conf.h'),
    '-DOV_BROWSER_SD_ROOT="'+str(sd)+'"']
subprocess.run(['gcc', *flags, '-fPIC', '-shared', str(root/'tests/openvela/module_fixture.c'),
    '-o', str(app/'browser.elf')], check=True)
subprocess.run(['gcc', *flags, str(root/'platform/openvela_module/browser_native_host.c'),
    str(root/'tests/openvela/module_host.c'), '-ldl', '-o', str(out/'module_host')], check=True)
subprocess.run([str(out/'module_host'), str(app.parent/'app.js'), str(app/'host.abi')], check=True, timeout=20)
subprocess.run(['gcc', *flags, str(root/'platform/openvela_module/browser_native_host.c'),
    str(root/'platform/openvela_module/browser_async.c'), str(root/'tests/openvela/module_async.c'),
    '-pthread', '-ldl', '-o', str(out/'module_async')], check=True)
subprocess.run([str(out/'module_async'), str(app.parent/'app.js')], check=True, timeout=20)
subprocess.run(['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-I',str(root/'platform/openvela_module'),
    str(root/'platform/openvela_module/browser_cache.c'),str(root/'tests/openvela/module_cache.c'),
    '-o',str(out/'module_cache')],check=True)
subprocess.run([str(out/'module_cache')],check=True,timeout=10)
