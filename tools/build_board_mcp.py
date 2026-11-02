#!/usr/bin/env python3
"""Build board MCP against an explicitly prepared, already linked P4 workspace."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--workspace', type=Path, required=True)
p.add_argument('--link-command', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
root = a.workspace.resolve()
out = a.output.resolve()
repo = Path(__file__).resolve().parents[1]
overlay = repo / 'app/espdl-quickapp/overlay'
out.mkdir(parents=True, exist_ok=True)
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
configuration = sha(root / 'nuttx/.config')
command = json.loads(a.link_command.read_text())
assert command[0] == 'make' and any(x.startswith('EXTRA_LIBS=') for x in command)
if not (out / 'rollback.bin').exists():
    shutil.copy2(root / 'nuttx/nuttx.bin', out / 'rollback.bin')
names = ['desktop_main.c', 'glass_agent.c', 'glass_agent_profiles.h', 'glass_board.h',
         'glass_board_agent.inc', 'glass_board_ui.inc', 'glass_board_reference.h',
         'glass_board_home.c', 'glass_board_home.h', 'glass_board_music.c', 'glass_board_music.h',
           'glass_music.h', 'glass_music_service.c', 'glass_music_ui.inc', 'glass_music_full.inc', 'glass_music_pv.inc', 'glass_music_cache.h', 'glass_music_lyrics.h', 'glass_lock_photo.c', 'glass_music_sd.h', 'glass_music_track_key.h',
         'glass_ui.inc', 'glass_lockscreen.inc', 'glass_status.h', 'glass_status_power.c', 'glass_status_storage.c',
         'glass_status_ui.inc', 'glass_home_ui.inc', 'glass_netboot.inc', 'Makefile', 'glass_agent.h',
         'glass_agent_config.c', 'glass_portal_config.c', 'glass_portal_resource.c',
         'glass_chat.h', 'glass_chat_service.c', 'glass_chat_store.c', 'glass_chat_ui.inc',
         'glass_chat_trace.inc', 'glass_chat_cards.inc', 'glass_icons.h', 'glass_icons.c']
names += ['glass_sd_font.c','glass_sd_font.h','glass_sd_font_asset.h',
          'glass_music_cover_ui.inc','glass_lock_photo.h',
          'glass_lyric_fonts.inc','glass_lyric_font_assets.h',
          'glass_bluetooth.inc','glass_mouse.inc','glass_ble.h','glass_ble_central.h',
          'glass_music_hans.h','glass_music_hans_table.h','glass_lock_pages.inc',
          'glass_settings_ui.inc','glass_about_ui.inc']
names += ['glass_assistant.h','glass_board_assistant.c','glass_assistant_platform.c','glass_assistant_tools.h',
          'qpk_storage.c','qpk_storage.h','qpk_runtime.c','qpk_runtime.h','qpk_espdl.h','qpk_espdl_service.c',
          'glass_speech.c','glass_speech.h']
sources = [Path('apps/system/desktop') / name for name in names]
sources += [Path('apps/wireless/bluetooth/nimble')/name for name in
            ['glass_ble.h','glass_ble.c','glass_ble_central.h','glass_ble_central.c']]
sources += [Path('apps/system/espclaw/port') / name for name in ['espclaw_main.c', 'espclaw_qpk.c']]
sources += [Path('apps/system/espclaw/include/claw_stream.h')]
sources += [Path('apps/system/c6probe')/name for name in ['status_rssi.h','status_rssi.inc']]
hashes = {}
for rel in sources:
    src, dst = overlay / rel, root / rel
    backup = out / 'source-before' / rel
    if dst.exists() and not backup.exists():
        backup.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(dst, backup)
    shutil.copyfile(src, dst)
    hashes[str(rel)] = sha(src)
from adapt_c6_status import apply as apply_c6_status
apply_c6_status(root/'apps')
toolchain = next(x.split('=', 1)[1] for x in command if x.startswith('CROSSDEV='))
env = dict(os.environ)
env['PATH'] = str(Path(toolchain).parent) + ':/home/flash/.local/bin:/home/flash/vela-p4/.venv/bin:/home/flash/vela-p4/host-tools/kconfig/usr/bin:' + env['PATH']
env['LD_LIBRARY_PATH'] = '/home/flash/vela-p4/host-tools/kconfig/usr/lib/x86_64-linux-gnu'
(out / 'command.json').write_text(json.dumps(command, indent=2))
with (out / 'build.log').open('w') as log:
    subprocess.run(command, cwd=root / 'nuttx', env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
assert sha(root / 'nuttx/.config') == configuration
config = (root / 'nuttx/.config').read_text()
offset = int(re.search(r'^CONFIG_ESPRESSIF_STORAGE_MTD_OFFSET=(\S+)$', config, re.M).group(1), 0)
assert (root / 'nuttx/nuttx.bin').stat().st_size + 0x2000 < offset
symbols = subprocess.check_output([toolchain + 'nm', str(root / 'nuttx/nuttx')], text=True)
ebss = int(re.search(r'^([0-9a-f]+) \w _ebss$', symbols, re.M).group(1), 16)
assert ebss <= 0x4ffaefc0, hex(ebss)
assert re.search(r'\bglass_board_ui$', symbols, re.M)
for src, dst in [('nuttx', 'nuttx.elf'), ('nuttx.bin', 'nuttx.bin'), ('.config', 'resolved.config')]:
    shutil.copy2(root / 'nuttx' / src, out / dst)
report = dict(linked=True, board_written=False, config_sha256=configuration, ebss=hex(ebss),
              data_offset=hex(offset), source_sha256=hashes,
              sha256={name: sha(out / name) for name in ['nuttx.bin', 'nuttx.elf', 'resolved.config', 'rollback.bin']})
(out / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
print('BOARD_MCP_LINKED', report['sha256']['nuttx.bin'])
