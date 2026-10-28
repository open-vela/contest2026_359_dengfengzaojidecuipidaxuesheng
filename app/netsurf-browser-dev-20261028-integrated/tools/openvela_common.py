"""Local build preparation; input dependency trees remain read-only."""
from pathlib import Path
import re

def prepare_png(source: Path, output: Path, zlib: Path) -> Path:
    dest = output / 'libpng-source'
    dest.mkdir(parents=True, exist_ok=True)
    match = re.search(r'^#define ZLIB_VERNUM\s+(\S+)', (zlib / 'zlib.h').read_text(), re.M)
    if not match:
        raise RuntimeError('cannot identify the linked zlib version')
    for file in source.iterdir():
        if not file.is_file() or (file.suffix not in ['.c', '.h'] and file.name not in ['LICENSE', 'README']):
            continue
        data = file.read_bytes()
        if file.name == 'pnglibconf.h':
            data = re.sub(rb'(#define PNG_ZLIB_VERNUM)\s+\S+',
                          rb'\1 ' + match[1].encode(), data)
        target = dest / file.name
        if not target.exists() or target.read_bytes() != data:
            target.write_bytes(data)
    return dest
