"""Start a loopback-only writable store with a persistent local admin token.

The private state directory belongs outside the repository. Never prints tokens.
"""
import argparse
import json
import os
from pathlib import Path
import secrets


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--state',type=Path,required=True)
    parser.add_argument('--database',type=Path,required=True)
    parser.add_argument('--port',type=int,default=8080)
    args=parser.parse_args()
    args.state.mkdir(parents=True,exist_ok=True,mode=0o700)
    secret=args.state/'admin-token.txt'
    if not secret.exists():
        fd=os.open(secret,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
        with os.fdopen(fd,'w',encoding='utf-8') as output:output.write(secrets.token_urlsafe(48))
    token=secret.read_text(encoding='utf-8').strip()
    if len(token.encode())<32:raise SystemExit('Local administrator token is invalid; existing file was preserved')
    from cloud.app_store.app import create_app
    import uvicorn
    application=create_app(args.database,token)
    (args.state/'server.json').write_text(json.dumps({'pid':os.getpid(),'url':f'http://127.0.0.1:{args.port}/admin','database':str(args.database.resolve()),'mode':'administrator-enabled'},indent=2),encoding='utf-8')
    uvicorn.run(application,host='127.0.0.1',port=args.port,access_log=False)


if __name__=='__main__':main()
