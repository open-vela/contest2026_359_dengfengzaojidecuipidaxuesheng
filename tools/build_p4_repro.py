#!/usr/bin/env python3
"""Build the documented P4 rev3.2 XIP profile from verified prepared inputs."""
import argparse,json,shlex,subprocess,shutil
from pathlib import Path
from prepare_p4_repro_inputs import LOCK,verify,digest,read_lock
from esp_tts_inputs import verify_prepared

FLAGS=['--offline-inputs','--usb-camera','--factory-header','--psram-xip','--hardware-jpeg',
       '--ethernet-share','--share-forward-slots','16','--share-net-stats','--ble-eight-test','--lcd-backlight']
OPENH264_SHA='558544ad358283a7ab2930d69a9ceddf913f4a51ee9bf1bfb9e377322af81a69'

def command(repo,inputs,workspace,toolchain,archive,output,jobs,revision=None):
    if not 1<=jobs<=64:raise ValueError('--jobs must be between 1 and 64')
    if not shutil.which('esptool.py'):raise ValueError('esptool.py missing: activate the documented esptool 5.2.0 environment before building')
    esptool=subprocess.check_output(['esptool.py','version'],text=True)
    if '5.2.0' not in esptool:raise ValueError('This profile records esptool 5.2.0; use the matching host tool')
    repo=Path(repo).resolve();inputs=Path(inputs).resolve()
    lock=read_lock(LOCK);ready=json.loads((inputs/'P4-INPUTS.json').read_text());paths={}
    for row in lock['inputs']:
        path=Path(ready[row['role']]).resolve()
        if not path.is_relative_to(inputs):raise ValueError('Input directory must remain inside --inputs')
        marker=verify(path)
        if marker['archive_sha256']!=row['sha256']:raise ValueError('Input archive differs from profile lock')
        paths[row['role']]=path
    verify_prepared(paths['esp_tts'])
    if Path(workspace).resolve().is_relative_to(inputs):raise ValueError('Keep the build workspace outside the prepared input directory')
    if digest(archive)!=OPENH264_SHA:raise ValueError('OpenH264 archive differs from pinned 2.6.0 input')
    gcc=Path(toolchain)/'riscv32-esp-elf-gcc'
    compiler=subprocess.check_output([str(gcc),'--version'],text=True).splitlines()[0]
    if '14.2.0' not in compiler:raise ValueError('This profile requires GCC 14.2.0 ILP32F libraries')
    cmd=['python3',str(repo/'tools/build_bilibili_v3.py'),'--baseline',str(paths['baseline']),
         '--workspace',str(Path(workspace).resolve()),'--toolchain',str(Path(toolchain).resolve()),
         '--archive',str(Path(archive).resolve()),'--output',str(Path(output).resolve()),
         '--speech-runtime',str(paths['speech_runtime']),'--esp-tts-prepared',str(paths['esp_tts']),
         *FLAGS,'--jobs',str(jobs)]
    if revision:cmd+=['--source-revision',revision]
    return cmd

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--inputs',type=Path,required=True)
    p.add_argument('--workspace',type=Path,required=True);p.add_argument('--toolchain',type=Path,required=True)
    p.add_argument('--archive',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--jobs',type=int,default=4);p.add_argument('--source-revision');p.add_argument('--print-command',action='store_true')
    a=p.parse_args();cmd=command(Path(__file__).resolve().parents[1],a.inputs,a.workspace,a.toolchain,a.archive,a.output,a.jobs,a.source_revision)
    print(shlex.join(cmd),flush=True)
    if not a.print_command:subprocess.run(cmd,check=True)
if __name__=='__main__':main()
