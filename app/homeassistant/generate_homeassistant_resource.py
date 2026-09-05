#!/usr/bin/env python3
"""Generate the built-in Home Assistant Quick App C resource."""

from pathlib import Path

ROOT = Path(__file__).resolve().parent
SOURCE = ROOT / "homeassistant" / "app.js"
OUTPUT = ROOT / "homeassistant_resource.c"


def main() -> None:
    data = SOURCE.read_bytes()
    rows = []
    for offset in range(0, len(data), 12):
        rows.append("  " + ", ".join(f"0x{x:02x}" for x in data[offset:offset + 12]) + ",")
    OUTPUT.write_text(
        "/* Auto-generated from homeassistant/app.js. */\n\n#include <stddef.h>\n\n"
        "static const unsigned char g_homeassistant_app_js[] =\n{\n"
        + "\n".join(rows)
        + "\n};\n\nconst char *homeassistant_get_app_js(unsigned int *len)\n{\n"
          "  if (len != NULL)\n    {\n      *len = sizeof(g_homeassistant_app_js);\n    }\n\n"
          "  return (const char *)g_homeassistant_app_js;\n}\n",
        encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
