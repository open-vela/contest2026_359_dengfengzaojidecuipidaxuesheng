#!/usr/bin/env python3
"""Run actual SoftAP RPC and AP/STA network code against bounded host peers."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

from prepare_c6_softap import apply


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apps', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--toolchain', type=Path)
    parser.add_argument('--nuttx', type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    src = repo / 'app/espdl-quickapp/overlay/apps/system/c6probe'
    tests = repo / 'app/espdl-quickapp/tests'
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    stub = out / 'include/nuttx'
    stub.mkdir(parents=True, exist_ok=True)
    (stub / 'config.h').write_text('#define CONFIG_NET 1\n'
        '#define CONFIG_SYSTEM_C6PROBE 1\n#define CONFIG_NET_ARP 1\n')
    (stub / 'compiler.h').write_text('#define FAR\n')
    prepared = out / 'prepared/system/c6probe'
    prepared.mkdir(parents=True, exist_ok=True)
    for name in ['esp_hosted_rpc.c', 'esp_hosted_rpc.pb-c.h',
                 'esp_hosted.c', 'esp_hosted.h', 'c6_rx_burst.h']:
        shutil.copyfile(args.apps / 'system/c6probe' / name, prepared / name)
    first = apply(prepared.parents[1])
    assert first == apply(prepared.parents[1])
    text = (prepared / 'esp_hosted_rpc.c').read_text()
    assert text.count('#include "c6_softap_rpc.inc"') == 1
    assert 'req_payload.mode = c6_softap_preserve_mode(mode);' in text
    assert 'uint8_t if_type,' in (prepared / 'esp_hosted.h').read_text()
    (prepared / 'esp_hosted_rpc.c').write_text('unknown owner\n')
    try:
        apply(prepared.parents[1])
    except ValueError:
        pass
    else:
        raise AssertionError('unknown RPC template accepted')
    (prepared / 'esp_hosted_rpc.c').write_text(text)
    transport_text = (prepared / 'esp_hosted.c').read_text()
    (prepared / 'esp_hosted.c').write_text(transport_text.replace(
        '  cb = g_hosted.rx_cb[if_type];', '  cb = NULL;', 1))
    try:
        apply(prepared.parents[1])
    except ValueError:
        pass
    else:
        raise AssertionError('unknown transport dispatch template accepted')
    (prepared / 'esp_hosted.c').write_text(transport_text)
    support = args.apps / 'system/c6probe'
    flags = ['gcc', '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra',
             '-Werror', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
             '-pthread', '-I' + str(out / 'include'), '-I' + str(src),
             '-I' + str(support)]
    exe = out / 'rpc-host'
    subprocess.run(flags + [str(tests / 'c6_softap_rpc_host.c'),
        str(support / 'esp_hosted_rpc.pb-c.c'), str(support / 'protobuf-c.c'),
        '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    # Exercise the actual locked entry (including UID begin/finish), with
    # only transport I/O replaced. C9's UID-zero body is a negative control.
    owner = (prepared / 'esp_hosted_rpc.c').read_text()
    start = owner.index('static FAR Rpc *rpc_transact_locked(')
    opening = owner.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (owner[end] == '{') - (owner[end] == '}')
        end += 1
    locked = owner[start:end]
    # Test peer label is metadata only; actual body below is unchanged.
    (out / 'rpc-locked-under-test.inc').write_text(locked.replace(
        '{', '{\n  wire_label = label;', 1) + '\n')
    shutil.copyfile(repo / 'tools/c6/rpc_mailbox.h', out / 'rpc_mailbox.h')
    fixed = (src / 'c6_wifi_phy_rpc.inc').read_text()
    bad_phy = fixed.replace('  Rpc owned = *req;\n'
                            '  owned.uid = c6_rpc_next_uid(&g_rpc);\n'
                            '  return owned.uid == 0 ? NULL : rpc_transact_locked(&owned, label);',
                            '  return rpc_transact_locked(req, label);')
    assert bad_phy != fixed
    (out / 'bad-phy-rpc.inc').write_text(bad_phy)
    exe = out / 'phy-rpc-host'
    phy_compile = flags + ['-I' + str(out), str(tests / 'c6_wifi_phy_rpc_host.c'),
        str(support / 'esp_hosted_rpc.pb-c.c'), str(support / 'protobuf-c.c'),
        '-o', str(exe)]
    subprocess.run(phy_compile, check=True)
    tested = subprocess.run([str(exe)], capture_output=True, text=True)
    (out / 'phy-rpc-host.log').write_text(tested.stdout + tested.stderr)
    assert tested.returncode == 0, tested.stderr
    print(tested.stdout.splitlines()[-1])
    bad_exe = out / 'phy-rpc-host-bad-uid'
    subprocess.run(phy_compile[:-1] + [str(bad_exe), '-DHOST_BAD_PHY'], check=True)
    negative_phy = subprocess.run([str(bad_exe)], capture_output=True, text=True)
    (out / 'phy-rpc-negative.log').write_text(negative_phy.stdout + negative_phy.stderr)
    assert negative_phy.returncode != 0 and 'esp_hosted_rpc_phy_set_protocol' in negative_phy.stderr
    print('C6_WIFI_PHY_UID_NEGATIVE_OK: zero UID fails actual locked owner/mailbox before transport send')
    # Remove platform includes only; compile all production functions intact.
    text = (src / 'c6net.c').read_text()
    text = re.sub(r'^#include <nuttx/[^>]+>\n', '', text, flags=re.M)
    text = text.replace('#include "esp_hosted.h"', '')
    net_source = '#define _GNU_SOURCE\n#include <nuttx/config.h>\n#include "net-stubs.h"\n' + text
    (out / 'net-under-test.c').write_text(net_source)
    ap_source = (src / 'c6_softap_net.inc').read_text()
    assert ap_source.count('(void)netdev_ifdown(&ap->net.dev);') == 1
    (out / 'c6-softap-bad-down.inc').write_text(ap_source.replace(
        '(void)netdev_ifdown(&ap->net.dev);',
        '(void)c6net_ifdown(&ap->net.dev);'))
    (out / 'net-under-test-bad-down.c').write_text(net_source.replace(
        '#include "c6_softap_net.inc"', '#include "c6-softap-bad-down.inc"'))
    old_check = 'if (if_type != priv->if_type || len < 14'
    assert net_source.count(old_check) == 1
    (out / 'net-under-test-bad.c').write_text(net_source.replace(
        old_check, 'if (if_type != 0 || len < 14', 1))
    # Exercise the actual production transport dispatch, not an invented
    # callback invocation that quietly supplies an impossible selector zero.
    transport = (prepared / 'esp_hosted.c').read_text()
    start = transport.index('static void hosted_dispatch(')
    opening = transport.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (transport[end] == '{') - (transport[end] == '}')
        end += 1
    (out / 'transport-dispatch.inc').write_text(transport[start:end] + '\n')
    shutil.copyfile(repo / 'tools/c6/link_state.h', out / 'link_state.h')
    (out / 'net-stubs.h').write_text(r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <net/if.h>
#define SEM_PRIO_NONE 0
#define sem_setprotocol(s,p) ((void)(s), (void)(p), 0)
#define FAR
#define UNUSED(v) (void)(v)
#define ESP_HOSTED_IF_STA 1
#define ESP_HOSTED_IF_AP 2
#define ESP_HOSTED_IF_SERIAL 3
#define ESP_HOSTED_IF_HCI 4
#define ESP_HOSTED_IF_PRIV 5
#define ESP_HOSTED_IF_MAX 8
#define ESP_HOSTED_HEADER_SIZE 12
#define ESP_HOSTED_PACKET_TYPE_EVENT 0x33
#define ESP_HOSTED_PRIV_EVENT_INIT 0x22
#define syslog(...) ((void)0)
#define NET_LL_ETHERNET 1
#define ETHTYPE_IP 0x0800
#define ETHTYPE_ARP 0x0806
struct net_driver_s {
  char d_ifname[16]; unsigned d_flags; uint16_t d_len, d_pktsize, d_llhdrlen;
  uint8_t *d_buf; void *d_private;
  struct { struct { uint8_t ether_addr_octet[6]; } ether; } d_mac;
  int (*d_ifup)(struct net_driver_s *), (*d_ifdown)(struct net_driver_s *);
  int (*d_txavail)(struct net_driver_s *);
};
typedef void (*esp_hosted_rx_cb_t)(void *, uint8_t, const uint8_t *, uint16_t, uint8_t);
typedef void (*esp_hosted_wifi_event_cb_t)(void *, bool);
size_t strlcpy(char *, const char *, size_t);
void netdev_lock(struct net_driver_s *);
void netdev_unlock(struct net_driver_s *);
void netdev_carrier_on(struct net_driver_s *);
void netdev_carrier_off(struct net_driver_s *);
int netdev_register(struct net_driver_s *, int);
int netdev_ifdown(struct net_driver_s *);
int devif_poll(struct net_driver_s *, int (*)(struct net_driver_s *));
void ipv4_input(struct net_driver_s *);
void arp_input(struct net_driver_s *);
pid_t task_create(const char *, int, int, int (*)(int, char **), char **);
int esp_hosted_initialize(bool);
int esp_hosted_rpc_wifi_init(void);
int esp_hosted_rpc_wifi_set_mode(int);
int esp_hosted_rpc_wifi_start(void);
int esp_hosted_rpc_get_mac(int, uint8_t [6]);
int esp_hosted_rpc_set_wifi_event_cb(esp_hosted_wifi_event_cb_t, void *);
int esp_hosted_register(uint8_t, esp_hosted_rx_cb_t, void *);
int esp_hosted_send(uint8_t, uint8_t, const uint8_t *, uint16_t);
int esp_hosted_poll(void);
bool esp_hosted_rpc_is_waiting(void);
int esp_hosted_rpc_wifi_connect(const char *, const char *);
int esp_hosted_rpc_wifi_disconnect(void);
''')
    exe = out / 'net-host'
    subprocess.run(flags + ['-I' + str(out),
        str(tests / 'c6_softap_net_host.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    bad = out / 'net-host-bad-selector'
    subprocess.run(flags + ['-I' + str(out), '-DHOST_BAD_SELECTOR',
        str(tests / 'c6_softap_net_host.c'), '-o', str(bad)], check=True)
    negative = subprocess.run([str(bad)], capture_output=True)
    assert negative.returncode != 0 and b'inputs == 1' in negative.stderr
    bad_down = out / 'net-host-bad-down'
    subprocess.run(flags + ['-I' + str(out), '-DHOST_BAD_DOWN',
        str(tests / 'c6_softap_net_host.c'), '-o', str(bad_down)], check=True)
    negative_down = subprocess.run([str(bad_down)], capture_output=True)
    assert negative_down.returncode != 0 and b'offline_notifications == down_before + 1' in negative_down.stderr
    print('C6_SOFTAP_DOWN_NEGATIVE_OK: direct driver ifdown strands pending callback owners')
    print('C6_SOFTAP_TRANSPORT_NEGATIVE_OK: original zero-selector check rejects the actual AP dispatch frame')
    target_compiled = False
    if args.toolchain or args.nuttx:
        assert args.toolchain and args.nuttx
        subprocess.run([str(args.toolchain / 'riscv32-esp-elf-gcc'),
            '-c', '-Os', '-g', '-std=gnu11',
            '-march=rv32imafc_zicsr_zifencei', '-mabi=ilp32f',
            '-fstack-usage', '-Werror=implicit-function-declaration',
            '-I' + str(src), '-I' + str(support),
            '-I' + str(args.nuttx / 'include'),
            '-I' + str(args.apps / 'include'), str(src / 'c6net.c'),
            '-o', str(out / 'c6net-target.o')], check=True)
        subprocess.run([str(args.toolchain / 'riscv32-esp-elf-gcc'),
            '-c', '-Os', '-g', '-std=gnu11',
            '-march=rv32imafc_zicsr_zifencei', '-mabi=ilp32f',
            '-Werror=implicit-function-declaration',
            '-I' + str(prepared), '-I' + str(args.nuttx / 'include'),
            '-I' + str(args.nuttx / 'arch/risc-v/src/common/espressif'),
            str(prepared / 'esp_hosted.c'),
            '-o', str(out / 'transport-target.o')], check=True)
        target_compiled = True
    (out / 'RESULT.json').write_text(json.dumps({
        'status': 'host-only-no-hardware',
        'real_protobuf_and_complete_network_functions': True,
        'phy_actual_locked_owner_and_mailbox_with_zero_uid_negative_control': True,
        'remote_ap_reconciled_without_reset_start_or_connect': True,
        'startup_mode_readback_required': True,
        'actual_transport_dispatch_to_sta_ap_and_legacy_consumers': True,
        'original_zero_selector_check_rejected_by_integrated_test': True,
        'target_network_unit_compiled': target_compiled,
        'target_transport_unit_compiled': target_compiled,
        'prepared_transport_sha256': {
            name: hashlib.sha256((prepared / name).read_bytes()).hexdigest()
            for name in ['esp_hosted.c', 'esp_hosted.h']},
        'production_source_sha256': {
            name: hashlib.sha256((src / name).read_bytes()).hexdigest()
            for name in ['c6net.c', 'c6net.h', 'c6_softap.h',
                         'c6_softap_net.inc', 'c6_softap_rpc.inc',
                         'c6_wifi_phy.h', 'c6_wifi_phy_net.inc',
                         'c6_wifi_phy_rpc.inc']},
    }, indent=2) + '\n')


if __name__ == '__main__':
    main()
