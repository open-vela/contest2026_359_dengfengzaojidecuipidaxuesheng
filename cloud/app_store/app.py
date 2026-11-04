"""ESPClaw store API. Public reads; explicit authenticated publication only."""
from contextlib import asynccontextmanager, contextmanager
from datetime import datetime, timezone
import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import sqlite3
from typing import Annotated, Literal

from fastapi import Depends, FastAPI, HTTPException, Request, Response
from fastapi.responses import FileResponse, JSONResponse
from fastapi.security import HTTPAuthorizationCredentials, HTTPBearer
from fastapi.exceptions import RequestValidationError
from pydantic import BaseModel, ConfigDict, Field, field_validator

BUILTINS = frozenset({'store.calculator', 'store.focus', 'store.counter'})
MAX_CATALOG_BYTES = 24 * 1024
MAX_CLOUD_APPS = 21
HERE = Path(__file__).resolve().parent


def utf8_text(value: str, maximum: int, *, empty=False, multiline=False):
    if (not empty and not value.strip()) or len(value.encode('utf-8')) > maximum:
        raise ValueError(f'must contain 1..{maximum} UTF-8 bytes')
    if '\0' in value or any(ord(c) < 32 and not (multiline and c == '\n') or ord(c) == 127 for c in value):
        raise ValueError('control characters are not allowed')
    if multiline and value.count('\n') > 1:
        raise ValueError('at most two title lines')
    return value


Package = Annotated[str, Field(pattern=r'^store\.[a-z0-9_]{1,41}$', max_length=47)]


class StrictModel(BaseModel):
    model_config = ConfigDict(extra='forbid', strict=True)


class Metadata(StrictModel):
    package: Package
    name: str
    version: str
    category: Annotated[int, Field(ge=0, le=2)]
    summary: str
    icon: Literal['calculator', 'timer', 'counter', 'grid'] = 'grid'

    @field_validator('name', 'version', 'summary')
    @classmethod
    def bounded(cls, value, info):
        return utf8_text(value, {'name': 47, 'version': 23, 'summary': 191}[info.field_name])


class Publication(Metadata):
    source: str

    @field_validator('source')
    @classmethod
    def source_limit(cls, value):
        if not value or '\0' in value or len(value.encode('utf-8')) > 65536:
            raise ValueError('app.js must contain 1..65536 UTF-8 bytes without NUL')
        return value


class Feature(StrictModel):
    package: Package
    title: str
    subtitle: str = ''
    art: Literal['timer', 'bars', 'none'] = 'none'
    tone: Literal['forest', 'sand', 'lavender'] = 'forest'
    value: str = ''
    unit: str = ''

    @field_validator('title', 'subtitle', 'value', 'unit')
    @classmethod
    def bounded(cls, value, info):
        cap = {'title': 95, 'subtitle': 127, 'value': 23, 'unit': 23}[info.field_name]
        return utf8_text(value, cap, empty=info.field_name != 'title', multiline=info.field_name == 'title')


class Home(StrictModel):
    headline: str
    tagline: str
    featured: Annotated[list[Feature], Field(max_length=2)]

    @field_validator('headline', 'tagline')
    @classmethod
    def bounded(cls, value, info):
        return utf8_text(value, 95 if info.field_name == 'headline' else 127)

    @field_validator('featured')
    @classmethod
    def distinct(cls, features):
        if len({f.package for f in features}) != len(features):
            raise ValueError('featured packages must be distinct')
        return features


def encode(value):
    return json.dumps(value, ensure_ascii=False, separators=(',', ':'), sort_keys=True)


class Store:
    def __init__(self, path: Path):
        self.path = path

    @contextmanager
    def db(self):
        connection = sqlite3.connect(self.path, timeout=5, isolation_level=None)
        connection.row_factory = sqlite3.Row
        connection.execute('PRAGMA foreign_keys=ON')
        try:
            connection.execute('BEGIN IMMEDIATE')
            yield connection
            connection.commit()
        except BaseException:
            connection.rollback()
            raise
        finally:
            connection.close()

    def initialize(self):
        self.path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        with sqlite3.connect(self.path) as db:
            db.execute('PRAGMA journal_mode=WAL')
            db.executescript('''
                CREATE TABLE IF NOT EXISTS blobs(sha TEXT PRIMARY KEY, data BLOB NOT NULL);
                CREATE TABLE IF NOT EXISTS releases(
                    package TEXT NOT NULL, version TEXT NOT NULL, sha TEXT NOT NULL REFERENCES blobs(sha),
                    metadata TEXT NOT NULL, created TEXT NOT NULL, PRIMARY KEY(package,version));
                CREATE TABLE IF NOT EXISTS apps(
                    package TEXT PRIMARY KEY, version TEXT NOT NULL, active INTEGER NOT NULL,
                    FOREIGN KEY(package,version) REFERENCES releases(package,version));
                CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY,value TEXT NOT NULL);
            ''')
            home = Home.model_validate_json((HERE/'default-home.json').read_text(encoding='utf-8'))
            db.execute('INSERT OR IGNORE INTO settings VALUES(?,?)', ('home', encode(home.model_dump())))

    def catalog(self, db):
        apps = [json.loads(row['metadata']) for row in db.execute('''
            SELECT r.metadata FROM apps a JOIN releases r ON r.package=a.package AND r.version=a.version
            WHERE a.active=1 ORDER BY a.package''')]
        home = json.loads(db.execute('SELECT value FROM settings WHERE key=?', ('home',)).fetchone()[0])
        result = {'schema': 1, 'apps': apps, **home}
        if len(encode(result).encode('utf-8')) > MAX_CATALOG_BYTES:
            raise HTTPException(409, 'catalog exceeds the device 24 KiB limit')
        return result


