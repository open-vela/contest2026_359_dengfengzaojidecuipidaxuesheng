from pathlib import Path
import asyncio,json,uuid,re,subprocess,sys,hashlib
from winrt.windows.devices.bluetooth import BluetoothLEDevice,BluetoothAddressType,BluetoothCacheMode
from winrt.windows.devices.bluetooth.genericattributeprofile import GattClientCharacteristicConfigurationDescriptorValue as Cccd,GattWriteOption,GattProtectionLevel
from winrt.windows.storage.streams import DataReader,DataWriter

r=Path(__file__).resolve().parent;root=r.parent.parent
candidate=int(sys.argv[1]) if len(sys.argv)>1 else 18
label=sys.argv[2] if len(sys.argv)>2 else 'channel-c18-01'
assert label.replace('-','').isalnum() and len(label)<=32
c=r.parent/'network-share-20261026'/('candidate-'+str(candidate));record=r/(label+'.json');assert not record.exists()
identity=json.loads((c/'firmware/BLE-IDENTITY.json').read_text());meta=json.loads((c/'DEPLOYED.json').read_text());image=hashlib.sha256((c/'firmware/nuttx.bin').read_bytes()).hexdigest();assert image==meta['image_sha256']
uuids=json.loads((r/'UUIDS.json').read_text());address=int(identity['address'].replace(':',''),16)
state={'image_sha256':image,'identity':identity['address'],'name':identity['name'],'uuids':uuids,'rounds':[],'passed':False,'write_buffer':'DataWriter.detach_buffer IBuffer'}
def save():record.write_text(json.dumps(state,indent=2)+'\n')
def unpack(buffer):
 reader=DataReader.from_buffer(buffer);data=bytearray(reader.unconsumed_buffer_length);reader.read_bytes(data);reader.close();return bytes(data)
def packet(data):
 writer=DataWriter();writer.write_bytes(data);buffer=writer.detach_buffer();writer.close();return buffer
def fnv(data):
 n=2166136261
 for b in data:n=((n^b)*16777619)&0xffffffff
 return f'{n:08x}'
async def console(phase,*commands):
 args=[str(root/'.hardware-venv/Scripts/python.exe'),'-X','utf8',str(c/'console.py'),label+'-'+phase,*commands]
 def call():return subprocess.run(args,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,encoding='utf-8',timeout=90)
 assert all(len(command.encode())<80 for command in commands),'diagnostic console line limit'
 result=await asyncio.to_thread(call)
 if result.returncode:raise RuntimeError('console phase '+phase+' failed: '+result.stdout[-180:])
 return result.stdout
