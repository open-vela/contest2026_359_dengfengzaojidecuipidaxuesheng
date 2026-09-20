#!/usr/bin/env python3
from pathlib import Path

mp = Path("/mnt/c/Users/flash/Desktop/openvela-contest/contest2026_359_dengfengzaojidecuipidaxuesheng/firmware/v3/esp32p4-nsh-v3.2/nuttx-v3.2.map")
text = mp.read_text(encoding="utf-8", errors="replace")
for name in ("bootloader_init", "bootloader_hardware_init", "bootloader_ana_reset_config",
             "bootloader_init_mem", "map_rom_segments", "esp_cpu_intr_set",
             "cache_hal_init", "bootloader_flash_xmc"):
    print("====", name)
    n = 0
    for line in text.splitlines():
        if name in line and ("0x" in line or ".text" in line):
            print(line[:220])
            n += 1
            if n >= 6:
                break

hal = Path("/home/flash/vela-p4/nuttx/arch/risc-v/src/chip/esp-hal-3rdparty")
mwdt = hal / "components/esp_hal_wdt/esp32p4/include/hal/mwdt_ll.h"
print("==== mwdt_ll flashboot ====")
t = mwdt.read_text(encoding="utf-8", errors="replace")
for i, line in enumerate(t.splitlines(), 1):
    if "flashboot" in line.lower() or "wkey" in line.lower() or "WDT_WKEY" in line or "disable" in line.lower() and "static inline" in line:
        print(f"{i}|{line}")

print("==== chip_rev macro ====")
for p in hal.rglob("chip_revision.h"):
    tt = p.read_text(encoding="utf-8", errors="replace")
    i = tt.find("ESP_CHIP_REV_ABOVE")
    print(p, tt[i:i+400] if i>=0 else "no")

print("==== TIMG0 base hw_ver1 ====")
reg = hal / "components/soc/esp32p4/register/hw_ver1/soc/timer_group_reg.h"
t = reg.read_text(encoding="utf-8", errors="replace")
for line in t.splitlines()[:40]:
    if "DR_REG" in line or "WDT" in line and "WKEY" in line or "FLASHBOOT" in line:
        print(line)
