"""Run explicit console commands without retry/reset or logging configuration."""
from pathlib import Path
import sys,time,re,serial
sys.stdout.reconfigure(encoding='utf-8')
root=Path(__file__).resolve().parent
name=sys.argv[1];commands=sys.argv[2:]
s=serial.Serial(port=None,baudrate=115200,timeout=.05,write_timeout=3)
s.dtr=False;s.rts=False;s.port='COM4'
with s,(root/(name+'.log')).open('xb') as log:
 def read(seconds):
  data=bytearray();end=time.monotonic()+seconds
  while time.monotonic()<end:
   part=s.read(s.in_waiting or 1)
   if part:log.write(part);log.flush();data.extend(part)
  return data
 read(.5)
 for cmd in commands:
  s.write(b'\x15');read(.1)
  data=bytearray();reply=bytearray();terminated=False
  for b in (cmd+'\r\n').encode():
   s.write(bytes([b]));terminated=terminated or b in (10,13)
   # Keep USB input serviced while sending console echo/background logs.
   # Retain responses which arrive as soon as CR is sent, before LF.
   part=read(.045);data.extend(part)
   if terminated:reply.extend(part)
  end=time.monotonic()+45
  while time.monotonic()<end:
   part=read(.15);data.extend(part);reply.extend(part)
   if re.search(rb'(?:^|[\r\n])nsh> ',reply):break
  else:raise RuntimeError('No prompt; no replay/reset')
  print(data.decode('utf-8',errors='replace'),flush=True)

