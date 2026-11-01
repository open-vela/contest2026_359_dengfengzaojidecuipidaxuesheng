"""Opt-in storage generation guard for retained offline-model sessions.

Wrap the known FAT mutation and SPI block-write entrypoints, preserving their
original bodies/arguments/results. All FAT volumes conservatively invalidate
the receipt; this is not a claim to cure SPI transport failures.
"""
import hashlib
from pathlib import Path
import re

MARKER = 'ESPCLAW LOCAL SESSION STORAGE GUARD'
FAT = 'fs/fat/fs_fat32.c'
SPI = 'drivers/mmcsd/mmcsd_spi.c'
CONTRACT = {
    'fat_open': ('2f85b5f0f9a0842459cbf69070d1a5a6af04cacdb8b527e9f1bf66119e74e304', 'filep, relpath, oflags, mode'),
    'fat_write': ('61fcd67e1b4076d0448279598576876e663a625e51847049b2a564c58f3924b1', 'filep, buffer, buflen'),
    'fat_truncate': ('ac2ff0bfbccacecfb9a437ba2158b78ac7980cb3ab98ec3bd840811ebb49f655', 'filep, length'),
    'fat_rename': ('f424935957fb536866fd8425efbc972cb0158f8024096161b7c5937d07767681', 'mountpt, oldrelpath, newrelpath'),
    'fat_unlink': ('56c9302aa07b3f45d9cbb387a58c99e281ba52ef6cbf4ee744a7246eb2b89352', 'mountpt, relpath'),
    'fat_bind': ('f589a0c82fd69825ba3a4d843b6b323f4bd9b730dd8df0040befc4b5b76915b5', 'blkdriver, data, handle'),
    'fat_unbind': ('55f8e0b58ffda105cd3a4958c5a8b9729fef5db97f7747b52bb633bd5e0b5e8f', 'handle, blkdriver, flags'),
    'fat_seek': ('cde27a10d8b4345b0b224422a1b16fe2e06b349f354c49ea63c3ccfca97eab05', 'filep, offset, whence'),
    'mmcsd_write': ('c2bf111fd3e76034c102c37d4648b9530e0c9c871e47407cf1e981548b894f3b', 'inode, buffer, start_sector, nsectors'),
}
FAT_SUPPORT = '''/* ESPCLAW LOCAL SESSION STORAGE GUARD: FAT */

const uint32_t g_fat_local_session_guard_abi = 1;
extern void mmcsd_local_session_change_begin(void)
  __attribute__((weak));
extern void mmcsd_local_session_change_end(void)
  __attribute__((weak));

/* END ESPCLAW LOCAL SESSION STORAGE GUARD: FAT */

'''
SPI_SUPPORT = '''/* ESPCLAW LOCAL SESSION STORAGE GUARD: SPI */

static uint32_t g_local_session_generation;
static uint32_t g_local_session_writers;
static uint32_t g_local_session_exhausted;
extern const uint32_t g_fat_local_session_guard_abi __attribute__((weak));

void mmcsd_local_session_change_begin(void)
{
  uint32_t value;

  __atomic_add_fetch(&g_local_session_writers, 1, __ATOMIC_SEQ_CST);
  value = __atomic_add_fetch(&g_local_session_generation, 1,
                            __ATOMIC_SEQ_CST);
  if (value == 0)
    {
      __atomic_store_n(&g_local_session_exhausted, 1, __ATOMIC_SEQ_CST);
    }
}

void mmcsd_local_session_change_end(void)
{
  uint32_t value;

  value = __atomic_add_fetch(&g_local_session_generation, 1,
                            __ATOMIC_SEQ_CST);
  if (value == 0)
    {
      __atomic_store_n(&g_local_session_exhausted, 1, __ATOMIC_SEQ_CST);
    }
  __atomic_sub_fetch(&g_local_session_writers, 1, __ATOMIC_SEQ_CST);
}

int mmcsd_local_session_stamp(FAR uint32_t *generation)
{
  uint32_t before;
  uint32_t after;
  uint32_t writers;

  if (&g_fat_local_session_guard_abi == NULL ||
      g_fat_local_session_guard_abi != 1)
    {
      return -ENOTSUP;
    }
  if (__atomic_load_n(&g_local_session_exhausted, __ATOMIC_SEQ_CST) != 0)
    {
      return -EOVERFLOW;
    }

  before = __atomic_load_n(&g_local_session_generation, __ATOMIC_SEQ_CST);
  writers = __atomic_load_n(&g_local_session_writers, __ATOMIC_SEQ_CST);
  after = __atomic_load_n(&g_local_session_generation, __ATOMIC_SEQ_CST);
  if (writers != 0 || before != after)
    {
      return -EAGAIN;
    }

  if (__atomic_load_n(&g_local_session_exhausted, __ATOMIC_SEQ_CST) != 0)
    {
      return -EOVERFLOW;
    }

  *generation = after;
  return OK;
}

/* END ESPCLAW LOCAL SESSION STORAGE GUARD: SPI */

'''


