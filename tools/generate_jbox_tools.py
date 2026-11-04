#!/usr/bin/env python3
"""Generate the shared direct-tool / board MCP JBOX catalog and dispatch map."""
import json
from pathlib import Path


def integer(low, high, **extra):
    return dict(type='integer', minimum=low, maximum=high, **extra)


def array(items, maximum, minimum=0, **extra):
    return dict(type='array', items=items, minItems=minimum, maxItems=maximum, **extra)


generation = integer(1, 2147483647)
io = integer(0, 17)
bus = integer(0, 1)
slot = integer(0, 3, default=0)
data = array(integer(0, 255), 4096)
tools = []


def tool(name, operation, description, properties=None, required=(), connected=True):
    properties = dict(properties or {})
    if connected:
        properties = dict(generation=generation, **properties)
        required = ('generation', *required)
    description += (' Returns an async job id; use hardware_poll until done and ok. '
                    'Never replay an action after an uncertain receipt or timeout.')
    tools.append((name, 'jbox.' + operation, dict(
        name=name, description=description, inputSchema=dict(
            type='object', properties=properties, required=list(required),
            additionalProperties=False))))


tool('jbox_discover', 'discover',
     'Discover untrusted JBOX-S3 LAN candidates; return host, deviceId, paired and pairingOpen. '
     'Select the user-confirmed target; discovery neither binds nor controls it.',
     dict(timeoutMs=integer(100, 5000, default=1500)), connected=False)
tool('jbox_connect', 'connect',
     'Connect only the saved JBOX binding. Omitted host/deviceId use saved values; '
     'a changed IP must retain the bound deviceId. Does not pair, expose keys, replay routes '
     'or replace an active connection. Poll result returns authenticated and generation.',
     dict(host=dict(type='string', minLength=7, maxLength=15,
                    description='Unicast IPv4 from discovery; no URL or hostname.'),
          deviceId=dict(type='string', minLength=12, maxLength=12,
                        description='12 hexadecimal digits identifying the saved binding.'),
          timeoutMs=integer(100, 15000, default=5000)), connected=False)
tool('jbox_disconnect', 'disconnect',
     'Disconnect this JBOX session; S3 clears routes, releases buses and restores high-impedance IO.')
tool('jbox_caps', 'caps',
     'Read actual JBOX capabilities and ioGpios mapping. JBOX IO ids and matrix nodes are not P4 GPIOs.')
tool('jbox_status', 'status',
     'Read JBOX uptimeMs, heapFree, actual nets, ioMode and ioLease without changing outputs.')
tool('jbox_safe', 'safe',
     'Explicitly clear all JBOX routes, functions and peripherals while keeping the connection. '
     'This changes hardware state; use only when requested.')
tool('jbox_route_set', 'routeSet',
     'Replace the entire matrix routing with up to eight nets. Nodes 0..51 must be unique '
     'across all nets. This is not an incremental add; read current status first.',
     dict(nets=array(array(integer(0, 51), 52, uniqueItems=True), 8)), ('nets',))
tool('jbox_connect_nodes', 'connectNodes',
     'Add a connection between two different matrix node ids (0..51). '
     'The S3 retains its power-conflict and external-wiring checks.',
     dict(a=integer(0, 51), b=integer(0, 51)), ('a', 'b'))
tool('jbox_remove_node', 'removeNode', 'Remove one node from its matrix net.',
     dict(node=integer(0, 51)), ('node',))
tool('jbox_route_clear', 'routeClear', 'Explicitly clear all matrix routes; not a read-only diagnostic.')
tool('jbox_gpio_config', 'gpioConfig',
     'Configure one JBOX IO id: mode 0=input, 1=push-pull, 2=open-drain, 3=ADC; '
     'pull 0=none, 1=up, 2=down. initial is the initial output level.',
     dict(io=io, mode=integer(0, 3, default=0), pull=integer(0, 2, default=0),
          initial=integer(0, 1, default=0)), ('io',))
tool('jbox_gpio_write', 'gpioWrite',
     'Write JBOX IO bits selected by an 18-bit mask. levels must be a subset of mask; '
     'a transport receipt does not prove the external circuit worked.',
     dict(mask=integer(0, 262143), levels=integer(0, 262143)), ('mask', 'levels'))
tool('jbox_gpio_read', 'gpioRead', 'Read JBOX digital IO levels; ADC inputs use jbox_adc_read.',
     dict(mask=integer(0, 262143, default=262143)))
tool('jbox_adc_read', 'adcRead', 'Read raw ADC value from a JBOX IO configured for ADC.',
     dict(io=io), ('io',))