class BodyLimit:
    """Bound chunked bodies as well as Content-Length, before JSON parsing."""
    def __init__(self, app, maximum=512*1024):
        self.app, self.maximum = app, maximum

    async def __call__(self, scope, receive, send):
        if scope['type'] != 'http':
            return await self.app(scope, receive, send)
        chunks, total = [], 0
        while True:
            message = await receive()
            if message['type'] == 'http.disconnect':
                return
            body = message.get('body', b'')
            total += len(body)
            if total > self.maximum:
                return await JSONResponse({'detail': 'request body too large'}, status_code=413)(scope, receive, send)
            chunks.append(body)
            if not message.get('more_body', False):
                break
        delivered = False

        async def replay():
            nonlocal delivered
            if not delivered:
                delivered = True
                return {'type': 'http.request', 'body': b''.join(chunks), 'more_body': False}
            return await receive()

        await self.app(scope, replay, send)


def create_app(database: Path | None = None, admin_token: str | None = None):
    store = Store(database or Path(os.environ.get('STORE_DATABASE', 'var/app-store/store.sqlite3')))
    token = os.environ.get('STORE_ADMIN_TOKEN', '') if admin_token is None else admin_token
    if token and len(token.encode('utf-8')) < 32:
        raise ValueError('STORE_ADMIN_TOKEN must contain at least 32 bytes')

    @asynccontextmanager
    async def lifespan(app):
        store.initialize()
        yield

    app = FastAPI(title='ESPClaw App Store', version='1.0.0', lifespan=lifespan)
    app.add_middleware(BodyLimit)
    bearer = HTTPBearer(auto_error=False)

    @app.exception_handler(RequestValidationError)
    async def invalid_request(request, error):
        # Do not echo entire scripts or malformed Unicode back in error bodies.
        return JSONResponse({'detail': [{'loc': e['loc'], 'msg': e['msg'], 'type': e['type']} for e in error.errors()]}, status_code=422)

    def admin(credentials: Annotated[HTTPAuthorizationCredentials | None, Depends(bearer)]):
        if not token:
            raise HTTPException(503, 'publication is disabled until an administrator token is configured')
        if not credentials or not hmac.compare_digest(credentials.credentials.encode(), token.encode()):
            raise HTTPException(401, 'invalid administrator credentials', headers={'WWW-Authenticate': 'Bearer'})

    @app.middleware('http')
    async def response_headers(request: Request, call_next):
        response = await call_next(request)
        response.headers['X-Content-Type-Options'] = 'nosniff'
        response.headers['Referrer-Policy'] = 'no-referrer'
        if '/admin' in request.url.path:
            response.headers['Cache-Control'] = 'no-store'
            response.headers['Content-Security-Policy'] = "default-src 'self'; script-src 'self'; style-src 'self'; frame-ancestors 'none'; base-uri 'none'"
        return response

    @app.get('/healthz')
    def health():
        with store.db() as db:
            db.execute('SELECT 1').fetchone()
        return {'ok': True, 'schema': 1}

    @app.get('/v1/catalog')
    def catalog(request: Request):
        with store.db() as db:
            data = encode(store.catalog(db)).encode('utf-8')
        etag = '"'+hashlib.sha256(data).hexdigest()+'"'
        headers = {'ETag': etag, 'Cache-Control': 'public, max-age=30'}
        if request.headers.get('if-none-match') == etag:
            return Response(status_code=304, headers=headers)
        return Response(data, media_type='application/json', headers=headers)

    @app.get('/v1/blobs/{sha}/app.js')
    def blob(sha: str, request: Request):
        if not re.fullmatch(r'[a-f0-9]{64}', sha):
            raise HTTPException(404, 'package not found')
        with store.db() as db:
            row = db.execute('SELECT data FROM blobs WHERE sha=?', (sha,)).fetchone()
        if not row:
            raise HTTPException(404, 'package not found')
        headers = {'ETag': '"'+sha+'"', 'Cache-Control': 'public, max-age=31536000, immutable',
                   'Content-Disposition': 'attachment; filename="app.js"'}
        if request.headers.get('if-none-match') == headers['ETag']:
            return Response(status_code=304, headers=headers)
        return Response(bytes(row['data']), media_type='application/javascript', headers=headers)

    @app.get('/v1/admin/apps', dependencies=[Depends(admin)])
    def admin_apps():
        with store.db() as db:
            rows = db.execute('''SELECT r.metadata,r.created,a.active,a.version AS current_version FROM releases r
                JOIN apps a ON a.package=r.package ORDER BY r.package,r.created DESC''').fetchall()
            return {'releases': [{**json.loads(r['metadata']), 'created': r['created'],
                                 'current': json.loads(r['metadata'])['version']==r['current_version'],
                                 'active': bool(r['active']) and json.loads(r['metadata'])['version']==r['current_version']} for r in rows],
                    'home': store.catalog(db) | {'apps': []}}

    @app.post('/v1/admin/apps', dependencies=[Depends(admin)])
    def publish(body: Publication):
        if body.package in BUILTINS:
            raise HTTPException(409, 'use a separate store.* identity; built-in applications are preserved')
        source = body.source.encode('utf-8')
        sha = hashlib.sha256(source).hexdigest()
        metadata = body.model_dump(exclude={'source'}) | {'sha256': sha, 'bytes': len(source)}
        serialized = encode(metadata)
        with store.db() as db:
            prior = db.execute('SELECT sha,metadata FROM releases WHERE package=? AND version=?', (body.package, body.version)).fetchone()
            if prior and (prior['sha'] != sha or prior['metadata'] != serialized):
                raise HTTPException(409, 'published versions are immutable; publish a new version')
            if prior:
                return {'ok': True, 'already_published': True, 'application': metadata}
            current = db.execute('SELECT active FROM apps WHERE package=?', (body.package,)).fetchone()
            active = db.execute('SELECT COUNT(*) FROM apps WHERE active=1').fetchone()[0]
            if (not current or not current['active']) and active >= MAX_CLOUD_APPS:
                raise HTTPException(409, 'device catalog supports at most 21 cloud applications')
            db.execute('INSERT OR IGNORE INTO blobs VALUES(?,?)', (sha, source))
            db.execute('INSERT OR IGNORE INTO releases VALUES(?,?,?,?,?)', (body.package, body.version, sha, serialized, datetime.now(timezone.utc).isoformat()))
            db.execute('INSERT INTO apps VALUES(?,?,1) ON CONFLICT(package) DO UPDATE SET version=excluded.version,active=1', (body.package, body.version))
            store.catalog(db)  # Roll the transaction back if the device limit would be exceeded.
        return {'ok': True, 'application': metadata}

    @app.delete('/v1/admin/apps/{package}', dependencies=[Depends(admin)])
    def unpublish(package: str):
        with store.db() as db:
            if not db.execute('SELECT 1 FROM apps WHERE package=?', (package,)).fetchone():
                raise HTTPException(404, 'application not found')
            db.execute('UPDATE apps SET active=0 WHERE package=?', (package,))
            home = json.loads(db.execute('SELECT value FROM settings WHERE key=?', ('home',)).fetchone()[0])
            home['featured'] = [f for f in home['featured'] if f['package'] != package]
            db.execute('UPDATE settings SET value=? WHERE key=?', (encode(home), 'home'))
        return {'ok': True, 'note': 'catalog entry removed; immutable releases remain available to existing readers'}

    @app.post('/v1/admin/apps/{package}/restore', dependencies=[Depends(admin)])
    def restore(package: str):
        with store.db() as db:
            row = db.execute('SELECT active FROM apps WHERE package=?', (package,)).fetchone()
            if not row:
                raise HTTPException(404, 'application not found')
            if not row['active'] and db.execute('SELECT COUNT(*) FROM apps WHERE active=1').fetchone()[0] >= MAX_CLOUD_APPS:
                raise HTTPException(409, 'device catalog supports at most 21 cloud applications')
            db.execute('UPDATE apps SET active=1 WHERE package=?', (package,))
            store.catalog(db)
        return {'ok': True}

    @app.put('/v1/admin/home', dependencies=[Depends(admin)])
    def update_home(home: Home):
        with store.db() as db:
            known = BUILTINS | {r['package'] for r in db.execute('SELECT package FROM apps WHERE active=1')}
            if any(f.package not in known for f in home.featured):
                raise HTTPException(422, 'recommendations must refer to available applications')
            db.execute('UPDATE settings SET value=? WHERE key=?', (encode(home.model_dump()), 'home'))
            store.catalog(db)
        return {'ok': True, 'home': home.model_dump()}

    @app.get('/admin', include_in_schema=False)
    def admin_page():
        return FileResponse(HERE/'static/index.html')

    @app.get('/admin/{asset}', include_in_schema=False)
    def admin_asset(asset: str):
        if asset not in ('app.js', 'style.css'):
            raise HTTPException(404)
        return FileResponse(HERE/'static'/asset)

    return app


app = create_app()