def definition(text, name):
    matches = list(re.finditer(r'^static\s+(int|ssize_t|off_t)\s+' + name + r'\s*\([^;{}]+\)\n\{', text, re.M))
    if len(matches) != 1:
        raise RuntimeError('Unrecognized entrypoint: ' + name)
    match = matches[0]; end = match.end(); depth = 1
    while depth:
        if text[end] == '{': depth += 1
        elif text[end] == '}': depth -= 1
        end += 1
    return text[match.start():end], match[1]


def wrapper(body, name, kind):
    header = body[:body.index('\n{')]
    condition = '((oflags & O_ACCMODE) != O_RDONLY ||\n             (oflags & (O_CREAT | O_TRUNC)) != 0)' if name == 'fat_open' else \
                '(filep->f_oflags & O_ACCMODE) != O_RDONLY' if name == 'fat_seek' else 'true'
    calls = CONTRACT[name][1]
    begin = 'mmcsd_local_session_change_begin()'
    end = 'mmcsd_local_session_change_end()'
    if name.startswith('fat_'):
        condition += ' &&\n            mmcsd_local_session_change_begin != NULL &&\n            mmcsd_local_session_change_end != NULL'
    return header + '\n{\n  bool tracked;\n  ' + kind + ' ret;\n\n' + \
        '  tracked = ' + condition + ';\n  if (tracked)\n    {\n      ' + begin + ';\n    }\n\n' + \
        '  ret = ' + name + '_local_session_body(\n    ' + calls + ');\n\n' + \
        '  if (tracked)\n    {\n      ' + end + ';\n    }\n\n  return ret;\n}'


def renamed_body(body, name):
    return body.replace(name+'(',name+'_local_session_body(\n    ',1)


def restored_body(body, name):
    return body.replace(name+'_local_session_body(\n    ',name+'(',1)


def transform(text, relative, enabled):
    names = [name for name in CONTRACT if name.startswith('fat_') == (relative == FAT)]
    support = FAT_SUPPORT if relative == FAT else SPI_SUPPORT
    already = MARKER in text
    if already:
        if text.count(support) != 1:
            raise RuntimeError('Partial session guard support')
        text = text.replace(support, '', 1)
        for name in names:
            renamed, kind = definition(text, name + '_local_session_body')
            original = restored_body(renamed,name)
            expected = renamed + '\n\n' + wrapper(original, name, kind)
            if text.count(expected) != 1:
                raise RuntimeError('Session guard wrapper changed: ' + name)
            text = text.replace(expected, original, 1)
    for name in names:
        original, kind = definition(text, name)
        if hashlib.sha256(original.encode()).hexdigest() != CONTRACT[name][0]:
            raise RuntimeError('Original function changed; review required: ' + name)
    if not enabled:
        return text
    first = 'fat_open' if relative == FAT else 'mmcsd_write'
    match = re.search(r'^static\s+(?:int|ssize_t)\s+' + first + r'\(', text, re.M)
    assert match
    text = text[:match.start()] + support + text[match.start():]
    for name in names:
        original, kind = definition(text, name)
        renamed = renamed_body(original,name)
        text = text.replace(original, renamed + '\n\n' + wrapper(original, name, kind), 1)
    return text


def apply(nuttx, enabled=False):
    result = dict(enabled=bool(enabled), sources={}, original_function_sha256={name: value[0] for name, value in CONTRACT.items()},
                  clock_timeout_retries_unchanged=True, original_io_bodies_preserved=True)
    # Validate both files before writing either prepared source.
    updates = {}
    for relative in (FAT, SPI):
        path = Path(nuttx)/relative
        old = path.read_bytes(); new = transform(old.decode(), relative, enabled).encode()
        updates[path] = (old, new)
        result['sources'][relative] = hashlib.sha256(new).hexdigest()
    for path, (old, new) in updates.items():
        if old != new: path.write_bytes(new)
    return result
