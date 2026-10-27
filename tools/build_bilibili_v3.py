#!/usr/bin/env python3
"""Build current v3 overlay + live app using an explicit prepared baseline."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import re

def storage_layout(config,requested='preserve'):
    layouts={'12m-4m':(0xc00000,0x400000),'14m-2m':(0xe00000,0x200000)}
    if requested!='preserve':
        if requested not in layouts:raise ValueError('Unknown storage layout')
        return layouts[requested]
    values=[]
    for key in ['OFFSET','SIZE']:
        match=re.search(r'^CONFIG_ESPRESSIF_STORAGE_MTD_'+key+r'=(\S+)$',config,re.M)
        if not match:raise ValueError('Missing prepared storage '+key.lower())
        values.append(int(match.group(1),0))
    if tuple(values) not in layouts.values():raise ValueError('Unsupported prepared storage layout; inspect the board before changing it')
    return tuple(values)

def forwarding_config(config, slots=4, statistics=False):
    """Bound forwarding and retain IOB/callback capacity for local traffic."""
    if slots not in (4,8,16):
        raise ValueError('Forwarding slots must be 4, 8 or 16')
    match=re.search(r'^CONFIG_IOB_NBUFFERS=(\d+)$',config,re.M)
    if not match or slots*4+32>int(match.group(1)):
        raise ValueError('Forwarding queue must leave at least 32 IOBs reserved')
    callbacks=re.search(r'^CONFIG_NET_PREALLOC_DEVIF_CALLBACKS=(\d+)$',config,re.M)
    if not callbacks:
        raise ValueError('Missing prepared network callback pool size')
    callback_count=max(int(callbacks.group(1)),slots+16)
    for name,value in [('NET_FORWARD_NSTRUCT',str(slots)),
                       ('NET_FORWARD_ALLOC_STRUCT','0'),
                       ('NET_PREALLOC_DEVIF_CALLBACKS',str(callback_count)),
                       ('NET_STATISTICS','y' if statistics else 'n')]:
        key='CONFIG_'+name
        line=key+'='+value if value!='n' else '# '+key+' is not set'
        pattern=r'^(?:'+key+r'=.*|# '+key+r' is not set)$'
        config=re.sub(pattern,line,config,flags=re.M) if re.search(pattern,config,re.M) else config+line+'\n'
    return config

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline',type=Path,required=True)
    p.add_argument('--workspace',type=Path,required=True)
    p.add_argument('--toolchain',type=Path,required=True)
    p.add_argument('--archive',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--jobs',type=int,default=4)
    p.add_argument('--storage-layout',choices=['preserve','12m-4m','14m-2m'],default='preserve',help='Preserve the prepared data partition by default; explicit layouts do not migrate existing board data')
    p.add_argument('--l2-kb',type=int,choices=[256,512],default=256,help='P4 L2 cache experiment; 512 KiB uses 128-byte lines')
    p.add_argument('--speech-runtime',type=Path,help='Prepared, verified speech runtime directory (SD models, no flash fallback)')
    p.add_argument('--esp-tts-prepared',type=Path,help='Pinned ESP-TTS libraries/voice; use as the normal Chinese speech backend')
    p.add_argument('--tts-backend',choices=['esp-tts','sanotts'],default='esp-tts',help='Chinese speech backend when --speech-runtime is enabled (default: official esp-tts)')
    p.add_argument('--offline-inputs',action='store_true',help='Do not download missing official ESP-TTS inputs')
    p.add_argument('--esp-tts-storage',choices=['sd','flash'],default='sd',help='Official voice storage (default: verified SD asset, preserving the 12 MB firmware / 4 MB data layout)')
    p.add_argument('--source-revision',help='Explicit 40-hex source revision for Windows-mounted Git worktrees')
    p.add_argument('--usb-camera',action='store_true',help='Include the dedicated P4 HS/UTMI UVC MJPEG host alongside the existing CSI camera')
    p.add_argument('--psram-xip',action='store_true',help='Opt-in experimental P4 Simple Boot text/rodata PSRAM XIP; requires dedicated trial workspace and physical validation')
    p.add_argument('--hardware-jpeg',action='store_true',help='Opt-in native P4 JPEG decoder with shared PPA/DMA2D ownership; requires image and display validation')
    p.add_argument('--ethernet-share',action='store_true',help='Opt-in eth0 to SoftAP NAT44 with resident wired DHCP ownership')
    p.add_argument('--share-forward-slots',type=int,choices=[4,8,16],default=4,help='Bounded static forwarding pool; requires --ethernet-share for non-default values')
    p.add_argument('--share-net-stats',action='store_true',help='Enable native network statistics for an Ethernet-sharing diagnostic candidate')
    p.add_argument('--ble-connections',type=int,choices=[1,2,3,8],help='NimBLE host pool capacity; 8 requires --ble-eight-test and does not change C6 firmware')
    p.add_argument('--ble-eight-test',action='store_true',help='Isolated offline experiment: 8 ordinary sessions and 8 host slots; current C6 controller is only configured for 3, do not treat this as hardware qualification')
    p.add_argument('--flash-auto-suspend-jedec',choices=['c84018'],help='Rejected experiment; retained only to reject old deployment commands explicitly')
    p.add_argument('--dsi-flash-reload',action='store_true',help='Rejected physical experiment; retained to reject old deployment commands')
    p.add_argument('--factory-header',action='store_true',help='Use factory P4X J1 pins, reserving GPIO0/1 for XTAL32K: I2C1=2/3, SPI2=6/3/4/5, PWM0=2')
    a=p.parse_args();repo=Path(__file__).resolve().parent.parent
    if a.ble_connections==8 and not a.ble_eight_test:
        p.error('Eight host slots require explicit --ble-eight-test; current C6 firmware is not qualified for eight links')
    if a.ble_eight_test:
        if a.ble_connections not in (None,8):p.error('--ble-eight-test requires eight host slots')
        a.ble_connections=8
    if a.flash_auto_suspend_jedec:
        p.error('P4 Flash auto-suspend candidate failed physical Bot/PSRAM testing; deployment is disabled')
    if a.dsi_flash_reload:
        p.error('P4 DSI reload candidate still flickered during USB/Flash testing; deployment is disabled')
    if not a.ethernet_share and (a.share_forward_slots!=4 or a.share_net_stats):
        p.error('Forwarding diagnostics require --ethernet-share')
    if a.esp_tts_prepared and not a.speech_runtime:p.error('--esp-tts-prepared requires --speech-runtime')
    if a.esp_tts_prepared and a.tts_backend!='esp-tts':p.error('--esp-tts-prepared conflicts with --tts-backend sanotts')
    base=a.baseline.resolve();root=a.workspace.resolve();out=a.output.resolve()
    if root==base or base in root.parents: p.error('Use an isolated workspace outside baseline')
    if not (base/'tls-build-espdl/library/libmbedtls.a').exists():p.error('Prepared v3 TLS libraries required')
    if a.speech_runtime and a.tts_backend=='esp-tts':
        from esp_tts_inputs import verify_prepared
        if not a.esp_tts_prepared:
            from prepare_esp_tts import prepare
            a.esp_tts_prepared=out/'esp-tts-inputs'
            prepare(a.esp_tts_prepared,offline=a.offline_inputs)
        verify_prepared(a.esp_tts_prepared)
    env=os.environ.copy();env['PATH']=str(a.toolchain.resolve())+':'+str(Path.home()/'.local/bin')+':'+env['PATH']
    for program in ['make','python3','kconfig-conf','riscv32-esp-elf-gcc','riscv32-esp-elf-g++','riscv32-esp-elf-ar','riscv32-esp-elf-nm','riscv32-esp-elf-objcopy']:
        if not shutil.which(program,path=env['PATH']):p.error('Required build tool not on PATH: '+program+'; load the prepared workspace environment first')
    def run(argv,**kw):subprocess.run(list(map(str,argv)),env=env,check=True,**kw)
    first=not root.exists()
    if first:run(['cp','-a','--reflink=auto',base,root])
    marker=root/'bilibili-baseline.txt'
    if not first and (not marker.exists() or marker.read_text().strip()!=str(base)):p.error('Workspace belongs to another baseline')
    marker.write_text(str(base)+'\n')
    # Generated Kconfig files embed absolute source paths. Relocate all of
    # them before olddefconfig, otherwise options resolve in the old tree.
    for folder in ['apps','nuttx']:
        for f in (root/folder).rglob('Kconfig*'):
            if f.is_file() and not f.is_symlink():
                data=f.read_bytes();updated=data.replace(str(base).encode(),str(root).encode())
                if updated!=data:f.write_bytes(updated)
    # Apply only the requested current overlay, preserving the external tree.
    overlay=repo/'app/espdl-quickapp/overlay'
    radio_sources=[repo/'tools/c6'/name for name in
                   ['desktop_worker.h','desktop_worker.c','desktop_backend.h','desktop_backend.c']]
    def source_inputs():
        files=[f for folder in [repo/'app/bilibili_live',overlay]
               for f in folder.rglob('*') if f.is_file()]+radio_sources
        return {str(f.relative_to(repo)):hashlib.sha256(f.read_bytes()).hexdigest()
                for f in files}
    copied_source_hashes=source_inputs()
    for src in overlay.rglob('*'):
        if not src.is_file():continue
        # The shipped overlay .config predates hardware/API preparation.
        # Start from the explicitly prepared baseline's resolved configuration.
        if src.relative_to(overlay)==Path('nuttx/.config'):continue
        target=root/src.relative_to(overlay);target.parent.mkdir(parents=True,exist_ok=True)
        data=src.read_bytes()
        if src.name.startswith('Kconfig'):data=data.replace(b'\r\n',b'\n')
        if not target.exists() or target.read_bytes()!=data:target.write_bytes(data)
    for src in radio_sources:
        target=root/'apps/system/c6probe'/src.name
        if not target.exists() or target.read_bytes()!=src.read_bytes():
            target.write_bytes(src.read_bytes())
    central_capacity=root/'apps/wireless/bluetooth/nimble/glass_ble_capacity_config.h'
    if a.ble_eight_test:
        central_capacity.write_text('/* Generated offline eight-peer experiment. C6 unchanged. */\n'
                                    '#pragma once\n#define GLASS_BLE_CENTRAL_LIMIT 8\n')
    from adapt_c6_status import apply as apply_c6_status
    apply_c6_status(root/'apps')
    from prepare_c6_ota import apply as prepare_c6_ota
    c6_ota_sources=prepare_c6_ota(root/'apps')
    from prepare_c6_softap import apply as prepare_c6_softap
    c6_softap_sources=prepare_c6_softap(root/'apps')
    from prepare_c6_wifi_trial import apply as prepare_c6_wifi_trial
    c6_wifi_trial_sources=prepare_c6_wifi_trial(root/'apps')
    from prepare_dhcpd_status import apply as prepare_dhcpd_status
    dhcpd_sources=prepare_dhcpd_status(root/'apps')
    network_share_sources={}
    if a.ethernet_share:
        from prepare_network_share import apply as prepare_network_share
        network_share_sources=prepare_network_share(root/'nuttx')
    from prepare_nimble_store import apply as prepare_nimble_store
    ble_store_sources=prepare_nimble_store(root/'apps')
    from ble_candidate_identity import create as create_ble_identity
    ble_identity=create_ble_identity(root/'apps',out)
    from instrument_p4_timing import apply as apply_camera_timing
    camera_timing_sources=apply_camera_timing(root/'nuttx')
    from protect_p4_display_flash import restore as restore_display_flash
    display_flash_sources={'enabled':False,'rejected_cache_off_irq_removed':
                          restore_display_flash(root/'nuttx')}
    from prepare_p4_flash_suspend import apply as prepare_flash_suspend
    flash_suspend_sources=prepare_flash_suspend(root/'nuttx')
    from prepare_p4_scanout_reload import apply as prepare_scanout_reload
    scanout_reload_sources=prepare_scanout_reload(root/'nuttx')
    psram_xip_sources={}
    if a.psram_xip:
        from prepare_p4_psram_xip import apply as prepare_psram_xip
        psram_xip_sources=prepare_psram_xip(root/'nuttx')
    speech_libraries=[]
    desktop_make=root/'apps/system/desktop/Makefile'
    make=desktop_make.read_text()
    make=make.split('# BEGIN OPTIONAL LOCAL SPEECH')[0].rstrip()+'\n'
    if a.speech_runtime:
        prepared=a.speech_runtime.resolve()
        make+='\n# BEGIN OPTIONAL LOCAL SPEECH\nCFLAGS += -DGLASS_LOCAL_SPEECH -DQPK_DL_SD_ONLY\n'
    if desktop_make.read_text()!=make:desktop_make.write_text(make)

    # No private credential header is imported into the candidate.
    for name in ['music_credentials.h','music_api_key.h']:
        (root/'apps/system/desktop'/name).unlink(missing_ok=True)
    run(['python3',repo/'tools/install_bilibili_live.py','--apps',root/'apps','--archive',a.archive])
    run(['../tools/mkkconfig.sh','-m','System Libraries and NSH Add-Ons','-o','Kconfig'],cwd=root/'apps/system')
    import re
    cfg=root/'nuttx/.config';s=cfg.read_text() if (root/'bilibili-clean.txt').exists() else (base/'nuttx/.config').read_text()
    try:data_offset,data_size=storage_layout(s,a.storage_layout)
    except ValueError as error:p.error(str(error))
    # Live FLV needs more than the baseline's 3 KiB socket window. Keep the
    # pool bounded (48 KiB payload) and leave internal RAM for DMA/threads.
    for name,value in [('ESP32P4_FLASH_AUTO_SUSPEND','y' if a.flash_auto_suspend_jedec else 'n'),('ESPRESSIF_STORAGE_MTD_OFFSET',hex(data_offset)),('ESPRESSIF_STORAGE_MTD_SIZE',hex(data_size)),('LIBM','y'),('LIBM_TOOLCHAIN','n'),('LIBM_NONE','n'),('NET_BROADCAST','y'),('SYSTEM_BILIBILI_LIVE','y'),('PTHREAD_MUTEX_TYPES','y'),('NSH_DISABLE_DATE','n'),
                       ('ESP32P4_PSRAM_XIP','y' if a.psram_xip else 'n'),
                       ('ESP32P4_JPEG','y' if a.hardware_jpeg else 'n'),
                       ('NET_IPFORWARD','y' if a.ethernet_share else 'n'),
                       ('NET_NAT','y' if a.ethernet_share else 'n'),
                       ('NET_NAT44','y' if a.ethernet_share else 'n'),
                       ('ESP32P4_DSI_FLASH_RELOAD','y' if a.dsi_flash_reload else 'n'),
                       ('NET_RECV_BUFSIZE','32768'),('NET_MAX_RECV_BUFSIZE','32768'),('NSH_NETINIT','n'),('LV_USE_SNAPSHOT','y'),
                       # A cancelled ASR must not sit behind 30s x 3 DNS retries.
                       ('NETDB_DNSCLIENT_RECV_TIMEOUT','3'),('NETDB_DNSCLIENT_SEND_TIMEOUT','3'),('NETDB_DNSCLIENT_RETRIES','2'),
                       ('IOB_NBUFFERS','96'),('IOB_BUFSIZE','512'),('RR_INTERVAL','20'),('ESP32P4_PPA','y'),
                       # Lease reservations outlive association, including
                       # failed handshakes and private MAC reconnects.
                       ('NETUTILS_DHCPD','y'),('NETUTILS_DHCPD_MAXLEASES','8'),
                       ('NETUTILS_DHCPD_OFFERTIME','30'),
                       ('NETUTILS_DHCPD_STACKSIZE','4096'),
                       ('NETUTILS_DHCPD_LEASETIME','600'),('NETUTILS_DHCPD_MINLEASETIME','60'),
                       ('NETUTILS_DHCPD_MAXLEASETIME','600'),('NETUTILS_DHCPD_ROUTERIP','0x0'),
                       ('NETUTILS_DHCPD_DNSIP','0x0'),('NETUTILS_DHCPD_NETMASK','0xffffff00'),
                       # Manifest model filenames and audio clips exceed 32 bytes.
                       ('NAME_MAX','64'),('FAT_MAXFNAME','64'),
                       ('ESPRESSIF_CACHE_L2_CACHE_128KB','n'),
                       ('ESPRESSIF_CACHE_L2_CACHE_256KB','y' if a.l2_kb==256 else 'n'),
                       ('ESPRESSIF_CACHE_L2_CACHE_512KB','y' if a.l2_kb==512 else 'n'),
                       ('ESPRESSIF_CACHE_L2_CACHE_LINE_64B','y' if a.l2_kb==256 else 'n'),
                       ('ESPRESSIF_CACHE_L2_CACHE_LINE_128B','y' if a.l2_kb==512 else 'n')]:
        key='CONFIG_'+name;line=key+'='+value if value!='n' else '# '+key+' is not set'
        pattern=r'^(?:'+key+r'=.*|# '+key+r' is not set)$'
        if re.search(pattern,s,re.M):s=re.sub(pattern,line,s,flags=re.M)
        else:s+=line+'\n'
    if a.ethernet_share:
        for name,value in [('NET_IPFORWARD','y'),('NET_NAT','y'),('NET_NAT44','y'),
                           ('NET_NAT66','n'),('NET_NAT44_FULL_CONE','y'),
                           ('NET_IPFORWARD_BROADCAST','n'),('NET_NAT_HASH_BITS','8'),
                           ('NET_NAT_TCP_EXPIRE_SEC','300'),('NET_NAT_UDP_EXPIRE_SEC','60'),
                           ('NET_NAT_ENTRY_RECLAIM_SEC','30'),
                           ('NETUTILS_DHCPD_ROUTERIP','0xc0a80401'),
                           ('NETUTILS_DHCPD_DNSIP','0x01010101'),
                           ('NETUTILS_DHCPD_LEASETIME','120'),('NETUTILS_DHCPD_MAXLEASETIME','120')]:
            key='CONFIG_'+name;line=key+'='+value if value!='n' else '# '+key+' is not set'
            pattern=r'^(?:'+key+r'=.*|# '+key+r' is not set)$'
            s=re.sub(pattern,line,s,flags=re.M) if re.search(pattern,s,re.M) else s+line+'\n'
        try:s=forwarding_config(s,a.share_forward_slots,a.share_net_stats)
        except ValueError as error:p.error(str(error))
    if cfg.read_text()!=s:cfg.write_text(s)
    if a.ble_connections is not None:
        key='CONFIG_NIMBLE_BLE_MAX_CONN'
        s,n=re.subn(r'^'+key+r'=.*$',key+'='+str(a.ble_connections),s,flags=re.M)
        if n!=1:p.error('Prepared NimBLE connection capacity missing or duplicated')
        cfg.write_text(s)
    if a.factory_header:
        for key,value in [('ESPRESSIF_I2C1_SDAPIN',2),('ESPRESSIF_I2C1_SCLPIN',3),('ESPRESSIF_SPI2_CSPIN',6),('ESPRESSIF_LEDC_CHANNEL0_PIN',2)]:
            name='CONFIG_'+key;pattern=r'^'+name+r'=.*$'
            if re.search(pattern,s,re.M):s=re.sub(pattern,name+'='+str(value),s,flags=re.M)
            else:s+='\n'+name+'='+str(value)+'\n'
        if cfg.read_text()!=s:cfg.write_text(s)
    if a.usb_camera:
        s=re.sub(r'^(?:CONFIG_ESP32P4_CAMERA_USB=.*|# CONFIG_ESP32P4_CAMERA_USB is not set)$','CONFIG_ESP32P4_CAMERA_USB=y',s,flags=re.M)
        if 'CONFIG_ESP32P4_CAMERA_USB=y' not in s:s+='\nCONFIG_ESP32P4_CAMERA_USB=y\n'
        if cfg.read_text()!=s:cfg.write_text(s)
    tc=a.toolchain.resolve();cross=f'CROSSDEV={tc}/riscv32-esp-elf-'
    tls=base/'standalone-tls'
    includes=f'-I{tls}/include -I{tls}/tf-psa-crypto/include -I{tls}/tf-psa-crypto/drivers/builtin/include -DMBEDTLS_THREADING_C -DMBEDTLS_THREADING_PTHREAD'
    abi=['-march=rv32imafc_zicsr_zifencei','-mabi=ilp32f']
    libs=[base/'espdl-build/libqpk_espdl.a',base/'espdl-build/libfbs_model_nuttx.a',
          base/'tls-build-espdl/library/libmbedtls.a',base/'tls-build-espdl/library/libmbedx509.a',
          base/'tls-build-espdl/tf-psa-crypto/core/libtfpsacrypto.a']
    for name in ['libstdc++.a','libsupc++.a','libgcc.a']:
        libs.append(Path(subprocess.check_output([str(tc/'riscv32-esp-elf-g++'),*abi,'-print-file-name='+name],text=True).strip()))
    run(['make','olddefconfig',cross],cwd=root/'nuttx')
    config=cfg.read_text()
    if a.ble_connections is not None and f'CONFIG_NIMBLE_BLE_MAX_CONN={a.ble_connections}\n' not in config:
        raise RuntimeError('Requested NimBLE connection capacity disabled by Kconfig')
    if a.factory_header:
        for value in ['CONFIG_ESPRESSIF_I2C1_SDAPIN=2','CONFIG_ESPRESSIF_I2C1_SCLPIN=3','CONFIG_ESPRESSIF_SPI2_CSPIN=6','CONFIG_ESPRESSIF_LEDC_CHANNEL0_PIN=2']:
            if value not in config:raise RuntimeError('Factory J1 pin configuration missing: '+value)
    if a.usb_camera and 'CONFIG_ESP32P4_CAMERA_USB=y' not in config:raise RuntimeError('USB camera disabled by Kconfig')
    for option in ['NETUTILS_DHCPD','NET_BINDTODEVICE','SCHED_WAITPID']:
        if 'CONFIG_'+option+'=y\n' not in config:raise RuntimeError('SoftAP recovery dependency not enabled: '+option)
    if a.ethernet_share:
        for option in ['NET_IPFORWARD','NET_NAT','NET_NAT44','NET_NAT44_FULL_CONE']:
            if 'CONFIG_'+option+'=y\n' not in config:raise RuntimeError('Sharing dependency disabled: '+option)
        for option in ['NET_IPFORWARD_BROADCAST','NET_NAT66','NSH_NETINIT']:
            if 'CONFIG_'+option+'=y\n' in config:raise RuntimeError('Unexpected sharing configuration: '+option)
    if a.psram_xip:
        for option in ['ESP32P4_PSRAM_XIP','SPIRAM_FETCH_INSTRUCTIONS','SPIRAM_RODATA','SPIRAM_XIP_FROM_PSRAM','SPIRAM_FLASH_LOAD_TO_PSRAM']:
            if 'CONFIG_'+option+'=y' not in config:raise RuntimeError('PSRAM XIP dependency not enabled: '+option)
    if a.flash_auto_suspend_jedec and 'CONFIG_ESP32P4_FLASH_AUTO_SUSPEND=y' not in config:
        raise RuntimeError('P4 auto-suspend dependency/revision check failed')
    for value in ['CONFIG_SYSTEM_C6_DESKTOP=y','CONFIG_SYSTEM_BILIBILI_LIVE=y','CONFIG_ARCH_FPU=y',
                  'CONFIG_ESPRESSIF_MIPI_DSI=y',
                  'CONFIG_NAME_MAX=64','CONFIG_FAT_MAXFNAME=64',
                  f'CONFIG_ESPRESSIF_STORAGE_MTD_OFFSET={hex(data_offset)}',f'CONFIG_ESPRESSIF_STORAGE_MTD_SIZE={hex(data_size)}',
                  f'CONFIG_ESPRESSIF_CACHE_L2_CACHE_SIZE={hex(a.l2_kb*1024)}',
                  f'CONFIG_ESPRESSIF_CACHE_L2_CACHE_LINE_SIZE={64 if a.l2_kb==256 else 128}']:
        if value not in config:raise RuntimeError('Unexpected v3 configuration: '+value)
    clean_stamp=root/'bilibili-clean.txt'
    # NuttX's imported chip sources do not always regenerate header dependencies.
    # A struct/extern change must not leave an old ISR in libarch.a.
    headers=hashlib.sha256()
    for header in radio_sources:
        headers.update(hashlib.sha256(header.read_bytes()).digest())
    for header in sorted(path for path in overlay.rglob('*') if path.suffix in ['.h','.inc']):
        headers.update(str(header.relative_to(overlay)).encode())
        headers.update(hashlib.sha256(header.read_bytes()).digest())
    headers.update(ble_identity['header_sha256'].encode())
    # The SDK DMA propagation API is inline: invalidate every object if its
    # guarded implementation changes, even when overlay headers do not.
    headers.update(hashlib.sha256((repo/'tools/protect_p4_display_flash.py').read_bytes()).digest())
    headers.update(hashlib.sha256((repo/'tools/prepare_p4_flash_suspend.py').read_bytes()).digest())
    headers.update(hashlib.sha256((repo/'tools/prepare_p4_scanout_reload.py').read_bytes()).digest())
    if a.psram_xip:headers.update(hashlib.sha256((repo/'tools/prepare_p4_psram_xip.py').read_bytes()).digest())
    config_hash=hashlib.sha256((config+'\nspeech='+str(bool(a.speech_runtime))+'\nesp_tts='+str(bool(a.esp_tts_prepared))+'\nvoice_storage='+a.esp_tts_storage+'\nheaders='+headers.hexdigest()).encode()).hexdigest()
    if not clean_stamp.exists() or clean_stamp.read_text().strip()!=config_hash:
        run(['make','clean',cross],cwd=root/'nuttx');clean_stamp.write_text(config_hash+'\n')
    run(['make','context',cross,f'ESPCLAW_TLS_CFLAGS={includes}'],cwd=root/'nuttx')
    # Compile speech using the final resolved configuration and generated
    # headers, after olddefconfig/clean/context have completed.
    if a.speech_runtime:
        from build_speech_runtime import build as build_speech
        archive=build_speech(root/'apps',prepared/'vendor',out/'speech',tc,
            esp_tts_prepared=a.esp_tts_prepared.resolve() if a.esp_tts_prepared else None,esp_tts_storage=a.esp_tts_storage)
        speech_compile=json.loads((out/'speech/compile.json').read_text())
        speech_libraries=[archive]+[Path(path) for path in speech_compile['link_libraries']]
        libs=['--start-group',*speech_libraries,*libs,'--end-group']
    run(['make',f'-j{a.jobs}',cross,f'ESPCLAW_TLS_CFLAGS={includes}','EXTRA_LIBS='+' '.join(map(str,libs))],cwd=root/'nuttx')
    program_limit=data_offset
    if 0x2000+(root/'nuttx/nuttx.bin').stat().st_size>program_limit:raise RuntimeError('Firmware overlaps reserved storage')
    out.mkdir(parents=True,exist_ok=True)
    for name,target in [('nuttx.bin','nuttx.bin'),('nuttx','nuttx.elf'),('.config','resolved.config')]:shutil.copy2(root/'nuttx'/name,out/target)
    from nimble_capacity import audit as audit_nimble_capacity
    ble_capacity=audit_nimble_capacity(root/'apps',root/'nuttx',
        tc/'riscv32-esp-elf-gcc',out/'nimble-capacity',out/'nuttx.elf')
    def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
    nm=subprocess.check_output([str(tc/'riscv32-esp-elf-nm'),'-C',str(out/'nuttx.elf')],text=True)
    for symbol in ['bili_live_show','bili_flv_run','glass_https_tls_ops','WelsCreateDecoder',
                   'qpk_photo_store_initialize','esp_camera_display_frame','esp_camera_flash_end',
                   'c6_softap_start','c6net_trial_connect','glass_network_recovery_start',
                   'dhcpd_runtime_status','esp_hosted_rpc_c6_ota']:
        if symbol not in nm:raise RuntimeError('Missing linked symbol '+symbol)
    if re.search(r'\besp_display_flash_isr_enter$',nm,re.M):
        raise RuntimeError('Rejected cache-off IRQ experiment is still linked')
    if a.flash_auto_suspend_jedec:
        for symbol in ['esp_flash_chip_gd','spi_flash_chip_gd_get_caps','esp32p4_flash_auto_diagnostics']:
            if not re.search(r'\b'+symbol+r'$',nm,re.M):raise RuntimeError('Missing auto-suspend symbol '+symbol)
    if a.usb_camera:
        for symbol in ['camera_usb_initialize','glass_vision_analyze']:
            if not re.search(r'\b'+symbol+r'$',nm,re.M):raise RuntimeError('Missing linked camera/vision symbol '+symbol)
    if a.hardware_jpeg:
        table_header=root/'nuttx/arch/risc-v/src/esp32p4/esp32p4_jpeg_tables.h'
        table_source=root/'nuttx/arch/risc-v/src/esp32p4/esp-hal-3rdparty/components/upper_hal_jpeg/jpeg_param.c'
        if 'SHA256: '+sha(table_source) not in table_header.read_text():
            raise RuntimeError('JPEG tables do not match the prepared SDK; regenerate explicitly')
        for symbol in ['esp32p4_jpeg_register','esp32p4_jpeg_parse','esp32p4_jpeg_emit',
                       'qpk_jpeg_hw_encode565','qpk_jpeg_encode_selftest',
                       'jpeg_hal_init','esp32p4_dma2d_acquire','esp32p4_dma2d_stop']:
            if not re.search(r'\b'+symbol+r'$',nm,re.M):raise RuntimeError('Missing linked JPEG/DMA2D symbol '+symbol)
    if a.esp_tts_prepared:
        for symbol in ['glass_esp_synthesize','esp_tts_create','esp_tts_stream_play','glass_esp_tts_voice_load']:
            if not re.search(r'\b'+symbol+r'$',nm,re.M):raise RuntimeError('Missing linked ESP-TTS symbol '+symbol)
        if re.search(r'\bglass_sano_synthesize$',nm,re.M):raise RuntimeError('Unexpected legacy TTS backend in ESP-TTS image')
    bss=re.search(r'^([0-9a-f]+) \w _ebss$',nm,re.M)
    if not bss or int(bss.group(1),16)>0x4ffaefc0:raise RuntimeError('P4 early-boot BSS exceeds the prepared board boundary')
    revision=a.source_revision
    if revision:
        if not re.fullmatch('[0-9a-f]{40}',revision):p.error('source-revision must be 40-hex')
    else:revision=subprocess.check_output(['git','-C',str(repo),'rev-parse','HEAD'],text=True).strip()
    metadata={'camera_timing':camera_timing_sources,
              'hardware_jpeg':{'enabled':a.hardware_jpeg,
                'sdk_sources':{name:sha(root/'nuttx/arch/risc-v/src/esp32p4/esp-hal-3rdparty'/name)
                  for name in ['components/esp_hal_jpeg/jpeg_hal.c',
                    'components/esp_hal_jpeg/esp32p4/include/hal/jpeg_ll.h',
                    'components/esp_hal_dma/esp32p4/include/hal/dma2d_ll.h',
                    'components/upper_hal_jpeg/jpeg_param.c',
                    'components/upper_hal_jpeg/jpeg_encode.c',
                    'components/upper_hal_jpeg/jpeg_emit_marker.c']} if a.hardware_jpeg else {}},
              'ble_connections_requested':a.ble_connections,
              'ble_eight_test':a.ble_eight_test,
              'ble_central_capacity_header_sha256':sha(central_capacity),
              'ble_c6_eight_link_qualified':False,
              'ble_capacity':ble_capacity,
              'ble_store':ble_store_sources,
              'ble_identity':ble_identity,
              'c6_ota':c6_ota_sources,
              'network_recovery':{'softap':c6_softap_sources,
                                  'credential_trial':c6_wifi_trial_sources,
                                  'dhcpd':dhcpd_sources,'ethernet_share':network_share_sources},
              'forwarding_diagnostics':{'enabled':a.ethernet_share,
                                        'static_slots':a.share_forward_slots if a.ethernet_share else None,
                                        'dynamic_allocations':False,
                                        'network_statistics':a.share_net_stats,
                                        'sdio_clock_or_scheduler_changed':False},
              'psram_xip':{'enabled':a.psram_xip,'sources':psram_xip_sources,'tool_sha256':sha(repo/'tools/prepare_p4_psram_xip.py')},
              'display_flash':display_flash_sources,
              'scanout_reload':{'enabled':a.dsi_flash_reload,'sources':scanout_reload_sources},
              'scanout_reload_tool_sha256':sha(repo/'tools/prepare_p4_scanout_reload.py'),
              'flash_auto_suspend':{'measured_jedec':a.flash_auto_suspend_jedec,'sdk_sources':flash_suspend_sources},
              'flash_suspend_tool_sha256':sha(repo/'tools/prepare_p4_flash_suspend.py'),
              'display_flash_tool_sha256':sha(repo/'tools/protect_p4_display_flash.py'),
              'camera_timing_tool_sha256':sha(repo/'tools/instrument_p4_timing.py'),
              'upstream':revision,
              'baseline':str(base),'baseline_config_sha256':sha(base/'nuttx/.config'),
              'workspace':str(root),'tls':'existing glass_https / mbedTLS',
              'tts_backend':a.tts_backend if a.speech_runtime else None,'ebss':hex(int(bss.group(1),16)),
              'usb_camera':a.usb_camera,
              'factory_header':a.factory_header,
              'esp_tts_voice_storage':a.esp_tts_storage if a.esp_tts_prepared else None,
              'compiler':subprocess.check_output([str(tc/'riscv32-esp-elf-gcc'),'--version'],text=True).splitlines()[0],
              'flash_offset':'0x2000','program_limit':hex(program_limit),'vision_models_sd_only':True,'data_offset':hex(data_offset),'data_size':hex(data_size),'flash_layout':f'{data_offset//0x100000}m-firmware-{data_size//0x100000}m-data','board_tested':False,'l2_cache_kb':a.l2_kb,
              'sha256':{name:sha(out/name) for name in ['nuttx.bin','nuttx.elf','resolved.config']},
              'reused_libraries':{str(lib):sha(Path(lib)) for lib in libs if Path(lib).is_file()},
              'source_sha256':copied_source_hashes}
    if source_inputs()!=copied_source_hashes:
        raise RuntimeError('Source inputs changed during build; rebuild before deployment')
    if a.speech_runtime:
        metadata['speech']={'lock_sha256':sha(repo/'app/espdl-quickapp/speech/upstream.lock.json'),
                           'sd_manifest_sha256':sha(prepared/'sdcard/speech/manifest.json'),
                           'compile':json.loads((out/'speech/compile.json').read_text()),
                           'tools':{str(f.relative_to(repo)):sha(f) for f in [repo/'tools/build_bilibili_v3.py',repo/'tools/build_speech_runtime.py',repo/'tools/prepare_speech_runtime.py',repo/'tools/prepare_esp_tts.py',repo/'tools/esp_tts_inputs.py']}}
    (out/'BUILD-METADATA.json').write_text(json.dumps(metadata,indent=2)+'\n')
    print('Build verified:',out,metadata['sha256']['nuttx.bin'])
if __name__=='__main__':main()
