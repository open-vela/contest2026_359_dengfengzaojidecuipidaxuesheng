#!/usr/bin/env python3
"""Wire the dedicated MCPWM1 LCD backlight into an isolated NuttX tree."""
from pathlib import Path
import hashlib


def apply(nuttx: Path):
    chip = nuttx / 'arch/risc-v/src/common/espressif'
    board = nuttx / 'boards/risc-v/esp32p4/esp32p4-function-ev-board'
    changed = []

    def save(path, text):
        if path.read_text() != text:
            path.write_text(text)
        changed.append(path)

    path = board / 'Kconfig'
    text = path.read_text()
    if 'config ESP32P4_LCD_BACKLIGHT\n' not in text:
        text += '''
config ESP32P4_LCD_BACKLIGHT
	bool "Dedicated Function EV LCD backlight PWM"
	default n
	depends on ESP32P4_FUNCTION_EV_BOARD_LCD && !PM
	select PWM
	select ESP_MCPWM
	---help---
		Register /dev/backlight0 on MCPWM group 1, GPIO26.
		Keep LEDC/header pins and LCD reset GPIO27 unchanged.
'''
    save(path, text)

    path = chip / 'Make.defs'
    text = path.read_text()
    if 'CHIP_CSRCS += esp_lcd_backlight.c' not in text:
        anchor = 'ifeq ($(CONFIG_ESPRESSIF_MIPI_DSI),y)'
        assert text.count(anchor) == 1
        text = text.replace(anchor, 'ifeq ($(CONFIG_ESP32P4_LCD_BACKLIGHT),y)\n\tCHIP_CSRCS += esp_lcd_backlight.c\nendif\n\n' + anchor)
    save(path, text)

    path = chip / 'CMakeLists.txt'
    text = path.read_text()
    if 'list(APPEND SRCS esp_lcd_backlight.c)' not in text:
        anchor = 'if(CONFIG_ESPRESSIF_MIPI_DSI)'
        assert text.count(anchor) == 1
        text = text.replace(anchor, 'if(CONFIG_ESP32P4_LCD_BACKLIGHT)\n  list(APPEND SRCS esp_lcd_backlight.c)\nendif()\n\n' + anchor)
    save(path, text)

    path = board / 'src/esp32p4_lcd.c'
    text = path.read_text()
    if '"espressif/esp_lcd_backlight.h"' not in text:
        anchor = '#include "espressif/esp_gpio.h"'
        assert text.count(anchor) == 1
        text = text.replace(anchor, anchor + '\n#ifdef CONFIG_ESP32P4_LCD_BACKLIGHT\n#  include "espressif/esp_lcd_backlight.h"\n#endif')
    if 'esp_lcd_backlight_initialize();' not in text:
        anchor = '  esp_gpiowrite(LCD_GPIO_BL, true);'
        assert text.count(anchor) == 1
        text = text.replace(anchor, anchor + '''
#ifdef CONFIG_ESP32P4_LCD_BACKLIGHT
  ret = esp_lcd_backlight_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "Backlight registration failed: %d\\n", ret);
    }
#endif
''')
    save(path, text)
    return {'gpio': 26, 'group': 1, 'frequency_hz': 50000,
            'device': '/dev/backlight0', 'runtime_tested': False,
            'prepared_sha256': {str(p.relative_to(nuttx)): hashlib.sha256(p.read_bytes()).hexdigest() for p in changed},
            'tool_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
