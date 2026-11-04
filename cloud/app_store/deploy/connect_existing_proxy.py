"""Attach /app-store to this server's existing IP HTTPS virtual host.

Run as root after install.sh. The complete original config is saved once;
nginx must validate before reload. Other routes and certificate renewal remain.
"""
from pathlib import Path
import subprocess

config = Path('/opt/1panel/www/conf.d/work-replay-public.conf')
backup = Path('/opt/espclaw-app-store/proxy-before-app-store.conf')
container = '1Panel-openresty-dK6G'
source = Path('/opt/espclaw-app-store/current/cloud/app_store/deploy/app-store.locations.conf')
target = Path('/opt/1panel/www/sites/app-store/app-store.locations.conf')
include = 'include /www/sites/app-store/app-store.locations.conf;\n'
original = config.read_text()
needle = 'root /www/sites/work-replay/html;'
if original.count(needle) != 1:
    raise RuntimeError('Expected exactly one existing HTTPS site root; no changes made')
if not backup.exists():
    with backup.open('x') as f:
        f.write(original)
target.parent.mkdir(parents=True, exist_ok=True)
previous = target.read_text() if target.exists() else None
target.write_text(source.read_text())
updated = original if include in original else original.replace(needle, include + needle, 1)
config.write_text(updated)
try:
    subprocess.run(['docker', 'exec', container, 'openresty', '-t'], check=True)
    subprocess.run(['docker', 'exec', container, 'openresty', '-s', 'reload'], check=True)
except BaseException:
    config.write_text(original)
    if previous is not None:
        target.write_text(previous)
    else:
        target.unlink(missing_ok=True)
    raise
print('APP_STORE_PROXY_CONNECTED: existing routes and TLS configuration retained')
