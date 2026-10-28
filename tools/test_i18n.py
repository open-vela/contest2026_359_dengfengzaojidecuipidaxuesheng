#!/usr/bin/env python3
"""Validate i18n generation, asynchronous storage, fallback and user overrides."""
import argparse
import json
import os
from pathlib import Path
import subprocess


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    repo=Path(__file__).resolve().parents[1]
    subprocess.run(['python3',str(repo/'tools/generate_i18n.py'),'--check'],check=True)
    desktop=repo/'app/espdl-quickapp/overlay/apps/system/desktop'
    env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1',UBSAN_OPTIONS='halt_on_error=1')
    results={}
    for failure in [False,True]:
        choice=out/('missing-parent/language.txt' if failure else 'language.txt')
        executable=out/('i18n-failure' if failure else 'i18n-host')
        subprocess.run(['gcc','-std=gnu11','-O1','-g','-pthread','-fsanitize=address,undefined',
            '-I'+str(desktop),'-DGLASS_LANGUAGE_PATH="'+str(choice)+'"',
            str(desktop/'glass_i18n.c'),str(repo/'app/espdl-quickapp/tests/i18n_host.c'),'-o',str(executable)],check=True)

        def run(mode,expected):
            result=subprocess.run([str(executable),mode,str(expected)],capture_output=True,text=True,env=env,timeout=20)
            (out/(mode+('-failure' if failure else '')+'.log')).write_text(result.stdout+result.stderr)
            assert result.returncode==0 and 'I18N_HOST_OK' in result.stdout,result.stdout+result.stderr

        if failure:run('failure',1);results['save_failure_keeps_language']=True;continue
        run('save',1);assert choice.read_text()=='VelaLanguage 1 en\n'
        results['rapid_save_latest_wins']=True
        for name,record,expected in [('english','VelaLanguage 1 en\n',1),
                ('chinese','VelaLanguage 1 zh-CN\n',0),('invalid','broken\n',0),
                ('unsupported','VelaLanguage 1 unknown\n',0),('wrong-version','VelaLanguage 2 en\n',0),
                ('extra-data','VelaLanguage 1 en extra\n',0)]:
            choice.write_text(record);before=choice.stat().st_mtime_ns
            run('restore',expected)
            assert choice.read_text()==record and choice.stat().st_mtime_ns==before
            results[name]=True
        choice.write_text('VelaLanguage 1 en\n');run('override',0)
        assert choice.read_text()=='VelaLanguage 1 zh-CN\n'
        results['explicit_default_overrides_delayed_restore']=True
    (out/'verification.json').write_text(json.dumps(results,indent=2)+'\n')
    print('I18N_TESTS_OK',json.dumps(results))


if __name__=='__main__':main()
