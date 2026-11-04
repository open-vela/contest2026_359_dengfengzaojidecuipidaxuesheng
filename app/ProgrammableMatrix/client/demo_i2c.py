#!/usr/bin/env python3
"""Demo: configure an I2C sensor via CH446Q with no manual key entry."""
import os
from jbox_api import MatrixClient, discover_devices

def choose_host():
    if os.getenv('JBOX_IP'):return os.environ['JBOX_IP']
    found=discover_devices()
    if len(found)!=1:raise RuntimeError('Need one LAN JBOX device or set JBOX_IP')
    return found[0]['host']

with MatrixClient(choose_host()) as j:
    # IO0=ESP GPIO1 -> CN1-1/2, IO1=ESP GPIO2 -> CN1-3/4.
    # Must add actual external pull-ups on the I2C bus (e.g. 4.7 kOhm).
    j.route_set([[40,0],[41,1]])
    j.i2c_setup(0,sda=0,scl=1,hz=50000)
    print('I2C addresses:',[hex(a) for a in j.i2c_scan(0)])
    # Example sensor transaction after confirming its register map:
    # print(j.i2c_xfer(0,0x44,write=b'\x00',read=2).hex())
    j.i2c_close(0)
