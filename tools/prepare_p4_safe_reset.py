"""Prepare an internal-code/stack, SMP-aware P4 software-reset closure."""
from pathlib import Path
import hashlib

MARK = 'ESPCLAW P4 SAFE RESET'


def once(text, old, new, label):
    if text.count(old) != 1:
        raise RuntimeError('Unexpected safe-reset anchor: ' + label)
    return text.replace(old, new, 1)


def apply(nuttx):
    root = Path(nuttx)
    sdk = root / 'arch/risc-v/src/esp32p4/esp-hal-3rdparty'
    common = root / 'arch/risc-v/src/common/espressif'
    pending = {}
    reset = common / 'esp_systemreset.c'
    text = reset.read_text()
    if MARK not in text:
        text = once(text, '  esp_restart_noos();',
                    '  /* ' + MARK + ': switch before disabling cache. */\n'
                    '  extern void esp_safe_reset_entry(void) '
                    '__attribute__((noreturn));\n'
                    '  esp_safe_reset_entry();', 'entry')
    pending[reset] = text

    internal = sdk / 'components/esp_system/port/soc/esp32p4/system_internal.c'
    text = internal.read_text()
    if MARK not in text:
        guard = '#if !CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE'
        if text.count(guard) != 4:
            raise RuntimeError('Unexpected SDK reset SMP guards')
        text = text.replace(guard, '#if defined(CONFIG_SMP) && CONFIG_SMP_NCPUS > 1')
        text = once(text, '#include "sdkconfig.h"',
                    '#include "sdkconfig.h"\n#include <nuttx/config.h>\n'
                    '/* ' + MARK + ': NuttX, not the SDK adapter, owns SMP. */\n'
                    'extern int esp_mipi_dsi_reset_prepare(void);', 'SDK includes')
        text = once(text, '    // Disable TG0/TG1 watchdogs',
                    '    /* Other hart is stopped; local IRQs are disabled.\n'
                    '     * No lock or scheduler call is permitted here.\n'
                    '     * The RTC WDT is already armed if quiescence fails. */\n'
                    '    (void)esp_mipi_dsi_reset_prepare();\n\n'
                    '    // Disable TG0/TG1 watchdogs', 'display shutdown')
    pending[internal] = text

    make = root / 'arch/risc-v/src/esp32p4/Make.defs'
    text = make.read_text()
    if MARK not in text:
        text += '\n# ' + MARK + '\nCHIP_CSRCS += esp_safe_reset.c\n'
    pending[make] = text

    sections = root / 'boards/risc-v/esp32p4/common/scripts/esp32p4_sections.rev3.ld'
    text = sections.read_text()
    if MARK not in text:
        code = '\n'.join('    *libarch.a:' + name +
                          '.*(.literal .literal.* .text .text.*)'
                          for name in ['system_internal', 'esp_systemreset', 'esp_safe_reset'])
        data = '\n'.join('    *libarch.a:' + name +
                          '.*(.rodata .rodata.* .srodata .srodata.*)'
                          for name in ['system_internal', 'esp_systemreset', 'esp_safe_reset'])
        anchor = '    *(.iram1)\n    *(.iram1.*)'
        if anchor not in text:
            anchor = '    *(.iram1 .iram1.*)'
        text = once(text, anchor,
                    '    /* ' + MARK + ': complete SDK reset object closure. */\n'
                    + code + '\n' + anchor, 'internal code')
        anchor = '    *libarch.a:esp_psram_impl_ap_hex.*(.rodata .rodata.*)'
        text = once(text, anchor, data + '\n' + anchor, 'internal constants')
    pending[sections] = text

    for path, text in pending.items():
        if path.read_text() != text:
            path.write_text(text)
    return {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in pending}
