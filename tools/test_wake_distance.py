#!/usr/bin/env python3
"""Check production wake acceptance boundaries and DTW evidence offline."""
import argparse
import os
from pathlib import Path
import subprocess

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    repo=Path(__file__).resolve().parents[1]
    desktop=repo/'app/espdl-quickapp/overlay/apps/system/desktop'
    tests=repo/'app/espdl-quickapp/tests'
    flags=['gcc','-std=gnu11','-O1','-g','-fsanitize=address,undefined',
           '-fno-omit-frame-pointer','-I'+str(desktop)]
    env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'}
    for name,sources in [('wake-distance',[tests/'wake_distance_host.c']),
                         ('wake-match',[tests/'wake_match_host.c',desktop/'glass_wake_features.c'])]:
        executable=args.output/name
        subprocess.run([*flags,*map(str,sources),'-lm','-o',str(executable)],check=True)
        subprocess.run([str(executable)],check=True,env=env,timeout=30)

if __name__=='__main__':main()
