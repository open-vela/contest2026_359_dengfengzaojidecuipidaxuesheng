from pathlib import Path
import serial,time,re,json,sys
r=Path(__file__).resolve().parent
candidate=int(sys.argv[1]);label=sys.argv[2];cycles=int(sys.argv[3]) if len(sys.argv)>3 else 6
assert label.replace('-','').isalnum();record=r/(label+'.json');assert not record.exists()
c=r.parent/'network-share-20261026'/('candidate-'+str(candidate))
identity=json.loads((c/'firmware/BLE-IDENTITY.json').read_text())
state={'candidate':candidate,'identity':identity['address'],'cycles':[],'passed':False}
def save():record.write_text(json.dumps(state,indent=2)+'\n')
s=serial.Serial(port=None,baudrate=115200,timeout=.03,write_timeout=3);s.dtr=False;s.rts=False;s.port='COM4'
start=time.monotonic()
with s,(r/(label+'.log')).open('xb') as log:
 def read(seconds):
  data=bytearray();end=time.monotonic()+seconds
  while time.monotonic()<end:
   part=s.read(s.in_waiting or 1)
   if part:log.write(part);log.flush();data.extend(part)
  return data
 def command(text):
  read(.1);s.write(b'\x15')
  for byte in (text+'\r\n').encode():s.write(bytes([byte]));time.sleep(.012)
  data=bytearray();deadline=time.monotonic()+20
  while time.monotonic()<deadline:
   data.extend(read(.1))
   if re.search(rb'(?:^|[\r\n])nsh> ',data):return data.decode('utf-8',errors='replace')
  raise RuntimeError('No prompt; no replay')
 def snapshot():
  text=command('c6ble mouse status')
  m=re.search(r'host_ready=(\d) enabled=(\d) advertising=(\d) connected=(\d) encrypted=(\d) notify=(\d).*?epoch=(\d+) error=(-?\d+) ble_error=(-?\d+) store_error=(-?\d+)',text)
  assert m,text[-300:]
  row=dict(zip(['ready','enabled','advertising','connected','encrypted','notify','epoch','error','ble_error','store_error'],map(int,m.groups())))
  row['t_ms']=round((time.monotonic()-start)*1000);return row
 def timer_snapshot():
  text=command('c6ble timers')
  # Console label bytes can be lost/interleaved by background output. The
  # immutable firmware prints these five fields in this exact order. Preserve
  # every numeric digit and the conservation check; do not fuzz values.
  m=re.search(r'NPL_CALLOUT [^=\r\n]+=(\d+) [^=\r\n]+=(\d+) [^=\r\n]+=(\d+) [^=\r\n]+=(\d+) [^=\r\n]+=(\d+)',text)
  assert m,text[-300:]
  result=dict(zip(['expirations','dispatched','coalesced','pending','highwater'],map(int,m.groups())))
  assert result['expirations']==result['dispatched']+result['coalesced']+result['pending']
  return result
 try:
  assert identity['address'] in command('c6ble identity')
  for i in range(cycles):
   row={'cycle':i+1,'before':snapshot(),'samples':[]};state['cycles'].append(row);save()
   if candidate>=19:row['timers_before']=timer_snapshot()
   assert 'mouse stop: ok' in command('c6ble mouse stop')
   for _ in range(10):
    stopped=snapshot()
    if not stopped['connected']:break
    read(.2)
   assert not stopped['connected'];row['stopped']=stopped
   assert 'mouse start: ok' in command('c6ble mouse start')
   for _ in range(30):
    sample=snapshot();row['samples'].append(sample);save()
    if sample['connected']==sample['encrypted']==sample['notify']==1:break
    read(.2)
   assert sample['connected']==sample['encrypted']==sample['notify']==1
   settled=sample['epoch'];row['first_secure_epoch']=settled
   end=time.monotonic()+8
   while time.monotonic()<end:
    sample=snapshot();row['samples'].append(sample);save();read(.3)
   row['unexpected_epoch_change']=any(x['epoch']!=settled for x in row['samples'] if x['t_ms']>=next(x['t_ms'] for x in row['samples'] if x['epoch']==settled and x['encrypted']==x['notify']==1))
   row['final']=row['samples'][-1];save();print('CYCLE',i+1,'secure',settled,'final',row['final']['epoch'],'changed',row['unexpected_epoch_change'],flush=True)
   if candidate>=19:
    row['timers_after']=timer_snapshot();assert row['timers_after']['dispatched']>row['timers_before']['dispatched'];save()
  state['passed']=all(not x['unexpected_epoch_change'] and x['final']['connected']==x['final']['encrypted']==x['final']['notify']==1 for x in state['cycles']);save()
 except BaseException as exc:state['error']=type(exc).__name__+': '+str(exc)[:180];save();raise
