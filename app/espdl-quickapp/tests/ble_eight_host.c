/* SPDX-License-Identifier: Apache-2.0 */
/* Controlled eight-peer transport: exercises the actual central/mailbox,
 * never a controller or real peripheral. Reuse the same NimBLE substitutes. */
#define main two_peer_reference_main
#include "ble_central_host.c"
#undef main

int main(void)
{
  _Static_assert(GLASS_BLE_CENTRAL_LIMIT == 8, "eight-peer fixture required");
  pthread_t host, workers[8];
  struct query queries[8];
  struct remote replies[8], old_links[8];
  uint32_t generations[8];
  struct glass_ble_state state={0};
  struct glass_ble_capacity_state slots;
  struct glass_gatt_result out;
  capacity=8;
  assert(!pthread_create(&host,NULL,host_loop,NULL));
  assert(!glass_ble_central_init());
  assert(!glass_ble_central_scan());
  for(unsigned i=0;i<9;i++)advertise_peer(i+1,"Eight-peer fixture");
  glass_ble_central_read(&state);
  uint32_t scan=state.scan_generation;
  assert(state.count==9&&state.central_capacity==8);
  assert(!glass_ble_central_scan_stop(scan));
  for(unsigned i=0;i<8;i++){
    next_handle=(uint16_t)(7+i);
    assert(!glass_ble_central_connect_session(i,scan,&generations[i]));
    connected();
  }
  glass_ble_central_read(&state);glass_ble_capacity_read(&slots);
  assert(state.session_count==8&&state.connection_count==8&&slots.reserved==8);
  assert(glass_ble_central_connect(8,scan)==-ENOSPC);
  assert(glass_ble_central_connect(0,scan)==-EALREADY);
  assert(glass_ble_role_claim(GLASS_BLE_ROLE_HID)==-EBUSY);
  unsigned peak=state.connection_count;
  for(unsigned i=0;i<8;i++){
    struct glass_gatt_request request={.generation=generations[i],.operation=GLASS_GATT_READ,.handle=3};
    assert(!glass_ble_gatt(&request,&out,NULL)&&out.data[0]==7+i);
    request.operation=GLASS_GATT_MTU;
    assert(!glass_ble_gatt(&request,&out,NULL)&&out.mtu==100);
    if(!(i&1)){request.operation=GLASS_GATT_PAIR;assert(!glass_ble_gatt(&request,&out,NULL));}
    notify((uint16_t)(7+i),10,i*16);
    assert(!glass_ble_notification(generations[i],&out));
    assert(out.data[0]==i*16+2&&out.dropped==2);
  }
  puts("BLE_EIGHT_ADMISSION_OK: eight live logical peers, ninth/duplicate rejected; isolated GATT/MTU/security/notification overflow");

  atomic_store(&stall,true);memset(queries,0,sizeof(queries));
  for(unsigned i=0;i<8;i++){
    queries[i].r=(struct glass_gatt_request){.generation=generations[i],.operation=GLASS_GATT_READ,.handle=3};
    assert(!pthread_create(&workers[i],NULL,query_thread,&queries[i]));
    replies[i]=wait_read((uint16_t)(7+i));
  }
  atomic_store(&queries[3].cancel,true);pthread_join(workers[3],NULL);
  assert(queries[3].result==-ECANCELED);
  for(unsigned i=0;i<8;i++)if(i!=3){
    assert(!atomic_load(&queries[i].finished));
    answer((uint16_t)(7+i),replies[i]);pthread_join(workers[i],NULL);
    assert(!queries[i].result&&queries[i].out.data[0]==7+i);
  }
  next_handle=10;
  uint32_t old_generation=generations[3];
  assert(!glass_ble_central_connect_session(3,scan,&generations[3]));connected();
  assert(generations[3]!=old_generation);
  clear_read(10);struct query replacement={.r={.generation=generations[3],.operation=GLASS_GATT_READ,.handle=3}};
  assert(!pthread_create(&workers[3],NULL,query_thread,&replacement));
  struct remote fresh=wait_read(10);
  answer(10,replies[3]);usleep(60000);assert(!atomic_load(&replacement.finished));
  answer(10,fresh);pthread_join(workers[3],NULL);assert(!replacement.result);
  atomic_store(&stall,false);
  puts("BLE_EIGHT_PARALLEL_OK: eight independent waiters; one cancellation preserves seven; reused handle rejects stale callback");

  for(unsigned cycle=0;cycle<10;cycle++){
    for(unsigned i=0;i<8;i++)old_links[i]=remotes[7+i];
    assert(!glass_ble_central_mouse_pause());
    glass_ble_central_read(&state);glass_ble_capacity_read(&slots);
    assert(state.mouse_paused&&state.session_count==8&&!state.connection_count&&!slots.reserved);
    assert(!glass_ble_role_claim(GLASS_BLE_ROLE_HID));
    assert(glass_ble_central_scan()==-EBUSY);
    assert(glass_ble_central_mouse_resume()==-EBUSY);
    glass_ble_role_release(GLASS_BLE_ROLE_HID);
    atomic_store(&auto_connect,true);
    assert(!glass_ble_central_mouse_resume());atomic_store(&auto_connect,false);
    glass_ble_central_read(&state);
    assert(!state.restoring&&!state.mouse_paused&&state.connection_count==8&&state.restore_count==8);
    for(unsigned i=0;i<8;i++){
      assert(state.restores[i].generation==generations[i]);
      assert(state.restores[i].attempted&&state.restores[i].connected&&!state.restores[i].error);
      struct ble_gap_event late={.type=BLE_GAP_EVENT_DISCONNECT};late.disconnect.conn.conn_handle=(uint16_t)(7+i);
      old_links[i].gap(&late,old_links[i].arg);
    }
    glass_ble_central_read(&state);assert(state.connection_count==8);
  }
  puts("BLE_EIGHT_RESTORE_OK: ten eight-peer pause/restore cycles; HID exclusivity; eighty old GAP callbacks rejected");

  /* A controller which accepts cancel but withholds its terminal event
   * must not receive another initiator. Exercise the production deadlines. */
  assert(!glass_ble_central_mouse_pause());restore_hold_peer=1;
  atomic_store(&cancel_requested,false);atomic_store(&defer_connect_cancel,true);
  atomic_store(&auto_connect,true);assert(glass_ble_central_mouse_resume()==-ETIMEDOUT);
  assert(atomic_load(&cancel_requested));glass_ble_central_read(&state);
  assert(state.mouse_paused&&!state.connection_count&&state.restore_count==8);
  assert(state.restores[0].attempted&&state.restores[0].error==-ETIMEDOUT);
  for(unsigned i=1;i<8;i++)assert(!state.restores[i].attempted&&state.restores[i].error==-EAGAIN);
  struct ble_gap_event terminal={.type=BLE_GAP_EVENT_CONNECT};terminal.connect.status=BLE_HS_ETIMEOUT;
  remotes[7].gap(&terminal,remotes[7].arg);
  restore_hold_peer=0;atomic_store(&defer_connect_cancel,false);
  assert(!glass_ble_central_mouse_resume());atomic_store(&auto_connect,false);
  glass_ble_central_read(&state);assert(state.connection_count==7&&!state.mouse_paused);
  next_handle=7;assert(!glass_ble_central_connect_session(0,scan,&generations[0]));connected();
  puts("BLE_EIGHT_CANCEL_BARRIER_OK: missing cancel acknowledgement blocks seven initiators; explicit retry after terminal event restores them");

  assert(!glass_ble_central_mouse_pause());restore_fail_peer=4;atomic_store(&auto_connect,true);
  assert(glass_ble_central_mouse_resume()==-ETIMEDOUT);
  atomic_store(&auto_connect,false);restore_fail_peer=0;
  glass_ble_central_read(&state);
  assert(state.connection_count==7&&state.session_count==7&&state.restore_count==8&&!state.mouse_paused);
  for(unsigned i=0;i<8;i++){
    assert(state.restores[i].generation==generations[i]&&state.restores[i].attempted);
    assert(state.restores[i].connected==(i!=3));
    assert(state.restores[i].error==(i==3?-ETIMEDOUT:0));
  }
  next_handle=10;assert(!glass_ble_central_connect_session(3,scan,&generations[3]));connected();
  assert(!glass_ble_central_mouse_pause());
  assert(!glass_ble_central_disconnect_generation(generations[5]));
  atomic_store(&auto_connect,true);assert(!glass_ble_central_mouse_resume());atomic_store(&auto_connect,false);
  glass_ble_central_read(&state);assert(state.connection_count==7&&state.restore_count==7);
  next_handle=12;assert(!glass_ble_central_connect_session(5,scan,&generations[5]));connected();
  puts("BLE_EIGHT_FAILURE_OK: one failed reconnect preserves seven; owner exit while paused cancels only its restore");

  atomic_store(&stall,true);memset(queries,0,sizeof(queries));
  for(unsigned i=0;i<8;i++){
    clear_read((uint16_t)(7+i));
    queries[i].r=(struct glass_gatt_request){.generation=generations[i],.operation=GLASS_GATT_READ,.handle=3};
    assert(!pthread_create(&workers[i],NULL,query_thread,&queries[i]));wait_read((uint16_t)(7+i));
  }
  glass_ble_central_reset();
  for(unsigned i=0;i<8;i++){pthread_join(workers[i],NULL);assert(queries[i].result==-EIO);}
  glass_ble_central_read(&state);assert(!state.session_count&&!state.connection_count&&!state.mouse_paused);
  assert(!glass_ble_central_mouse_resume());
  atomic_store(&stop_host,true);glass_ble_central_hid_wake();pthread_join(host,NULL);
  printf("BLE_EIGHT_OK: simulated_peak=%u restore_cycles=10 link_slots=%u snapshot_bytes=%zu; ASan/UBSan; NO_RADIO_TEST\n",
         peak,GLASS_BLE_CENTRAL_LIMIT,sizeof(state));
  return 0;
}
