#!/usr/bin/env python3
"""Recompile one current frontend file with its recorded cross-build command."""
import argparse,json,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--commands',type=Path,required=True)
p.add_argument('--source',type=Path,required=True)
a=p.parse_args()
commands=json.loads(a.commands.read_text())
command=next(c for c in commands if '-c' in c and Path(c[c.index('-c')+1]).name==a.source.name)
command[command.index('-c')+1]=str(a.source.resolve())
subprocess.run(command,check=True)
