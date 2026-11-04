#!/usr/bin/env python3
"""Verify published source contents and SD assets against board109 receipts."""
from pathlib import Path
import argparse,hashlib,json

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--repo',type=Path,default=Path(__file__).resolve().parents[1]);a=p.parse_args();root=a.repo.resolve()
    evidence=root/'docs/evidence/board109-20261104';manifest=json.loads((evidence/'SOURCE-MANIFEST.json').read_text(encoding='utf-8'))
    for f in manifest['files']:
        b=(root/f['path']).read_bytes();actual=hashlib.sha256(b.replace(b'\r\n',b'\n') if f['text_eol_normalized'] else b).hexdigest()
        assert actual==f['published_sha256'],f['path']
    assets=root/'materials/board109-ui';data=json.loads((assets/'MANIFEST.json').read_text(encoding='utf-8'))
    for f in data['files']:assert hashlib.sha256((assets/'desktop-assets/v2'/f['path']).read_bytes()).hexdigest()==f['sha256'],f['path']
    icons=root/'materials/material-symbols';lock=json.loads((icons/'sources.json').read_text(encoding='utf-8'))
    expected={f['file'] for f in lock['files']};assert expected=={p.name for p in icons.glob('*.svg')},'Material SVG lock coverage'
    for f in lock['files']:assert hashlib.sha256((icons/f['file']).read_bytes()).hexdigest()==f['sha256'],f['file']
    print(json.dumps({'source_inputs':len(manifest['files']),'ui_assets':len(data['files']),'locked_icon_svgs':len(expected),'deployed_image_sha256':manifest['firmware_sha256'],
       'source_content_matched':True,'comparison':'Text sources normalize CRLF/LF; binary assets and original board receipts retain exact SHA256. This is not a new firmware build or physical test.'}))
if __name__=='__main__':main()