async def main():
 device=None;service=None;tx=None;token=None;subscribed=False;events=asyncio.Queue();loop=asyncio.get_running_loop()
 try:
  before=await console('before','c6ble identity','c6ble mouse status','c6ble channel status','c6ble status')
  assert identity['address'] in before and 'connected=1 encrypted=1 notify=1' in before,before[-600:]
  epoch=int(re.search(r'BLE_CHANNEL .*?epoch=(\d+)',before)[1]);state['epoch']=epoch;save()
  device=await asyncio.wait_for(BluetoothLEDevice.from_bluetooth_address_async(address,BluetoothAddressType.RANDOM),20)
  assert device is not None and device.device_information.pairing.is_paired
  # This Windows UUID-filtered discovery operation returned
  # ERROR_BAD_COMMAND. Enumerate through the official unfiltered API, then
  # select only our exact UUID; retain the failed first attempt as evidence.
  result=await asyncio.wait_for(device.get_gatt_services_async(BluetoothCacheMode.UNCACHED),30)
  assert result.status.name=='SUCCESS',result.status.name
  matches=[]
  for entry in result.services:
   if entry.uuid==uuid.UUID(uuids['service']):matches.append(entry)
   else:entry.close()
  assert len(matches)==1,len(matches)
  service=matches[0]
  state['service_access']=(await asyncio.wait_for(service.request_access_async(),20)).name;save()
  async def characteristic(name):
   result=await asyncio.wait_for(service.get_characteristics_for_uuid_async(uuid.UUID(uuids[name]),BluetoothCacheMode.UNCACHED),25)
   assert result.status.name=='SUCCESS' and len(result.characteristics)==1,(name,result.status.name)
   return result.characteristics[0]
  rx=await characteristic('rx');tx=await characteristic('tx');info=await characteristic('info')
  state['characteristics']={key:{'handle':char.attribute_handle,'properties':int(char.characteristic_properties),'protection':char.protection_level.name} for key,char in [('rx',rx),('tx',tx),('info',info)]};save()
  for char in (rx,tx,info):char.protection_level=GattProtectionLevel.ENCRYPTION_REQUIRED
  state['requested_protection']='ENCRYPTION_REQUIRED';save()
  result=await asyncio.wait_for(info.read_value_async(BluetoothCacheMode.UNCACHED),20);assert result.status.name=='SUCCESS'
  protocol=unpack(result.value);assert protocol==bytes([1,0,244,0,4,0,3,0]),protocol.hex();state['protocol_info']=list(protocol);save()
  def changed(sender,args):loop.call_soon_threadsafe(events.put_nowait,unpack(args.characteristic_value))
  token=tx.add_value_changed(changed)
  status=await asyncio.wait_for(tx.write_client_characteristic_configuration_descriptor_async(Cccd.NOTIFY),20)
  assert status.name=='SUCCESS',status.name;subscribed=True
  subscribed_status=await console('subscribed','c6ble channel status','c6ble mouse status','c6ble status')
  assert re.search(r'BLE_CHANNEL ready=1 connected=1 encrypted=1 notify=1',subscribed_status),subscribed_status[-700:]
  assert 'reserved_connections=1 central_reserved=0 hid_reserved=1' in subscribed_status
  payload=int(re.search(r'BLE_CHANNEL .*?payload=(\d+)',subscribed_status)[1]);state['negotiated_payload']=payload;save()
  lengths=[1,4,20,min(payload,20),20,7,20,16]
  for i,n in enumerate(lengths):
   frame=bytes(((i*29+j*13+7)&255) for j in range(n))
   result=await asyncio.wait_for(rx.write_value_with_result_async(packet(frame),GattWriteOption.WRITE_WITH_RESPONSE),25)
   state['last_write']={'round':i+1,'status':result.status.name,'protocol_error':result.protocol_error};save()
   assert result.status.name=='SUCCESS',result.status.name
   commands=['c6ble channel receive '+str(epoch),'c6ble channel send '+str(epoch)+' '+frame.hex(),'c6ble mouse release','c6ble mouse status']
   text=await console('r'+str(i+1),*commands)
   # NSH echo and background logs can interleave the command-name prefix;
   # retain the complete operation/result suffix and independent peer bytes.
   assert f'bytes={n} fnv32={fnv(frame)}' in text and 'channel send result=0' in text
   assert 'connected=1 encrypted=1 notify=1' in text
   notified=await asyncio.wait_for(events.get(),15);assert notified==frame,('notification mismatch',i+1)
   value=await asyncio.wait_for(tx.read_value_async(BluetoothCacheMode.UNCACHED),20)
   assert value.status.name=='SUCCESS' and unpack(value.value)==frame,('readback mismatch',i+1)
   state['rounds'].append({'round':i+1,'bytes':n,'fnv32':fnv(frame),'receive_and_notify_and_readback':True,'hid_release_accepted':True});save();print('SHARED_CHANNEL_ROUND_OK',i+1,n,flush=True)
  large=bytes((j*7+3)&255 for j in range(244))
  result=await asyncio.wait_for(rx.write_value_with_result_async(packet(large),GattWriteOption.WRITE_WITH_RESPONSE),25)
  assert result.status.name=='SUCCESS',result.status.name
  text=await console('large-rx','c6ble channel receive '+str(epoch))
  assert 'bytes=244 fnv32='+fnv(large) in text
  state['large_receive_bytes_verified']=244;state['outgoing_bytes_verified']=max(lengths);save()
  frames=[bytes([0x90+i])*4 for i in range(5)]
  for frame in frames[:4]:
   result=await asyncio.wait_for(rx.write_value_with_result_async(packet(frame),GattWriteOption.WRITE_WITH_RESPONSE),20);assert result.status.name=='SUCCESS'
  result=await asyncio.wait_for(rx.write_value_with_result_async(packet(frames[4]),GattWriteOption.WRITE_WITH_RESPONSE),20)
  state['queue_full_status']=result.status.name;state['queue_full_att_error']=result.protocol_error;save()
  assert result.status.name=='PROTOCOL_ERROR' and result.protocol_error==0x11,(result.status.name,result.protocol_error)
  text=await console('drain',*['c6ble channel receive '+str(epoch)]*4)
  hashes=re.findall(r'BLE_CHANNEL_DATA bytes=4 fnv32=([0-9a-f]{8})',text);assert hashes==[fnv(f) for f in frames[:4]],hashes
  status=await asyncio.wait_for(tx.write_client_characteristic_configuration_descriptor_async(Cccd.NONE),20);assert status.name=='SUCCESS';subscribed=False
  text=await console('after','c6ble channel status','c6ble mouse release','c6ble mouse status','c6ble status','desktop xip','desktop ui render-metrics')
  assert re.search(r'BLE_CHANNEL ready=1 connected=1 encrypted=1 notify=0',text)
  assert 'connected=1 encrypted=1 notify=1' in text and 'reserved_connections=1 central_reserved=0 hid_reserved=1' in text
  state.update(passed=True,round_count=len(state['rounds']),rx_fifo_backpressure_and_order_verified=True,channel_unsubscribe_keeps_hid=True,same_physical_connection_verified=True,ordinary_connections=0,final_connection=device.connection_status.name);save();print('SHARED_HID_GATT_OK',len(state['rounds']),flush=True)
 except BaseException as exc:state['error']=type(exc).__name__+': '+str(exc)[:220];save();raise
 finally:
  if tx is not None and subscribed:
   try:await asyncio.wait_for(tx.write_client_characteristic_configuration_descriptor_async(Cccd.NONE),10)
   except Exception:pass
  if tx is not None and token is not None:tx.remove_value_changed(token)
  if service is not None:service.close()
  if device is not None:device.close()
asyncio.run(main())
