#!/usr/bin/env python3
"""Exercise real portal sockets through the supplied NuttX file cloning path."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess


def function(text, signature):
    start = text.index(signature)
    body = text.index('{', start)
    depth = 1
    end = body + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end] + '\n'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apps', type=Path, required=True)
    parser.add_argument('--nuttx', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--toolchain', type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    desktop = repo / 'app/espdl-quickapp/overlay/apps/system/desktop'
    tests = repo / 'app/espdl-quickapp/tests'
    cjson = args.apps.resolve() / 'netutils/cjson/cJSON'
    nuttx = args.nuttx.resolve()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out / 'nuttx').mkdir(exist_ok=True)
    (out / 'nuttx/config.h').write_text('#pragma once\n')
    kernel_file = nuttx / 'fs/inode/fs_files.c'
    config = (nuttx / '.config').read_text()
    assert '# CONFIG_FDCLONE_DISABLE is not set' in config
    assert '# CONFIG_FDCLONE_STDIO is not set' in config
    assert 'CONFIG_NFILE_DESCRIPTORS_PER_BLOCK=8\n' in config
    task_init = nuttx / 'sched/task/task_init.c'
    task_create = nuttx / 'sched/task/task_create.c'
    setup = nuttx / 'sched/group/group_setuptaskfiles.c'
    socket = nuttx / 'fs/socket/socket.c'
    accept = nuttx / 'fs/socket/accept.c'
    assert 'group_setuptaskfiles(tcb, actions, true)' in task_init.read_text()
    assert 'nxtask_init(tcb, name, entry, NULL, &attr, argv, envp)' in task_create.read_text()
    assert '&group->tg_fdlist, actions, cloexec)' in setup.read_text()
    assert re.search(r'if \(type & SOCK_CLOEXEC\)\s*{\s*oflags \|= O_CLOEXEC;', socket.read_text())
    assert re.search(r'if \(flags & SOCK_CLOEXEC\)\s*{\s*oflags \|= O_CLOEXEC;', accept.read_text())
    clone = function(kernel_file.read_text(), 'int fdlist_copy(')
    (out / 'target-fdlist-copy.inc').write_text(clone)
    source = desktop / 'glass_portal_http.c'
    original = source.read_text()
    files = [source, desktop / 'glass_portal.h', desktop / 'glass_network_recovery.h',
             tests / 'portal_network_host.c', tests / 'portal_fd_inheritance_host.c',
             kernel_file, task_init, task_create, setup, socket, accept,
             nuttx / '.config', cjson / 'cJSON.c', cjson / 'cJSON.h', Path(__file__).resolve()]
    initial_hashes = {str(path): digest(path) for path in files}
    assert original.count('socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0)') == 1
    assert original.count('accept4(listen_fd,NULL,NULL,SOCK_CLOEXEC)') == 1
    variants = {
        'fixed': original,
        'legacy-listener': original.replace('socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0)',
                                             'socket(AF_INET,SOCK_STREAM,0)', 1),
        'legacy-client': original.replace('accept4(listen_fd,NULL,NULL,SOCK_CLOEXEC)',
                                           'accept(listen_fd,NULL,NULL)', 1),
    }
    results = {}
    for name, text in variants.items():
        root = out / name
        copied_desktop = root / 'overlay/apps/system/desktop'
        copied_tests = root / 'tests'
        copied_desktop.mkdir(parents=True, exist_ok=True)
        copied_tests.mkdir(parents=True, exist_ok=True)
        (copied_desktop / source.name).write_text(text)
        for filename in ['portal_network_host.c', 'portal_fd_inheritance_host.c']:
            shutil.copyfile(tests / filename, copied_tests / filename)
        shutil.copyfile(desktop / 'glass_portal.h', copied_desktop / 'glass_portal.h')
        binary = root / 'portal-fd-inheritance'
        command = ['gcc', '-std=gnu11', '-D_GNU_SOURCE', '-O1', '-g', '-pthread',
                   '-Wall', '-Wextra', '-Werror', '-Wno-misleading-indentation',
                   '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                   '-fno-pie', '-no-pie', '-ffunction-sections', '-fdata-sections',
                   '-I' + str(out), '-I' + str(desktop),
                   '-I' + str(repo / 'tools/c6'), '-I' + str(cjson),
                   str(copied_tests / 'portal_fd_inheritance_host.c'),
                   str(cjson / 'cJSON.c'), '-Wl,--gc-sections', '-o', str(binary)]
        compiled = subprocess.run(command, text=True, capture_output=True)
        (root / 'compile.log').write_text(compiled.stdout + compiled.stderr)
        if compiled.returncode:
            print(compiled.stdout + compiled.stderr)
            raise SystemExit(compiled.returncode)
        run = subprocess.run([str(binary)], text=True, capture_output=True, timeout=15,
                             env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1:halt_on_error=1',
                                  'UBSAN_OPTIONS': 'halt_on_error=1'})
        (root / 'run.log').write_text(run.stdout + run.stderr)
        expected = ('PORTAL_FD_INHERITANCE_OK:' if name == 'fixed' else
                    'INHERITED_LISTENER_REPRODUCED:' if name == 'legacy-listener' else
                    'INHERITED_CLIENT_REPRODUCED:')
        assert expected in run.stdout + run.stderr, (name, run.returncode, run.stdout, run.stderr)
        assert (run.returncode == 0) == (name == 'fixed'), (name, run.returncode)
        results[name] = {'expected_outcome_observed': True, 'returncode': run.returncode,
                         'tested_portal_sha256': hashlib.sha256(text.encode()).hexdigest()}
        print(name + ': ' + expected)
    target = None
    if args.toolchain:
        compiled_source = out / 'fixed/overlay/apps/system/desktop/glass_portal_http.c'
        obj = out / 'portal-target.o'
        command = [str(args.toolchain / 'riscv32-esp-elf-gcc'), '-c', '-Os', '-g',
                   '-std=gnu11', '-march=rv32imafc_zicsr_zifencei', '-mabi=ilp32f',
                   '-fstack-usage', '-Werror=implicit-function-declaration',
                   '-I' + str(desktop), '-I' + str(repo / 'tools/c6'),
                   '-I' + str(nuttx / 'include'), '-I' + str(args.apps / 'include'),
                   '-I' + str(cjson), str(compiled_source), '-o', str(obj)]
        result = subprocess.run(command, text=True, capture_output=True)
        (out / 'target-compile.log').write_text(result.stdout + result.stderr)
        if result.returncode:
            print(result.stdout + result.stderr)
            raise SystemExit(result.returncode)
        target = {'passed': True, 'object_sha256': digest(obj),
                  'portal_sha256': digest(compiled_source)}
    final_hashes = {str(path): digest(path) for path in files}
    assert initial_hashes == final_hashes, 'Source changed during testing; rerun against the final source.'
    report = {'passed': True, 'scope': 'native TCP, actual portal worker and target fdlist_copy; no hardware',
              'target_config': {'FDCLONE_DISABLE': False, 'FDCLONE_STDIO': False,
                                'NFILE_DESCRIPTORS_PER_BLOCK': 8},
              'target_fdlist_copy_sha256': hashlib.sha256(clone.encode()).hexdigest(),
              'target_compile': target, 'variants': results, 'source_sha256': final_hashes}
    (out / 'RESULT.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')


if __name__ == '__main__':
    main()
