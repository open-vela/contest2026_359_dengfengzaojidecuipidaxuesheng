#!/usr/bin/env python3
"""Verify actual LVGL global font views, SD persistence and failure recovery."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ['apps','cache','font-root','music-font','output']:
        parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args()
    repo=Path(__file__).resolve().parents[1]
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    choice=out/'global-font.txt'
    command=['python3',str(repo/'tools/render_all_ui.py'),'--apps',str(args.apps),
        '--cache',str(args.cache),'--output',str(out/'ui'),'--lyric-fonts',str(args.font_root),
        '--music-font',str(args.music_font),'--font-choice',str(choice),
        '--harness-source',str(repo/'app/espdl-quickapp/tests/render_global_fonts.c')]
    with (out/'render.log').open('w') as log:
        subprocess.run(command,check=True,stdout=log,stderr=subprocess.STDOUT)
    renderer=out/'ui/render-all-ui'
    base=args.apps/'system/desktop/AlibabaPuHuiTi-3-55-Regular.otf'
    results={}

    def run(case,environment,marker):
        env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1',**environment)
        test=subprocess.run([str(renderer),str(base.resolve()),str(out/'ui')],env=env,
            capture_output=True,text=True,timeout=100)
        (out/(case+'.log')).write_text(test.stdout+test.stderr)
        assert test.returncode==0 and marker in test.stdout,test.stdout+test.stderr
        results[case]=True

    # A fresh process must restore the saved family without rewriting it.
    for case,record,expected in [('restore','VelaGlobalFont 1 2\n',2),
                                 ('invalid-choice','broken setting\n',0),
                                 ('out-of-range-choice','VelaGlobalFont 1 999\n',0)]:
        choice.write_text(record)
        before=choice.stat().st_mtime_ns
        run(case,{'GLOBAL_FONT_RESTORE_EXPECT':str(expected)},'GLOBAL_FONT_RESTORE_OK')
        assert choice.read_text()==record and choice.stat().st_mtime_ns==before

    manifest=json.loads((args.font_root/'manifest.json').read_text())
    record=manifest[0];source=args.font_root/record['file'];original=source.read_bytes()
    with tempfile.TemporaryDirectory(prefix='global-font-fixture-') as folder:
        root=Path(folder);fixture=root/record['file']
        for case in ['missing','truncated','checksum']:
            if case=='truncated':fixture.write_bytes(original[:4])
            elif case=='checksum':fixture.write_bytes(original[:-1]+bytes([original[-1]^1]))
            run(case,{'LYRIC_FONT_TEST_ROOT':str(root),'LYRIC_FONT_EXPECT_FAILURE':'1'},'LYRIC_FONT_REJECTED')
        shutil.copyfile(source,fixture)
        run('read-failure',{'LYRIC_FONT_TEST_ROOT':str(root),'GLOBAL_FONT_IO_FAILURE':'1'},'GLOBAL_FONT_IO_FALLBACK_OK')

    choice.write_text('VelaGlobalFont 1 0\n')
    results['native_switches_and_render']=True
    results['screens']=len(json.loads((out/'ui/screens.json').read_text()))
    (out/'verification.json').write_text(json.dumps(results,indent=2)+'\n')
    print('GLOBAL_FONT_TESTS_OK',json.dumps(results))


if __name__=='__main__':main()
