from pathlib import Path
import asyncio,json,re,subprocess,sys,uuid,time,hashlib
from winrt.windows.devices.bluetooth import BluetoothLEDevice,BluetoothAddressType,BluetoothCacheMode
from winrt.windows.devices.bluetooth.genericattributeprofile import GattWriteOption,GattProtectionLevel,GattClientCharacteristicConfigurationDescriptorValue as Cccd
from winrt.windows.storage.streams import DataWriter,DataReader
r=Path(__file__).resolve().parent;root=r.parent.parent
candidate=int(sys.argv[1]);label=sys.argv[2];count=int(sys.argv[3]);assert label.replace('-','').isalnum()
c=r.parent/'network-share-20261026'/('candidate-'+str(candidate));identity=json.loads((c/'firmware/BLE-IDENTITY.json').read_text())
u=json.loads((r.parent/'ble-channel-20261027/UUIDS.json').read_text());record=r/(label+'.json');assert not record.exists()
state={'candidate':candidate,'image_sha256':hashlib.sha256((c/'firmware/nuttx.bin').read_bytes()).hexdigest(),'identity':identity['address'],'cycles':[],'passed':False};start=time.monotonic()
def save():record.write_text(json.dumps(state,indent=2)+'\n')
def packet(data):
 writer=DataWriter();writer.write_bytes(data);result=writer.detach_buffer();writer.close();return result
def unpack(buffer):
 reader=DataReader.from_buffer(buffer);data=bytearray(reader.unconsumed_buffer_length);reader.read_bytes(data);reader.close();return bytes(data)
def fnv(data):
 n=2166136261
 for b in data:n=((n^b)*16777619)&0xffffffff
 return f'{n:08x}'
async def console(phase,*commands):
 assert all(len(x.encode())<80 for x in commands)
 def run():return subprocess.run([str(root/'.hardware-venv/Scripts/python.exe'),'-X','utf8',str(c/'console.py'),label+'-'+phase,*commands],capture_output=True,encoding='utf-8',timeout=90,check=True).stdout
 return await asyncio.to_thread(run)
def mouse(text):
 match=re.search(r'host_ready=1 enabled=(\d) advertising=(\d) connected=(\d) encrypted=(\d) notify=(\d).*?epoch=(\d+).*?store_error=(-?\d+)',text)
 assert match,text[-400:]
 return dict(zip(['enabled','advertising','connected','encrypted','notify','epoch','store_error'],map(int,match.groups())))
def secure(m):return m['connected']==m['encrypted']==m['notify']==1 and not m['store_error']
def timers(text):
 match=re.search(r'NPL_CALLOUT [^=\r\n]+=(\d+) [^=\r\n]+=(\d+) [^=\r\n]+=(\d+) [^=\r\n]+=(\d+) [^=\r\n]+=(\d+)',text)
 assert match,text[-400:]
 s=dict(zip(['expirations','dispatched','coalesced','pending','highwater'],map(int,match.groups())))
 assert s['expirations']==s['dispatched']+s['coalesced']+s['pending'],s
 return s
