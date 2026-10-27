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
  for b in (cmd+'\r\n').encode():s.write(bytes([b]));time.sleep(.045)
  data=bytearray();end=time.monotonic()+45
  while time.monotonic()<end:
   data.extend(read(.15))
   if re.search(rb'(?:^|[\r\n])nsh> ',data):break
  else:raise RuntimeError('No prompt; no replay/reset')
  print(data.decode('utf-8',errors='replace'),flush=True)

