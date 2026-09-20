set pagination off
set confirm off
set remotetimeout 20
target remote 127.0.0.1:3333
monitor halt
python
address = int(gdb.parse_and_eval('g_ui_probe_frame'))
size = int(gdb.parse_and_eval('g_ui_probe_frame_bytes'))
assert address and 0 < size <= 1024 * 600 * 4 and size % 4 == 0
gdb.execute('monitor dump_image "05-logs-and-handoff/homeassistant-ui-capture-20260919/light-ha.rle565" 0x%x 0x%x' % (address, size))
end
monitor resume
detach
quit
