#!/usr/bin/env python3
"""Capture a complete hard-reset boot after an explicitly prepared flash."""
import argparse,time
from pathlib import Path
import serial
from esptool.reset import USBJTAGSerialReset,HardReset
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--port',default='COM4')
p.add_argument('--log',type=Path,required=True)
p.add_argument('--seconds',type=float,default=20)
a=p.parse_args()
s=serial.Serial(port=None,baudrate=115200,timeout=.1)
s.dtr=False;s.rts=False;s.port=a.port;s.open()
with s,a.log.open('wb') as log:
    # Windows usbser only commits some DTR changes on an RTS write. Use the
    # toolchain's USB-JTAG sequence before releasing normal boot strapping.
    USBJTAGSerialReset(s)()
    HardReset(s,uses_usb=True)()
    end=time.monotonic()+a.seconds
    while time.monotonic()<end:
        data=s.read(8192)
        if data:log.write(data);log.flush()
print('BOOT_CAPTURED',a.log)