tool('jbox_pwm', 'pwm',
     'Set a JBOX PWM channel; io=255 stops that channel. Duty is raw 0..8191, not a percentage.',
     dict(channel=integer(0, 3), io=dict(type='integer', enum=[*range(18), 255]),
          hz=integer(10, 40000, default=1000), duty=integer(0, 8191, default=4096)),
     ('channel', 'io'))
tool('jbox_i2c_setup', 'i2cSetup',
     'Set up I2C with distinct JBOX SDA/SCL IO ids. Physical pull-up resistors are required; '
     'never connect a signal directly to power as a substitute.',
     dict(bus=bus, sda=io, scl=io, hz=integer(10000, 400000, default=50000)), ('bus', 'sda', 'scl'))
tool('jbox_i2c_transfer', 'i2cTransfer',
     'Transfer I2C to a 7-bit address 8..119. data is required (use [] for read-only); '
     'combined write/read length must not exceed 4096. Poll returns actual data and bytes.',
     dict(bus=bus, address=integer(8, 119), data=data, read=integer(0, 4096, default=0)),
     ('bus', 'address', 'data'))
tool('jbox_i2c_close', 'i2cClose', 'Release this JBOX I2C bus.', dict(bus=bus), ('bus',))
tool('jbox_spi_setup', 'spiSetup',
     'Set up SPI with distinct JBOX IO ids. miso=255 disables receive; '
     'matrix-related IO0..8 limit frequency to 2MHz. Start at low speed.',
     dict(bus=bus, mosi=io, miso=dict(type='integer', enum=[*range(18), 255]), sck=io, cs=io,
          mode=integer(0, 3, default=0), hz=integer(1000, 20000000, default=100000)),
     ('bus', 'mosi', 'miso', 'sck', 'cs'))
tool('jbox_spi_transfer', 'spiTransfer', 'Perform full-duplex SPI, returning actual data and bytes.',
     dict(bus=bus, data=array(integer(0, 255), 4096, 1)), ('bus', 'data'))
tool('jbox_spi_close', 'spiClose', 'Release this JBOX SPI bus.', dict(bus=bus), ('bus',))
tool('jbox_uart_setup', 'uartSetup',
     'Set up UART using distinct JBOX TX/RX IO ids. Matrix IO0..8 limit baud to 460800; '
     'parity 0=none, 1=odd, 2=even; stopbits 1 or 2.',
     dict(bus=bus, tx=io, rx=io, baud=integer(300, 921600, default=115200),
          parity=integer(0, 2, default=0), stopbits=integer(1, 2, default=1)), ('bus', 'tx', 'rx'))
tool('jbox_uart_write', 'uartWrite', 'Write UART bytes; poll returns the actual transmitted byte count.',
     dict(bus=bus, data=data), ('bus', 'data'))
tool('jbox_uart_read', 'uartRead', 'Read up to length UART bytes; a successful result may be a partial frame.',
     dict(bus=bus, length=integer(0, 4096, default=512), timeoutMs=integer(0, 500, default=100)), ('bus',))
tool('jbox_uart_close', 'uartClose', 'Release this JBOX UART bus.', dict(bus=bus), ('bus',))
tool('jbox_function_load', 'functionLoad',
     'Upload compiled JBoxScript bytecode to an S3 slot, not arbitrary P4 source. '
     'code length must be a multiple of four bytes; arguments is the function IO parameter count.',
     dict(slot=slot, arguments=integer(0, 4, default=0),
          code=array(integer(0, 255), 4096, 4)), ('code',))
tool('jbox_function_call', 'functionCall',
     'Call an explicitly requested S3 function with unique JBOX IO ids; poll returns up to 128 uint32 values.',
     dict(slot=slot, ios=array(io, 4, uniqueItems=True)), ('ios',))
tool('jbox_function_clear', 'functionClear', 'Clear one S3 function slot.', dict(slot=slot))

root = Path(__file__).resolve().parents[1]
out = root / 'app/espdl-quickapp/overlay/apps/system/desktop/glass_jbox_tools.h'
header = '/* Generated by tools/generate_jbox_tools.py; SPDX-License-Identifier: Apache-2.0 */\n'
header += 'static const struct {const char *name,*operation,*definition;} board_jbox_tools[]={\n'
for name, operation, definition in tools:
    packed = json.dumps(definition, ensure_ascii=False, separators=(',', ':'))
    header += '  {' + ','.join(json.dumps(x) for x in (name, operation, packed)) + '},\n'
header += '};\n'
out.write_text(header, encoding='utf-8', newline='\n')
print(f'{len(tools)} JBOX tools: {out.name}')
