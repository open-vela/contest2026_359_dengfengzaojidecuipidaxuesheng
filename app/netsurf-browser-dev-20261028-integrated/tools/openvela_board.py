#!/usr/bin/env python3
"""Run explicit NSH commands on the test board, preserving raw evidence."""
import argparse,time,re
from pathlib import Path
import serial
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--port',default='COM4')
p.add_argument('--log',type=Path,required=True)
p.add_argument('--command',action='append',default=[])
p.add_argument('--timeout',type=float,default=60)
p.add_argument('--listen',type=float,default=0,help='capture asynchronous board logs after the last command')
p.add_argument('--stop-file',type=Path,help='end only the asynchronous listen phase when this file exists')
p.add_argument('--command-delay',type=float,default=0,help='capture continuously for this interval between commands')
a=p.parse_args()
s=serial.Serial(port=None,baudrate=115200,timeout=.1,write_timeout=5)
s.dtr=False;s.rts=False;s.port=a.port;s.open()
a.log.parent.mkdir(parents=True,exist_ok=True)
with s,a.log.open('wb') as log:
    time.sleep(.2);s.reset_input_buffer()
    for index,command in enumerate(a.command):
        if '\r' in command or '\n' in command:raise ValueError('one NSH line per command')
        if len(command.encode())>75:raise ValueError('NSH line exceeds the prepared console limit: '+command)
        log.write(('\nCOMMAND: '+command+'\n').encode());log.flush()
        encoded=(command+'\r').encode()
        for offset in range(0,len(encoded),8):s.write(encoded[offset:offset+8]);time.sleep(.04)
        begin=time.monotonic();response=bytearray()
        while time.monotonic()-begin<a.timeout:
            data=s.read(8192)
            if data:response.extend(data);log.write(data);log.flush()
            if re.search(rb'(?:nsh|sh)>',response):break
        else:raise TimeoutError('NSH prompt timeout after '+command)
        if re.search(rb'nsh: [^\r\n]*(?:failed|not found|unreachable|No such|Permission denied)',response,re.I):
            raise RuntimeError('NSH command failed; see '+str(a.log))
        print('COMMAND_DONE',command.split()[0],round(time.monotonic()-begin,2),flush=True)
        if a.command_delay>0:
            # Keep asynchronous network events between a Refresh and Stop;
            # resetting input here would discard the cancellation evidence.
            end=time.monotonic()+(a.command_delay if index+1<len(a.command) else 0)
            while time.monotonic()<end:
                data=s.read(8192)
                if data:log.write(data);log.flush()
        else:
            time.sleep(.1);s.reset_input_buffer()
    end=time.monotonic()+a.listen
    while time.monotonic()<end:
        if a.stop_file and a.stop_file.exists():break
        data=s.read(8192)
        if data:log.write(data);log.flush()
print('BOARD_LOG',a.log)
