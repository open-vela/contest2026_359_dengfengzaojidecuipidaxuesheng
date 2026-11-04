#!/bin/bash
# Install a pre-uploaded release. Does not alter the public proxy or existing data.
set -euo pipefail
release=${1:?Pass the absolute /opt/espclaw-app-store/releases/<release> path}
case "$release" in /opt/espclaw-app-store/releases/*) ;; *) exit 2 ;; esac
test -f "$release/cloud/app_store/app.py"
test -f "$release/cloud/app_store/requirements-lock.txt"
if ! id espclaw-store >/dev/null 2>&1; then
    useradd --system --home-dir /var/lib/espclaw-app-store --shell /usr/sbin/nologin espclaw-store
fi
install -d -m 750 -o espclaw-store -g espclaw-store /var/lib/espclaw-app-store
install -d -m 700 /etc/espclaw-app-store
if [ ! -e /etc/espclaw-app-store/service.env ]; then
    python3 - <<'PY'
from pathlib import Path
import os, secrets
p = Path('/etc/espclaw-app-store/service.env')
with p.open('x') as f:
    os.chmod(p, 0o600)
    f.write('STORE_DATABASE=/var/lib/espclaw-app-store/store.sqlite3\n')
    f.write('STORE_ADMIN_TOKEN=' + secrets.token_urlsafe(48) + '\n')
PY
fi
if [ ! -x /opt/espclaw-app-store/venv/bin/python ]; then
    python3 -m venv /opt/espclaw-app-store/venv
fi
/opt/espclaw-app-store/venv/bin/python -m pip install --disable-pip-version-check -r "$release/cloud/app_store/requirements-lock.txt"
if [ -L /opt/espclaw-app-store/current ]; then
    readlink -f /opt/espclaw-app-store/current > /opt/espclaw-app-store/previous-release.txt
elif [ -e /opt/espclaw-app-store/current ]; then
    echo 'Refusing to replace a non-symlink current directory' >&2; exit 2
fi
ln -sfn "$release" /opt/espclaw-app-store/current
install -m 644 "$release/cloud/app_store/deploy/espclaw-app-store.service" /etc/systemd/system/espclaw-app-store.service
systemctl daemon-reload
systemctl enable --now espclaw-app-store.service
systemctl restart espclaw-app-store.service
for attempt in $(seq 1 30); do
    if curl --fail --silent http://127.0.0.1:18765/healthz >/dev/null; then
        echo APP_STORE_SERVICE_READY; exit 0
    fi
    sleep 1
done
echo 'Service did not become healthy; inspect journalctl -u espclaw-app-store' >&2
exit 1
