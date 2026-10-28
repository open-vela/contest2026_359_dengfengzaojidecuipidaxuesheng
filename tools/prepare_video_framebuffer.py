"""Opt-in framebuffer allocation alignment for the existing PPA DMA ABI."""
import hashlib
from pathlib import Path

NAME='boards/risc-v/esp32p4/esp32p4-function-ev-board/src/esp32p4_lcd.c'
OLD='  g_lcd_fb = memalign(64, g_lcd_plane.fblen);'
NEW='''  /* GLASS VIDEO DMA FRAMEBUFFER ALIGNMENT
   * Keep the base and every whole display page usable by PPA/DMA2D.
   */

  g_lcd_fb = memalign(128, g_lcd_plane.fblen);'''


def apply(nuttx, enabled):
    path=Path(nuttx)/NAME
    source=path.read_text()
    if enabled:
        if NEW not in source:
            if source.count(OLD)!=1:
                raise RuntimeError('Unrecognized framebuffer allocator; review alignment before DMA')
            source=source.replace(OLD,NEW,1)
    elif NEW in source:
        source=source.replace(NEW,OLD,1)
    if path.read_text()!=source:path.write_text(source)
    return {NAME:hashlib.sha256(path.read_bytes()).hexdigest()}
