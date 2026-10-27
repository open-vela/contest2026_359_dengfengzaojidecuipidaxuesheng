#!/usr/bin/env python3
"""Test the pinned real DHCP daemon with isolated task/socket failures."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

from prepare_dhcpd_status import apply


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apps', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--toolchain', type=Path)
    parser.add_argument('--nuttx', type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    prepared = out / 'prepared'
    folder = prepared / 'netutils/dhcpd'
    folder.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args.apps / 'netutils/dhcpd/dhcpd.c', folder / 'dhcpd.c')
    first = apply(prepared)
    assert apply(prepared) == first
    (folder / 'dhcpd.c').write_bytes((folder / 'dhcpd.c').read_bytes() + b'\n')
    try:
        apply(prepared)
    except ValueError:
        pass
    else:
        raise AssertionError('Unknown modified DHCP server must be rejected')
    shutil.copyfile(args.apps / 'netutils/dhcpd/dhcpd.c', folder / 'dhcpd.c')
    apply(prepared)
    stubs = out / 'stubs'
    (stubs / 'nuttx').mkdir(parents=True, exist_ok=True)
    (stubs / 'netutils').mkdir(parents=True, exist_ok=True)
    (stubs / 'nuttx/config.h').write_text('''
#define CONFIG_SCHED_WAITPID 1
#define CONFIG_NETUTILS_DHCPD_PRIORITY 100
#define CONFIG_NETUTILS_DHCPD_STACKSIZE 4096
#define CONFIG_NETUTILS_DHCPD_SIGWAKEUP SIGUSR1
#define CONFIG_NETUTILS_DHCPD_IGNOREBROADCAST 1
#define CONFIG_NETUTILS_DHCPD_MAXLEASES 2
#define CONFIG_NETUTILS_DHCPD_LEASETIME 600
#define CONFIG_NETUTILS_DHCPD_MINLEASETIME 60
#define CONFIG_NETUTILS_DHCPD_MAXLEASETIME 600
#define CONFIG_NETUTILS_DHCPD_NETMASK 0xffffff00
#define CONFIG_NETUTILS_DHCPD_ROUTERIP 0
#define CONFIG_NETUTILS_DHCPD_DNSIP 0
''')
    (stubs / 'nuttx/compiler.h').write_text(
        '#define FAR\n#define HTONS(a) htons(a)\n#define HTONL(a) htonl(a)\n')
    (stubs / 'debug.h').write_text('''
#define OK 0
#define ERROR (-1)
#define nerr(...) do { if (0) fprintf(stderr, __VA_ARGS__); } while (0)
#define ninfo(...) do { if (0) fprintf(stderr, __VA_ARGS__); } while (0)
''')
    (stubs / 'netutils/netlib.h').write_text('')
    common = ['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
              '-Wno-unused-parameter', '-Wno-sign-compare',
              '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
              '-I' + str(stubs), '-I' + str(prepared / 'include'),
              '-I' + str(args.apps / 'include'), '-I' + str(folder)]
    subprocess.run([*common,
                    str(repo / 'app/espdl-quickapp/tests/dhcpd_runtime_host.c'),
                    '-pthread', '-o', str(out / 'runtime')], check=True)
    subprocess.run([str(out / 'runtime')], check=True, timeout=30)
    config=stubs/'nuttx/config.h'
    config.write_text(config.read_text().replace('DHCPD_ROUTERIP 0\n','DHCPD_ROUTERIP 0xc0a80401\n')
                      .replace('DHCPD_DNSIP 0\n','DHCPD_DNSIP 0x01010101\n'))
    subprocess.run([*common,str(repo/'app/espdl-quickapp/tests/dhcpd_runtime_host.c'),
                    '-pthread','-o',str(out/'runtime-options')],check=True)
    subprocess.run([str(out/'runtime-options')],check=True,timeout=30)
    target_compiled = False
    if args.toolchain or args.nuttx:
        assert args.toolchain and args.nuttx
        compiler = args.toolchain / 'riscv32-esp-elf-gcc'
        subprocess.run([str(compiler), '-c', '-Os', '-g', '-std=gnu11',
                        '-march=rv32imafc_zicsr_zifencei', '-mabi=ilp32f',
                        '-fstack-usage', '-Werror=implicit-function-declaration',
                        '-Wno-cpp', '-I' + str(prepared / 'include'),
                        '-I' + str(args.nuttx / 'include'),
                        '-I' + str(args.apps / 'include'),
                        '-DCONFIG_NETUTILS_DHCPD_PRIORITY=100',
                        '-DCONFIG_NETUTILS_DHCPD_STACKSIZE=4096',
                        '-DCONFIG_NETUTILS_DHCPD_SIGWAKEUP=10',
                        '-DCONFIG_NETUTILS_DHCPD_MAXLEASES=2',
                        '-DCONFIG_NETUTILS_DHCPD_LEASETIME=600',
                        '-DCONFIG_NETUTILS_DHCPD_MINLEASETIME=60',
                        '-DCONFIG_NETUTILS_DHCPD_MAXLEASETIME=600',
                        '-DCONFIG_NETUTILS_DHCPD_NETMASK=0xffffff00',
                        '-DCONFIG_NETUTILS_DHCPD_ROUTERIP=0',
                        '-DCONFIG_NETUTILS_DHCPD_DNSIP=0',
                        str(folder / 'dhcpd.c'), '-o', str(out / 'dhcpd-target.o')],
                       check=True)
        target_compiled = True
    (out / 'RESULT.json').write_text(json.dumps({
        'host_only': True, 'device_updated': False,
        'real_dhcp_daemon_fault_matrix': True,
        'bind_completed_before_ready': True,
        'startup_and_stop_races': True,
        'stop_failure_retains_pid_and_resources': True,
        'target_unit_compiled': target_compiled,
        'source_sha256': first['source_sha256'],
        'support_sha256': first['support_sha256']}, indent=2) + '\n')


if __name__ == '__main__':
    main()
