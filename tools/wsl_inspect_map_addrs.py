#!/usr/bin/env python3
from pathlib import Path
mp = Path("/home/flash/vela-p4/nuttx/nuttx.map")
if not mp.exists():
    mp = Path("/mnt/c/Users/flash/Desktop/openvela-contest/contest2026_359_dengfengzaojidecuipidaxuesheng/firmware/v3/esp32p4-nsh-v3.2/nuttx-v3.2.map")
text = mp.read_text(encoding="utf-8", errors="replace")
needles = [
    "__esp_start", "__start", "esp_cpu_intr_set_ivt_addr", "esp_cpu_intr_set_mtvt_addr",
    "bootloader_clear_bss_section", "bootloader_init", "bootloader_hardware_init",
    "bootloader_clock_configure", "bootloader_console_init", "bootloader_init_ext_mem",
    "bootloader_flash_update_id", "map_rom_segments", "_vector_table", "_mtvt_table",
    "ets_printf",
]
for name in needles:
    print("====", name)
    n = 0
    for line in text.splitlines():
        if name in line and n < 4:
            print(line[:200])
            n += 1
    if n == 0:
        print("NOT FOUND")
