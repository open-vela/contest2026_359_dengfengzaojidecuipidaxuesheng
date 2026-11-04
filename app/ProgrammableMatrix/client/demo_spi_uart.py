#!/usr/bin/env python3
"""Demo: hardware SPI and UART through DIRECT GPIO (no crosspoint loss)."""
import os
from jbox_api import MatrixClient, discover_devices

host=os.getenv('JBOX_IP')
if not host:
    devices=discover_devices()
    if len(devices)!=1:raise RuntimeError('Need one LAN JBOX device or set JBOX_IP')
    host=devices[0]['host']

with MatrixClient(host) as j:
    # IO9..17 are direct GPIO18,21,38,39,40,41,42,47,48.
    j.spi_setup(0,mosi=9,miso=10,sck=11,cs=12,mode=0,hz=1_000_000)
    print('SPI RX:',j.spi_xfer(0,b'\x9f\x00\x00\x00').hex())
    j.spi_close(0)
    # UART0 is reserved for programming/debug; this uses hardware UART1.
    j.uart_setup(0,tx=13,rx=14,baud=115200)
    j.uart_write(0,b'AT\r\n')
    print('UART RX:',repr(j.uart_read(0,512,200)))
    j.uart_close(0)
