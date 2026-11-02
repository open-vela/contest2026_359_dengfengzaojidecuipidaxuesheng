#!/usr/bin/env python3
"""Build a narrow P4 music update against the verified integration firmware."""
import argparse,hashlib,json,os,re,shutil,subprocess
from pathlib import Path
def main():
    p=argparse.ArgumentParser();p.add_argument('--metadata',type=Path,required=True);p.add_argument('--rollback',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    repo=Path(__file__).resolve().parents[1];meta=json.loads(a.metadata.read_text());work=Path(meta['workspace']);out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    sha=lambda path:hashlib.sha256(path.read_bytes()).hexdigest()
    config=sha(work/'nuttx/.config');assert config==meta['sha256']['resolved.config']
    shutil.copy2(a.rollback,out/'rollback.bin')
    desktop=work/'apps/system/desktop';overlay=repo/'app/espdl-quickapp/overlay/apps/system/desktop'
    names=['glass_sd_font.c','glass_sd_font.h','glass_sd_font_asset.h','glass_ui.inc','glass_lockscreen.inc','glass_lock_pages.inc','glass_music.h','glass_music_parse.c','glass_board_music.c','glass_agent_profiles.h','glass_portal_ui.inc','glass_music_service.c','glass_music_ui.inc','glass_music_full.inc','glass_music_pv.inc',
           'glass_music_lyrics.h','glass_music_hans.h','glass_music_hans_table.h','glass_music_cache.h','glass_music_audio_cache.h','glass_music_favorite_cache.h','glass_lock_photo.c','glass_portal_resource.c','glass_music_sd.h','glass_music_track_key.h','glass_ui_probe.inc','desktop_main.c',
           'qpk_tjpgdcnf.h','qpk_tjpgd.c','qpk_mjpeg.c','qpk_mjpeg.h']
    names += ['glass_settings_ui.inc', 'glass_about_ui.inc', 'glass_home_ui.inc',
              'glass_music_cover_ui.inc','glass_lock_photo.h',
              'glass_lyric_fonts.inc','glass_lyric_font_assets.h',
              'glass_bluetooth.inc','glass_mouse.inc','glass_ble.h','glass_ble_central.h','glass_status_ui.inc']
    records={}
    # Preserve the prepared tree's pinned optional build flags. Add only the
    # private streaming backend; do not replace its full integration Makefile.
    makefile=desktop/'Makefile';make_text=makefile.read_text()
    if 'CSRCS += glass_sd_font.c' not in make_text:
        makefile.write_text(make_text.replace('include $(APPDIR)/Application.mk',
            'CSRCS += glass_sd_font.c\ninclude $(APPDIR)/Application.mk'))
    for name in names:
        before=out/'before'/name;before.parent.mkdir(exist_ok=True)
        if (desktop/name).exists() and not before.exists():shutil.copy2(desktop/name,before)
        shutil.copyfile(overlay/name,desktop/name);records[name]=sha(overlay/name)
    # Keep the peer snapshot ABI and service implementation together even in
    # a narrow desktop rebuild. Otherwise an old host cannot identify peers.
    ble_sources={}
    for name in ['glass_ble.h','glass_ble.c','glass_ble_central.h','glass_ble_central.c']:
        source=repo/'app/espdl-quickapp/overlay/apps/wireless/bluetooth/nimble'/name
        target=work/'apps/wireless/bluetooth/nimble'/name
        before=out/'before/nimble'/name;before.parent.mkdir(parents=True,exist_ok=True)
        if target.exists() and not before.exists():shutil.copy2(target,before)
        shutil.copyfile(source,target);ble_sources[target]=sha(source);records['nimble/'+name]=sha(source)
    (desktop/'desktop_main.c').touch()
    inputs={str(f.relative_to(desktop)):sha(f) for f in desktop.iterdir() if f.suffix in ('.c','.h','.inc')}
    libraries=list(meta['reused_libraries'])
    for name,digest in meta['reused_libraries'].items():assert sha(Path(name))==digest,name
    runtime=[s for s in libraries if s.endswith(('libstdc++.a','libsupc++.a','libgcc.a'))]
    tc='/home/flash/vela-p4/toolchains/gcc14/bin/riscv32-esp-elf-';tls=Path(meta['baseline'])/'standalone-tls'
    flags=' '.join('-I'+str(tls/s) for s in ['include','tf-psa-crypto/include','tf-psa-crypto/drivers/builtin/include'])+' -DMBEDTLS_THREADING_C -DMBEDTLS_THREADING_PTHREAD'
    command=['make','-j6','CROSSDEV='+tc,'ESPCLAW_TLS_CFLAGS='+flags,'EXTRA_LIBS=--start-group '+' '.join(s for s in libraries if s not in runtime)+' --end-group '+' '.join(runtime)]
    env=dict(os.environ);env['PATH']=str(Path(tc).parent)+':/home/flash/vela-p4/.venv/bin:/home/flash/vela-p4/host-tools/kconfig/usr/bin:'+env['PATH']
    env['LD_LIBRARY_PATH']='/home/flash/vela-p4/host-tools/kconfig/usr/lib/x86_64-linux-gnu'
    (out/'command.json').write_text(json.dumps(command,indent=2))
    with (out/'build.log').open('w') as log:subprocess.run(command,cwd=work/'nuttx',env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
    assert sha(work/'nuttx/.config')==config
    for name,digest in inputs.items():assert sha(desktop/name)==digest,'Concurrent build input changed: '+name
    for path,digest in ble_sources.items():assert sha(path)==digest,'Concurrent BLE build input changed: '+str(path)
    nm=subprocess.check_output([tc+'nm',str(work/'nuttx/nuttx')],text=True)
    ebss=int(re.search(r'^([0-9a-f]+) \w _ebss$',nm,re.M)[1],16);assert ebss<=0x4ffaefc0
    assert not re.search(r'\b(glass_esp_synthesize|glass_nano_probe|glass_zh_q8_probe)$',nm,re.M),'TTS experiment linked'
    size=(work/'nuttx/nuttx.bin').stat().st_size;end=(0x2000+size+4095)&~4095;assert end<0xc00000
    for src,dst in [('nuttx','nuttx.elf'),('nuttx.bin','nuttx.bin'),('.config','resolved.config')]:shutil.copy2(work/'nuttx'/src,out/dst)
    report=dict(linked=True,board_written=False,lyric_renderer='native-three',firmware_bytes=size,flash_offset='0x2000',erase_end=hex(end),data_offset='0xc00000',ebss=hex(ebss),
                config_unchanged=True,tts_experiments_linked=False,source_sha256=records,sha256={n:sha(out/n) for n in ['nuttx.bin','nuttx.elf','resolved.config','rollback.bin']})
    (out/'verification.json').write_text(json.dumps(report,indent=2));print('JIZURA_DEVICE_LINKED',size,hex(ebss),report['sha256']['nuttx.bin'])
if __name__=='__main__':main()
