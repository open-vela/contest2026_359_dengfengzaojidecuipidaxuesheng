from pathlib import Path
import asyncio,json,sys,time
from winrt.windows.devices.bluetooth import BluetoothLEDevice,BluetoothAddressType,BluetoothCacheMode
from winrt.windows.devices.bluetooth.advertisement import BluetoothLEAdvertisementWatcher,BluetoothLEScanningMode
from winrt.windows.devices.enumeration import DevicePairingKinds,DevicePairingProtectionLevel

r=Path(__file__).resolve().parent;c=r.parent/'network-share-20261026/candidate-18'
identity=json.loads((c/'firmware/BLE-IDENTITY.json').read_text())
address=int(identity['address'].replace(':',''),16);name=identity['name']
label=sys.argv[1];assert label.replace('-','').isalnum() and len(label)<=32
record=r/(label+'.json');assert not record.exists()
mode=sys.argv[2] if len(sys.argv)>2 else 'pair'
state={'identity':identity['address'],'name':name,'mode':mode,'observed':False,'pairing':None,'services':[]}
def save():record.write_text(json.dumps(state,indent=2)+'\n')
async def main():
 loop=asyncio.get_running_loop();seen=asyncio.Event();watcher=BluetoothLEAdvertisementWatcher();watcher.scanning_mode=BluetoothLEScanningMode.ACTIVE
 def received(sender,args):
  if args.bluetooth_address==address and args.advertisement.local_name==name:loop.call_soon_threadsafe(seen.set)
 token=watcher.add_received(received);device=None;pair_token=None;custom=None;services=[]
 try:
  watcher.start()
  if mode=='pair':
   await asyncio.wait_for(seen.wait(),35);state['observed']=True
  else:
   # A Windows HID reconnect can complete before this process starts.
   # Already connected peers no longer advertise; verify exact saved identity
   # and existing pairing below rather than require a new advertisement.
   try:await asyncio.wait_for(seen.wait(),2);state['observed']=True
   except asyncio.TimeoutError:state['discovery']='known_exact_identity'
  save()
  watcher.stop();device=await asyncio.wait_for(BluetoothLEDevice.from_bluetooth_address_async(address,BluetoothAddressType.RANDOM),20)
  assert device is not None
  info=device.device_information;state['initially_paired']=info.pairing.is_paired;save()
  if mode=='pair' and not info.pairing.is_paired:
   custom=info.pairing.custom
   def requested(sender,args):
    # This handler belongs only to the exact fresh candidate selected above.
    if args.pairing_kind==DevicePairingKinds.CONFIRM_ONLY:args.accept()
   pair_token=custom.add_pairing_requested(requested)
   paired=await asyncio.wait_for(custom.pair_async(DevicePairingKinds.CONFIRM_ONLY,DevicePairingProtectionLevel.ENCRYPTION),60)
   state['pairing']=paired.status.name;state['protection']=paired.protection_level_used.name;save()
   assert state['pairing'] in ('PAIRED','ALREADY_PAIRED'),state['pairing']
  elif mode=='reconnect':assert info.pairing.is_paired,'candidate bond missing'
  result=await asyncio.wait_for(device.get_gatt_services_async(BluetoothCacheMode.UNCACHED),40)
  state['service_result']=result.status.name;save()
  assert result.status.name=='SUCCESS',result.status.name
  services=list(result.services);state['services']=[str(s.uuid) for s in services];state['connected']=device.connection_status.name;save();print(json.dumps(state),flush=True)
  for _ in range(120):
   if (r/(label+'.stop')).exists():break
   await asyncio.sleep(1)
  state['final_connection']=device.connection_status.name;save()
 except BaseException as exc:state['error']=type(exc).__name__+': '+str(exc)[:180];save();raise
 finally:
  watcher.stop();watcher.remove_received(token)
  if custom is not None and pair_token is not None:custom.remove_pairing_requested(pair_token)
  for service in services:service.close()
  if device is not None:device.close()
asyncio.run(main())
