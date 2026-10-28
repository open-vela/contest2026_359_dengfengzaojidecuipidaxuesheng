#!/usr/bin/env python3
"""Temporary App entry for real-board website tests; no persistent URL writes."""
import argparse,json
from pathlib import Path
from urllib.parse import urlsplit
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--url',required=True)
p.add_argument('--app',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
url=urlsplit(a.url)
if url.scheme not in ('http','https') or not url.hostname or url.username or url.password:
    p.error('use a public HTTP/HTTPS URL without credentials')
prefix='''// Temporary website test entry. Restore the packaged entry afterwards.
const siteStorageGet = system.storage.get;
system.storage.get = function (key) {
  return key === 'netsurf.lastUrl' ? SITE_URL : siteStorageGet(key);
};
'''.replace('SITE_URL',json.dumps(a.url))
monitor='''
let siteChecks = 0;
const siteMonitor = setInterval(function () {
  if (system.netsurf) console.log('WEBSITE_STATE', JSON.stringify(system.netsurf.status()));
  if (++siteChecks >= 100) clearInterval(siteMonitor);
}, 2000);
'''
a.output.parent.mkdir(parents=True,exist_ok=True)
a.output.write_text(prefix+a.app.read_text(encoding='utf-8')+monitor,encoding='utf-8')
print('SITE_ENTRY_READY',a.output,a.output.stat().st_size)