async def main():
 device=None;service=None;tx=None;token=None;subscribed=False
 try:
  for cycle in range(1,count+1):
   row={'cycle':cycle,'samples':[]};state['cycles'].append(row);save()
   text=await console(f'{cycle}a','c6ble identity','c6ble mouse status','c6ble channel status',*(['c6ble timers'] if candidate>=19 else []))
   assert identity['address'] in text;before=mouse(text);assert secure(before);epoch=before['epoch'];row['before']=before
   if candidate>=19:row['timers_before']=timers(text)
   device=await asyncio.wait_for(BluetoothLEDevice.from_bluetooth_address_async(int(identity['address'].replace(':',''),16),BluetoothAddressType.RANDOM),20)
   assert device is not None and device.device_information.pairing.is_paired
   result=await asyncio.wait_for(device.get_gatt_services_async(BluetoothCacheMode.UNCACHED),30);assert result.status.name=='SUCCESS'
   for entry in result.services:
    if entry.uuid==uuid.UUID(u['service']):service=entry
    else:entry.close()
   assert service is not None
   chars={}
   for name in ['rx','tx']:
    result=await service.get_characteristics_for_uuid_async(uuid.UUID(u[name]),BluetoothCacheMode.UNCACHED);assert result.status.name=='SUCCESS' and len(result.characteristics)==1
    chars[name]=result.characteristics[0];chars[name].protection_level=GattProtectionLevel.ENCRYPTION_REQUIRED
   tx=chars['tx'];events=asyncio.Queue();loop=asyncio.get_running_loop()
   def changed(sender,args):loop.call_soon_threadsafe(events.put_nowait,unpack(args.characteristic_value))
   token=tx.add_value_changed(changed)
   result=await tx.write_client_characteristic_configuration_descriptor_async(Cccd.NOTIFY);assert result.name=='SUCCESS';subscribed=True
   for n in range(2):
    data=bytes((cycle*17+n*31+i*7)&255 for i in range(20))
    result=await chars['rx'].write_value_with_result_async(packet(data),GattWriteOption.WRITE_WITH_RESPONSE);assert result.status.name=='SUCCESS'
    text=await console(f'{cycle}r{n}','c6ble channel receive '+str(epoch),'c6ble channel send '+str(epoch)+' '+data.hex(),'c6ble mouse release','c6ble mouse status')
    assert 'bytes=20 fnv32='+fnv(data) in text and 'channel send result=0' in text and secure(mouse(text))
    assert await asyncio.wait_for(events.get(),15)==data
    result=await tx.read_value_async(BluetoothCacheMode.UNCACHED);assert result.status.name=='SUCCESS' and unpack(result.value)==data
   row['byte_verified_rounds']=2
   result=await tx.write_client_characteristic_configuration_descriptor_async(Cccd.NONE);assert result.name=='SUCCESS';subscribed=False
   tx.remove_value_changed(token);token=None;tx=None;service.close();service=None;device.close();device=None
   # Test the exact client-exit -> mouse stop/start ordering previously seen.
   text=await console(f'{cycle}closed','c6ble mouse status');row['after_client_close']=mouse(text)
   assert secure(row['after_client_close']) and row['after_client_close']['epoch']==epoch
   assert 'mouse stop: ok' in await console(f'{cycle}stop','c6ble mouse stop')
   for i in range(10):
    text=await console(f'{cycle}s{i}','c6ble mouse status')
    if not mouse(text)['connected']:break
    await asyncio.sleep(.2)
   assert not mouse(text)['connected']
   assert 'mouse start: ok' in await console(f'{cycle}start','c6ble mouse start')
   for i in range(20):
    text=await console(f'{cycle}q{i}','c6ble mouse status');m=mouse(text);m['t_ms']=round((time.monotonic()-start)*1000);row['samples'].append(m);save()
    if secure(m):break
    await asyncio.sleep(.2)
   assert secure(m);settled=m['epoch'];row['first_secure_epoch']=settled
   deadline=time.monotonic()+8
   for i in range(20):
    if time.monotonic()>=deadline:break
    text=await console(f'{cycle}h{i}','c6ble mouse status');m=mouse(text);m['t_ms']=round((time.monotonic()-start)*1000);row['samples'].append(m);save()
    assert secure(m) and m['epoch']==settled,('unexpected disconnect',row)
   row['final']=m
   if candidate>=19:
    row['timers_after']=timers(await console(f'{cycle}timers','c6ble timers'))
    assert row['timers_after']['dispatched']>row['timers_before']['dispatched']
   row['passed']=True;save();print('BUSINESS_RECONNECT_OK',cycle,epoch,settled,flush=True)
  state['passed']=True;save()
 except BaseException as exc:state['error']=type(exc).__name__+': '+str(exc)[:400];save();raise
 finally:
  if tx is not None and subscribed:
   try:await asyncio.wait_for(tx.write_client_characteristic_configuration_descriptor_async(Cccd.NONE),10)
   except Exception:pass
  if tx is not None and token is not None:tx.remove_value_changed(token)
  if service is not None:service.close()
  if device is not None:device.close()
asyncio.run(main())
