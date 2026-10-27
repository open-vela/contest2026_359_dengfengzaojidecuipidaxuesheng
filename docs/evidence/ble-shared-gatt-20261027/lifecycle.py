from pathlib import Path
import asyncio,json,re,subprocess,uuid
from winrt.windows.devices.bluetooth import BluetoothLEDevice,BluetoothAddressType,BluetoothCacheMode
from winrt.windows.devices.bluetooth.genericattributeprofile import GattWriteOption
from winrt.windows.storage.streams import DataWriter

r=Path(__file__).resolve().parent;c=r.parent/'network-share-20261026/candidate-18'
record=r/'channel-lifecycle-18b.json';assert not record.exists()
identity=json.loads((c/'firmware/BLE-IDENTITY.json').read_text());u=json.loads((r/'UUIDS.json').read_text())
state={'identity':identity['address'],'passed':False}
def save():record.write_text(json.dumps(state,indent=2)+'\n')
async def console(label,*commands):
 def run():return subprocess.run([str(r.parent.parent/'.hardware-venv/Scripts/python.exe'),'-X','utf8',str(c/'console.py'),'channel-lifeb-'+label,*commands],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,encoding='utf-8',timeout=90,check=True).stdout
 return await asyncio.to_thread(run)
async def main():
 device=None;service=None
 try:
  before=await console('before','c6ble channel status','c6ble mouse status')
  assert 'connected=1 encrypted=1 notify=1' in before
  old=int(re.search(r'BLE_CHANNEL .*?epoch=(\d+)',before)[1]);state['old_epoch']=old;save()
  device=await BluetoothLEDevice.from_bluetooth_address_async(int(identity['address'].replace(':',''),16),BluetoothAddressType.RANDOM)
  result=await device.get_gatt_services_async(BluetoothCacheMode.UNCACHED);assert result.status.name=='SUCCESS'
  for entry in result.services:
   if entry.uuid==uuid.UUID(u['service']):service=entry
   else:entry.close()
  assert service
  result=await service.get_characteristics_for_uuid_async(uuid.UUID(u['rx']),BluetoothCacheMode.UNCACHED);assert result.status.name=='SUCCESS'
  writer=DataWriter();writer.write_bytes(b'\x12\x34\x56\x78');buffer=writer.detach_buffer();writer.close()
  result=await result.characteristics[0].write_value_with_result_async(buffer,GattWriteOption.WRITE_WITH_RESPONSE);assert result.status.name=='SUCCESS'
  text=await console('queued','c6ble channel status');assert 'queued=1' in text
  service.close();service=None;device.close();device=None
  text=await console('stop','c6ble mouse stop','c6ble channel status','c6ble channel receive '+str(old))
  assert 'BLE_CHANNEL ready=1 connected=0 encrypted=0 notify=0' in text and 'queued=0 received=0 sent=0' in text
  assert 'c6ble channel receive result=-' in text
  text=await console('start','c6ble mouse start')
  for i in range(10):
   text=await console('query'+str(i),'c6ble mouse status','c6ble channel status')
   if 'connected=1 encrypted=1 notify=1' in text:break
   await asyncio.sleep(1)
  else:raise AssertionError('HID encrypted subscription not restored')
  new=int(re.search(r'BLE_CHANNEL .*?epoch=(\d+)',text)[1]);assert new>old
  assert 'queued=0 received=0 sent=0' in text
  text=await console('stale','c6ble channel send '+str(old)+' aa','c6ble channel receive '+str(old),'c6ble channel receive '+str(new),'c6ble mouse release','c6ble mouse status','c6ble status')
  # NuttX errno values are checked against the current firmware errno.h.
  assert 'c6ble channel send result=-116' in text and 'c6ble channel receive result=-116' in text
  assert re.search(r'channel receive result=-11[\r\n]',text) and 'c6ble mouse release: ok' in text
  assert 'connected=1 encrypted=1 notify=1' in text and 'reserved_connections=1 central_reserved=0 hid_reserved=1' in text
  state.update(new_epoch=new,queued_frame_cleared_on_disconnect=True,stale_epoch_send_receive_rejected=True,new_epoch_empty_fifo=True,hid_reconnected=True,passed=True);save();print('CHANNEL_LIFECYCLE_OK',old,new,flush=True)
 except BaseException as exc:state['error']=type(exc).__name__+': '+str(exc)[:180];save();raise
 finally:
  if service is not None:service.close()
  if device is not None:device.close()
asyncio.run(main())
